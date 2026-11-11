#include "bsa_folder.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <strsafe.h>

#include <propsys.h>
#include <propvarutil.h>

// Defines the PKEY_ values in this file, so it comes after the headers above.
#include <initguid.h>

#include <propkey.h>

// The Winner column inside archives in the game's Data folder: Yes when this copy
// is the one the game loads.
DEFINE_PROPERTYKEY(PKEY_WHAM_BsaWinner, 0x78b8355f, 0xf091, 0x418e, 0x84, 0xf8, 0xfe, 0x81, 0xdd, 0x8d, 0x04, 0x12, 2);

#include <bsa/reader.hpp>

#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include <thumbcache.h>
#include "bsa_archive.h"
#include "work_budget.h"
#include "context_menu.h"
#include "registration.h"
#include "winner.h"

namespace pse {
namespace {

// Anything that allocates runs inside this: no exception may cross into Explorer.
template <typename F>
HRESULT Guard(F&& f) noexcept {
    try {
        return f();
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_FAIL;
    }
}

// ---- Item IDs ----

#pragma pack(push, 1)
struct ItemData {
    USHORT cb;
    DWORD magic;
    BYTE isDir;
    WCHAR name[1];
};
#pragma pack(pop)
constexpr DWORD kMagic = 0x31415342;  // "BSA1"

LPITEMIDLIST MakeItem(const std::wstring& name, bool dir) {
    const size_t bytes = offsetof(ItemData, name) + (name.size() + 1) * sizeof(WCHAR);
    auto* p = static_cast<BYTE*>(CoTaskMemAlloc(bytes + sizeof(USHORT)));
    if (!p) throw std::bad_alloc();
    ZeroMemory(p, bytes + sizeof(USHORT));
    auto* d = reinterpret_cast<ItemData*>(p);
    d->cb = static_cast<USHORT>(bytes);
    d->magic = kMagic;
    d->isDir = dir ? 1 : 0;
    memcpy(d->name, name.c_str(), (name.size() + 1) * sizeof(WCHAR));
    return reinterpret_cast<LPITEMIDLIST>(p);
}

// Item IDs can reach us from anywhere, so each one is checked before use.
const ItemData* ReadItem(LPCITEMIDLIST id) {
    if (!id || id->mkid.cb < offsetof(ItemData, name) + sizeof(WCHAR)) return nullptr;
    auto* d = reinterpret_cast<const ItemData*>(id);
    if (d->magic != kMagic) return nullptr;
    const size_t chars = (d->cb - offsetof(ItemData, name)) / sizeof(WCHAR);
    return wmemchr(d->name, L'\0', chars) ? d : nullptr;
}

const wchar_t* TypeText(const std::wstring& name) {
    static std::mutex lock;
    static std::map<std::wstring, std::wstring> types;
    const size_t dot = name.rfind(L'.');
    const std::wstring ext = dot == std::wstring::npos ? L"." : Lower(name.substr(dot));
    std::lock_guard<std::mutex> g(lock);
    auto it = types.find(ext);
    if (it == types.end()) {
        SHFILEINFOW info{};
        SHGetFileInfoW(ext.c_str(), FILE_ATTRIBUTE_NORMAL, &info, sizeof(info), SHGFI_USEFILEATTRIBUTES | SHGFI_TYPENAME);
        it = types.emplace(ext, info.szTypeName).first;
    }
    return it->second.c_str();
}

class Folder;
HRESULT MakeMenu(Folder* folder, const std::vector<int>& nodes, REFIID riid, void** ppv);
HRESULT MakeData(const ArchivePtr& archive, const std::vector<int>& nodes, REFIID riid, void** ppv);

// ---- Enumerating a folder's children ----

class Enumerator final : public IEnumIDList {
public:
    Enumerator(ArchivePtr archive, std::vector<int> items) : archive_(std::move(archive)), items_(std::move(items)) { InterlockedIncrement(&g_dllRefs); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IEnumIDList) {
            *ppv = static_cast<IEnumIDList*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    IFACEMETHODIMP Next(ULONG want, LPITEMIDLIST* out, ULONG* got) override {
        if (!out) return E_POINTER;
        return Guard([&]() -> HRESULT {
            ULONG n = 0;
            for (; n < want && next_ < items_.size(); ++n, ++next_) {
                const Node& node = archive_->At(items_[next_]);
                out[n] = MakeItem(node.name, node.dir());
            }
            if (got) *got = n;
            return n == want ? S_OK : S_FALSE;
        });
    }
    IFACEMETHODIMP Skip(ULONG count) override {
        next_ = std::min(items_.size(), next_ + count);
        return next_ < items_.size() ? S_OK : S_FALSE;
    }
    IFACEMETHODIMP Reset() override {
        next_ = 0;
        return S_OK;
    }
    IFACEMETHODIMP Clone(IEnumIDList** out) override {
        if (!out) return E_POINTER;
        return Guard([&]() -> HRESULT {
            auto* e = new Enumerator(archive_, items_);
            e->next_ = next_;
            *out = e;
            return S_OK;
        });
    }

private:
    ~Enumerator() { InterlockedDecrement(&g_dllRefs); }
    long refs_ = 1;
    ArchivePtr archive_;
    std::vector<int> items_;
    size_t next_ = 0;
};

// ---- The folder itself ----

struct Column {
    const wchar_t* title;
    int format;
    int width;
};
const Column kColumns[] = {{L"Name", LVCFMT_LEFT, 40}, {L"Size", LVCFMT_RIGHT, 12}, {L"Type", LVCFMT_LEFT, 24}, {L"Winner", LVCFMT_CENTER, 8}};
constexpr UINT kWinnerColumn = 3;  // only inside archives in the game's Data folder

bool HasPictureThumbnail(const std::wstring& name) {
    const wchar_t* ext = PathFindExtensionW(name.c_str());
    for (const wchar_t* e : {L".dds", L".tga", L".png", L".jpg", L".jpeg", L".bmp", L".gif", L".tif", L".tiff"}) {
        if (_wcsicmp(ext, e) == 0) return true;
    }
    return false;
}

// A picture for the large icon views. The file is copied out for a moment and
// Windows makes the thumbnail, so it looks the same as one in a normal folder.
class Thumbnail final : public IThumbnailProvider, public IExtractImage {
public:
    Thumbnail(ArchivePtr archive, int node) : archive_(std::move(archive)), node_(node) { InterlockedIncrement(&g_dllRefs); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IThumbnailProvider) *ppv = static_cast<IThumbnailProvider*>(this);
        else if (riid == IID_IExtractImage) *ppv = static_cast<IExtractImage*>(this);
        else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    IFACEMETHODIMP GetThumbnail(UINT size, HBITMAP* bitmap, WTS_ALPHATYPE* alpha) override {
        if (!bitmap) return E_POINTER;
        *bitmap = nullptr;
        if (alpha) *alpha = WTSAT_UNKNOWN;
        return Make(size, bitmap);
    }

    IFACEMETHODIMP GetLocation(LPWSTR path, DWORD chars, DWORD*, const SIZE* size, DWORD, DWORD*) override {
        if (size) size_ = (std::max)(size->cx, size->cy);
        if (!path || !chars) return E_POINTER;
        return StringCchCopyW(path, chars, (archive_->Path() + L"\\" + archive_->RelativePath(node_)).c_str());
    }
    IFACEMETHODIMP Extract(HBITMAP* bitmap) override {
        if (!bitmap) return E_POINTER;
        *bitmap = nullptr;
        return Make(static_cast<UINT>(size_), bitmap);
    }

private:
    ~Thumbnail() { InterlockedDecrement(&g_dllRefs); }

    HRESULT Make(UINT size, HBITMAP* bitmap) {
        try {
            WorkSlot slot(WorkKind::Background);
            std::vector<uint8_t> data;
            if (!archive_->Read(node_, &data) || data.empty()) return E_FAIL;
            wchar_t temp[MAX_PATH];
            if (!GetTempPathW(ARRAYSIZE(temp), temp)) return E_FAIL;
            static volatile long counter = 0;
            const std::wstring dir = std::wstring(temp) + L"WHAM thumbnails";
            CreateDirectoryW(dir.c_str(), nullptr);
            const std::wstring file = dir + L"\\" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(InterlockedIncrement(&counter)) + PathFindExtensionW(archive_->At(node_).name.c_str());
            HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_TEMPORARY, nullptr);
            if (h == INVALID_HANDLE_VALUE) return HRESULT_FROM_WIN32(GetLastError());
            DWORD wrote = 0;
            const BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr) && wrote == data.size();
            CloseHandle(h);
            HRESULT hr = ok ? S_OK : E_FAIL;
            IShellItemImageFactory* factory = nullptr;
            if (SUCCEEDED(hr)) hr = SHCreateItemFromParsingName(file.c_str(), nullptr, IID_PPV_ARGS(&factory));
            if (SUCCEEDED(hr)) {
                const SIZE want{static_cast<LONG>(size), static_cast<LONG>(size)};
                hr = factory->GetImage(want, SIIGBF_THUMBNAILONLY | SIIGBF_RESIZETOFIT, bitmap);
                factory->Release();
            }
            DeleteFileW(file.c_str());
            slot.Pace();
            return hr;
        } catch (const std::bad_alloc&) {
            return E_OUTOFMEMORY;
        }
    }

    long refs_ = 1;
    ArchivePtr archive_;
    int node_;
    LONG size_ = 96;
};

class Folder final : public IShellFolder2, public IPersistFolder2 {
public:
    Folder() { InterlockedIncrement(&g_dllRefs); }
    Folder(ArchivePtr archive, int node, LPITEMIDLIST pidl, std::wstring display) : archive_(std::move(archive)), node_(node), pidl_(pidl), display_(std::move(display)) {
        InterlockedIncrement(&g_dllRefs);
        inData_ = archive_ && IsInGameData(archive_->Path());
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IShellFolder || riid == IID_IShellFolder2) {
            *ppv = static_cast<IShellFolder2*>(this);
        } else if (riid == IID_IPersist || riid == IID_IPersistFolder || riid == IID_IPersistFolder2) {
            *ppv = static_cast<IPersistFolder2*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    // IPersistFolder2
    IFACEMETHODIMP GetClassID(CLSID* clsid) override {
        if (!clsid) return E_POINTER;
        *clsid = kClsidBsaFolder;
        return S_OK;
    }
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE pidl) override {
        if (!pidl) return E_INVALIDARG;
        return Guard([&]() -> HRESULT {
            wchar_t path[MAX_PATH * 4];
            if (!SHGetPathFromIDListEx(pidl, path, ARRAYSIZE(path), GPFIDL_DEFAULT)) return E_INVALIDARG;
            HRESULT hr;
            auto archive = Archive::Open(path, &hr);
            if (pidl_) ILFree(pidl_);
            pidl_ = ILCloneFull(pidl);
            if (!pidl_) return E_OUTOFMEMORY;
            display_ = path;
            archive_ = std::move(archive);
            node_ = 0;
            inData_ = archive_ && IsInGameData(path);
            // A damaged file still opens, as an empty folder that says why.
            openResult_ = hr;
            return S_OK;
        });
    }
    IFACEMETHODIMP GetCurFolder(PIDLIST_ABSOLUTE* out) override {
        if (!out) return E_POINTER;
        *out = pidl_ ? ILCloneFull(pidl_) : nullptr;
        return *out ? S_OK : E_FAIL;
    }

    // IShellFolder
    IFACEMETHODIMP ParseDisplayName(HWND, IBindCtx*, LPWSTR name, ULONG* eaten, PIDLIST_RELATIVE* out, ULONG* attrs) override {
        if (!name || !out) return E_POINTER;
        *out = nullptr;
        if (!archive_) return openResult_;
        return Guard([&]() -> HRESULT {
            std::wstring path = name;
            std::replace(path.begin(), path.end(), L'/', L'\\');
            int node = node_;
            LPITEMIDLIST result = nullptr;
            for (size_t p = 0; p <= path.size();) {
                size_t slash = path.find(L'\\', p);
                if (slash == std::wstring::npos) slash = path.size();
                const std::wstring part = path.substr(p, slash - p);
                p = slash + 1;
                if (part.empty()) continue;
                const int child = archive_->Child(node, part.c_str());
                if (child < 0) {
                    if (result) ILFree(result);
                    return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
                }
                LPITEMIDLIST item = MakeItem(archive_->At(child).name, archive_->At(child).dir());
                LPITEMIDLIST joined = result ? ILCombine(result, item) : item;
                if (result) {
                    ILFree(result);
                    ILFree(item);
                }
                if (!joined) return E_OUTOFMEMORY;
                result = joined;
                node = child;
            }
            if (!result) return E_INVALIDARG;
            if (eaten) *eaten = static_cast<ULONG>(path.size());
            if (attrs) *attrs &= AttributesOf(node);
            *out = reinterpret_cast<PIDLIST_RELATIVE>(result);
            return S_OK;
        });
    }

    IFACEMETHODIMP EnumObjects(HWND, SHCONTF flags, IEnumIDList** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!archive_) return openResult_;
        return Guard([&]() -> HRESULT {
            std::vector<int> items;
            for (int child : archive_->At(node_).children) {
                const bool dir = archive_->At(child).dir();
                if ((dir && (flags & SHCONTF_FOLDERS)) || (!dir && (flags & SHCONTF_NONFOLDERS))) items.push_back(child);
            }
            *out = new Enumerator(archive_, std::move(items));
            return S_OK;
        });
    }

    IFACEMETHODIMP BindToObject(PCUIDLIST_RELATIVE pidl, IBindCtx*, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!archive_) return openResult_;
        return Guard([&]() -> HRESULT {
            int node = Resolve(pidl);
            if (node < 0) return E_INVALIDARG;
            if (riid == IID_IStream) return OpenStream(node, reinterpret_cast<IStream**>(ppv));
            if (!archive_->At(node).dir()) return HRESULT_FROM_WIN32(ERROR_DIRECTORY);
            if (riid != IID_IShellFolder && riid != IID_IShellFolder2 && riid != IID_IPersistFolder2 && riid != IID_IPersistFolder && riid != IID_IPersist && riid != IID_IUnknown)
                return E_NOINTERFACE;
            LPITEMIDLIST full = ILCombine(pidl_, pidl);
            if (!full) return E_OUTOFMEMORY;
            std::wstring display = archive_->Path() + L"\\" + archive_->RelativePath(node);
            auto* child = new Folder(archive_, node, full, std::move(display));
            HRESULT hr = child->QueryInterface(riid, ppv);
            child->Release();
            return hr;
        });
    }
    IFACEMETHODIMP BindToStorage(PCUIDLIST_RELATIVE pidl, IBindCtx* bc, REFIID riid, void** ppv) override { return BindToObject(pidl, bc, riid, ppv); }

    IFACEMETHODIMP CompareIDs(LPARAM lparam, PCUIDLIST_RELATIVE a, PCUIDLIST_RELATIVE b) override {
        return Guard([&]() -> HRESULT { return Compare(node_, lparam, a, b); });
    }

    HRESULT Compare(int node, LPARAM lparam, PCUIDLIST_RELATIVE a, PCUIDLIST_RELATIVE b) const {
        {
            const ItemData* x = ReadItem(a);
            const ItemData* y = ReadItem(b);
            if (!x || !y) return E_INVALIDARG;
            int r = 0;
            if (x->isDir != y->isDir) {
                r = x->isDir ? -1 : 1;
            } else {
                const UINT column = static_cast<UINT>(lparam & SHCIDS_COLUMNMASK);
                if (column == 1 && archive_) {
                    const int cx = archive_->Child(node, x->name), cy = archive_->Child(node, y->name);
                    const uint64_t sx = cx >= 0 ? archive_->Size(cx) : 0, sy = cy >= 0 ? archive_->Size(cy) : 0;
                    r = sx < sy ? -1 : sx > sy ? 1 : 0;
                } else if (column == 2) {
                    r = _wcsicmp(TypeText(x->name), TypeText(y->name));
                } else if (column == kWinnerColumn && inData_ && archive_) {
                    const int cx = archive_->Child(node, x->name), cy = archive_->Child(node, y->name);
                    r = _wcsicmp(WinnerText(cx), WinnerText(cy));
                }
                if (r == 0) r = StrCmpLogicalW(x->name, y->name);
            }
            if (r == 0) {
                PCUIDLIST_RELATIVE nextA = ILNext(a), nextB = ILNext(b);
                const bool endA = ILIsEmpty(nextA), endB = ILIsEmpty(nextB);
                if (endA != endB) r = endA ? -1 : 1;
                else if (!endA && archive_) {
                    const int child = archive_->Child(node, x->name);
                    if (child >= 0) return Compare(child, lparam, nextA, nextB);
                }
            }
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, static_cast<USHORT>(static_cast<short>(r < 0 ? -1 : r > 0 ? 1 : 0)));
        }
    }

    IFACEMETHODIMP CreateViewObject(HWND, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (riid == IID_IShellView) {
            SFV_CREATE create{sizeof(create), static_cast<IShellFolder*>(static_cast<IShellFolder2*>(this)), nullptr, nullptr};
            return SHCreateShellFolderView(&create, reinterpret_cast<IShellView**>(ppv));
        }
        return E_NOINTERFACE;  // no IDropTarget: nothing can be dropped in
    }

    IFACEMETHODIMP GetAttributesOf(UINT count, PCUITEMID_CHILD_ARRAY items, SFGAOF* flags) override {
        if (!flags) return E_POINTER;
        return Guard([&]() -> HRESULT {
            SFGAOF all = 0xFFFFFFFF;
            if (count == 0) all = SFGAO_FOLDER | SFGAO_BROWSABLE | SFGAO_READONLY;
            for (UINT i = 0; i < count; ++i) {
                const int node = archive_ ? Resolve(items[i]) : -1;
                if (node < 0) return E_INVALIDARG;
                all &= AttributesOf(node);
            }
            *flags &= all;
            return S_OK;
        });
    }

    IFACEMETHODIMP GetUIObjectOf(HWND, UINT count, PCUITEMID_CHILD_ARRAY items, REFIID riid, UINT*, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (!archive_) return openResult_;
        return Guard([&]() -> HRESULT {
            std::vector<int> nodes;
            for (UINT i = 0; i < count; ++i) {
                const int node = Resolve(items[i]);
                if (node < 0) return E_INVALIDARG;
                nodes.push_back(node);
            }
            if (nodes.empty()) return E_INVALIDARG;
            if (riid == IID_IContextMenu || riid == IID_IContextMenu2 || riid == IID_IContextMenu3) return MakeMenu(this, nodes, riid, ppv);
            if (riid == IID_IDataObject) return MakeData(archive_, nodes, riid, ppv);
            if ((riid == IID_IThumbnailProvider || riid == IID_IExtractImage) && nodes.size() == 1) {
                const Node& n = archive_->At(nodes[0]);
                if (n.dir() || !HasPictureThumbnail(n.name)) return E_NOINTERFACE;
                auto* thumbnail = new Thumbnail(archive_, nodes[0]);
                const HRESULT hr = thumbnail->QueryInterface(riid, ppv);
                thumbnail->Release();
                return hr;
            }
            if ((riid == IID_IExtractIconW || riid == IID_IExtractIconA) && nodes.size() == 1) {
                const Node& n = archive_->At(nodes[0]);
                return SHCreateFileExtractIconW(n.name.c_str(), n.dir() ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL, riid, ppv);
            }
            return E_NOINTERFACE;  // IDropTarget included: read-only
        });
    }

    IFACEMETHODIMP GetDisplayNameOf(PCUITEMID_CHILD item, SHGDNF flags, STRRET* out) override {
        if (!out) return E_POINTER;
        return Guard([&]() -> HRESULT {
            const ItemData* d = ReadItem(item);
            if (!d) return E_INVALIDARG;
            std::wstring text = d->name;
            if ((flags & SHGDN_FORPARSING) && !(flags & SHGDN_INFOLDER)) text = display_ + L"\\" + text;
            out->uType = STRRET_WSTR;
            return SHStrDupW(text.c_str(), &out->pOleStr);
        });
    }
    IFACEMETHODIMP SetNameOf(HWND, PCUITEMID_CHILD, LPCWSTR, SHGDNF, PITEMID_CHILD*) override { return E_ACCESSDENIED; }

    // IShellFolder2
    IFACEMETHODIMP GetDefaultSearchGUID(GUID*) override { return E_NOTIMPL; }
    IFACEMETHODIMP EnumSearches(IEnumExtraSearch**) override { return E_NOTIMPL; }
    IFACEMETHODIMP GetDefaultColumn(DWORD, ULONG* sort, ULONG* display) override {
        if (sort) *sort = 0;
        if (display) *display = 0;
        return S_OK;
    }
    IFACEMETHODIMP GetDefaultColumnState(UINT column, SHCOLSTATEF* state) override {
        if (!state) return E_POINTER;
        if (column >= ColumnCount()) return E_INVALIDARG;
        *state = SHCOLSTATE_ONBYDEFAULT | (column == 1 ? SHCOLSTATE_TYPE_INT : SHCOLSTATE_TYPE_STR);
        if (column == kWinnerColumn) *state |= SHCOLSTATE_SLOW;  // Explorer asks for it on a background thread
        return S_OK;
    }
    IFACEMETHODIMP GetDetailsEx(PCUITEMID_CHILD item, const SHCOLUMNID* id, VARIANT* out) override {
        if (!id || !out) return E_POINTER;
        return Guard([&]() -> HRESULT {
            const int node = archive_ ? Resolve(item) : -1;
            if (node < 0) return E_INVALIDARG;
            const PROPERTYKEY& key = *reinterpret_cast<const PROPERTYKEY*>(id);
            const Node& n = archive_->At(node);
            if (IsEqualPropertyKey(key, PKEY_ItemNameDisplay)) return InitVariantFromString(n.name.c_str(), out);
            if (IsEqualPropertyKey(key, PKEY_Size) && !n.dir()) return InitVariantFromUInt64(archive_->Size(node), out);
            if (IsEqualPropertyKey(key, PKEY_ItemTypeText)) return InitVariantFromString(n.dir() ? L"File folder" : TypeText(n.name), out);
            if (IsEqualPropertyKey(key, PKEY_WHAM_BsaWinner) && inData_ && !n.dir()) {
                const wchar_t* text = WinnerText(node);
                return *text ? InitVariantFromString(text, out) : E_FAIL;
            }
            return E_FAIL;
        });
    }
    IFACEMETHODIMP GetDetailsOf(PCUITEMID_CHILD item, UINT column, SHELLDETAILS* out) override {
        if (!out) return E_POINTER;
        if (column >= ColumnCount()) return E_INVALIDARG;
        return Guard([&]() -> HRESULT {
            out->fmt = kColumns[column].format;
            out->cxChar = kColumns[column].width;
            std::wstring text;
            if (!item) {
                text = kColumns[column].title;
            } else {
                const int node = archive_ ? Resolve(item) : -1;
                if (node < 0) return E_INVALIDARG;
                const Node& n = archive_->At(node);
                if (column == 0) {
                    text = n.name;
                } else if (column == 1 && !n.dir()) {
                    wchar_t size[64] = {};
                    StrFormatKBSizeW(static_cast<LONGLONG>(archive_->Size(node)), size, ARRAYSIZE(size));
                    text = size;
                } else if (column == 2) {
                    text = n.dir() ? L"File folder" : TypeText(n.name);
                } else if (column == kWinnerColumn && !n.dir()) {
                    text = WinnerText(node);
                }
            }
            out->str.uType = STRRET_WSTR;
            return SHStrDupW(text.c_str(), &out->str.pOleStr);
        });
    }
    IFACEMETHODIMP MapColumnToSCID(UINT column, SHCOLUMNID* id) override {
        if (!id) return E_POINTER;
        if (column >= ColumnCount()) return E_INVALIDARG;
        const PROPERTYKEY keys[] = {PKEY_ItemNameDisplay, PKEY_Size, PKEY_ItemTypeText, PKEY_WHAM_BsaWinner};
        *reinterpret_cast<PROPERTYKEY*>(id) = keys[column];
        return S_OK;
    }

    const ArchivePtr& GetArchive() const { return archive_; }
    int Current() const { return node_; }
    PCIDLIST_ABSOLUTE Pidl() const { return pidl_; }
    bool InData() const { return inData_; }
    UINT ColumnCount() const { return inData_ ? kWinnerColumn + 1 : kWinnerColumn; }

    // Which copy of this file node the game loads. Unknown for folders, outside
    // the Data folder, or when the plan can't name a winner. The first request in
    // a folder resolves every file in that folder at once.
    WinnerResult WinnerOf(int node) const {
        if (!inData_ || !archive_ || node < 0 || archive_->At(node).dir()) return {};
        std::lock_guard<std::mutex> g(winnerLock_);
        auto found = winners_.find(node);
        if (found != winners_.end()) return found->second;
        if (!plan_) plan_ = DataWinners::ForArchive(archive_->Path());
        if (!plan_) return {};
        std::vector<int> nodes;
        std::vector<std::wstring> paths;
        const int parent = archive_->At(node).parent;
        for (int child : archive_->At(parent < 0 ? 0 : parent).children) {
            if (archive_->At(child).dir() || winners_.count(child)) continue;
            nodes.push_back(child);
            paths.push_back(archive_->RelativePath(child));
        }
        if (std::find(nodes.begin(), nodes.end(), node) == nodes.end()) {
            nodes.push_back(node);
            paths.push_back(archive_->RelativePath(node));
        }
        std::vector<WinnerResult> results;
        plan_->Resolve(paths, &results);
        for (size_t i = 0; i < nodes.size(); ++i) winners_[nodes[i]] = results[i];
        return winners_[node];
    }

    // "Yes" when this archive's copy is the one the game loads, "No" when another
    // copy is (or none is loaded), "" when that can't be said.
    const wchar_t* WinnerText(int node) const {
        const WinnerResult r = WinnerOf(node);
        if (r.kind == WinnerResult::Unknown) return L"";
        return r.kind == WinnerResult::Archive && _wcsicmp(r.path.c_str(), archive_->Path().c_str()) == 0 ? L"Yes" : L"No";
    }

private:
    ~Folder() {
        if (pidl_) ILFree(pidl_);
        InterlockedDecrement(&g_dllRefs);
    }

    int Resolve(PCUIDLIST_RELATIVE pidl) const {
        if (!pidl || ILIsEmpty(pidl)) return -1;
        int node = node_;
        for (; !ILIsEmpty(pidl); pidl = ILNext(pidl)) {
            const ItemData* d = ReadItem(pidl);
            if (!d) return -1;
            node = archive_->Child(node, d->name);
            if (node < 0) return -1;
        }
        return node;
    }

    SFGAOF AttributesOf(int node) const {
        const Node& n = archive_->At(node);
        SFGAOF flags = SFGAO_CANCOPY | SFGAO_READONLY;
        if (n.dir()) {
            flags |= SFGAO_FOLDER | SFGAO_BROWSABLE;
            for (int child : n.children)
                if (archive_->At(child).dir()) {
                    flags |= SFGAO_HASSUBFOLDER;
                    break;
                }
        } else {
            flags |= SFGAO_STREAM;
        }
        return flags;
    }

    HRESULT OpenStream(int node, IStream** out) const {
        std::vector<uint8_t> data;
        if (!archive_->Read(node, &data)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        *out = SHCreateMemStream(data.data(), static_cast<UINT>(data.size()));
        return *out ? S_OK : E_OUTOFMEMORY;
    }

    long refs_ = 1;
    ArchivePtr archive_;
    int node_ = 0;
    LPITEMIDLIST pidl_ = nullptr;
    std::wstring display_;
    HRESULT openResult_ = E_UNEXPECTED;
    bool inData_ = false;
    mutable std::mutex winnerLock_;
    mutable std::shared_ptr<const DataWinners> plan_;
    mutable std::unordered_map<int, WinnerResult> winners_;
};

// ---- Copy and drag: Windows' "virtual files", extracted as they're pasted ----

class DataObject final : public IDataObject {
public:
    DataObject(ArchivePtr archive, const std::vector<int>& nodes) : archive_(std::move(archive)) {
        InterlockedIncrement(&g_dllRefs);
        descriptor_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW));
        contents_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_FILECONTENTS));
        effect_ = static_cast<CLIPFORMAT>(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT));
        for (int node : nodes) Add(node, archive_->At(node).name);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
            *ppv = static_cast<IDataObject*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    IFACEMETHODIMP GetData(FORMATETC* format, STGMEDIUM* medium) override {
        if (!format || !medium) return E_POINTER;
        ZeroMemory(medium, sizeof(*medium));
        return Guard([&]() -> HRESULT {
            if (format->cfFormat == descriptor_ && (format->tymed & TYMED_HGLOBAL)) return Descriptor(medium);
            if (format->cfFormat == contents_ && (format->tymed & TYMED_ISTREAM)) return Contents(format->lindex, medium);
            if (format->cfFormat == effect_ && (format->tymed & TYMED_HGLOBAL)) {
                HGLOBAL g = GlobalAlloc(GPTR, sizeof(DWORD));
                if (!g) return E_OUTOFMEMORY;
                *static_cast<DWORD*>(g) = DROPEFFECT_COPY;
                medium->tymed = TYMED_HGLOBAL;
                medium->hGlobal = g;
                return S_OK;
            }
            for (const auto& e : extra_) {
                if (e.first == format->cfFormat && (format->tymed & TYMED_HGLOBAL)) {
                    HGLOBAL g = GlobalAlloc(GPTR, e.second.size());
                    if (!g) return E_OUTOFMEMORY;
                    memcpy(g, e.second.data(), e.second.size());
                    medium->tymed = TYMED_HGLOBAL;
                    medium->hGlobal = g;
                    return S_OK;
                }
            }
            return DV_E_FORMATETC;
        });
    }
    IFACEMETHODIMP GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    IFACEMETHODIMP QueryGetData(FORMATETC* format) override {
        if (!format) return E_POINTER;
        if ((format->cfFormat == descriptor_ || format->cfFormat == effect_) && (format->tymed & TYMED_HGLOBAL)) return S_OK;
        if (format->cfFormat == contents_ && (format->tymed & TYMED_ISTREAM)) return S_OK;
        for (const auto& e : extra_)
            if (e.first == format->cfFormat) return S_OK;
        return DV_E_FORMATETC;
    }
    IFACEMETHODIMP GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return E_NOTIMPL;
    }
    // Windows leaves notes here during a drag; keep them so it can read them back.
    IFACEMETHODIMP SetData(FORMATETC* format, STGMEDIUM* medium, BOOL release) override {
        if (!format || !medium) return E_POINTER;
        return Guard([&]() -> HRESULT {
            if (medium->tymed != TYMED_HGLOBAL) return DV_E_TYMED;
            const SIZE_T size = GlobalSize(medium->hGlobal);
            void* p = GlobalLock(medium->hGlobal);
            if (!p) return E_INVALIDARG;
            std::string bytes(static_cast<const char*>(p), size);
            GlobalUnlock(medium->hGlobal);
            bool found = false;
            for (auto& e : extra_)
                if (e.first == format->cfFormat) {
                    e.second = bytes;
                    found = true;
                }
            if (!found) extra_.emplace_back(format->cfFormat, std::move(bytes));
            if (release) ReleaseStgMedium(medium);
            return S_OK;
        });
    }
    IFACEMETHODIMP EnumFormatEtc(DWORD direction, IEnumFORMATETC** out) override {
        if (!out) return E_POINTER;
        if (direction != DATADIR_GET) return E_NOTIMPL;
        FORMATETC formats[] = {{descriptor_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL},
                               {contents_, nullptr, DVASPECT_CONTENT, -1, TYMED_ISTREAM},
                               {effect_, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL}};
        return SHCreateStdEnumFmtEtc(ARRAYSIZE(formats), formats, out);
    }
    IFACEMETHODIMP DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    IFACEMETHODIMP DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    IFACEMETHODIMP EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    struct Item {
        std::wstring path;
        int node;
    };

    ~DataObject() { InterlockedDecrement(&g_dllRefs); }

    void Add(int node, const std::wstring& path) {
        if (path.size() >= MAX_PATH) return;  // Windows' copy format can't name it
        items_.push_back({path, node});
        const Node& n = archive_->At(node);
        for (int child : n.children) Add(child, path + L"\\" + archive_->At(child).name);
    }

    HRESULT Descriptor(STGMEDIUM* medium) {
        const size_t bytes = offsetof(FILEGROUPDESCRIPTORW, fgd) + items_.size() * sizeof(FILEDESCRIPTORW);
        HGLOBAL g = GlobalAlloc(GHND, bytes);
        if (!g) return E_OUTOFMEMORY;
        auto* group = static_cast<FILEGROUPDESCRIPTORW*>(GlobalLock(g));
        group->cItems = static_cast<UINT>(items_.size());
        for (size_t i = 0; i < items_.size(); ++i) {
            FILEDESCRIPTORW& d = group->fgd[i];
            const bool dir = archive_->At(items_[i].node).dir();
            d.dwFlags = FD_ATTRIBUTES | FD_UNICODE | FD_PROGRESSUI | (dir ? 0 : FD_FILESIZE);
            d.dwFileAttributes = dir ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
            if (!dir) d.nFileSizeLow = static_cast<DWORD>(archive_->Size(items_[i].node));
            StringCchCopyW(d.cFileName, ARRAYSIZE(d.cFileName), items_[i].path.c_str());
        }
        GlobalUnlock(g);
        medium->tymed = TYMED_HGLOBAL;
        medium->hGlobal = g;
        return S_OK;
    }

    HRESULT Contents(LONG index, STGMEDIUM* medium) {
        if (index < 0 || static_cast<size_t>(index) >= items_.size()) return DV_E_LINDEX;
        const int node = items_[static_cast<size_t>(index)].node;
        std::vector<uint8_t> data;
        if (!archive_->Read(node, &data)) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        IStream* stream = SHCreateMemStream(data.data(), static_cast<UINT>(data.size()));
        if (!stream) return E_OUTOFMEMORY;
        medium->tymed = TYMED_ISTREAM;
        medium->pstm = stream;
        return S_OK;
    }

    long refs_ = 1;
    ArchivePtr archive_;
    std::vector<Item> items_;
    CLIPFORMAT descriptor_, contents_, effect_;
    std::vector<std::pair<CLIPFORMAT, std::string>> extra_;
};

HRESULT MakeData(const ArchivePtr& archive, const std::vector<int>& nodes, REFIID riid, void** ppv) {
    auto* d = new DataObject(archive, nodes);
    HRESULT hr = d->QueryInterface(riid, ppv);
    d->Release();
    return hr;
}

// ---- Selecting a file after navigating ----

// Navigation is asynchronous, so the file is selected from a timer on the
// window's own thread once the view shows the folder (up to about 5 seconds).
struct PendingSelect {
    IShellBrowser* browser;
    std::wstring folder, name;
    int tries;
};
std::mutex g_selectLock;
std::map<UINT_PTR, PendingSelect> g_selects;

std::wstring ViewFolder(IShellBrowser* browser, IShellFolder** folder) {
    std::wstring out;
    IShellView* view = nullptr;
    if (FAILED(browser->QueryActiveShellView(&view)) || !view) return out;
    IFolderView* fv = nullptr;
    if (SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&fv)))) {
        IPersistFolder2* pf = nullptr;
        if (SUCCEEDED(fv->GetFolder(IID_PPV_ARGS(&pf)))) {
            PIDLIST_ABSOLUTE pidl = nullptr;
            if (SUCCEEDED(pf->GetCurFolder(&pidl)) && pidl) {
                PWSTR name = nullptr;
                if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &name))) out = name;
                CoTaskMemFree(name);
                ILFree(pidl);
            }
            pf->Release();
        }
        if (folder) fv->GetFolder(IID_PPV_ARGS(folder));
        fv->Release();
    }
    view->Release();
    return out;
}

bool TrySelect(IShellBrowser* browser, const std::wstring& folderPath, const std::wstring& name) {
    IShellFolder* folder = nullptr;
    const std::wstring shown = ViewFolder(browser, &folder);
    bool done = false;
    if (folder && _wcsicmp(shown.c_str(), folderPath.c_str()) == 0) {
        PIDLIST_RELATIVE child = nullptr;
        if (SUCCEEDED(folder->ParseDisplayName(nullptr, nullptr, const_cast<LPWSTR>(name.c_str()), nullptr, &child, nullptr)) && child) {
            IShellView* view = nullptr;
            if (SUCCEEDED(browser->QueryActiveShellView(&view)) && view) {
                done = SUCCEEDED(view->SelectItem(child, SVSI_SELECT | SVSI_DESELECTOTHERS | SVSI_ENSUREVISIBLE | SVSI_FOCUSED));
                IFolderView* fv = nullptr;
                int selected = 0;
                if (done && SUCCEEDED(view->QueryInterface(IID_PPV_ARGS(&fv)))) {
                    fv->ItemCount(SVGIO_SELECTION, &selected);
                    fv->Release();
                    done = selected > 0;
                }
                view->Release();
            }
            ILFree(child);
        }
    }
    if (folder) folder->Release();
    return done;
}

void CALLBACK SelectTimer(HWND, UINT, UINT_PTR id, DWORD) {
    PendingSelect p;
    {
        std::lock_guard<std::mutex> g(g_selectLock);
        auto it = g_selects.find(id);
        if (it == g_selects.end()) {
            KillTimer(nullptr, id);
            return;
        }
        p = it->second;
    }
    bool finished = false;
    try {
        finished = TrySelect(p.browser, p.folder, p.name);
    } catch (...) {
        finished = true;
    }
    std::lock_guard<std::mutex> g(g_selectLock);
    auto it = g_selects.find(id);
    if (it == g_selects.end()) return;
    if (finished || --it->second.tries <= 0) {
        KillTimer(nullptr, id);
        it->second.browser->Release();
        g_selects.erase(it);
        InterlockedDecrement(&g_dllRefs);
    }
}

void SelectWhenShown(IShellBrowser* browser, const std::wstring& folder, const std::wstring& name) {
    const UINT_PTR id = SetTimer(nullptr, 0, 100, SelectTimer);
    if (!id) return;
    browser->AddRef();
    InterlockedIncrement(&g_dllRefs);  // the DLL stays loaded until the timer is done
    std::lock_guard<std::mutex> g(g_selectLock);
    g_selects[id] = {browser, folder, name, 50};
}

// ---- The right-click menu: Open and Copy ----

enum { kCmdOpen, kCmdCopy, kCmdWinnerCopy, kCmdWinnerGoTo, kCmdWinnerWindow, kCmdWinnerMenu, kCmdCount };

class Menu final : public IContextMenu, public IObjectWithSite {
public:
    Menu(Folder* folder, std::vector<int> nodes) : folder_(folder), nodes_(std::move(nodes)) {
        folder_->AddRef();
        InterlockedIncrement(&g_dllRefs);
    }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IContextMenu) {
            *ppv = static_cast<IContextMenu*>(this);
        } else if (riid == IID_IObjectWithSite) {
            *ppv = static_cast<IObjectWithSite*>(this);
        } else {
            *ppv = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }

    IFACEMETHODIMP SetSite(IUnknown* site) override {
        if (site) site->AddRef();
        if (site_) site_->Release();
        site_ = site;
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        return site_ ? site_->QueryInterface(riid, ppv) : E_FAIL;
    }

    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT first, UINT last, UINT flags) override {
        if (last < first + kCmdCount - 1) return E_FAIL;
        InsertMenuW(menu, index, MF_BYPOSITION | MF_STRING, first + kCmdOpen, L"&Open");
        SetMenuDefaultItem(menu, first + kCmdOpen, FALSE);
        if (flags & CMF_DEFAULTONLY) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, kCmdOpen + 1);
        InsertMenuW(menu, index + 1, MF_BYPOSITION | MF_STRING, first + kCmdCopy, L"&Copy");
        AddWinnerMenu(menu, index + 2, first);
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, kCmdCount);
    }

    IFACEMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO info) override {
        if (!info) return E_POINTER;
        return Guard([&]() -> HRESULT {
            int command = -1;
            if (!HIWORD(info->lpVerb)) {
                command = LOWORD(info->lpVerb);
            } else {
                const auto* ex = reinterpret_cast<const CMINVOKECOMMANDINFOEX*>(info);
                if (info->cbSize >= sizeof(CMINVOKECOMMANDINFOEX) && (info->fMask & CMIC_MASK_UNICODE) && ex->lpVerbW) {
                    if (!_wcsicmp(ex->lpVerbW, L"open")) command = kCmdOpen;
                    else if (!_wcsicmp(ex->lpVerbW, L"copy")) command = kCmdCopy;
                } else {
                    if (!_stricmp(info->lpVerb, "open")) command = kCmdOpen;
                    else if (!_stricmp(info->lpVerb, "copy")) command = kCmdCopy;
                }
            }
            if (command == kCmdCopy) return Copy();
            if (command == kCmdOpen) return Open(info->hwnd);
            if (command == kCmdWinnerCopy || command == kCmdWinnerGoTo || command == kCmdWinnerWindow) return Winner(info->hwnd, command);
            return E_INVALIDARG;
        });
    }
    IFACEMETHODIMP GetCommandString(UINT_PTR command, UINT type, UINT*, CHAR* out, UINT size) override {
        if (command == kCmdWinnerCopy || command == kCmdWinnerGoTo || command == kCmdWinnerWindow) {
            if (type != GCS_HELPTEXTW) return E_NOTIMPL;  // no canonical verbs
            const wchar_t* help = command == kCmdWinnerCopy   ? L"Copies the path of the copy the game loads."
                                  : command == kCmdWinnerGoTo ? L"Shows the copy the game loads in this window."
                                                              : L"Shows the copy the game loads in a new window.";
            return StringCchCopyW(reinterpret_cast<LPWSTR>(out), size, help);
        }
        const wchar_t* verb = command == kCmdOpen ? L"open" : command == kCmdCopy ? L"copy" : nullptr;
        if (!verb) return E_INVALIDARG;
        if (type == GCS_VERBW) return StringCchCopyW(reinterpret_cast<LPWSTR>(out), size, verb);
        if (type == GCS_VERBA) return StringCchCopyA(out, size, command == kCmdOpen ? "open" : "copy");
        return E_INVALIDARG;
    }

private:
    ~Menu() {
        if (site_) site_->Release();
        folder_->Release();
        InterlockedDecrement(&g_dllRefs);
    }

    // "Winner" > Copy path, Go to, New window: one file, in an archive in the game's Data folder.
    void AddWinnerMenu(HMENU menu, UINT index, UINT first) {
        if (!folder_->InData() || nodes_.size() != 1 || folder_->GetArchive()->At(nodes_[0]).dir()) return;
        target_ = WinnerTarget();
        HMENU sub = CreatePopupMenu();
        if (!sub) return;
        const UINT state = target_.empty() ? MF_GRAYED : 0;
        if (target_.empty()) AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, winnerNote_.c_str());
        AppendMenuW(sub, MF_STRING | state, first + kCmdWinnerCopy, L"Copy path");
        AppendMenuW(sub, MF_STRING | state, first + kCmdWinnerGoTo, L"Go to");
        AppendMenuW(sub, MF_STRING | state, first + kCmdWinnerWindow, L"New window");
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_ID;
        mii.wID = first + kCmdWinnerMenu;
        mii.hSubMenu = sub;
        mii.dwTypeData = const_cast<wchar_t*>(L"Winner");
        if (!InsertMenuItemW(menu, index, TRUE, &mii)) DestroyMenu(sub);
    }

    // The full path of the copy the game loads: the loose file, or the file inside
    // the winning archive (<x.bsa>\<path>, as Explorer shows it). Empty with a note
    // when there is none.
    std::wstring WinnerTarget() {
        const int node = nodes_[0];
        const WinnerResult r = folder_->WinnerOf(node);
        switch (r.kind) {
            case WinnerResult::Loose: return r.path;
            case WinnerResult::Archive: return r.path + L"\\" + folder_->GetArchive()->RelativePath(node);
            case WinnerResult::None: winnerNote_ = L"The game doesn't load this file"; return {};
            default: {
                auto plan = DataWinners::ForArchive(folder_->GetArchive()->Path());
                winnerNote_ = plan && !plan->Why().empty() ? plan->Why() : L"The winner can't be established";
                return {};
            }
        }
    }

    HRESULT Winner(HWND hwnd, int command) {
        if (target_.empty()) target_ = WinnerTarget();
        if (target_.empty()) return E_FAIL;
        if (command == kCmdWinnerCopy) return CopyText(hwnd, target_);
        if (command == kCmdWinnerGoTo && SUCCEEDED(GoTo(target_))) return S_OK;
        // A new window, or no window to navigate (the desktop, a dialog).
        const std::wstring args = L"/select,\"" + target_ + L"\"";
        HINSTANCE h = ShellExecuteW(hwnd, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(h) > 32 ? S_OK : E_FAIL;
    }

    static HRESULT CopyText(HWND owner, const std::wstring& text) {
        if (!OpenClipboard(owner)) return HRESULT_FROM_WIN32(GetLastError());
        HRESULT hr = E_OUTOFMEMORY;
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            memcpy(GlobalLock(mem), text.c_str(), bytes);
            GlobalUnlock(mem);
            EmptyClipboard();
            if (SetClipboardData(CF_UNICODETEXT, mem)) hr = S_OK;
            else GlobalFree(mem);
        }
        CloseClipboard();
        return hr;
    }

    // Opens the target's folder in this window (Back returns here), then selects the
    // file once the new view is up.
    HRESULT GoTo(const std::wstring& target) {
        IShellBrowser* browser = nullptr;
        HRESULT hr = site_ ? IUnknown_QueryService(site_, SID_STopLevelBrowser, IID_PPV_ARGS(&browser)) : E_NOINTERFACE;
        if (FAILED(hr)) return hr;
        const size_t slash = target.find_last_of(L'\\');
        const std::wstring parent = target.substr(0, slash), name = target.substr(slash + 1);
        PIDLIST_ABSOLUTE pidl = nullptr;
        hr = SHParseDisplayName(parent.c_str(), nullptr, &pidl, 0, nullptr);
        if (SUCCEEDED(hr)) hr = browser->BrowseObject(pidl, SBSP_SAMEBROWSER | SBSP_ABSOLUTE);
        if (pidl) ILFree(pidl);
        if (SUCCEEDED(hr)) SelectWhenShown(browser, parent, name);
        browser->Release();
        return hr;
    }

    HRESULT Copy() {
        IDataObject* data = nullptr;
        HRESULT hr = MakeData(folder_->GetArchive(), nodes_, IID_PPV_ARGS(&data));
        if (FAILED(hr)) return hr;
        hr = OleSetClipboard(data);
        data->Release();
        return hr;
    }

    HRESULT Open(HWND hwnd) {
        const ArchivePtr& archive = folder_->GetArchive();
        if (nodes_.size() == 1 && archive->At(nodes_[0]).dir()) return Browse(hwnd, nodes_[0]);
        for (int node : nodes_)
            if (!archive->At(node).dir()) OpenFile(hwnd, node);
        return S_OK;
    }

    HRESULT Browse(HWND hwnd, int node) {
        const ArchivePtr& archive = folder_->GetArchive();
        LPITEMIDLIST item = MakeItem(archive->At(node).name, true);
        IShellBrowser* browser = nullptr;
        HRESULT hr = site_ ? IUnknown_QueryService(site_, SID_SShellBrowser, IID_PPV_ARGS(&browser)) : E_NOINTERFACE;
        if (SUCCEEDED(hr)) {
            hr = browser->BrowseObject(item, SBSP_SAMEBROWSER | SBSP_RELATIVE);
            browser->Release();
        } else {
            LPITEMIDLIST full = ILCombine(folder_->Pidl(), item);
            SHELLEXECUTEINFOW exec{sizeof(exec)};
            exec.fMask = SEE_MASK_IDLIST;
            exec.hwnd = hwnd;
            exec.lpVerb = L"open";
            exec.lpIDList = full;
            exec.nShow = SW_SHOWNORMAL;
            hr = ShellExecuteExW(&exec) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
            if (full) ILFree(full);
        }
        ILFree(item);
        return hr;
    }

    void OpenFile(HWND hwnd, int node) {
        const ArchivePtr& archive = folder_->GetArchive();
        std::vector<uint8_t> data;
        if (!archive->Read(node, &data)) {
            MessageBoxW(hwnd, L"This file couldn't be read from the archive.", L"WHAM", MB_ICONERROR);
            return;
        }
        wchar_t temp[MAX_PATH];
        if (!GetTempPathW(ARRAYSIZE(temp), temp)) return;
        const std::wstring& source = archive->Path();
        const std::wstring archiveName = source.substr(source.find_last_of(L'\\') + 1);
        const std::wstring relative = archive->RelativePath(node);
        const std::wstring file = std::wstring(temp) + L"PluginShellExtension\\" + archiveName + L"\\" + relative;
        const std::wstring dir = file.substr(0, file.find_last_of(L'\\'));
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
        HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_READONLY, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            // Already extracted and in use, or read-only from last time: open that copy.
            if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return;
        } else {
            DWORD wrote = 0;
            WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr);
            CloseHandle(h);
        }
        ShellExecuteW(hwnd, nullptr, file.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    long refs_ = 1;
    Folder* folder_;
    std::vector<int> nodes_;
    IUnknown* site_ = nullptr;
    std::wstring target_, winnerNote_;
};

HRESULT MakeMenu(Folder* folder, const std::vector<int>& nodes, REFIID riid, void** ppv) {
    auto* m = new Menu(folder, nodes);
    HRESULT hr = m->QueryInterface(riid, ppv);
    m->Release();
    return hr;
}

class Factory final : public IClassFactory {
public:
    Factory() { InterlockedIncrement(&g_dllRefs); }
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IClassFactory) {
            *ppv = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    IFACEMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&refs_)); }
    IFACEMETHODIMP_(ULONG) Release() override {
        long r = InterlockedDecrement(&refs_);
        if (r == 0) delete this;
        return static_cast<ULONG>(r);
    }
    IFACEMETHODIMP CreateInstance(IUnknown* outer, REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        if (outer) return CLASS_E_NOAGGREGATION;
        return Guard([&]() -> HRESULT {
            auto* obj = new Folder();
            HRESULT hr = obj->QueryInterface(riid, ppv);
            obj->Release();
            return hr;
        });
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_dllRefs);
        else InterlockedDecrement(&g_dllRefs);
        return S_OK;
    }

private:
    ~Factory() { InterlockedDecrement(&g_dllRefs); }
    long refs_ = 1;
};

}  // namespace

HRESULT CreateBsaFolderFactory(REFIID riid, void** ppv) {
    auto* factory = new (std::nothrow) Factory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

}  // namespace pse

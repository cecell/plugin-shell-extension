#include "tool_menus.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>

#include <new>
#include <string>
#include <vector>

#include "context_menu.h"
#include "property_window.h"
#include "tool_windows.h"

namespace pse {
namespace {

enum class MenuKind { Bsa, BsaDrop, Hash, Papyrus };
enum { kExtractHere, kExtractFolder, kTest, kProperties, kBsaCommands };
enum { kHashes, kHashCommands };

bool IsBsa(const std::wstring& path) { return _wcsicmp(PathFindExtensionW(path.c_str()), L".bsa") == 0; }
bool HasExtension(const std::wstring& path, const wchar_t* ext) { return _wcsicmp(PathFindExtensionW(path.c_str()), ext) == 0; }

std::wstring ExtractFolderLabel(const std::vector<std::wstring>& paths) {
    if (paths.size() != 1) return L"Extract to \"*\\\"";
    std::wstring name = PathFindFileNameW(paths[0].c_str());
    const size_t dot = name.rfind(L'.');
    if (dot != std::wstring::npos) name.resize(dot);
    // An ampersand in a menu label is a shortcut marker.
    std::wstring safe;
    for (wchar_t c : name) {
        if (c == L'&') safe += L'&';
        safe += c;
    }
    return L"Extract to \"" + safe + L"\"";
}

class ToolMenu final : public IShellExtInit, public IContextMenu {
public:
    explicit ToolMenu(MenuKind kind) : kind_(kind) { InterlockedIncrement(&g_dllRefs); }

    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IShellExtInit) {
            *ppv = static_cast<IShellExtInit*>(this);
        } else if (riid == IID_IContextMenu) {
            *ppv = static_cast<IContextMenu*>(this);
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

    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE folder, IDataObject* data, HKEY) override {
        paths_.clear();
        if (!data) return E_INVALIDARG;
        if (kind_ == MenuKind::BsaDrop) {
            // For a right-drag, the folder is where the files were dropped.
            wchar_t target[MAX_PATH * 2] = {};
            if (!folder || !SHGetPathFromIDListEx(folder, target, ARRAYSIZE(target), GPFIDL_DEFAULT)) return E_FAIL;
            try {
                target_ = target;
            } catch (...) {
                return E_OUTOFMEMORY;
            }
        }
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM stg{};
        if (FAILED(data->GetData(&fe, &stg))) return E_INVALIDARG;
        try {
            if (auto drop = static_cast<HDROP>(GlobalLock(stg.hGlobal))) {
                const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
                for (UINT i = 0; i < count; ++i) {
                    const UINT len = DragQueryFileW(drop, i, nullptr, 0);
                    std::wstring path(len + 1, L' ');
                    DragQueryFileW(drop, i, path.data(), len + 1);
                    path.resize(len);
                    const DWORD attributes = GetFileAttributesW(path.c_str());
                    const bool isFile = attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
                    if (!isFile) continue;
                    if (kind_ == MenuKind::Papyrus) {
                        // One command at a time: whichever type comes first in the selection.
                        if (paths_.empty()) {
                            if (!HasExtension(path, L".pex") && !HasExtension(path, L".psc")) continue;
                            compile_ = HasExtension(path, L".psc");
                        } else if (!HasExtension(path, compile_ ? L".psc" : L".pex")) {
                            continue;
                        }
                        paths_.push_back(std::move(path));
                    } else if (kind_ == MenuKind::Hash || IsBsa(path)) {
                        paths_.push_back(std::move(path));
                    }
                }
                GlobalUnlock(stg.hGlobal);
            }
        } catch (...) {
            paths_.clear();
        }
        ReleaseStgMedium(&stg);
        return paths_.empty() ? E_FAIL : S_OK;
    }

    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT first, UINT last, UINT flags) override {
        if (flags & CMF_DEFAULTONLY) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        if (kind_ == MenuKind::Papyrus) {
            if (last < first) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
            InsertMenuW(menu, index, MF_BYPOSITION | MF_STRING, first, compile_ ? L"Compile" : L"Decompile");
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 1);
        }
        if (kind_ == MenuKind::Hash) {
            if (paths_.size() != 1 || last < first) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
            InsertMenuW(menu, index, MF_BYPOSITION | MF_STRING, first + kHashes, L"Hashes");
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, kHashCommands);
        }
        if (last < first + kBsaCommands - 1) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        HMENU sub = CreatePopupMenu();
        if (!sub) return E_OUTOFMEMORY;
        try {
            const std::wstring folderLabel = ExtractFolderLabel(paths_);
            AppendMenuW(sub, MF_STRING, first + kExtractHere, L"Extract here");
            AppendMenuW(sub, MF_STRING, first + kExtractFolder, folderLabel.c_str());
            if (kind_ == MenuKind::Bsa) {
                AppendMenuW(sub, MF_STRING, first + kTest, L"Test BSA");
                AppendMenuW(sub, MF_STRING, first + kProperties, L"BSA properties");
            }
        } catch (...) {
            DestroyMenu(sub);
            return E_OUTOFMEMORY;
        }
        InsertMenuW(menu, index, MF_BYPOSITION | MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), L"BSA manager");
        return MAKE_HRESULT(SEVERITY_SUCCESS, 0, kBsaCommands);
    }

    IFACEMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO info) override {
        if (!info) return E_POINTER;
        if (HIWORD(info->lpVerb)) return E_INVALIDARG;
        const UINT command = LOWORD(info->lpVerb);
        try {
            if (kind_ == MenuKind::Papyrus) {
                if (command != 0 || paths_.empty()) return E_INVALIDARG;
                PapyrusAsync(paths_, compile_);
                return S_OK;
            }
            if (kind_ == MenuKind::Hash) {
                if (command != kHashes || paths_.empty()) return E_INVALIDARG;
                ShowHashWindowAsync(paths_[0]);
                return S_OK;
            }
            switch (command) {
                case kExtractHere: ExtractBsasAsync(paths_, false, target_); return S_OK;
                case kExtractFolder: ExtractBsasAsync(paths_, true, target_); return S_OK;
                case kTest: TestBsasAsync(paths_); return S_OK;
                case kProperties: ShowPropertyWindowsAsync(paths_); return S_OK;
            }
        } catch (...) {
            return E_OUTOFMEMORY;
        }
        return E_INVALIDARG;
    }

    IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, CHAR*, UINT) override { return E_NOTIMPL; }

private:
    ~ToolMenu() { InterlockedDecrement(&g_dllRefs); }
    long refs_ = 1;
    MenuKind kind_;
    bool compile_ = false;
    std::wstring target_;  // BsaDrop: where the files were dropped
    std::vector<std::wstring> paths_;
};

class Factory final : public IClassFactory {
public:
    explicit Factory(MenuKind kind) : kind_(kind) { InterlockedIncrement(&g_dllRefs); }
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
        auto* obj = new (std::nothrow) ToolMenu(kind_);
        if (!obj) return E_OUTOFMEMORY;
        HRESULT hr = obj->QueryInterface(riid, ppv);
        obj->Release();
        return hr;
    }
    IFACEMETHODIMP LockServer(BOOL lock) override {
        if (lock) InterlockedIncrement(&g_dllRefs);
        else InterlockedDecrement(&g_dllRefs);
        return S_OK;
    }

private:
    ~Factory() { InterlockedDecrement(&g_dllRefs); }
    long refs_ = 1;
    MenuKind kind_;
};

HRESULT Make(MenuKind kind, REFIID riid, void** ppv) {
    auto* factory = new (std::nothrow) Factory(kind);
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

}  // namespace

HRESULT CreateBsaManagerFactory(REFIID riid, void** ppv) { return Make(MenuKind::Bsa, riid, ppv); }
HRESULT CreateBsaDropFactory(REFIID riid, void** ppv) { return Make(MenuKind::BsaDrop, riid, ppv); }
HRESULT CreateHashMenuFactory(REFIID riid, void** ppv) { return Make(MenuKind::Hash, riid, ppv); }
HRESULT CreatePapyrusMenuFactory(REFIID riid, void** ppv) { return Make(MenuKind::Papyrus, riid, ppv); }

}  // namespace pse

// IShellExtInit + IContextMenu + IObjectWithSite handler for every file:
//   not a hardlink:  "Not a hardlink" (greyed out)
//   a hardlink:      Hardlink > Copy path
//                               Open path (label)
//                                   Here / New tab / New window
// one block per other link (normally one: Vortex's staging copy). "Here"
// navigates the Explorer window the menu came from, so Back returns to
// where the user was; "New tab" needs Windows 11's tabbed Explorer.
#include "hardlink_menu.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>

#include <exdisp.h>  // after shlobj.h: needs the OLE types it brings in

#include <new>
#include <thread>

#include "context_menu.h"
#include "debug_log.h"

namespace pse {

namespace {

enum class Action { CopyPath, OpenHere, OpenTab, OpenWindow };

struct Command {
    Action action;
    size_t link;  // index into links_
};

std::wstring Folder(const std::wstring& path) { return path.substr(0, path.find_last_of(L"\\/")); }

bool SamePath(const std::wstring& a, const std::wstring& b) { return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL; }

// Windows 11 22H2 (build 22621) and later have tabs in File Explorer.
bool HasExplorerTabs() {
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto fn = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion"));
    OSVERSIONINFOW v{sizeof(v)};
    return fn && fn(&v) == 0 && v.dwBuildNumber >= 22621;
}

// The tab strip host of an Explorer window, or nullptr when it has none.
HWND TabWindow(HWND top) { return top ? FindWindowExW(top, nullptr, L"ShellTabWindowClass", nullptr) : nullptr; }

// The ShellWindows entries (tabs) that belong to one Explorer window.
std::vector<IWebBrowser2*> BrowsersOf(IShellWindows* windows, HWND top) {
    std::vector<IWebBrowser2*> out;
    long count = 0;
    if (FAILED(windows->get_Count(&count))) return out;
    for (long i = 0; i < count; ++i) {
        VARIANT index;
        VariantInit(&index);
        index.vt = VT_I4;
        index.lVal = i;
        IDispatch* disp = nullptr;
        if (FAILED(windows->Item(index, &disp)) || !disp) continue;
        IWebBrowser2* browser = nullptr;
        if (SUCCEEDED(disp->QueryInterface(IID_PPV_ARGS(&browser)))) {
            SHANDLE_PTR hwnd = 0;
            if (SUCCEEDED(browser->get_HWND(&hwnd)) && reinterpret_cast<HWND>(hwnd) == top) out.push_back(browser);
            else browser->Release();
        }
        disp->Release();
    }
    return out;
}

// Opens a new tab in Explorer window `top` and navigates it to `folder`, on a
// worker thread so Explorer's own thread keeps pumping messages while the tab
// is created. There is no public API for this: the tab strip's "new tab"
// command (WM_COMMAND 0xA21B, what Ctrl+T sends) opens the tab, which then
// shows up in ShellWindows and is navigated like any other.
void OpenInNewTab(HWND top, std::wstring folder) {
    InterlockedIncrement(&g_dllRefs);
    std::thread([top, folder]() {
        if (SUCCEEDED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
            IShellWindows* windows = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) {
                std::vector<IUnknown*> before;
                for (IWebBrowser2* b : BrowsersOf(windows, top)) {
                    IUnknown* id = nullptr;
                    b->QueryInterface(IID_PPV_ARGS(&id));
                    before.push_back(id);
                    b->Release();
                }
                PostMessageW(TabWindow(top), WM_COMMAND, 0xA21B, 0);
                bool done = false;
                for (int wait = 0; wait < 60 && !done; ++wait) {  // up to 3 s
                    Sleep(50);
                    for (IWebBrowser2* b : BrowsersOf(windows, top)) {
                        IUnknown* id = nullptr;
                        b->QueryInterface(IID_PPV_ARGS(&id));
                        bool isNew = true;
                        for (IUnknown* old : before) isNew = isNew && old != id;
                        if (isNew && !done) {
                            VARIANT target, empty;
                            VariantInit(&target);
                            VariantInit(&empty);
                            target.vt = VT_BSTR;
                            target.bstrVal = SysAllocString(folder.c_str());
                            done = SUCCEEDED(b->Navigate2(&target, &empty, &empty, &empty, &empty));
                            VariantClear(&target);
                        }
                        if (id) id->Release();
                        b->Release();
                    }
                }
                for (IUnknown* id : before)
                    if (id) id->Release();
                windows->Release();
                DebugLog(L"Hardlink: new tab %s", done ? L"navigated" : L"not found");
            }
            CoUninitialize();
        }
        InterlockedDecrement(&g_dllRefs);
    }).detach();
}

class HardlinkMenu final : public IShellExtInit, public IContextMenu, public IObjectWithSite {
public:
    HardlinkMenu() { InterlockedIncrement(&g_dllRefs); }

    // IUnknown
    IFACEMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == IID_IShellExtInit) *ppv = static_cast<IShellExtInit*>(this);
        else if (riid == IID_IContextMenu) *ppv = static_cast<IContextMenu*>(this);
        else if (riid == IID_IObjectWithSite) *ppv = static_cast<IObjectWithSite*>(this);
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

    // IObjectWithSite: Explorer passes the view the menu belongs to.
    IFACEMETHODIMP SetSite(IUnknown* site) override {
        if (site_) site_->Release();
        site_ = site;
        if (site_) site_->AddRef();
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        return site_ ? site_->QueryInterface(riid, ppv) : E_FAIL;
    }

    // IShellExtInit: one file only (a folder or several files get no entry).
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE, IDataObject* data, HKEY) override {
        path_.clear();
        if (!data) return E_INVALIDARG;
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM stg{};
        if (FAILED(data->GetData(&fe, &stg))) return E_INVALIDARG;
        if (auto drop = static_cast<HDROP>(GlobalLock(stg.hGlobal))) {
            if (DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0) == 1) {
                UINT len = DragQueryFileW(drop, 0, nullptr, 0);
                path_.assign(len + 1, L'\0');
                DragQueryFileW(drop, 0, &path_[0], len + 1);
                path_.resize(len);
            }
            GlobalUnlock(stg.hGlobal);
        }
        ReleaseStgMedium(&stg);
        DWORD attrs = path_.empty() ? INVALID_FILE_ATTRIBUTES : GetFileAttributesW(path_.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            path_.clear();
            return E_FAIL;
        }
        return S_OK;
    }

    // IContextMenu
    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT idFirst, UINT idLast, UINT flags) override {
        if ((flags & CMF_DEFAULTONLY) || path_.empty()) return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
        commands_.clear();
        DWORD linkCount = 0;
        links_ = OtherHardlinks(path_, &linkCount);
        DebugLog(L"Hardlink: %s has %u link(s)", path_.c_str(), linkCount);
        if (links_.empty()) {
            // Not a command: greyed out, and no id is used.
            InsertMenuW(menu, index, MF_BYPOSITION | MF_STRING | MF_GRAYED, idFirst, L"Not a hardlink");
            return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 1);
        }
        const bool tabs = HasExplorerTabs() && TabWindow(ExplorerWindow());
        HMENU sub = CreatePopupMenu();
        UINT id = idFirst;
        auto add = [&](const wchar_t* text, Action action, size_t link, bool enabled = true) {
            if (id > idLast) return;
            AppendMenuW(sub, MF_STRING | (enabled ? 0 : MF_GRAYED), id++, text);
            commands_.push_back({action, link});
        };
        for (size_t i = 0; i < links_.size(); ++i) {
            if (i > 0) AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
            if (links_.size() > 1) AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, links_[i].c_str());  // which link
            add(L"Copy path", Action::CopyPath, i);
            AppendMenuW(sub, MF_STRING | MF_GRAYED, 0, L"Open path");
            add(L"      Here", Action::OpenHere, i);
            add(L"      New tab", Action::OpenTab, i, tabs);
            add(L"      New window", Action::OpenWindow, i);
        }
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_ID;
        mii.wID = id++;
        mii.hSubMenu = sub;
        mii.dwTypeData = const_cast<wchar_t*>(L"Hardlink");
        if (!InsertMenuItemW(menu, index, TRUE, &mii)) {
            DestroyMenu(sub);
            return HRESULT_FROM_WIN32(GetLastError());
        }
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, id - idFirst);
    }

    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* ici) override {
        if (!ici || !IS_INTRESOURCE(ici->lpVerb)) return E_FAIL;
        const UINT offset = LOWORD(reinterpret_cast<UINT_PTR>(ici->lpVerb));
        if (offset >= commands_.size()) return E_FAIL;
        const Command& c = commands_[offset];
        const std::wstring& link = links_[c.link];
        const std::wstring folder = Folder(link);
        switch (c.action) {
            case Action::CopyPath: return CopyToClipboard(ici->hwnd, link);
            case Action::OpenHere:
                if (SUCCEEDED(NavigateHere(folder))) return S_OK;
                return OpenWindow(ici->hwnd, link);  // no window to navigate (e.g. the desktop)
            case Action::OpenTab: {
                HWND top = ExplorerWindow();
                if (!TabWindow(top)) return OpenWindow(ici->hwnd, link);
                OpenInNewTab(top, folder);
                return S_OK;
            }
            case Action::OpenWindow: return OpenWindow(ici->hwnd, link);
        }
        return E_FAIL;
    }

    IFACEMETHODIMP GetCommandString(UINT_PTR idCmd, UINT type, UINT*, CHAR* name, UINT cch) override {
        if (idCmd >= commands_.size() || (type != GCS_HELPTEXTW && type != GCS_VERBW)) return E_NOTIMPL;
        if (type == GCS_VERBW) return E_NOTIMPL;  // no canonical verbs
        static const wchar_t* kHelp[] = {L"Copies the other link's full path.",
                                         L"Opens the other link's folder in this window.",
                                         L"Opens the other link's folder in a new tab.",
                                         L"Opens the other link's folder in a new window."};
        return StringCchCopyW(reinterpret_cast<wchar_t*>(name), cch, kHelp[static_cast<int>(commands_[idCmd].action)]);
    }

private:
    ~HardlinkMenu() {
        if (site_) site_->Release();
        InterlockedDecrement(&g_dllRefs);
    }

    // The Explorer window (or tab) that showed the menu, if any.
    IShellBrowser* Browser() const {
        IShellBrowser* browser = nullptr;
        if (site_) IUnknown_QueryService(site_, SID_STopLevelBrowser, IID_PPV_ARGS(&browser));
        return browser;
    }

    HWND ExplorerWindow() const {
        HWND hwnd = nullptr;
        if (IShellBrowser* b = Browser()) {
            b->GetWindow(&hwnd);
            b->Release();
        }
        return hwnd ? GetAncestor(hwnd, GA_ROOT) : nullptr;
    }

    // Navigates the window the menu came from: Explorer records it in the
    // window's history, so Back returns to the previous folder.
    HRESULT NavigateHere(const std::wstring& folder) const {
        IShellBrowser* browser = Browser();
        if (!browser) return E_FAIL;
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(folder.c_str());
        HRESULT hr = pidl ? browser->BrowseObject(pidl, SBSP_SAMEBROWSER | SBSP_ABSOLUTE) : E_FAIL;
        if (pidl) ILFree(pidl);
        browser->Release();
        return hr;
    }

    static HRESULT OpenWindow(HWND owner, const std::wstring& link) {
        std::wstring args = L"/select,\"" + link + L"\"";
        HINSTANCE r = ShellExecuteW(owner, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        return reinterpret_cast<INT_PTR>(r) > 32 ? S_OK : E_FAIL;
    }

    static HRESULT CopyToClipboard(HWND owner, const std::wstring& text) {
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

    long refs_ = 1;
    IUnknown* site_ = nullptr;
    std::wstring path_;
    std::vector<std::wstring> links_;
    std::vector<Command> commands_;
};

class HardlinkFactory final : public IClassFactory {
public:
    HardlinkFactory() { InterlockedIncrement(&g_dllRefs); }
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
        auto* obj = new (std::nothrow) HardlinkMenu();
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
    ~HardlinkFactory() { InterlockedDecrement(&g_dllRefs); }
    long refs_ = 1;
};

}  // namespace

std::vector<std::wstring> OtherHardlinks(const std::wstring& path, DWORD* linkCount) {
    std::vector<std::wstring> out;
    *linkCount = 0;
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return out;
    BY_HANDLE_FILE_INFORMATION info{};
    const bool ok = GetFileInformationByHandle(h, &info) != 0;
    CloseHandle(h);
    if (!ok) return out;
    *linkCount = info.nNumberOfLinks;
    if (info.nNumberOfLinks < 2) return out;
    // FindFirstFileNameW gives volume-relative names ("\Mods\x\a.esp").
    wchar_t volume[MAX_PATH] = {};
    if (!GetVolumePathNameW(path.c_str(), volume, MAX_PATH)) return out;
    std::wstring root = volume;
    if (!root.empty() && root.back() == L'\\') root.pop_back();
    wchar_t full[32768];
    GetFullPathNameW(path.c_str(), ARRAYSIZE(full), full, nullptr);
    std::wstring name(32768, L'\0');
    DWORD len = static_cast<DWORD>(name.size());
    HANDLE find = FindFirstFileNameW(path.c_str(), 0, &len, &name[0]);
    if (find == INVALID_HANDLE_VALUE) return out;
    do {
        std::wstring link = root + std::wstring(name.c_str());
        if (!SamePath(link, full)) out.push_back(link);
        len = static_cast<DWORD>(name.size());
    } while (FindNextFileNameW(find, &len, &name[0]));
    FindClose(find);
    return out;
}

HRESULT CreateHardlinkMenuFactory(REFIID riid, void** ppv) {
    auto* factory = new (std::nothrow) HardlinkFactory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

}  // namespace pse

// IShellExtInit + IContextMenu handler that adds the "Skyrim SE plugins" submenu
// to the context menu of .esp/.esm/.esl files.
#include "context_menu.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <strsafe.h>

#include <new>
#include <string>
#include <vector>

#include "debug_log.h"
#include "property_window.h"
#include "registration.h"
#include "tool_windows.h"

namespace pse {

volatile long g_dllRefs = 0;

namespace {

constexpr wchar_t kVerbW[] = L"pluginshellextension";
constexpr char kVerbA[] = "pluginshellextension";
constexpr wchar_t kUpgradeVerbW[] = L"pluginshellextensionupgrade";
constexpr char kUpgradeVerbA[] = "pluginshellextensionupgrade";
constexpr wchar_t kSubmenuText[] = L"Skyrim SE plugins";
constexpr wchar_t kPropertiesText[] = L"Plugin properties";
constexpr wchar_t kUpgradeText[] = L"Upgrade header";
constexpr wchar_t kHelpText[] = L"Shows the header, masters and record groups of this plugin.";
constexpr wchar_t kUpgradeHelpText[] = L"Changes a 1.70 plugin header to 1.71.";
constexpr wchar_t kDependentsVerbW[] = L"pluginshellextensiondependents";
constexpr char kDependentsVerbA[] = "pluginshellextensiondependents";
constexpr wchar_t kDependentsText[] = L"List dependents";
constexpr wchar_t kDependentsHelpText[] = L"Lists the plugins in this folder that have this plugin as a master.";
enum { kCommandProperties, kCommandUpgrade, kCommandDependents, kCommandCount };
constexpr size_t kMaxWindows = 16;

bool IsPluginPath(const std::wstring& path) {
    const wchar_t* ext = PathFindExtensionW(path.c_str());
    for (const wchar_t* e : kExtensions) {
        if (_wcsicmp(ext, e) == 0) return true;
    }
    return false;
}

class ContextMenu final : public IShellExtInit, public IContextMenu {
public:
    ContextMenu() { InterlockedIncrement(&g_dllRefs); }

    // IUnknown
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

    // IShellExtInit
    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE, IDataObject* data, HKEY) override {
        paths_.clear();
        if (!data) return E_INVALIDARG;
        FORMATETC fe{CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        STGMEDIUM stg{};
        if (FAILED(data->GetData(&fe, &stg))) {
            DebugLog(L"Initialize: no CF_HDROP");
            return E_INVALIDARG;
        }
        if (auto drop = static_cast<HDROP>(GlobalLock(stg.hGlobal))) {
            UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < count; ++i) {
                UINT len = DragQueryFileW(drop, i, nullptr, 0);
                std::wstring path(len + 1, L'\0');
                DragQueryFileW(drop, i, &path[0], len + 1);
                path.resize(len);
                if (IsPluginPath(path)) paths_.push_back(std::move(path));
            }
            GlobalUnlock(stg.hGlobal);
        }
        ReleaseStgMedium(&stg);
        DebugLog(L"Initialize: %zu plugin path(s)%s%s", paths_.size(), paths_.empty() ? L"" : L", first ",
                 paths_.empty() ? L"" : paths_[0].c_str());
        return paths_.empty() ? E_FAIL : S_OK;
    }

    // IContextMenu
    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT idFirst, UINT, UINT flags) override {
        DebugLog(L"QueryContextMenu flags=0x%X paths=%zu", flags, paths_.size());
        if ((flags & CMF_DEFAULTONLY) || paths_.empty()) return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 0);
        HMENU sub = CreatePopupMenu();
        if (!sub) return E_OUTOFMEMORY;
        AppendMenuW(sub, MF_STRING, idFirst + kCommandUpgrade, kUpgradeText);
        AppendMenuW(sub, MF_STRING, idFirst + kCommandProperties, kPropertiesText);
        AppendMenuW(sub, MF_STRING, idFirst + kCommandDependents, kDependentsText);
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_FTYPE;
        mii.fType = MFT_STRING;
        mii.hSubMenu = sub;
        mii.dwTypeData = const_cast<wchar_t*>(kSubmenuText);
        if (!InsertMenuItemW(menu, index, TRUE, &mii)) {
            const DWORD error = GetLastError();
            DestroyMenu(sub);
            return HRESULT_FROM_WIN32(error);
        }
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, kCommandCount);
    }

    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* ici) override {
        if (!ici) return E_INVALIDARG;
        bool unicode = ici->cbSize >= sizeof(CMINVOKECOMMANDINFOEX) && (ici->fMask & CMIC_MASK_UNICODE);
        auto* icix = reinterpret_cast<CMINVOKECOMMANDINFOEX*>(ici);
        DebugLog(L"InvokeCommand cbSize=%u mask=0x%X verb=%p verbW=%p", ici->cbSize, ici->fMask, ici->lpVerb,
                 unicode ? icix->lpVerbW : nullptr);
        UINT command = kCommandCount;
        if (unicode && !IS_INTRESOURCE(icix->lpVerbW)) {
            if (_wcsicmp(icix->lpVerbW, kVerbW) == 0) command = kCommandProperties;
            else if (_wcsicmp(icix->lpVerbW, kUpgradeVerbW) == 0) command = kCommandUpgrade;
            else if (_wcsicmp(icix->lpVerbW, kDependentsVerbW) == 0) command = kCommandDependents;
        } else if (!IS_INTRESOURCE(ici->lpVerb)) {
            if (_stricmp(ici->lpVerb, kVerbA) == 0) command = kCommandProperties;
            else if (_stricmp(ici->lpVerb, kUpgradeVerbA) == 0) command = kCommandUpgrade;
            else if (_stricmp(ici->lpVerb, kDependentsVerbA) == 0) command = kCommandDependents;
        } else {
            command = LOWORD(reinterpret_cast<UINT_PTR>(ici->lpVerb));
        }
        if (command >= kCommandCount) return E_FAIL;
        if (command == kCommandUpgrade) {
            try {
                UpgradeHeadersAsync(paths_);
            } catch (...) {
                return E_OUTOFMEMORY;
            }
            return S_OK;
        }
        std::vector<std::wstring> paths = paths_;
        if (paths.size() > kMaxWindows) paths.resize(kMaxWindows);
        if (command == kCommandDependents) {
            for (const auto& path : paths) ShowDependentsWindowAsync(path);
            return S_OK;
        }
        ShowPropertyWindowsAsync(paths);
        return S_OK;
    }

    IFACEMETHODIMP GetCommandString(UINT_PTR idCmd, UINT type, UINT*, CHAR* name, UINT cch) override {
        DebugLog(L"GetCommandString id=%zu type=0x%X", idCmd, type);
        if (idCmd >= kCommandCount) return E_INVALIDARG;
        const bool upgrade = idCmd == kCommandUpgrade, dependents = idCmd == kCommandDependents;
        switch (type) {
            case GCS_VERBW:
                return StringCchCopyW(reinterpret_cast<wchar_t*>(name), cch, upgrade ? kUpgradeVerbW : dependents ? kDependentsVerbW : kVerbW);
            case GCS_VERBA:
                return StringCchCopyA(name, cch, upgrade ? kUpgradeVerbA : dependents ? kDependentsVerbA : kVerbA);
            case GCS_HELPTEXTW:
                return StringCchCopyW(reinterpret_cast<wchar_t*>(name), cch, upgrade ? kUpgradeHelpText : dependents ? kDependentsHelpText : kHelpText);
            case GCS_HELPTEXTA:
                return StringCchCopyA(name, cch, upgrade        ? "Changes a 1.70 plugin header to 1.71."
                                                 : dependents ? "Lists the plugins in this folder that have this plugin as a master."
                                                              : "Shows the header, masters and record groups of this plugin.");
            case GCS_VALIDATEW:
            case GCS_VALIDATEA:
                return S_OK;
        }
        return E_NOTIMPL;
    }

private:
    ~ContextMenu() { InterlockedDecrement(&g_dllRefs); }

    long refs_ = 1;
    std::vector<std::wstring> paths_;
};

class ClassFactory final : public IClassFactory {
public:
    ClassFactory() { InterlockedIncrement(&g_dllRefs); }

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
        auto* obj = new (std::nothrow) ContextMenu();
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
    ~ClassFactory() { InterlockedDecrement(&g_dllRefs); }
    long refs_ = 1;
};

}  // namespace

HRESULT CreateClassFactory(REFIID riid, void** ppv) {
    auto* factory = new (std::nothrow) ClassFactory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

}  // namespace pse

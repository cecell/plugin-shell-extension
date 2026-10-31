// IShellExtInit + IContextMenu handler that adds "Plugin properties" to the
// context menu of .esp/.esm/.esl files.
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

namespace pp {

volatile long g_dllRefs = 0;

namespace {

constexpr wchar_t kVerbW[] = L"pluginproperties";
constexpr char kVerbA[] = "pluginproperties";
constexpr wchar_t kMenuText[] = L"Plugin properties";
constexpr wchar_t kHelpText[] = L"Shows the header, masters and record groups of this plugin.";
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
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask = MIIM_STRING | MIIM_ID | MIIM_FTYPE;
        mii.fType = MFT_STRING;
        mii.wID = idFirst;
        mii.dwTypeData = const_cast<wchar_t*>(kMenuText);
        if (!InsertMenuItemW(menu, index, TRUE, &mii)) return HRESULT_FROM_WIN32(GetLastError());
        return MAKE_HRESULT(SEVERITY_SUCCESS, FACILITY_NULL, 1);
    }

    IFACEMETHODIMP InvokeCommand(CMINVOKECOMMANDINFO* ici) override {
        if (!ici) return E_INVALIDARG;
        bool unicode = ici->cbSize >= sizeof(CMINVOKECOMMANDINFOEX) && (ici->fMask & CMIC_MASK_UNICODE);
        auto* icix = reinterpret_cast<CMINVOKECOMMANDINFOEX*>(ici);
        DebugLog(L"InvokeCommand cbSize=%u mask=0x%X verb=%p verbW=%p", ici->cbSize, ici->fMask, ici->lpVerb,
                 unicode ? icix->lpVerbW : nullptr);
        if (unicode && !IS_INTRESOURCE(icix->lpVerbW)) {
            if (_wcsicmp(icix->lpVerbW, kVerbW) != 0) return E_FAIL;
        } else if (!IS_INTRESOURCE(ici->lpVerb)) {
            if (_stricmp(ici->lpVerb, kVerbA) != 0) return E_FAIL;
        } else if (LOWORD(reinterpret_cast<UINT_PTR>(ici->lpVerb)) != 0) {
            return E_FAIL;
        }
        std::vector<std::wstring> paths = paths_;
        if (paths.size() > kMaxWindows) paths.resize(kMaxWindows);
        ShowPropertyWindowsAsync(paths);
        return S_OK;
    }

    IFACEMETHODIMP GetCommandString(UINT_PTR idCmd, UINT type, UINT*, CHAR* name, UINT cch) override {
        DebugLog(L"GetCommandString id=%zu type=0x%X", idCmd, type);
        if (idCmd != 0) return E_INVALIDARG;
        switch (type) {
            case GCS_VERBW:
                return StringCchCopyW(reinterpret_cast<wchar_t*>(name), cch, kVerbW);
            case GCS_VERBA:
                return StringCchCopyA(name, cch, kVerbA);
            case GCS_HELPTEXTW:
                return StringCchCopyW(reinterpret_cast<wchar_t*>(name), cch, kHelpText);
            case GCS_HELPTEXTA:
                return StringCchCopyA(name, cch, "Shows the header, masters and record groups of this plugin.");
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

}  // namespace pp

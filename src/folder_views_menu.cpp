// "Folder views" on the background of a folder: saves the window's layout under
// a name, applies a saved one to this folder or its subfolders, removes one.
#include "folder_views_menu.h"

#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <new>
#include <string>
#include <vector>

#include "context_menu.h"
#include "folder_views.h"
#include "tool_windows.h"

namespace pse {
namespace {

using Microsoft::WRL::ComPtr;

constexpr UINT kMaxViews = 40;
constexpr const wchar_t* kTitle = L"Folder views";

std::wstring MenuText(const std::wstring& name) {
    std::wstring safe;
    for (wchar_t c : name) {
        if (c == L'&') safe += L'&';
        safe += c;
    }
    return safe;
}

class FolderViewsMenu final : public IShellExtInit, public IContextMenu, public IObjectWithSite {
public:
    FolderViewsMenu() { InterlockedIncrement(&g_dllRefs); }

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

    IFACEMETHODIMP SetSite(IUnknown* site) override {
        site_ = site;
        return S_OK;
    }
    IFACEMETHODIMP GetSite(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        *ppv = nullptr;
        return site_ ? site_->QueryInterface(riid, ppv) : E_FAIL;
    }

    IFACEMETHODIMP Initialize(PCIDLIST_ABSOLUTE folder, IDataObject*, HKEY) override {
        wchar_t path[MAX_PATH * 2] = {};
        if (!folder || !SHGetPathFromIDListEx(folder, path, ARRAYSIZE(path), GPFIDL_DEFAULT)) return E_FAIL;
        try {
            root_ = path;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }

    IFACEMETHODIMP QueryContextMenu(HMENU menu, UINT index, UINT first, UINT last, UINT flags) override {
        if (flags & CMF_DEFAULTONLY) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        if (last < first) return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 0);
        HMENU sub = CreatePopupMenu();
        if (!sub) return E_OUTOFMEMORY;
        try {
            views_ = LoadFolderViews();
            const UINT room = (last - first) / 3;
            if (views_.size() > (std::min)(kMaxViews, room)) views_.resize((std::min)(kMaxViews, room));
            const UINT count = static_cast<UINT>(views_.size());
            AppendMenuW(sub, MF_STRING, first, L"Save current view as...");
            if (count) {
                AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
                for (UINT i = 0; i < count; ++i) AppendMenuW(sub, MF_STRING, first + 1 + i, MenuText(views_[i].name).c_str());
                AppendMenuW(sub, MF_SEPARATOR, 0, nullptr);
                HMENU below = CreatePopupMenu();
                HMENU remove = CreatePopupMenu();
                for (UINT i = 0; i < count; ++i) {
                    const std::wstring label = MenuText(views_[i].name);
                    AppendMenuW(below, MF_STRING, first + 1 + count + i, label.c_str());
                    if (!views_[i].shipped) AppendMenuW(remove, MF_STRING, first + 1 + 2 * count + i, label.c_str());
                }
                AppendMenuW(sub, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(below), L"Apply to subfolders");
                // Only what the user saved can be removed; a built-in preset always stays.
                if (GetMenuItemCount(remove) > 0) AppendMenuW(sub, MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(remove), L"Remove");
                else DestroyMenu(remove);
            }
            InsertMenuW(menu, index, MF_BYPOSITION | MF_POPUP | MF_STRING, reinterpret_cast<UINT_PTR>(sub), kTitle);
            return MAKE_HRESULT(SEVERITY_SUCCESS, 0, 1 + 3 * count);
        } catch (...) {
            DestroyMenu(sub);
            return E_OUTOFMEMORY;
        }
    }

    IFACEMETHODIMP InvokeCommand(LPCMINVOKECOMMANDINFO info) override {
        if (!info) return E_POINTER;
        if (HIWORD(info->lpVerb)) return E_INVALIDARG;
        const UINT command = LOWORD(info->lpVerb);
        const UINT count = static_cast<UINT>(views_.size());
        if (command > 3 * count) return E_INVALIDARG;
        try {
            if (command && command > 2 * count) {
                RemoveFolderViewAsync(views_[command - 1 - 2 * count].name);
                return S_OK;
            }
            ComPtr<IShellBrowser> browser;
            ComPtr<IShellView> view;
            if (!site_ || FAILED(IUnknown_QueryService(site_.Get(), SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) || FAILED(browser->QueryActiveShellView(&view))) {
                ShowMessageAsync(kTitle, L"This window's view couldn't be reached.");
                return S_OK;
            }
            if (command == 0) {
                FolderViewState state;
                const HRESULT hr = CaptureFolderView(view.Get(), state);
                if (FAILED(hr)) ShowMessageAsync(kTitle, L"This window's view couldn't be read.");
                else SaveFolderViewAsync(std::move(state));
                return S_OK;
            }
            const bool below = command > count;
            const FolderViewState& state = views_[below ? command - 1 - count : command - 1];
            if (FAILED(ApplyFolderView(view.Get(), state))) {
                ShowMessageAsync(kTitle, L"The view couldn't be applied to this folder.");
                return S_OK;
            }
            if (below) WalkSubfolders(browser.Get(), state);
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }

    IFACEMETHODIMP GetCommandString(UINT_PTR, UINT, UINT*, CHAR*, UINT) override { return E_NOTIMPL; }

private:
    ~FolderViewsMenu() { InterlockedDecrement(&g_dllRefs); }

    void WalkSubfolders(IShellBrowser* browser, const FolderViewState& state) {
        const std::wstring name = state.name;
        FolderViewWalkCallbacks callbacks;
        callbacks.confirm = [name](size_t count) {
            const std::wstring ask = L"Apply \"" + name + L"\" to " + std::to_wstring(count) + L" subfolders?\r\n\r\nThis window will step through each one and then come back here. Going to another folder stops it.";
            return MessageBoxW(nullptr, ask.c_str(), kTitle, MB_OKCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND) == IDOK;
        };
        callbacks.report = [](const std::wstring& message) { ShowMessageAsync(kTitle, message); };
        InterlockedIncrement(&g_dllRefs);
        callbacks.done = [] { InterlockedDecrement(&g_dllRefs); };
        if (!StartFolderViewWalk(browser, root_, state, std::move(callbacks))) {
            InterlockedDecrement(&g_dllRefs);
            ShowMessageAsync(kTitle, L"The subfolders couldn't be processed.");
        }
    }

    long refs_ = 1;
    ComPtr<IUnknown> site_;
    std::wstring root_;
    std::vector<FolderViewState> views_;
};

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
        auto* obj = new (std::nothrow) FolderViewsMenu();
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
};

}  // namespace

HRESULT CreateFolderViewsMenuFactory(REFIID riid, void** ppv) {
    auto* factory = new (std::nothrow) Factory();
    if (!factory) return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(riid, ppv);
    factory->Release();
    return hr;
}

}  // namespace pse

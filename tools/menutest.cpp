// End-to-end check of the installed handler: builds Explorer's context menu
// for a file the same way Explorer does, looks for "Plugin properties", and
// optionally invokes it and checks that the window opens.
//   menutest.exe <plugin> [--invoke]
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <cstdio>
#include <string>

namespace {

bool FindItem(HMENU menu, int depth, UINT* id) {
    bool found = false;
    int count = GetMenuItemCount(menu);
    for (int i = 0; i < count; ++i) {
        wchar_t text[256] = {};
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask = MIIM_STRING | MIIM_ID | MIIM_SUBMENU | MIIM_FTYPE;
        mii.dwTypeData = text;
        mii.cch = ARRAYSIZE(text);
        if (!GetMenuItemInfoW(menu, i, TRUE, &mii)) continue;
        if (mii.fType & MFT_SEPARATOR) continue;
        wprintf(L"%*s%s\n", depth * 2, L"", text);
        if (wcscmp(text, L"Plugin properties") == 0) {
            *id = mii.wID;
            found = true;
        }
        if (mii.hSubMenu && FindItem(mii.hSubMenu, depth + 1, id)) found = true;
    }
    return found;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: menutest <plugin> [--invoke]\n");
        return 2;
    }
    bool invoke = argc > 2 && wcscmp(argv[2], L"--invoke") == 0;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(argv[1], nullptr, &pidl, 0, nullptr))) {
        fwprintf(stderr, L"cannot parse path\n");
        return 2;
    }
    IShellFolder* folder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    IContextMenu* cm = nullptr;
    int rc = 1;
    if (SUCCEEDED(SHBindToParent(pidl, IID_PPV_ARGS(&folder), &child)) &&
        SUCCEEDED(folder->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr,
                                        reinterpret_cast<void**>(&cm)))) {
        HMENU menu = CreatePopupMenu();
        const UINT first = 1;
        cm->QueryContextMenu(menu, 0, first, 0x7FFF, CMF_NORMAL);
        UINT id = 0;
        if (FindItem(menu, 0, &id)) {
            wchar_t verb[64] = {};
            cm->GetCommandString(id - first, GCS_VERBW, nullptr, reinterpret_cast<char*>(verb), ARRAYSIZE(verb));
            wprintf(L"FOUND: 'Plugin properties' id=%u verb=%s\n", id, verb);
            rc = 0;
            if (invoke) {
                CMINVOKECOMMANDINFOEX ici{sizeof(ici)};
                ici.fMask = CMIC_MASK_UNICODE;
                ici.lpVerb = MAKEINTRESOURCEA(id - first);
                ici.lpVerbW = MAKEINTRESOURCEW(id - first);
                ici.nShow = SW_SHOWNORMAL;
                HRESULT hr = cm->InvokeCommand(reinterpret_cast<CMINVOKECOMMANDINFO*>(&ici));
                wprintf(L"InvokeCommand hr=0x%08lX\n", static_cast<unsigned long>(hr));
                HWND wnd = nullptr;
                for (int i = 0; i < 100 && !wnd; ++i) {
                    Sleep(100);
                    wnd = FindWindowW(L"PluginShellExtensionDialog", nullptr);
                }
                if (wnd) {
                    wchar_t title[256] = {};
                    GetWindowTextW(wnd, title, ARRAYSIZE(title));
                    wprintf(L"WINDOW: %s\n", title);
                    Sleep(1500);
                    PostMessageW(wnd, WM_CLOSE, 0, 0);
                    for (int i = 0; i < 50 && IsWindow(wnd); ++i) Sleep(100);
                    wprintf(L"closed: %s\n", IsWindow(wnd) ? L"no" : L"yes");
                } else {
                    wprintf(L"WINDOW: not found\n");
                    rc = 1;
                }
            }
        } else {
            wprintf(L"NOT FOUND\n");
        }
        DestroyMenu(menu);
        cm->Release();
    }
    if (folder) folder->Release();
    CoTaskMemFree(pidl);
    // Give the window thread time to unwind before the process exits.
    Sleep(500);
    CoUninitialize();
    return rc;
}

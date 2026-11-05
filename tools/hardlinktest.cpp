// Checks the Hardlink menu handler without registering it: loads the DLL,
// creates the handler from its class factory, initializes it with a file the
// way Explorer does, prints the menu it adds and can invoke "Copy path".
//   hardlinktest.exe <PluginShellExtension.dll> <file> [--copy]
#include <windows.h>
#include <shlobj.h>

#include <cstdio>
#include <string>

namespace {

// {9FB91679-FC8C-4040-9C35-61E735E81908}
constexpr GUID kClsidHardlinkMenu = {0x9fb91679, 0xfc8c, 0x4040, {0x9c, 0x35, 0x61, 0xe7, 0x35, 0xe8, 0x19, 0x08}};

int g_copyId = -1;

void Print(HMENU menu, int depth, UINT first) {
    for (int i = 0; i < GetMenuItemCount(menu); ++i) {
        wchar_t text[512] = {};
        MENUITEMINFOW mii{sizeof(mii)};
        mii.fMask = MIIM_STRING | MIIM_ID | MIIM_SUBMENU | MIIM_FTYPE | MIIM_STATE;
        mii.dwTypeData = text;
        mii.cch = ARRAYSIZE(text);
        if (!GetMenuItemInfoW(menu, i, TRUE, &mii)) continue;
        if (mii.fType & MFT_SEPARATOR) {
            wprintf(L"%*s----\n", depth * 2, L"");
            continue;
        }
        wprintf(L"%*s%s%s\n", depth * 2, L"", text, (mii.fState & MFS_GRAYED) ? L"  [greyed]" : L"");
        if (g_copyId < 0 && wcscmp(text, L"Copy path") == 0) g_copyId = static_cast<int>(mii.wID - first);
        if (mii.hSubMenu) Print(mii.hSubMenu, depth + 1, first);
    }
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fwprintf(stderr, L"usage: hardlinktest <PluginShellExtension.dll> <file> [--copy]\n");
        return 2;
    }
    const bool copy = argc > 3 && wcscmp(argv[3], L"--copy") == 0;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE dll = LoadLibraryW(argv[1]);
    auto get = dll ? reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
                         GetProcAddress(dll, "DllGetClassObject"))
                   : nullptr;
    IClassFactory* factory = nullptr;
    if (!get || FAILED(get(kClsidHardlinkMenu, IID_PPV_ARGS(&factory)))) {
        fwprintf(stderr, L"cannot load the handler from %s\n", argv[1]);
        return 2;
    }
    IShellExtInit* init = nullptr;
    factory->CreateInstance(nullptr, IID_PPV_ARGS(&init));
    // A data object for the file, as Explorer builds it.
    PIDLIST_ABSOLUTE pidl = nullptr;
    IShellFolder* folder = nullptr;
    PCUITEMID_CHILD child = nullptr;
    IDataObject* data = nullptr;
    if (FAILED(SHParseDisplayName(argv[2], nullptr, &pidl, 0, nullptr)) ||
        FAILED(SHBindToParent(pidl, IID_PPV_ARGS(&folder), &child)) ||
        FAILED(folder->GetUIObjectOf(nullptr, 1, &child, IID_IDataObject, nullptr, reinterpret_cast<void**>(&data)))) {
        fwprintf(stderr, L"cannot get a data object for %s\n", argv[2]);
        return 2;
    }
    HRESULT hr = init->Initialize(nullptr, data, nullptr);
    wprintf(L"Initialize: 0x%08lX\n", hr);
    int rc = 1;
    if (SUCCEEDED(hr)) {
        IContextMenu* cm = nullptr;
        init->QueryInterface(IID_PPV_ARGS(&cm));
        HMENU menu = CreatePopupMenu();
        const UINT first = 100;
        hr = cm->QueryContextMenu(menu, 0, first, first + 200, CMF_NORMAL);
        wprintf(L"QueryContextMenu: 0x%08lX (%u ids)\n", hr, HRESULT_CODE(hr));
        Print(menu, 0, first);
        rc = 0;
        if (copy && g_copyId >= 0) {
            CMINVOKECOMMANDINFO ici{sizeof(ici)};
            ici.lpVerb = MAKEINTRESOURCEA(g_copyId);
            ici.nShow = SW_SHOWNORMAL;
            hr = cm->InvokeCommand(&ici);
            wprintf(L"InvokeCommand(Copy path): 0x%08lX\n", hr);
            if (OpenClipboard(nullptr)) {
                if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                    wprintf(L"clipboard: %s\n", static_cast<const wchar_t*>(GlobalLock(h)));
                    GlobalUnlock(h);
                }
                CloseClipboard();
            }
        }
        DestroyMenu(menu);
        cm->Release();
    }
    data->Release();
    folder->Release();
    CoTaskMemFree(pidl);
    init->Release();
    factory->Release();
    return rc;
}

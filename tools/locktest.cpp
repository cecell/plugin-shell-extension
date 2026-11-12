// locktest <PluginShellExtension.dll> <archive.bsa> <plugin> [<watched folder>]
//
// Checks that WHAM doesn't keep files locked inside the process that loads it:
//  1. browses <archive.bsa> through the BSA folder and reads file sizes, then
//     checks the archive is closed again after its idle time;
//  2. runs the #ITM check on <plugin> the way Explorer does (background
//     workers), then checks its masters are unmapped once the check is done;
//  3. checks the folder watcher lets go of the plugin's folder when no Explorer
//     window shows it.
// "Unlocked" means this process can open the file with no sharing at all.
// Use copies or a test corpus: the files are only opened, never written.
#include <windows.h>

#include <propsys.h>
#include <propvarutil.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <psapi.h>

#include <cstdio>
#include <string>
#include <vector>

#include "plugin_parser.h"
#include "property_handler.h"
#include "registration.h"

namespace {

int g_failures = 0;

// Whether this process has the file mapped (a mapping keeps no file handle, but
// still stops the file from being overwritten or truncated).
bool Mapped(const std::wstring& path) {
    // GetMappedFileName gives \Device\HarddiskVolumeN\folder\file: compare from the folder on.
    std::wstring tail = path.size() > 2 && path[1] == L':' ? path.substr(2) : path;
    CharLowerBuffW(tail.data(), static_cast<DWORD>(tail.size()));
    MEMORY_BASIC_INFORMATION mbi{};
    for (const uint8_t* p = nullptr; VirtualQuery(p, &mbi, sizeof(mbi)) == sizeof(mbi); p = static_cast<const uint8_t*>(mbi.BaseAddress) + mbi.RegionSize) {
        if (mbi.Type != MEM_MAPPED || mbi.BaseAddress != mbi.AllocationBase) continue;
        wchar_t name[MAX_PATH * 2] = {};
        if (!GetMappedFileNameW(GetCurrentProcess(), mbi.BaseAddress, name, ARRAYSIZE(name))) continue;
        std::wstring n = name;
        CharLowerBuffW(n.data(), static_cast<DWORD>(n.size()));
        if (n.size() >= tail.size() && n.compare(n.size() - tail.size(), tail.size(), tail) == 0) return true;
    }
    return false;
}

// Not mapped, and no handle open in a way that stops an exclusive open.
bool Unlocked(const std::wstring& path, bool dir = false) {
    if (!dir && Mapped(path)) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, dir ? FILE_FLAG_BACKUP_SEMANTICS : 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

// Polls until the file is unlocked or `seconds` pass. Returns the seconds it took, or -1.
double WaitUnlocked(const std::wstring& path, int seconds, bool dir = false) {
    const ULONGLONG start = GetTickCount64();
    while (GetTickCount64() - start < static_cast<ULONGLONG>(seconds) * 1000) {
        if (Unlocked(path, dir)) return (GetTickCount64() - start) / 1000.0;
        Sleep(250);
    }
    return -1;
}

void Check(bool ok, const std::wstring& what) {
    wprintf(L"%s %s\n", ok ? L"ok  " : L"FAIL", what.c_str());
    g_failures += !ok;
}

HRESULT GetFactory(HMODULE dll, REFCLSID clsid, IClassFactory** factory) {
    auto get = reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(GetProcAddress(dll, "DllGetClassObject"));
    return get ? get(clsid, IID_IClassFactory, reinterpret_cast<void**>(factory)) : E_FAIL;
}

void ArchiveTest(HMODULE dll, const std::wstring& archive) {
    IClassFactory* factory = nullptr;
    if (FAILED(GetFactory(dll, pse::kClsidBsaFolder, &factory))) return Check(false, L"BSA folder factory");
    LPITEMIDLIST pidl = nullptr;
    IPersistFolder* persist = nullptr;
    IShellFolder2* folder = nullptr;
    SHParseDisplayName(archive.c_str(), nullptr, &pidl, 0, nullptr);
    factory->CreateInstance(nullptr, IID_PPV_ARGS(&persist));
    if (!pidl || !persist || FAILED(persist->Initialize(pidl)) || FAILED(persist->QueryInterface(IID_PPV_ARGS(&folder))))
        return Check(false, L"open the archive as a folder");
    Check(Unlocked(archive), L"archive not held after opening it as a folder (only the names are kept)");
    // Read sizes the way the Size column does, walking into the first folders.
    IEnumIDList* e = nullptr;
    PITEMID_CHILD item = nullptr;
    int read = 0;
    folder->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &e);
    IShellFolder2* sub = nullptr;
    while (e && e->Next(1, &item, nullptr) == S_OK) {
        SFGAOF a = SFGAO_FOLDER;
        PCUITEMID_CHILD arr[] = {item};
        folder->GetAttributesOf(1, arr, &a);
        if ((a & SFGAO_FOLDER) && !sub) folder->BindToObject(item, nullptr, IID_PPV_ARGS(&sub));
        CoTaskMemFree(item);
    }
    if (e) e->Release();
    for (IShellFolder2* f = sub; f && read == 0;) {
        IEnumIDList* e2 = nullptr;
        f->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS, &e2);
        IShellFolder2* deeper = nullptr;
        while (e2 && e2->Next(1, &item, nullptr) == S_OK) {
            SHELLDETAILS d{};
            if (SUCCEEDED(f->GetDetailsOf(item, 1, &d))) {
                CoTaskMemFree(d.str.pOleStr);
                ++read;
            }
            SFGAOF a = SFGAO_FOLDER;
            PCUITEMID_CHILD arr[] = {item};
            f->GetAttributesOf(1, arr, &a);
            if ((a & SFGAO_FOLDER) && !deeper) f->BindToObject(item, nullptr, IID_PPV_ARGS(&deeper));
            CoTaskMemFree(item);
        }
        if (e2) e2->Release();
        if (f != sub) f->Release();
        f = deeper;
    }
    wprintf(L"     read %d sizes\n", read);
    Check(read > 0 && !Unlocked(archive), L"archive open while sizes are being read");
    // The folder objects stay alive, as Explorer keeps them: the file must still be let go.
    const double t = WaitUnlocked(archive, 20);
    wprintf(L"     unlocked after %.1f s (folder objects still alive)\n", t);
    Check(t >= 0, L"archive closed again after its idle time");
    if (sub) sub->Release();
    folder->Release();
    persist->Release();
    CoTaskMemFree(pidl);
    factory->Release();
}

void ItmTest(HMODULE dll, const std::wstring& plugin) {
    SetEnvironmentVariableW(L"PSE_ASYNC_TEST", L"1");  // the Explorer path: background workers
    const pse::PluginHeader header = pse::ReadPluginHeader(plugin);
    const std::wstring dir = plugin.substr(0, plugin.find_last_of(L'\\') + 1);
    IClassFactory* factory = nullptr;
    if (FAILED(GetFactory(dll, pse::kClsidPropertyHandler, &factory))) return Check(false, L"property handler factory");
    IInitializeWithFile* init = nullptr;
    factory->CreateInstance(nullptr, IID_PPV_ARGS(&init));
    IPropertyStore* store = nullptr;
    if (!init || FAILED(init->Initialize(plugin.c_str(), STGM_READ)) || FAILED(init->QueryInterface(IID_PPV_ARGS(&store))))
        return Check(false, L"open the plugin's properties");
    const PROPERTYKEY itm = {pse::kPluginFmtid, 14};
    std::wstring value;
    bool sawLocked = false;
    // Ask until the background check has a value, as Explorer does after its refresh.
    for (int i = 0; i < 600; ++i) {
        PROPVARIANT pv;
        PropVariantInit(&pv);
        store->GetValue(itm, &pv);
        value = pv.vt == VT_LPWSTR ? pv.pwszVal : pv.vt == VT_EMPTY ? L"(blank: 0)" : L"?";
        PropVariantClear(&pv);
        for (const auto& m : header.masters) sawLocked = sawLocked || !Unlocked(dir + m);
        if (value != L"Working") break;
        Sleep(100);
    }
    wprintf(L"     #ITM = %s; a master was seen locked during the check: %s\n", value.c_str(), sawLocked ? L"yes" : L"no");
    for (const auto& m : header.masters) {
        const double t = WaitUnlocked(dir + m, 10);
        Check(t >= 0, L"master " + m + L" unmapped after the check (" + std::to_wstring(static_cast<int>(t * 1000)) + L" ms)");
    }
    store->Release();
    init->Release();
    factory->Release();
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        fwprintf(stderr, L"usage: locktest <dll> <archive.bsa> <plugin> [<watched folder>]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    HMODULE dll = LoadLibraryW(argv[1]);
    if (!dll) return 2;
    wprintf(L"-- archive\n");
    ArchiveTest(dll, argv[2]);
    wprintf(L"-- ITM masters\n");
    ItmTest(dll, argv[3]);
    const std::wstring plugin = argv[3];
    const std::wstring dir = plugin.substr(0, plugin.find_last_of(L'\\'));
    wprintf(L"-- folder watcher (%s)\n", dir.c_str());
    // The ITM check started a watcher on the plugin's folder; no Explorer window shows it here.
    const double t = WaitUnlocked(dir, 45, true);
    wprintf(L"     folder unlocked after %.1f s\n", t);
    Check(t >= 0, L"folder watcher let go of the folder (no window shows it)");
    wprintf(L"%s\n", g_failures ? L"FAILED" : L"all passed");
    return g_failures ? 1 : 0;
}

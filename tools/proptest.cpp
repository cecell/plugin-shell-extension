// Drives the property handler the way Explorer does, without registering it:
// loads the DLL, creates the handler class, initializes it with the file path
// and prints every property it reports.
//   proptest.exe <PluginShellExtension.dll> <plugin> [<plugin> ...]
#include <windows.h>
#include <io.h>
#include <fcntl.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <cstdio>

#include "../src/property_handler.h"
#include "../src/registration.h"

namespace {

const wchar_t* PidName(DWORD pid) {
    switch (pid) {
        case pse::kPidHeaderVersion: return L"Header Ver";
        case pse::kPidRecordCount: return L"#Records";
        case pse::kPidFlags: return L"Flags";
        case pse::kPidLight: return L"Light";
        case pse::kPidMasterCount: return L"#Masters";
        case pse::kPidDeletedCount: return L"#Deleted";
        case pse::kPidEslable: return L"ESLable";
        case pse::kPidMasters: return L"Masters";
        case pse::kPidEslNote: return L"ESL note";
        case pse::kPidFormVersion: return L"Form Ver";
        case pse::kPidDeletedNavm: return L"Del NAVM";
        case pse::kPidFormCount: return L"#Forms";
        case pse::kPidItm: return L"#ITM";
    }
    return L"?";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    if (argc < 3) {
        fwprintf(stderr, L"usage: proptest <PluginShellExtension.dll> <plugin> [...]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryW(argv[1]);
    auto getClassObject = dll ? reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
                                    GetProcAddress(dll, "DllGetClassObject"))
                              : nullptr;
    IClassFactory* factory = nullptr;
    if (!getClassObject || FAILED(getClassObject(pse::kClsidPropertyHandler, IID_PPV_ARGS(&factory)))) {
        fwprintf(stderr, L"cannot load the handler from %s\n", argv[1]);
        return 2;
    }
    int failures = 0;
    // PSE_ASYNC_TEST: take the Explorer path ("Working" + background check):
    // ask about every file, wait for the workers, then ask again.
    bool async = GetEnvironmentVariableW(L"PSE_ASYNC_TEST", nullptr, 0) > 0;
    for (int pass = 0; pass < (async ? 2 : 1); ++pass) {
    if (pass) {
        wprintf(L"-- second pass --\n");
        Sleep(20000);
    }
    for (int i = 2; i < argc; ++i) {
        wprintf(L"== %s\n", argv[i]);
        IInitializeWithFile* init = nullptr;
        IPropertyStore* store = nullptr;
        ULONGLONG start = GetTickCount64();
        if (FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&init))) || FAILED(init->Initialize(argv[i], STGM_READ)) ||
            FAILED(init->QueryInterface(IID_PPV_ARGS(&store)))) {
            wprintf(L"failed to open\n");
            ++failures;
        } else {
            DWORD count = 0;
            store->GetCount(&count);
            for (DWORD k = 0; k < count; ++k) {
                PROPERTYKEY key{};
                store->GetAt(k, &key);
                PROPVARIANT pv;
                store->GetValue(key, &pv);
                PWSTR text = nullptr;
                if (pv.vt == VT_EMPTY) {
                    wprintf(L"  %-10s (empty)\n", PidName(key.pid));
                } else if (SUCCEEDED(PropVariantToStringAlloc(pv, &text))) {
                    wprintf(L"  %-10s %s\n", PidName(key.pid), text);
                    CoTaskMemFree(text);
                }
                PropVariantClear(&pv);
            }
            if (!count) wprintf(L"  (no properties: not a plugin)\n");
            wprintf(L"  [%llu ms]\n", GetTickCount64() - start);
        }
        if (store) store->Release();
        if (init) init->Release();
    }
    }
    factory->Release();
    CoUninitialize();
    return failures ? 1 : 0;
}

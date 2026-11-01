// Drives the property handler the way Explorer does, without registering it:
// loads the DLL, creates the handler class, initializes it with a file stream
// and prints every property it reports.
//   proptest.exe <PluginProperties.dll> <plugin> [<plugin> ...]
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
        case pp::kPidHeaderVersion: return L"Header Ver";
        case pp::kPidRecordCount: return L"#Records";
        case pp::kPidFlags: return L"Flags";
        case pp::kPidLight: return L"Light";
        case pp::kPidMasterCount: return L"#Masters";
        case pp::kPidDeletedCount: return L"#Deleted";
        case pp::kPidEslable: return L"ESLable";
        case pp::kPidMasters: return L"Masters";
        case pp::kPidEslNote: return L"ESL note";
    }
    return L"?";
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    if (argc < 3) {
        fwprintf(stderr, L"usage: proptest <PluginProperties.dll> <plugin> [...]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryW(argv[1]);
    auto getClassObject = dll ? reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
                                    GetProcAddress(dll, "DllGetClassObject"))
                              : nullptr;
    IClassFactory* factory = nullptr;
    if (!getClassObject || FAILED(getClassObject(pp::kClsidPropertyHandler, IID_PPV_ARGS(&factory)))) {
        fwprintf(stderr, L"cannot load the handler from %s\n", argv[1]);
        return 2;
    }
    int failures = 0;
    for (int i = 2; i < argc; ++i) {
        wprintf(L"== %s\n", argv[i]);
        IStream* stream = nullptr;
        IInitializeWithStream* init = nullptr;
        IPropertyStore* store = nullptr;
        ULONGLONG start = GetTickCount64();
        if (FAILED(SHCreateStreamOnFileEx(argv[i], STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &stream)) ||
            FAILED(factory->CreateInstance(nullptr, IID_PPV_ARGS(&init))) || FAILED(init->Initialize(stream, STGM_READ)) ||
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
        if (stream) stream->Release();
    }
    factory->Release();
    CoUninitialize();
    return failures ? 1 : 0;
}

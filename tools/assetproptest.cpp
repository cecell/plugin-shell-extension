// Loads the asset property handler from the DLL without registering it, feeds it
// each file as a stream the way Explorer does, and prints every value.
//
//   assetproptest <PluginShellExtension.dll> <file> [...]
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <cstdio>

#include "registration.h"

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    if (argc < 3) {
        fwprintf(stderr, L"usage: assetproptest <PluginShellExtension.dll> <file> [...]\n");
        return 2;
    }
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HMODULE dll = LoadLibraryW(argv[1]);
    auto getClassObject = dll ? reinterpret_cast<HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, void**)>(
                                    GetProcAddress(dll, "DllGetClassObject"))
                              : nullptr;
    IClassFactory* factory = nullptr;
    if (!getClassObject || FAILED(getClassObject(pse::kClsidAssetPropertyHandler, IID_PPV_ARGS(&factory)))) {
        fwprintf(stderr, L"cannot load the handler from %s\n", argv[1]);
        return 2;
    }
    int failures = 0;
    for (int i = 2; i < argc; ++i) {
        wprintf(L"== %s\n", argv[i]);
        IStream* stream = nullptr;
        IInitializeWithStream* init = nullptr;
        IPropertyStore* store = nullptr;
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
                PWSTR name = nullptr;
                PWSTR text = nullptr;
                PSGetNameFromPropertyKey(key, &name);
                if (SUCCEEDED(PropVariantToStringAlloc(pv, &text))) {
                    wprintf(L"  %s=%s\n", name ? name : L"?", text);
                    CoTaskMemFree(text);
                }
                CoTaskMemFree(name);
                PropVariantClear(&pv);
            }
            if (!count) wprintf(L"  (no properties)\n");
        }
        if (store) store->Release();
        if (init) init->Release();
        if (stream) stream->Release();
    }
    factory->Release();
    CoUninitialize();
    return failures ? 1 : 0;
}

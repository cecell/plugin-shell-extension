// Loads the asset property handler from the DLL without registering it, feeds it
// each file as a stream the way Explorer does, and prints every value.
//
//   assetproptest <PluginShellExtension.dll> <file> [...]    .dds files go through the .dds handler
//   assetproptest --windows-dds <PluginShellExtension.dll> <file> [...]   Windows' own handler, to compare
//   assetproptest --test-registration   taking over and restoring .dds, in a throwaway HKCU key
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <propsys.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <shobjidl.h>

#include <cstdio>
#include <string>

#include "registration.h"

namespace {

std::wstring ReadValue(const std::wstring& key, const wchar_t* value) {
    wchar_t buf[128] = {};
    DWORD size = sizeof(buf);
    return RegGetValueW(HKEY_CURRENT_USER, key.c_str(), value, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS ? buf : L"";
}

bool Exists(const std::wstring& key) {
    HKEY h = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, KEY_READ, &h) != ERROR_SUCCESS) return false;
    RegCloseKey(h);
    return true;
}

int TestRegistration() {
    const std::wstring base = L"Software\\PluginShellExtensionRegistrationTest\\";
    const std::wstring classes = base + L"Classes\\", handlers = base + L"Handlers\\";
    const std::wstring handler = handlers + L".dds", clsid = classes + L"CLSID\\" + pse::kDdsPropertyHandlerClsidString;
    const std::wstring windows = L"{a38b883c-1682-497e-97b0-0a3a9e801682}";
    int failures = 0;
    auto check = [&](bool ok, const wchar_t* what) {
        wprintf(L"%s %s\n", ok ? L"ok  " : L"FAIL", what);
        failures += !ok;
    };
    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    RegSetKeyValueW(HKEY_CURRENT_USER, handler.c_str(), nullptr, REG_SZ, windows.c_str(), static_cast<DWORD>((windows.size() + 1) * sizeof(wchar_t)));

    check(pse::InstallDdsHandler(HKEY_CURRENT_USER, classes, handlers, L"C:\\x.dll"), L"install succeeds");
    check(ReadValue(handler, nullptr) == pse::kDdsPropertyHandlerClsidString, L"ours is now registered");
    check(ReadValue(clsid, pse::kWrappedHandlerValue) == windows, L"Windows' handler is recorded");
    check(ReadValue(clsid + L"\\InprocServer32", nullptr) == L"C:\\x.dll", L"the DLL path is written");
    check(pse::InstallDdsHandler(HKEY_CURRENT_USER, classes, handlers, L"C:\\y.dll"), L"reinstall succeeds");
    check(ReadValue(clsid, pse::kWrappedHandlerValue) == windows, L"a reinstall keeps the recorded handler");
    check(ReadValue(clsid + L"\\InprocServer32", nullptr) == L"C:\\y.dll", L"a reinstall updates the DLL path");
    pse::RemoveDdsHandler(HKEY_CURRENT_USER, classes, handlers);
    check(ReadValue(handler, nullptr) == windows, L"uninstall puts Windows' handler back");
    check(!Exists(clsid), L"uninstall removes our CLSID");

    // Something else took .dds after we installed: leave it alone.
    pse::InstallDdsHandler(HKEY_CURRENT_USER, classes, handlers, L"C:\\x.dll");
    const std::wstring other = L"{11111111-2222-3333-4444-555555555555}";
    RegSetKeyValueW(HKEY_CURRENT_USER, handler.c_str(), nullptr, REG_SZ, other.c_str(), static_cast<DWORD>((other.size() + 1) * sizeof(wchar_t)));
    pse::RemoveDdsHandler(HKEY_CURRENT_USER, classes, handlers);
    check(ReadValue(handler, nullptr) == other, L"uninstall leaves another program's handler alone");

    // No handler at all beforehand: uninstall removes the association.
    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    pse::InstallDdsHandler(HKEY_CURRENT_USER, classes, handlers, L"C:\\x.dll");
    check(ReadValue(clsid, pse::kWrappedHandlerValue).empty(), L"nothing recorded when nothing was there");
    pse::RemoveDdsHandler(HKEY_CURRENT_USER, classes, handlers);
    check(!Exists(handler), L"uninstall leaves no association when there was none");

    RegDeleteTreeW(HKEY_CURRENT_USER, base.c_str());
    RegDeleteKeyW(HKEY_CURRENT_USER, base.substr(0, base.size() - 1).c_str());
    return failures ? 1 : 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    if (argc == 2 && !wcscmp(argv[1], L"--test-registration")) return TestRegistration();
    const bool windowsDds = argc > 1 && !wcscmp(argv[1], L"--windows-dds");
    if (windowsDds) {
        --argc;
        ++argv;
    }
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
    IClassFactory* ddsFactory = nullptr;
    if (windowsDds ? FAILED(CoGetClassObject(pse::kClsidWindowsImageHandler, CLSCTX_INPROC_SERVER, nullptr, IID_PPV_ARGS(&factory)))
                   : (!getClassObject || FAILED(getClassObject(pse::kClsidAssetPropertyHandler, IID_PPV_ARGS(&factory))) ||
                      FAILED(getClassObject(pse::kClsidDdsPropertyHandler, IID_PPV_ARGS(&ddsFactory))))) {
        fwprintf(stderr, L"cannot load the handler from %s\n", argv[1]);
        return 2;
    }
    int failures = 0;
    for (int i = 2; i < argc; ++i) {
        wprintf(L"== %s\n", argv[i]);
        IStream* stream = nullptr;
        IInitializeWithStream* init = nullptr;
        IPropertyStore* store = nullptr;
        const std::wstring name = argv[i];
        IClassFactory* chosen = ddsFactory && name.size() > 4 && !_wcsicmp(name.c_str() + name.size() - 4, L".dds") ? ddsFactory : factory;
        if (FAILED(SHCreateStreamOnFileEx(argv[i], STGM_READ | STGM_SHARE_DENY_NONE, 0, FALSE, nullptr, &stream)) ||
            FAILED(chosen->CreateInstance(nullptr, IID_PPV_ARGS(&init))) || FAILED(init->Initialize(stream, STGM_READ)) ||
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
                    if (name) wprintf(L"  %s=%s\n", name, text);
                    else wprintf(L"  pid%u=%s\n", key.pid, text);
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
    if (ddsFactory) ddsFactory->Release();
    CoUninitialize();
    return failures ? 1 : 0;
}

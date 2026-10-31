// DLL entry points: COM server exports, self-registration, and rundll32 entry
// points for testing without installing.
#include <windows.h>
#include <shellapi.h>

#include <string>

#include "context_menu.h"
#include "debug_log.h"
#include "property_window.h"
#include "registration.h"

HINSTANCE g_hInst = nullptr;

namespace {

std::wstring ModulePath() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(g_hInst, buf, ARRAYSIZE(buf));
    return std::wstring(buf, n);
}

std::wstring Unquote(std::wstring s) {
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t')) s.pop_back();
    size_t start = s.find_first_not_of(L" \t");
    s = start == std::wstring::npos ? std::wstring() : s.substr(start);
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    return s;
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = module;
        DisableThreadLibraryCalls(module);
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID clsid, REFIID riid, void** ppv) {
    if (!ppv) return E_POINTER;
    *ppv = nullptr;
    pp::DebugLog(L"DllGetClassObject match=%d", clsid == pp::kClsidContextMenu);
    if (clsid != pp::kClsidContextMenu) return CLASS_E_CLASSNOTAVAILABLE;
    return pp::CreateClassFactory(riid, ppv);
}

STDAPI DllCanUnloadNow() { return pp::g_dllRefs == 0 ? S_OK : S_FALSE; }

// regsvr32 PluginProperties.dll registers for the current user only.
STDAPI DllRegisterServer() { return pp::RegisterHandler(HKEY_CURRENT_USER, ModulePath()); }
STDAPI DllUnregisterServer() { return pp::UnregisterHandler(HKEY_CURRENT_USER); }

// regsvr32 /i:machine PluginProperties.dll (elevated) registers for all users.
STDAPI DllInstall(BOOL install, PCWSTR cmdLine) {
    HKEY root = (cmdLine && _wcsicmp(cmdLine, L"machine") == 0) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    return install ? pp::RegisterHandler(root, ModulePath()) : pp::UnregisterHandler(root);
}

// rundll32 PluginProperties.dll,ShowProperties <plugin path>
extern "C" void CALLBACK ShowPropertiesW(HWND, HINSTANCE, LPWSTR cmdLine, int) {
    pp::RunPropertyWindow(Unquote(cmdLine ? cmdLine : L""), std::wstring(), 0);
}

// rundll32 PluginProperties.dll,Snapshot "<plugin path>" "<out.png>"
// Renders the window to a PNG once the GRUP scan finishes, then closes it.
extern "C" void CALLBACK SnapshotW(HWND, HINSTANCE, LPWSTR cmdLine, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine && *cmdLine ? cmdLine : L"x", &argc);
    if (argv && argc >= 2) pp::RunPropertyWindow(argv[0], argv[1], 0);
    if (argv) LocalFree(argv);
}

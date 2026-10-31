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

// Load probe: if load_trace.log already exists next to the DLL, append the
// loading process to it. Kernel32 only, so it is safe under the loader lock;
// shows whether a host loads the DLL at all, before any COM call.
void LogLoadIfEnabled(HMODULE module) {
    wchar_t path[MAX_PATH];
    DWORD n = GetModuleFileNameW(module, path, MAX_PATH);
    while (n && path[n - 1] != L'\\') --n;
    if (!n || n + 16 >= MAX_PATH) return;
    lstrcpyW(path + n, L"load_trace.log");
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char line[MAX_PATH * 3 + 32];
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    int len = wsprintfA(line, "attach pid=%lu ", GetCurrentProcessId());
    len += WideCharToMultiByte(CP_UTF8, 0, exe, -1, line + len, static_cast<int>(sizeof(line)) - len - 3, nullptr,
                               nullptr) - 1;
    line[len++] = '\r';
    line[len++] = '\n';
    DWORD written = 0;
    WriteFile(f, line, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(f);
}

}  // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = module;
        DisableThreadLibraryCalls(module);
        LogLoadIfEnabled(module);
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

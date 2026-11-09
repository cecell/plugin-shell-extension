// DLL entry points: COM server exports, self-registration, and rundll32 entry
// points for testing without installing.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>

#include <string>

#include "context_menu.h"
#include "debug_log.h"
#include "hardlink_menu.h"
#include "asset_property_handler.h"
#include "bsa_folder.h"
#include "tool_menus.h"
#include "tool_windows.h"
#include "property_handler.h"
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
    pse::DebugLog(L"DllGetClassObject menu=%d props=%d", clsid == pse::kClsidContextMenu,
                 clsid == pse::kClsidPropertyHandler || clsid == pse::kClsidAssetPropertyHandler ||
                 clsid == pse::kClsidDdsPropertyHandler);
    if (clsid == pse::kClsidBsaFolder) return pse::CreateBsaFolderFactory(riid, ppv);
    if (clsid == pse::kClsidBsaManager) return pse::CreateBsaManagerFactory(riid, ppv);
    if (clsid == pse::kClsidHashMenu) return pse::CreateHashMenuFactory(riid, ppv);
    if (clsid == pse::kClsidContextMenu) return pse::CreateClassFactory(riid, ppv);
    if (clsid == pse::kClsidPropertyHandler) return pse::CreatePropertyHandlerFactory(riid, ppv);
    if (clsid == pse::kClsidAssetPropertyHandler) return pse::CreateAssetPropertyHandlerFactory(riid, ppv);
    if (clsid == pse::kClsidDdsPropertyHandler) return pse::CreateDdsPropertyHandlerFactory(riid, ppv);
    if (clsid == pse::kClsidHardlinkMenu) return pse::CreateHardlinkMenuFactory(riid, ppv);
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow() { return pse::g_dllRefs == 0 ? S_OK : S_FALSE; }

// regsvr32 PluginShellExtension.dll registers for the current user only.
STDAPI DllRegisterServer() { return pse::RegisterHandler(HKEY_CURRENT_USER, ModulePath()); }
STDAPI DllUnregisterServer() { return pse::UnregisterHandler(HKEY_CURRENT_USER); }

// regsvr32 /i:machine PluginShellExtension.dll (elevated) registers for all users.
// regsvr32 /n /i:columns PluginShellExtension.dll (elevated) registers the
// property handler and schema (Explorer columns, details pane, tooltip); add
// /u to remove them. The .propdesc must sit next to the DLL.
STDAPI DllInstall(BOOL install, PCWSTR cmdLine) {
    if (cmdLine && _wcsicmp(cmdLine, L"columns") == 0) {
        std::wstring dll = ModulePath();
        std::wstring schema = dll.substr(0, dll.find_last_of(L'\\') + 1) + pse::kSchemaFileName;
        return install ? pse::RegisterPropertyHandler(dll, schema) : pse::UnregisterPropertyHandler(schema);
    }
    // regsvr32 /n /i:hash PluginShellExtension.dll adds Hashes to every file, for
    // this user; add /u to remove it.
    if (cmdLine && _wcsicmp(cmdLine, L"hash") == 0) {
        if (install) {
            if (!pse::InstallHashMenu(HKEY_CURRENT_USER, L"Software\\Classes\\", ModulePath())) return E_FAIL;
        } else {
            pse::RemoveHashMenu(HKEY_CURRENT_USER, L"Software\\Classes\\");
        }
        return S_OK;
    }
    // regsvr32 /n /i:bsa PluginShellExtension.dll browses .bsa files as folders,
    // for this user; add /u to remove it.
    if (cmdLine && _wcsicmp(cmdLine, L"bsa") == 0) {
        if (install) {
            if (!pse::InstallBsaBrowser(HKEY_CURRENT_USER, L"Software\\Classes\\", L".bsa", ModulePath())) return E_FAIL;
        } else {
            pse::RemoveBsaBrowser(HKEY_CURRENT_USER, L"Software\\Classes\\", L".bsa");
        }
        SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
        return S_OK;
    }
    HKEY root = (cmdLine && _wcsicmp(cmdLine, L"machine") == 0) ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER;
    return install ? pse::RegisterHandler(root, ModulePath()) : pse::UnregisterHandler(root);
}

// rundll32 PluginShellExtension.dll,ShowProperties <plugin path>
extern "C" void CALLBACK ShowPropertiesW(HWND, HINSTANCE, LPWSTR cmdLine, int) {
    pse::RunPropertyWindow(Unquote(cmdLine ? cmdLine : L""), std::wstring(), 0);
}

// rundll32 PluginShellExtension.dll,Snapshot "<plugin path>" "<out.png>"
// Renders the window to a PNG once the GRUP scan finishes, then closes it.
extern "C" void CALLBACK SnapshotW(HWND, HINSTANCE, LPWSTR cmdLine, int) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(cmdLine && *cmdLine ? cmdLine : L"x", &argc);
    if (argv && argc >= 2) pse::RunPropertyWindow(argv[0], argv[1], 0);
    if (argv) LocalFree(argv);
}

// For tests: runs a window, saves a picture of it, and closes it.
extern "C" int __stdcall PseHashSnapshot(const wchar_t* path, const wchar_t* png, const wchar_t* script) { return pse::RunHashWindow(path, png, script); }
extern "C" int __stdcall PseReportSnapshot(const wchar_t* title, int status, const wchar_t* text, const wchar_t* png) { return pse::RunReportWindow(title, status, text, png); }

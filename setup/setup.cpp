// Installer / uninstaller for the Plugin Properties shell extension. Installs
// the PluginProperties.dll that sits next to this exe. (The DLL is not embedded
// as a resource: antivirus heuristics delete an unsigned exe that carries a
// DLL payload.)
//
//   PluginPropertiesSetup.exe                 interactive install / reinstall / uninstall
//   PluginPropertiesSetup.exe /install        install (current user)
//   PluginPropertiesSetup.exe /uninstall      uninstall
//   add /allusers for a machine-wide install (asks for elevation),
//   add /quiet to suppress all message boxes.
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>

#include <string>

#include "registration.h"

namespace {

constexpr wchar_t kAppName[] = L"Plugin Properties";
constexpr wchar_t kFolderName[] = L"Plugin Properties";
constexpr wchar_t kDllName[] = L"PluginProperties.dll";
constexpr wchar_t kSetupName[] = L"Setup.exe";
constexpr wchar_t kUninstallKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PluginProperties";

bool g_quiet = false;

int Message(const std::wstring& text, UINT type) {
    if (g_quiet) return IDYES;
    return MessageBoxW(nullptr, text.c_str(), kAppName, type | MB_SETFOREGROUND);
}

std::wstring ErrorText(DWORD err) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring s = msg ? msg : L"Unknown error";
    if (msg) LocalFree(msg);
    return s;
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring s;
    if (SUCCEEDED(SHGetKnownFolderPath(id, KF_FLAG_CREATE, nullptr, &p))) s = p;
    CoTaskMemFree(p);
    return s;
}

std::wstring SelfPath() {
    wchar_t buf[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, buf, ARRAYSIZE(buf));
    return std::wstring(buf, n);
}

std::wstring SelfDir() {
    std::wstring self = SelfPath();
    return self.substr(0, self.find_last_of(L'\\'));
}

struct Scope {
    bool allUsers;
    HKEY root() const { return allUsers ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER; }
    std::wstring dir() const {
        return KnownFolder(allUsers ? FOLDERID_ProgramFiles : FOLDERID_UserProgramFiles) + L"\\" + kFolderName;
    }
};

bool IsInstalled(HKEY root) {
    HKEY key = nullptr;
    if (RegOpenKeyExW(root, kUninstallKey, 0, KEY_READ, &key) != ERROR_SUCCESS) return false;
    RegCloseKey(key);
    return true;
}

bool IsElevated() {
    HANDLE token = nullptr;
    TOKEN_ELEVATION elev{};
    DWORD size = 0;
    bool elevated = false;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
        if (GetTokenInformation(token, TokenElevation, &elev, sizeof(elev), &size)) elevated = elev.TokenIsElevated != 0;
        CloseHandle(token);
    }
    return elevated;
}

// Re-runs this exe elevated with the given arguments and returns its exit code.
int RelaunchElevated(const std::wstring& args) {
    std::wstring self = SelfPath();
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self.c_str();
    sei.lpParameters = args.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return 1;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return static_cast<int>(code);
}

LSTATUS SetValue(HKEY key, const wchar_t* name, const std::wstring& value) {
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                          static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
}
LSTATUS SetValue(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value));
}

// Old copies a running Explorer still had loaded; removable once it let go.
void DeleteStaleFiles(const std::wstring& dir) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*.old").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// Deletes a file; a DLL loaded by Explorer cannot be deleted but can be
// renamed, so it is moved aside to be cleaned up by the next setup run.
bool RemoveOrMoveAside(const std::wstring& path) {
    if (DeleteFileW(path.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) return true;
    std::wstring aside = path + L"." + std::to_wstring(GetTickCount64()) + L".old";
    MoveFileExW(path.c_str(), aside.c_str(), MOVEFILE_REPLACE_EXISTING);
    return false;
}

// Copies source over target via a temporary file.
bool InstallFile(const std::wstring& source, const std::wstring& target, std::wstring* error) {
    if (_wcsicmp(source.c_str(), target.c_str()) == 0) return true;  // reinstalling from the install folder
    std::wstring tmp = target + L".new";
    if (!CopyFileW(source.c_str(), tmp.c_str(), FALSE)) {
        *error = L"Cannot copy " + source + L" to " + tmp + L":\n" + ErrorText(GetLastError());
        return false;
    }
    RemoveOrMoveAside(target);
    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        *error = L"Cannot replace " + target + L":\n" + ErrorText(GetLastError());
        DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

int Install(const Scope& scope) {
    std::wstring sourceDll = SelfDir() + L"\\" + kDllName;
    if (GetFileAttributesW(sourceDll.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Message(L"PluginProperties.dll was not found next to the setup program:\n" + sourceDll +
                    L"\n\nExtract the whole zip before running setup.",
                MB_ICONERROR);
        return 1;
    }
    std::wstring dir = scope.dir();
    if (dir.size() <= wcslen(kFolderName) + 1) {
        Message(L"Cannot find the Programs folder.", MB_ICONERROR);
        return 1;
    }
    int rc = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    if (rc != ERROR_SUCCESS && rc != ERROR_ALREADY_EXISTS) {
        Message(L"Cannot create " + dir + L":\n" + ErrorText(static_cast<DWORD>(rc)), MB_ICONERROR);
        return 1;
    }
    DeleteStaleFiles(dir);

    std::wstring dll = dir + L"\\" + kDllName;
    std::wstring setup = dir + L"\\" + kSetupName;
    std::wstring error;
    if (!InstallFile(sourceDll, dll, &error) || !InstallFile(SelfPath(), setup, &error)) {
        Message(error, MB_ICONERROR);
        return 1;
    }

    HRESULT hr = pp::RegisterHandler(scope.root(), dll);
    if (FAILED(hr)) {
        Message(L"Cannot register the shell extension:\n" + ErrorText(static_cast<DWORD>(hr)), MB_ICONERROR);
        return 1;
    }

    HKEY key = nullptr;
    if (RegCreateKeyExW(scope.root(), kUninstallKey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        std::wstring scopeArg = scope.allUsers ? L" /allusers" : L"";
        SetValue(key, L"DisplayName", kAppName);
        SetValue(key, L"DisplayVersion", L"" PP_VERSION_STR);
        SetValue(key, L"Publisher", L"Cecell");
        SetValue(key, L"DisplayIcon", setup);
        SetValue(key, L"InstallLocation", dir);
        SetValue(key, L"UninstallString", L"\"" + setup + L"\" /uninstall" + scopeArg);
        SetValue(key, L"QuietUninstallString", L"\"" + setup + L"\" /uninstall /quiet" + scopeArg);
        SetValue(key, L"NoModify", 1);
        SetValue(key, L"NoRepair", 1);
        WIN32_FILE_ATTRIBUTE_DATA fad{};
        DWORD kb = 0;
        if (GetFileAttributesExW(dll.c_str(), GetFileExInfoStandard, &fad)) kb += fad.nFileSizeLow / 1024;
        if (GetFileAttributesExW(setup.c_str(), GetFileExInfoStandard, &fad)) kb += fad.nFileSizeLow / 1024;
        SetValue(key, L"EstimatedSize", kb);
        RegCloseKey(key);
    }

    Message(L"Plugin Properties is installed.\n\n"
            L"Right-click a .esp, .esm or .esl file and choose \"Plugin properties\". "
            L"On Windows 11 it is under \"Show more options\" (or Shift+right-click).\n\n"
            L"If you installed over an older version, restart Explorer or sign out to load the new one.",
            MB_ICONINFORMATION);
    return 0;
}

// Runs a command line hidden, without waiting.
void RunDetached(std::wstring cmdLine) {
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmdLine[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

int Uninstall(const Scope& scope) {
    std::wstring dir = scope.dir();
    pp::UnregisterHandler(scope.root());
    RegDeleteTreeW(scope.root(), kUninstallKey);

    bool dllGone = RemoveOrMoveAside(dir + L"\\" + kDllName);
    DeleteStaleFiles(dir);
    std::wstring setup = dir + L"\\" + kSetupName;
    if (_wcsicmp(SelfPath().c_str(), setup.c_str()) == 0) {
        // A running exe cannot delete itself; a hidden cmd removes the folder
        // once this process has exited.
        std::wstring cmd = KnownFolder(FOLDERID_System) + L"\\cmd.exe";
        RunDetached(L"\"" + cmd + L"\" /c ping -n 3 127.0.0.1 >nul & rd /s /q \"" + dir + L"\"");
    } else {
        DeleteFileW(setup.c_str());
        RemoveDirectoryW(dir.c_str());
    }

    Message(dllGone ? std::wstring(L"Plugin Properties has been uninstalled.")
                    : L"Plugin Properties has been uninstalled.\n\n"
                      L"Explorer still has the extension loaded, so its DLL was left in\n" +
                          dir + L"\nYou can delete that folder after restarting Explorer or signing out.",
            MB_ICONINFORMATION);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    bool install = false, uninstall = false, allUsers = false;
    for (int i = 1; i < argc; ++i) {
        std::wstring a = argv[i];
        if (!a.empty() && (a[0] == L'-' || a[0] == L'/')) a = a.substr(1);
        if (_wcsicmp(a.c_str(), L"install") == 0) install = true;
        else if (_wcsicmp(a.c_str(), L"uninstall") == 0) uninstall = true;
        else if (_wcsicmp(a.c_str(), L"allusers") == 0) allUsers = true;
        else if (_wcsicmp(a.c_str(), L"quiet") == 0) g_quiet = true;
    }
    LocalFree(argv);

    if (!install && !uninstall) {
        bool user = IsInstalled(HKEY_CURRENT_USER);
        bool machine = IsInstalled(HKEY_LOCAL_MACHINE);
        if (!allUsers && machine && !user) allUsers = true;
        if (user || machine) {
            int answer = Message(L"Plugin Properties is already installed.\n\n"
                                 L"Yes:\treinstall / update\nNo:\tuninstall\nCancel:\tdo nothing",
                                 MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDYES) install = true;
            else if (answer == IDNO) uninstall = true;
            else return 0;
        } else {
            std::wstring who = allUsers ? L"all users" : L"the current user";
            if (Message(L"Install Plugin Properties for " + who + L"?\n\n"
                        L"This adds \"Plugin properties\" to the right-click menu of .esp, .esm and .esl files.",
                        MB_YESNO | MB_ICONQUESTION) != IDYES)
                return 0;
            install = true;
        }
    }

    if (allUsers && !IsElevated()) {
        std::wstring args = install ? L"/install /allusers" : L"/uninstall /allusers";
        if (g_quiet) args += L" /quiet";
        return RelaunchElevated(args);
    }

    Scope scope{allUsers};
    int rc = uninstall ? Uninstall(scope) : Install(scope);
    CoUninitialize();
    return rc;
}

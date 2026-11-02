// Opt-in trace for debugging inside Explorer: when the environment variable
// PSE_LOG, or the registry value HKCU\Software\PluginShellExtension\LogFile
// (works without restarting Explorer), names a file, lines are appended to it.
// Off otherwise.
#pragma once

#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace pse {

inline void DebugLog(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"PSE_LOG", path, MAX_PATH)) {
        DWORD size = sizeof(path);
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\PluginShellExtension", L"LogFile", RRF_RT_REG_SZ, nullptr, path,
                         &size) != ERROR_SUCCESS || !path[0])
            return;
    }
    wchar_t line[1024];
    va_list args;
    va_start(args, fmt);
    int n = _vsnwprintf_s(line, _TRUNCATE, fmt, args);
    va_end(args);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const wchar_t* exeName = wcsrchr(exe, L'\\') ? wcsrchr(exe, L'\\') + 1 : exe;
    wchar_t prefixed[1200];
    swprintf_s(prefixed, L"[%lu %s] %s", GetCurrentProcessId(), exeName, line);
    wcscpy_s(line, prefixed);
    n = static_cast<int>(wcslen(line));
    HANDLE f = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char utf8[4096];
    int len = WideCharToMultiByte(CP_UTF8, 0, line, n, utf8, sizeof(utf8) - 2, nullptr, nullptr);
    utf8[len++] = '\r';
    utf8[len++] = '\n';
    DWORD written = 0;
    WriteFile(f, utf8, static_cast<DWORD>(len), &written, nullptr);
    CloseHandle(f);
}

}  // namespace pse

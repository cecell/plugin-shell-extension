// Opt-in trace for debugging inside Explorer: when the environment variable
// PLUGINPROPS_LOG names a file, lines are appended to it. Off otherwise.
#pragma once

#include <windows.h>
#include <cstdarg>
#include <cstdio>

namespace pp {

inline void DebugLog(const wchar_t* fmt, ...) {
    wchar_t path[MAX_PATH];
    if (!GetEnvironmentVariableW(L"PLUGINPROPS_LOG", path, MAX_PATH)) return;
    wchar_t line[1024];
    va_list args;
    va_start(args, fmt);
    int n = _vsnwprintf_s(line, _TRUNCATE, fmt, args);
    va_end(args);
    if (n < 0) n = static_cast<int>(wcslen(line));
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

}  // namespace pp

// Restarts Windows Explorer after an update, so the new DLL is the one it
// loads. Restart Manager closes Explorer gracefully and starts it again with
// its folder windows. Only Explorer's own processes are touched: other programs
// that load the DLL (Everything's right-click menu, for one) keep their copy
// until they restart, which the install doesn't need.
#include "restart_explorer.h"

#include <restartmanager.h>

#include <string>
#include <vector>

#pragma comment(lib, "rstrtmgr.lib")

namespace pse {

HRESULT RestartExplorer() {
    wchar_t windows[MAX_PATH] = {};
    if (!GetWindowsDirectoryW(windows, MAX_PATH)) return HRESULT_FROM_WIN32(GetLastError());
    const std::wstring explorer = std::wstring(windows) + L"\\explorer.exe";

    // Which processes are Explorer: asking about explorer.exe also lists System
    // (critical, which would make a shutdown fail), so keep only RmExplorer.
    std::vector<RM_UNIQUE_PROCESS> shells;
    {
        DWORD session = 0;
        WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
        if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return E_FAIL;
        LPCWSTR files[] = {explorer.c_str()};
        UINT needed = 0, count = 0;
        DWORD reasons = 0;
        std::vector<RM_PROCESS_INFO> infos;
        if (RmRegisterResources(session, 1, files, 0, nullptr, 0, nullptr) == ERROR_SUCCESS) {
            DWORD r = RmGetList(session, &needed, &count, nullptr, &reasons);
            if (r == ERROR_MORE_DATA && needed) {
                infos.resize(needed);
                count = needed;
                r = RmGetList(session, &needed, &count, infos.data(), &reasons);
                infos.resize(r == ERROR_SUCCESS ? count : 0);
            }
        }
        RmEndSession(session);
        for (const auto& p : infos)
            if (p.ApplicationType == RmExplorer) shells.push_back(p.Process);
    }
    if (shells.empty()) return S_FALSE;  // Explorer isn't running: nothing to do

    DWORD session = 0;
    WCHAR key[CCH_RM_SESSION_KEY + 1] = {};
    if (RmStartSession(&session, 0, key) != ERROR_SUCCESS) return E_FAIL;
    DWORD r = RmRegisterResources(session, 0, nullptr, static_cast<UINT>(shells.size()), shells.data(), 0, nullptr);
    if (r == ERROR_SUCCESS) {
        r = RmShutdown(session, 0, nullptr);  // graceful: Explorer saves its windows
        // Restart even after a partial shutdown, so the desktop always comes back.
        const DWORD restarted = RmRestart(session, 0, nullptr);
        if (r == ERROR_SUCCESS) r = restarted;
    }
    RmEndSession(session);
    return r == ERROR_SUCCESS ? S_OK : HRESULT_FROM_WIN32(r);
}

}  // namespace pse

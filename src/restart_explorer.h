#pragma once

#include <windows.h>

namespace pse {

// Exits Windows Explorer (as "Exit Explorer" on the taskbar does), ends any
// Explorer process of this session still left after 5 s, starts explorer.exe
// again and waits for the taskbar. S_FALSE when there was no shell to restart,
// when running elevated, or when the taskbar didn't come back within 15 s.
// Used by setup after an update: regsvr32 /s /n /i:restartexplorer PluginShellExtension.dll
HRESULT RestartExplorer();

}  // namespace pse

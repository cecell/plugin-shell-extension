#pragma once

#include <windows.h>

namespace pse {

// Closes Windows Explorer and starts it again (Restart Manager, Explorer's
// processes only). S_FALSE when Explorer isn't running. Used by setup after an
// update: regsvr32 /s /n /i:restartexplorer PluginShellExtension.dll
HRESULT RestartExplorer();

}  // namespace pse

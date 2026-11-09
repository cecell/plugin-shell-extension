// The Plugin Shell Extension window: a copy of the Explorer Properties dialog's
// Details tab, filled from plugin_parser.
#pragma once

#include <windows.h>
#include <string>
#include <vector>

namespace pse {

// Opens one window per path, each on its own thread so the caller (Explorer)
// is never blocked. The threads keep the DLL loaded until they finish.
void ShowPropertyWindowsAsync(const std::vector<std::wstring>& paths);

// Shows the window on the calling thread and pumps messages until it closes.
// With snapshotPng set, the window is captured to that PNG once the GRUP scan
// finishes and then closed (used for testing). Returns 0 on success.
int RunPropertyWindow(const std::wstring& path, const std::wstring& snapshotPng, int cascadeIndex);

// Saves a picture of a window as a PNG, without its invisible resize borders.
bool SaveWindowPng(HWND hwnd, const std::wstring& png, int* width, int* height);

}  // namespace pse

// The BSA manager's actions and the hash dialog. Each opens on its own thread,
// so Explorer is never blocked, and keeps the DLL loaded until it finishes.
#pragma once

#include <windows.h>

#include "folder_views.h"

#include <string>
#include <vector>

namespace pse {

void ExtractBsasAsync(const std::vector<std::wstring>& paths, bool intoOwnFolder);
void TestBsasAsync(const std::vector<std::wstring>& paths);
void PapyrusAsync(const std::vector<std::wstring>& paths, bool compile);
// These ask or tell the user on their own thread, so Explorer isn't held up.
void SaveFolderViewAsync(FolderViewState state);
void RemoveFolderViewAsync(const std::wstring& name);
void ShowMessageAsync(const std::wstring& title, const std::wstring& text);
// Asks for a name. False when cancelled. A non-empty png saves a picture and closes.
bool RunFolderViewNameWindow(std::wstring& name, const std::wstring& png);
void ShowHashWindowAsync(const std::wstring& path);

// Runs the hash dialog on this thread. With png set, it plays `script` (steps
// like "copy:0" and "compare:abc", separated by |), saves a picture, and closes.
int RunHashWindow(const std::wstring& path, const std::wstring& png, const std::wstring& script);

// Runs the test report window on this thread.
int RunReportWindow(const std::wstring& title, int status, const std::wstring& text, const std::wstring& png);

}  // namespace pse

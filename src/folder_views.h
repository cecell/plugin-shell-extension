// Named Explorer view layouts: saved from one folder, applied to others.
#pragma once

#include <windows.h>

#include <shobjidl.h>

#include <functional>
#include <string>
#include <vector>

namespace pse {

struct FolderViewColumn {
    std::wstring key;  // a property key as text
    UINT width = 0;
};

struct FolderViewSort {
    std::wstring key;
    int direction = 1;  // 1 ascending, -1 descending
};

struct FolderViewState {
    std::wstring name;
    int mode = 0;  // FOLDERVIEWMODE
    int iconSize = 0;
    std::vector<FolderViewColumn> columns;
    std::vector<FolderViewSort> sort;
    std::wstring groupKey;  // empty: not grouped
    bool groupAscending = true;
};

// Saved layouts, sorted by name. The file is folder_views.json next to the
// extension's other settings in %LOCALAPPDATA%\Plugin Shell Extension.
std::wstring FolderViewsFilePath();
std::vector<FolderViewState> LoadFolderViews();
bool StoreFolderView(const FolderViewState& state);  // replaces one of the same name
bool RemoveFolderView(const std::wstring& name);

HRESULT CaptureFolderView(IShellView* view, FolderViewState& state);
HRESULT ApplyFolderView(IShellView* view, const FolderViewState& state);

// Applies `state` to every subfolder of `root` by stepping `browser` through
// them, so Windows records each one as it would if the user had set it, and
// then returns to `root`. It works from the calling thread's message loop.
// Going to another folder stops it. `report` gets a sentence to show the user
// when it stops early or has nothing to do; `done` runs last, on this thread.
struct FolderViewWalkCallbacks {
    std::function<bool(size_t count)> confirm;  // called on a worker thread
    std::function<void(const std::wstring&)> report;
    std::function<void()> done;
};
bool StartFolderViewWalk(IShellBrowser* browser, const std::wstring& root, const FolderViewState& state, FolderViewWalkCallbacks callbacks);

}  // namespace pse

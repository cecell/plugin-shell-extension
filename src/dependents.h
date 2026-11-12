// Which plugins in a folder list a given plugin as a master. Each folder's
// plugin headers are read once and cached; on later asks only plugins whose
// size or write time changed are read again.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace pse {

struct Dependent {
    std::wstring name;  // as spelled on disk
    bool active = false;
};

struct DependentList {
    bool ok = false;           // the folder could be listed
    bool gameData = false;     // the folder is the game's Data folder, so "active" comes from plugins.txt
    std::vector<Dependent> plugins;  // sorted by name
};

// The plugins in `pluginPath`'s folder that have it as a master.
DependentList DependentsOf(const std::wstring& pluginPath);

// Just the count, for the #Dependents column.
size_t DependentCount(const std::wstring& pluginPath);

// A plugin's masters against its folder and the load order.
struct MasterState {
    bool gameData = false;              // the folder is the game's Data folder: "inactive" applies
    std::vector<std::wstring> missing;  // not in the plugin's folder
    std::vector<std::wstring> inactive; // present, but not active in plugins.txt (game Data only)
};
MasterState MasterStateOf(const std::wstring& pluginPath, const std::vector<std::wstring>& masters);

// The MasterStatus column: "Missing" (some master isn't in the folder),
// "Not active" (all present, not all active), "All active"; empty for a plugin
// with no masters, and outside the game's Data folder unless one is missing.
std::wstring MasterStatusText(const MasterState& state, size_t masterCount);

}  // namespace pse

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

}  // namespace pse

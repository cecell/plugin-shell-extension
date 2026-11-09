// Edits to a plugin file made from the right-click menu.
#pragma once

#include <string>

namespace pse {

struct HeaderUpgradeResult {
    int status = 3;  // 0 upgraded, 1 already 1.71, 2 not a 1.70 plugin, 3 failed
    std::wstring text;
};

// Changes a plugin's header version from 1.70 to 1.71. That is only the four
// bytes of the version number at file offset 30: nothing else is touched, and
// the file is edited in place so hardlinks to it stay linked.
HeaderUpgradeResult UpgradePluginHeader(const std::wstring& path);

}  // namespace pse

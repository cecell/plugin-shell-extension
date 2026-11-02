// Results of the slow columns (the record scan, the ITM check) saved across
// Explorer restarts and reboots, in %LOCALAPPDATA%\Plugin Shell Extension\cache.txt.
//
// One line per result: "<key>\t<value>". Keys name the file and its size and
// write time (and, for ITM, every master's), so a changed file simply misses.
// Lines are appended, so several processes can add results at once; the file
// is rewritten without outdated lines when it is mostly outdated. A version
// line at the top discards the whole cache when the extension version changes.
#pragma once

#include <string>

namespace pse {

// Looks up a value; false when there is none.
bool DiskCacheGet(const std::wstring& key, std::wstring* value);

// Stores a value (in memory and appended to the file). Keys must not contain
// tabs, and neither may contain line breaks.
void DiskCachePut(const std::wstring& key, const std::wstring& value);

}  // namespace pse

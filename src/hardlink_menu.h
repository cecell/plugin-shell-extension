// "Hardlink" context-menu entry for every file: shows whether the file is a
// hardlink and, for each of its other links, copies the path or opens the
// folder (in this window, a new tab or a new window). Its own COM class, so
// the installer can offer it as a separate, optional task.
#pragma once

#include <windows.h>

#include <string>
#include <vector>

namespace pse {

// {9FB91679-FC8C-4040-9C35-61E735E81908}
inline constexpr GUID kClsidHardlinkMenu = {0x9fb91679, 0xfc8c, 0x4040, {0x9c, 0x35, 0x61, 0xe7, 0x35, 0xe8, 0x19, 0x08}};
inline constexpr wchar_t kHardlinkClsidString[] = L"{9FB91679-FC8C-4040-9C35-61E735E81908}";

HRESULT CreateHardlinkMenuFactory(REFIID riid, void** ppv);

// The file's other hardlinks (full paths), excluding `path` itself; empty
// when it has a single link or cannot be read. *linkCount gets the link
// count NTFS reports (0 when unknown).
std::vector<std::wstring> OtherHardlinks(const std::wstring& path, DWORD* linkCount);

}  // namespace pse

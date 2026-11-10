// The facts behind the mesh, script, archive, animation and lip-sync columns.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pse {

// A pid from property_handler.h, or kFactDuration for Windows' own Length.
constexpr DWORD kFactDuration = 0xFFFFFFFF;

struct AssetFact {
    enum class Type { Text, UInt32, UInt64, Date };  // Date: a FILETIME as a number
    DWORD pid = 0;
    Type type = Type::Text;
    std::wstring text;
    uint64_t number = 0;
};

// True for the file types these columns cover.
bool HasAssetFacts(const std::wstring& path);
// Every column this file type has, and which of them need the whole file read.
std::vector<AssetFact> AssetFactColumns(const std::wstring& path);

// Reads only the start of the file or looks at its neighbours.
std::vector<AssetFact> ReadQuickAssetFacts(const std::wstring& path);
// Reads the whole file. A damaged file gives no facts, never a crash.
std::vector<AssetFact> ReadFullAssetFacts(const std::wstring& path);

std::wstring EncodeAssetFacts(const std::vector<AssetFact>& facts);
bool DecodeAssetFacts(const std::wstring& text, std::vector<AssetFact>* facts);

}  // namespace pse

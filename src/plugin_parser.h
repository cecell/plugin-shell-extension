// Reads a Skyrim SE plugin (.esp/.esm/.esl) header and top-level GRUPs from raw
// bytes. Rules follow PLUGIN_HEADER_EXTRACTION.md (TES4 header, HEDR, CNAM,
// SNAM, MAST, top-level GRUP walk).
#pragma once

#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

namespace pp {

constexpr uint32_t kFlagEsm = 0x00000001;
constexpr uint32_t kFlagLocalized = 0x00000080;
constexpr uint32_t kFlagEsl = 0x00000200;

struct PluginHeader {
    bool ok = false;           // TES4 record was read
    std::wstring error;        // set when !ok, or a warning when ok

    bool hasHedr = false;
    float version = 0.0f;      // HEDR float32
    int32_t numRecords = 0;    // HEDR int32 (records + GRUPs)

    uint32_t flags = 0;        // TES4 record flags
    bool light = false;        // .esl extension or ESL bit

    bool hasAuthor = false;
    std::wstring author;       // CNAM
    bool hasDescription = false;
    std::wstring description;  // SNAM
    std::vector<std::wstring> masters;  // MAST, file order

    uint64_t firstGroupOffset = 0;  // 24 + data_size
};

struct GroupCount {
    std::wstring label;   // 4-letter record type of the top-level GRUP
    uint64_t records = 0; // records inside it at every depth (GRUPs not counted)
};

struct GroupScan {
    bool ok = false;
    std::wstring error;
    std::vector<GroupCount> groups;  // merged by label, sorted alphabetically
    uint64_t totalRecords = 0;
    uint64_t totalGroups = 0;        // every GRUP at every depth
    // ESL-capable (Vortex's "Could be light"): every new record's object ID
    // fits the light range for the HEDR version. Settled when ok, or when false
    // (a violation found before an error is still a violation).
    bool eslCapable = true;
    // Deleted placed objects and navmeshes (REFR, ACHR, P***, NAVM with the
    // Deleted flag): nonzero types only, alphabetical.
    std::vector<GroupCount> deleted;
};

PluginHeader ReadPluginHeader(const std::wstring& path);
// Walks every GRUP after the TES4 record; header supplies the start offset and
// the master count / HEDR version for the ESL check.
GroupScan ScanGroups(const std::wstring& path, const PluginHeader& header);

// "ESM, ESL, Localized" in that order; empty when no bit is set.
std::wstring FlagsText(uint32_t flags);
// HEDR version rounded to 2 decimals, e.g. "1.71".
std::wstring VersionText(float version);

// Decodes a TES4 zstring payload: trailing NULs stripped, UTF-8 if valid,
// otherwise Windows-1252.
std::wstring DecodeZString(const uint8_t* data, size_t size);

}  // namespace pp

// Reads a Skyrim SE plugin (.esp/.esm/.esl) header and top-level GRUPs from raw
// bytes. Rules follow PLUGIN_HEADER_EXTRACTION.md (TES4 header, HEDR, CNAM,
// SNAM, MAST, top-level GRUP walk).
#pragma once

#include <windows.h>
#include <objidl.h>
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
    // Records new to this plugin (FormID top byte >= master count) and whether
    // all their object IDs fit each light range. Feed ClassifyEsl.
    uint64_t newRecords = 0;
    bool newIdsIn800 = true;  // 0x800..0xFFF
    bool newIdsIn000 = true;  // 0x000..0xFFF
    // Deleted placed objects and navmeshes (REFR, ACHR, P***, NAVM with the
    // Deleted flag): nonzero types only, alphabetical.
    std::vector<GroupCount> deleted;
    uint64_t totalDeleted = 0;
};

PluginHeader ReadPluginHeader(const std::wstring& path);
// Same from a stream (Explorer's property handler); fileName is only used for
// the .esl extension check.
PluginHeader ReadPluginHeader(IStream* stream, const std::wstring& fileName);
// Walks every GRUP after the TES4 record; header supplies the start offset and
// the master count / HEDR version for the ESL check.
GroupScan ScanGroups(const std::wstring& path, const PluginHeader& header);
GroupScan ScanGroups(IStream* stream, const PluginHeader& header);

// What it takes to make the plugin light (ESLable):
//   Already  - already loads as light (ESL flag or .esl)
//   True     - can be flagged as is: new records and their object IDs fit the
//              range for its header (< 1.71: 2,048 in 0x800-0xFFF; 1.71+:
//              4,096 in 0x000-0xFFF)
//   Compact  - few enough new records, but some IDs are outside that range
//   SaveInCk - header < 1.71 with 2,049-4,096 new records whose IDs all fit
//              0x000-0xFFF: resaving in the newer CK (header 1.71) makes it fit
//   False    - anything else
//   Unknown  - the record walk failed
enum class EslStatus { Already, True, Compact, SaveInCk, False, Unknown };
EslStatus ClassifyEsl(const PluginHeader& header, const GroupScan& scan);
// shortForm: "Already" (Explorer column) instead of "Already flagged".
const wchar_t* EslStatusText(EslStatus status, bool shortForm);

// "ESM, ESL, Localized" in that order; empty when no bit is set.
std::wstring FlagsText(uint32_t flags);
// HEDR version rounded to 2 decimals, e.g. "1.71".
std::wstring VersionText(float version);

// Decodes a TES4 zstring payload: trailing NULs stripped, UTF-8 if valid,
// otherwise Windows-1252.
std::wstring DecodeZString(const uint8_t* data, size_t size);

}  // namespace pp

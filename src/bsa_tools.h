// The work behind the BSA manager menu and the hash dialog, with no UI, so it
// can be tested on its own. All of it runs inside a WorkSlot.
#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace pse {

// Returns false to stop early.
using Progress = std::function<bool(uint64_t done, uint64_t total)>;

struct BsaKindCount {
    std::wstring label;
    uint64_t count = 0;
};

struct BsaFacts {
    bool ok = false;
    std::wstring error;
    uint32_t version = 0, archiveFlags = 0, fileFlags = 0, folders = 0, files = 0;
    uint64_t compressedFiles = 0, unpackedBytes = 0, archiveBytes = 0, largestBytes = 0;
    std::wstring largestName;
    uint64_t skippedNames = 0;  // entries whose names can't be used safely
    std::vector<BsaKindCount> kinds;
};
BsaFacts ReadBsaFacts(const std::wstring& path, const std::atomic<bool>* cancel = nullptr);

const wchar_t* BsaGameName(uint32_t version);
const wchar_t* BsaCompressionName(const BsaFacts& facts);
std::wstring BsaFlagsText(uint32_t archiveFlags);
std::wstring BsaContentText(uint32_t fileFlags);

struct ExtractResult {
    bool opened = false;
    uint64_t written = 0, skippedExisting = 0, skippedNames = 0, failed = 0;
    bool cancelled = false;
    std::wstring firstError;
};
enum class ExistingFiles { Replace, Skip };

// How many of the archive's files already exist under destination.
uint64_t CountExisting(const std::wstring& path, const std::wstring& destination);
ExtractResult ExtractBsa(const std::wstring& path, const std::wstring& destination, ExistingFiles existing, const Progress& progress);

struct BsaTestReport {
    int status = 3;  // 0 passed, 1 warnings, 2 errors, 3 couldn't be opened
    std::wstring text;
    bool cancelled = false;
};
// Checks every table and hash, decodes every file, and reports duplicates.
BsaTestReport TestBsa(const std::wstring& path, const Progress& progress);

struct FileHashes {
    bool ok = false;
    bool cancelled = false;
    std::wstring error, sha256, crc32, md5;
};
FileHashes ComputeHashes(const std::wstring& path, const std::atomic<bool>* cancel, const Progress& progress);

}  // namespace pse

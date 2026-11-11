// Which copy of an asset the game loads from its Data folder: a loose file in
// Data wins outright; otherwise the copy in the loaded archive with the highest
// rank. Archives load in this order: the ini archive lists (Skyrim.ini /
// SkyrimCustom.ini [Archive] sResourceArchiveList, then sResourceArchiveList2,
// or the vanilla lists), then for each plugin in load order "<plugin>.bsa" and
// "<plugin> - Textures.bsa". The load order is the five base masters,
// Skyrim.ccc, then plugins.txt's active lines, with masters (ESM flag) first.
//
// These are skqe's rules (skyrim-query-engine: PlanArchives and LoadOrder, which
// `skqe build-assets` and the Papyrus tools' --out patcher use), rewritten here
// so WHAM needs neither skqe nor a database.
#pragma once

#include <windows.h>

#include <memory>
#include <string>
#include <vector>

namespace pse {

struct WinnerResult {
    enum Kind {
        Unknown,  // the winner can't be established (see DataWinners::Why)
        None,     // no loose copy and no loaded archive has it: the game doesn't see it
        Loose,    // `path` is the loose file in Data
        Archive,  // `path` is the winning .bsa
    };
    Kind kind = Unknown;
    std::wstring path;
};

class DataWinners {
public:
    // The plan for the game's Data folder that holds `bsaPath` directly (Data\x.bsa,
    // with SkyrimSE.exe next to Data), or nullptr when it is anywhere else. Cached;
    // rebuilt when the load order, the inis or the Data folder's top level change.
    static std::shared_ptr<const DataWinners> ForArchive(const std::wstring& bsaPath);

    // The Data folder, ending in a backslash.
    const std::wstring& Data() const { return data_; }
    // Empty when winners can be established; otherwise why not.
    const std::wstring& Why() const { return why_; }
    // Whether the game loads this archive at all.
    bool Loaded(const std::wstring& bsaPath) const;

    // One result per path. Paths are relative to Data, e.g. L"textures\\sky\\stars.dds".
    void Resolve(const std::vector<std::wstring>& relativePaths, std::vector<WinnerResult>* out) const;

    struct LoadedArchive {
        std::wstring path;  // full path
        std::wstring lowerName;
        int rank = 0;       // 0 loads first; higher wins
        bool indexed = false;
    };
    const std::vector<LoadedArchive>& Archives() const { return loaded_; }
    const std::vector<std::wstring>& LoadOrder() const { return order_; }
    double BuildSeconds() const { return buildSeconds_; }

    DataWinners() = default;

private:
    friend struct WinnerBuilder;
    std::wstring data_, why_;
    std::vector<std::wstring> order_;
    std::vector<LoadedArchive> loaded_;
    // (folder hash, rank) for every folder of every loaded archive, sorted.
    std::vector<std::pair<uint64_t, int>> folders_;
    std::vector<int> unindexed_;  // ranks of archives without folder names: always searched
    double buildSeconds_ = 0;
};

// True when `path` (a file or folder) is directly inside a game Data folder.
bool IsInGameData(const std::wstring& path);

}  // namespace pse

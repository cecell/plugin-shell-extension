// winnertest <data\x.bsa> [limit]
// Prints the archive plan for the Data folder holding x.bsa, then which copy wins
// for each file in x.bsa (up to `limit`, default all): Yes / No and where.
#include <windows.h>

#include <bsa/reader.hpp>

#include <fcntl.h>
#include <io.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include "winner.h"

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr, L"usage: winnertest <data\\x.bsa> [limit]\n");
        return 2;
    }
    if (argc == 3 && !wcscmp(argv[1], L"--all")) {
        // Every file of every loaded archive: <archive name>\t<path>\tYes|No
        const std::wstring any = std::filesystem::absolute(argv[2]).wstring();
        auto plan = pse::DataWinners::ForArchive(any);
        if (!plan) return 1;
        _setmode(_fileno(stdout), 0x20000);  // _O_U8TEXT
        for (const auto& a : plan->Archives()) {
            bsa::Reader r;
            if (r.open(std::filesystem::path(a.path)) != bsa::Status::Ok) continue;
            std::vector<std::wstring> paths;
            for (uint32_t i = 0; i < r.header().fileCount; ++i) {
                bsa::Entry e;
                std::string_view folder, name;
                if (r.entry(i, e) != bsa::Status::Ok || r.folder_name(e.folderIndex, folder) != bsa::Status::Ok || r.file_name(e, name) != bsa::Status::Ok) continue;
                std::string p = std::string(folder) + "\\" + std::string(name);
                const int n = MultiByteToWideChar(1252, 0, p.data(), static_cast<int>(p.size()), nullptr, 0);
                std::wstring w(static_cast<size_t>(n), L'\0');
                MultiByteToWideChar(1252, 0, p.data(), static_cast<int>(p.size()), w.data(), n);
                paths.push_back(w);
            }
            r.close();
            std::vector<pse::WinnerResult> res;
            plan->Resolve(paths, &res);
            const std::wstring name = a.path.substr(a.path.find_last_of(L'\\') + 1);
            for (size_t i = 0; i < paths.size(); ++i) {
                const bool yes = res[i].kind == pse::WinnerResult::Archive && _wcsicmp(res[i].path.c_str(), a.path.c_str()) == 0;
                wprintf(L"%s\t%s\t%s\n", name.c_str(), paths[i].c_str(), yes ? L"Yes" : L"No");
            }
        }
        return 0;
    }
    const std::wstring bsaPath = std::filesystem::absolute(argv[1]).wstring();
    const size_t limit = argc > 2 ? static_cast<size_t>(_wtoi64(argv[2])) : SIZE_MAX;
    auto t0 = std::chrono::steady_clock::now();
    auto plan = pse::DataWinners::ForArchive(bsaPath);
    if (!plan) {
        wprintf(L"not in a game Data folder\n");
        return 1;
    }
    wprintf(L"data %ls\nplugins %zu, loaded archives %zu, build %.3f s\n", plan->Data().c_str(), plan->LoadOrder().size(), plan->Archives().size(), plan->BuildSeconds());
    if (!plan->Why().empty()) wprintf(L"unknown: %ls\n", plan->Why().c_str());
    wprintf(L"this archive loaded: %ls\n", plan->Loaded(bsaPath) ? L"yes" : L"no");
    auto t1 = std::chrono::steady_clock::now();
    auto again = pse::DataWinners::ForArchive(bsaPath);
    wprintf(L"cached lookup %.3f ms (same plan %ls)\n", std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count(),
            again == plan ? L"yes" : L"no");

    bsa::Reader r;
    if (r.open(std::filesystem::path(bsaPath)) != bsa::Status::Ok) return 1;
    std::vector<std::wstring> paths;
    for (uint32_t i = 0; i < r.header().fileCount && paths.size() < limit; ++i) {
        bsa::Entry e;
        std::string_view folder, name;
        if (r.entry(i, e) != bsa::Status::Ok || r.folder_name(e.folderIndex, folder) != bsa::Status::Ok || r.file_name(e, name) != bsa::Status::Ok) continue;
        std::string p = std::string(folder) + "\\" + std::string(name);
        const int n = MultiByteToWideChar(1252, 0, p.data(), static_cast<int>(p.size()), nullptr, 0);
        std::wstring w(static_cast<size_t>(n), L'\0');
        MultiByteToWideChar(1252, 0, p.data(), static_cast<int>(p.size()), w.data(), n);
        paths.push_back(w);
    }
    r.close();
    std::vector<pse::WinnerResult> res;
    auto t2 = std::chrono::steady_clock::now();
    plan->Resolve(paths, &res);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t2).count();
    size_t yes = 0, loose = 0, other = 0, none = 0;
    for (size_t i = 0; i < paths.size(); ++i) {
        const auto& x = res[i];
        const bool self = x.kind == pse::WinnerResult::Archive && _wcsicmp(x.path.c_str(), bsaPath.c_str()) == 0;
        const wchar_t* kind = x.kind == pse::WinnerResult::Loose ? L"loose" : x.kind == pse::WinnerResult::Archive ? L"archive" : x.kind == pse::WinnerResult::None ? L"none" : L"unknown";
        if (self) ++yes;
        else if (x.kind == pse::WinnerResult::Loose) ++loose;
        else if (x.kind == pse::WinnerResult::Archive) ++other;
        else ++none;
        if (!self) wprintf(L"No\t%ls\t%ls\t%ls\n", paths[i].c_str(), kind, x.path.c_str());
    }
    wprintf(L"files %zu: Yes %zu, No %zu (loose %zu, other archive %zu, none %zu); resolve %.1f ms; total %.3f s\n", paths.size(), yes,
            paths.size() - yes, loose, other, none, ms, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
    return 0;
}

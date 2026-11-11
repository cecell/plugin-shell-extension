#include "dependents.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <unordered_map>

#include "plugin_parser.h"
#include "winner.h"

namespace pse {
namespace {

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

bool IsPluginName(const std::wstring& n) {
    const std::wstring l = Lower(n);
    return l.size() > 4 && (l.compare(l.size() - 4, 4, L".esm") == 0 || l.compare(l.size() - 4, 4, L".esp") == 0 || l.compare(l.size() - 4, 4, L".esl") == 0);
}

struct Entry {
    std::wstring name;
    uint64_t size = 0, time = 0;
    std::vector<std::wstring> masters;  // lowercase
};

struct FolderIndex {
    std::mutex lock;
    ULONGLONG listedAt = 0;  // GetTickCount64 of the last listing
    uint64_t dirTime = 0;
    bool ok = false;
    std::unordered_map<std::wstring, Entry> plugins;                    // lowercase name -> entry
    std::unordered_map<std::wstring, std::vector<std::wstring>> users;  // lowercase master -> lowercase dependents
};

uint64_t DirTime(const std::wstring& dir) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(dir.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) | fa.ftLastWriteTime.dwLowDateTime;
}

// Lists the folder again unless it was listed a moment ago and hasn't changed
// since (Explorer asks once per plugin, thousands of times for a Data folder).
void Refresh(FolderIndex& f, const std::wstring& dir) {
    const ULONGLONG now = GetTickCount64();
    const uint64_t dirTime = DirTime(dir);
    if (f.listedAt && now - f.listedAt < 3000 && dirTime == f.dirTime) return;
    std::set<std::wstring> seen;
    bool changed = false;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileExW((dir + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    f.ok = h != INVALID_HANDLE_VALUE;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            const std::wstring name = fd.cFileName;
            if (!IsPluginName(name)) continue;
            const std::wstring key = Lower(name);
            seen.insert(key);
            const uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            const uint64_t time = (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
            Entry& e = f.plugins[key];
            if (!e.name.empty() && e.size == size && e.time == time) continue;
            e.name = name;
            e.size = size;
            e.time = time;
            e.masters.clear();
            const PluginHeader header = ReadPluginHeader(dir + name);  // the header only
            for (const auto& m : header.masters) e.masters.push_back(Lower(m));
            std::sort(e.masters.begin(), e.masters.end());
            e.masters.erase(std::unique(e.masters.begin(), e.masters.end()), e.masters.end());
            changed = true;
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    for (auto it = f.plugins.begin(); it != f.plugins.end();) {
        if (seen.count(it->first)) {
            ++it;
        } else {
            it = f.plugins.erase(it);
            changed = true;
        }
    }
    if (changed) {
        f.users.clear();
        for (const auto& [key, e] : f.plugins)
            for (const auto& m : e.masters) f.users[m].push_back(key);
    }
    f.listedAt = GetTickCount64();
    f.dirTime = dirTime;
}

std::shared_ptr<FolderIndex> IndexFor(const std::wstring& dir) {
    static std::mutex lock;
    static std::map<std::wstring, std::shared_ptr<FolderIndex>> folders;
    std::lock_guard<std::mutex> g(lock);
    auto& f = folders[Lower(dir)];
    if (!f) f = std::make_shared<FolderIndex>();
    return f;
}

}  // namespace

DependentList DependentsOf(const std::wstring& pluginPath) {
    DependentList out;
    const size_t slash = pluginPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return out;
    const std::wstring dir = pluginPath.substr(0, slash + 1);
    const std::wstring self = Lower(pluginPath.substr(slash + 1));
    auto f = IndexFor(dir);
    {
        std::lock_guard<std::mutex> g(f->lock);
        Refresh(*f, dir);
        out.ok = f->ok;
        auto it = f->users.find(self);
        if (it != f->users.end())
            for (const auto& key : it->second) out.plugins.push_back({f->plugins[key].name, false});
    }
    std::set<std::wstring> active;
    out.gameData = ActivePluginNames(dir, &active);
    for (auto& d : out.plugins) d.active = active.count(Lower(d.name)) > 0;
    std::sort(out.plugins.begin(), out.plugins.end(), [](const Dependent& a, const Dependent& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return out;
}

size_t DependentCount(const std::wstring& pluginPath) {
    const size_t slash = pluginPath.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return 0;
    const std::wstring dir = pluginPath.substr(0, slash + 1);
    auto f = IndexFor(dir);
    std::lock_guard<std::mutex> g(f->lock);
    Refresh(*f, dir);
    auto it = f->users.find(Lower(pluginPath.substr(slash + 1)));
    return it == f->users.end() ? 0 : it->second.size();
}

}  // namespace pse

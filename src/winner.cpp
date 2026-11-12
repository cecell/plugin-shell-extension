#include "winner.h"

#include <knownfolders.h>
#include <shlobj.h>

#include <bsa/reader.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <map>
#include <mutex>
#include <set>
#include <unordered_map>

#include "work_budget.h"

namespace pse {
namespace {

std::wstring LowerW(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

std::string Narrow(const std::wstring& w, UINT codepage) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(codepage, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(codepage, 0, w.c_str(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring WidenUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()), nullptr, 0);
    UINT cp = CP_UTF8;
    if (n <= 0) {
        cp = 1252;
        n = MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    }
    std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(cp, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

bool IsAscii(const std::wstring& s) {
    for (wchar_t c : s)
        if (c > 0x7F) return false;
    return true;
}

bool FileExists(const std::wstring& p) {
    const DWORD a = GetFileAttributesW(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR p = nullptr;
    std::wstring out;
    if (SHGetKnownFolderPath(id, 0, nullptr, &p) == S_OK) out = p;
    CoTaskMemFree(p);
    return out;
}

// Size and write time, as text; empty when the file is absent.
std::wstring Stamp(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return L"-";
    return std::to_wstring(fa.nFileSizeLow) + L"." + std::to_wstring(fa.ftLastWriteTime.dwLowDateTime) + L"." +
           std::to_wstring(fa.ftLastWriteTime.dwHighDateTime);
}

std::vector<std::string> ReadLines(const std::wstring& path) {
    std::vector<std::string> lines;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return lines;
    LARGE_INTEGER size{};
    std::string text;
    if (GetFileSizeEx(h, &size) && size.QuadPart < (16 << 20)) {
        text.resize(static_cast<size_t>(size.QuadPart));
        DWORD got = 0;
        if (!text.empty() && !ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &got, nullptr)) got = 0;
        text.resize(got);
    }
    CloseHandle(h);
    if (text.rfind("\xEF\xBB\xBF", 0) == 0) text.erase(0, 3);
    size_t pos = 0;
    while (pos < text.size()) {
        size_t nl = text.find('\n', pos);
        if (nl == std::string::npos) nl = text.size();
        std::string line = text.substr(pos, nl - pos);
        pos = nl + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.pop_back();
        lines.push_back(std::move(line));
    }
    return lines;
}

std::string LowerA(std::string s) {
    for (char& c : s)
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return s;
}

// key's value in [section]; the last occurrence wins.
bool IniValue(const std::vector<std::string>& lines, const char* section, const char* key, std::string* value) {
    std::string current;
    bool found = false;
    for (std::string line : lines) {
        const size_t a = line.find_first_not_of(" \t");
        if (a == std::string::npos || line[a] == ';' || line[a] == '#') continue;
        line = line.substr(a);
        if (line[0] == '[') {
            const size_t close = line.find(']');
            current = LowerA(line.substr(1, close == std::string::npos ? std::string::npos : close - 1));
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos || current != LowerA(section)) continue;
        std::string k = line.substr(0, eq);
        while (!k.empty() && (k.back() == ' ' || k.back() == '\t')) k.pop_back();
        if (LowerA(k) != LowerA(key)) continue;
        const size_t b = line.find_first_not_of(" \t", eq + 1);
        *value = b == std::string::npos ? std::string() : line.substr(b);
        found = true;
    }
    return found;
}

std::vector<std::string> SplitList(const std::string& list) {
    std::vector<std::string> out;
    size_t pos = 0;
    while (pos <= list.size()) {
        size_t comma = list.find(',', pos);
        if (comma == std::string::npos) comma = list.size();
        std::string name = list.substr(pos, comma - pos);
        pos = comma + 1;
        const size_t a = name.find_first_not_of(" \t");
        if (a == std::string::npos) continue;
        const size_t b = name.find_last_not_of(" \t");
        out.push_back(name.substr(a, b - a + 1));
    }
    return out;
}

// The vanilla lists, used when no ini sets them (Skyrim_Default.ini's).
const char kDefaultList1[] =
    "Skyrim - Misc.bsa, Skyrim - Shaders.bsa, Skyrim - Interface.bsa, Skyrim - Animations.bsa, Skyrim - Meshes0.bsa, "
    "Skyrim - Meshes1.bsa, Skyrim - Sounds.bsa";
const char kDefaultList2[] =
    "Skyrim - Voices_en0.bsa, Skyrim - Textures0.bsa, Skyrim - Textures1.bsa, Skyrim - Textures2.bsa, Skyrim - "
    "Textures3.bsa, Skyrim - Textures4.bsa, Skyrim - Textures5.bsa, Skyrim - Textures6.bsa, Skyrim - Textures7.bsa, "
    "Skyrim - Textures8.bsa, Skyrim - Patch.bsa";

// Where this install keeps plugins.txt and its inis: Steam's folder name, or
// GOG's when the install has no Steam API and only GOG's folder exists.
std::wstring GameFolderName(const std::wstring& gameDir) {
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    if (!FileExists(gameDir + L"steam_api64.dll") && !local.empty() &&
        FileExists(local + L"\\Skyrim Special Edition GOG\\plugins.txt") && !FileExists(local + L"\\Skyrim Special Edition\\plugins.txt"))
        return L"Skyrim Special Edition GOG";
    return L"Skyrim Special Edition";
}

std::vector<std::wstring> IniFiles(const std::wstring& folderName) {
    const std::wstring docs = KnownFolder(FOLDERID_Documents);
    if (docs.empty()) return {};
    const std::wstring dir = docs + L"\\My Games\\" + folderName + L"\\";
    return {dir + L"Skyrim.ini", dir + L"SkyrimCustom.ini"};
}

std::wstring PluginsTxt(const std::wstring& folderName) {
    const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
    return local.empty() ? std::wstring() : local + L"\\" + folderName + L"\\plugins.txt";
}

bool IsPluginName(const std::wstring& n) {
    const std::wstring l = LowerW(n);
    return l.size() > 4 && (l.compare(l.size() - 4, 4, L".esm") == 0 || l.compare(l.size() - 4, 4, L".esp") == 0 || l.compare(l.size() - 4, 4, L".esl") == 0);
}

// ESM flag from the header, or the .esm / .esl extension.
bool IsMaster(const std::wstring& path) {
    const std::wstring l = LowerW(path);
    if (l.size() > 4 && (l.compare(l.size() - 4, 4, L".esm") == 0 || l.compare(l.size() - 4, 4, L".esl") == 0)) return true;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    uint8_t hdr[12] = {};
    DWORD got = 0;
    const bool ok = ReadFile(h, hdr, sizeof hdr, &got, nullptr) && got == sizeof hdr && memcmp(hdr, "TES4", 4) == 0;
    CloseHandle(h);
    return ok && (hdr[8] & 1);
}

struct Fingerprint {
    std::wstring text;
};

Fingerprint MakeFingerprint(const std::wstring& data, const std::wstring& folderName) {
    Fingerprint f;
    f.text = Stamp(data.substr(0, data.size() - 1)) + L"|" + Stamp(data + L"..\\Skyrim.ccc") + L"|" + Stamp(PluginsTxt(folderName));
    for (const auto& ini : IniFiles(folderName)) f.text += L"|" + Stamp(ini);
    // The Data folder's own write time changes when a file is added, removed or renamed in it.
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExW(data.c_str(), GetFileExInfoStandard, &fa))
        f.text += L"|" + std::to_wstring(fa.ftLastWriteTime.dwLowDateTime) + L"." + std::to_wstring(fa.ftLastWriteTime.dwHighDateTime);
    return f;
}

uint64_t FolderKey(std::string_view folder) { return bsa::hash_folder(folder, 105); }

}  // namespace

bool IsInGameData(const std::wstring& path) {
    std::wstring p = path;
    while (!p.empty() && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
    const size_t slash = p.find_last_of(L"\\/");
    if (slash == std::wstring::npos) return false;
    const std::wstring dir = p.substr(0, slash);
    const size_t s2 = dir.find_last_of(L"\\/");
    if (s2 == std::wstring::npos || _wcsicmp(dir.c_str() + s2 + 1, L"Data") != 0) return false;
    return FileExists(dir.substr(0, s2 + 1) + L"SkyrimSE.exe");
}

bool ActivePluginNames(const std::wstring& dataDir, std::set<std::wstring>* lowerNames) {
    std::wstring data = dataDir;
    if (!data.empty() && data.back() != L'\\' && data.back() != L'/') data += L'\\';
    if (!IsInGameData(data + L"x")) return false;
    const std::wstring folderName = GameFolderName(data + L"..\\");
    // Explorer asks once per plugin: the set is kept until plugins.txt,
    // Skyrim.ccc or the Data folder's list of files changes.
    const std::wstring stamp = Stamp(PluginsTxt(folderName)) + L"|" + Stamp(data + L"..\\Skyrim.ccc") + L"|" + Stamp(data.substr(0, data.size() - 1));
    static std::mutex lock;
    static std::map<std::wstring, std::pair<std::wstring, std::set<std::wstring>>> cache;  // data -> (stamp, names)
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = cache.find(LowerW(data));
        if (it != cache.end() && it->second.first == stamp) {
            *lowerNames = it->second.second;
            return true;
        }
    }
    std::set<std::wstring> names;
    auto add = [&](const std::wstring& name) {
        if (FileExists(data + name)) names.insert(LowerW(name));
    };
    for (const wchar_t* n : {L"Skyrim.esm", L"Update.esm", L"Dawnguard.esm", L"HearthFires.esm", L"Dragonborn.esm"}) add(n);
    for (const std::string& l : ReadLines(data + L"..\\Skyrim.ccc"))
        if (!l.empty() && l[0] != '#') add(WidenUtf8(l));
    for (const std::string& l : ReadLines(PluginsTxt(folderName)))
        if (l.size() > 1 && l[0] == '*') add(WidenUtf8(l.substr(1)));
    *lowerNames = names;
    std::lock_guard<std::mutex> g(lock);
    cache[LowerW(data)] = {stamp, std::move(names)};
    return true;
}

struct WinnerBuilder {
    static std::shared_ptr<DataWinners> Build(const std::wstring& data, const std::wstring& folderName) {
        const auto t0 = std::chrono::steady_clock::now();
        auto w = std::make_shared<DataWinners>();
        w->data_ = data;

        // Plugins in Data, as spelled on disk.
        std::unordered_map<std::wstring, std::wstring> plugins, archives;  // lower -> name
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((data + L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
        if (f != INVALID_HANDLE_VALUE) {
            do {
                if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const std::wstring name = fd.cFileName;
                if (IsPluginName(name)) plugins[LowerW(name)] = name;
                const std::wstring l = LowerW(name);
                if (l.size() > 4 && l.compare(l.size() - 4, 4, L".bsa") == 0) archives[l] = name;
            } while (FindNextFileW(f, &fd));
            FindClose(f);
        }

        // Load order: base masters, Skyrim.ccc, plugins.txt's active lines; masters first.
        std::vector<std::wstring> order;
        std::set<std::wstring> seen;
        auto add = [&](const std::wstring& name) {
            auto it = plugins.find(LowerW(name));
            if (it == plugins.end() || !seen.insert(it->first).second) return;
            order.push_back(it->second);
        };
        for (const wchar_t* n : {L"Skyrim.esm", L"Update.esm", L"Dawnguard.esm", L"HearthFires.esm", L"Dragonborn.esm"}) add(n);
        for (const std::string& l : ReadLines(data + L"..\\Skyrim.ccc"))
            if (!l.empty() && l[0] != '#') add(WidenUtf8(l));
        for (const std::string& l : ReadLines(PluginsTxt(folderName)))
            if (l.size() > 1 && l[0] == '*') add(WidenUtf8(l.substr(1)));
        std::stable_partition(order.begin(), order.end(), [&](const std::wstring& n) { return IsMaster(data + n); });
        w->order_ = order;

        // A plugin's own ini could change the archive lists: not modelled, so say so
        // rather than name a winner that may be wrong.
        for (const std::wstring& p : order) {
            const std::wstring ini = data + p.substr(0, p.rfind(L'.')) + L".ini";
            if (!FileExists(ini)) continue;
            for (const std::string& line : ReadLines(ini)) {
                if (LowerA(line).find("sresourcearchive") != std::string::npos) {
                    w->why_ = ini.substr(data.size()) + L" changes the archive lists, so the winners can't be established";
                    break;
                }
            }
            if (!w->why_.empty()) break;
        }

        // The ini lists: the first folder with a Skyrim.ini or SkyrimCustom.ini.
        std::string list1 = kDefaultList1, list2 = kDefaultList2;
        for (const std::wstring& ini : IniFiles(folderName)) {
            if (!FileExists(ini)) continue;
            const auto lines = ReadLines(ini);
            std::string v;
            if (IniValue(lines, "Archive", "sResourceArchiveList", &v)) list1 = v;
            if (IniValue(lines, "Archive", "sResourceArchiveList2", &v)) list2 = v;
        }

        // Ranks.
        std::set<std::wstring> claimed;
        auto claim = [&](const std::wstring& name) {
            const std::wstring l = LowerW(name);
            auto it = archives.find(l);
            if (it == archives.end() || !claimed.insert(l).second) return;
            DataWinners::LoadedArchive a;
            a.path = data + it->second;
            a.lowerName = l;
            a.rank = static_cast<int>(w->loaded_.size());
            w->loaded_.push_back(std::move(a));
        };
        for (const auto& n : SplitList(list1)) claim(WidenUtf8(n));
        for (const auto& n : SplitList(list2)) claim(WidenUtf8(n));
        for (const std::wstring& p : order) {
            const std::wstring base = p.substr(0, p.rfind(L'.'));
            claim(base + L".bsa");
            claim(base + L" - Textures.bsa");
        }

        // Folder index: which loaded archives have each folder.
        WorkSlot slot(WorkKind::Background);
        for (auto& a : w->loaded_) {
            bsa::Reader r;
            if (r.open(std::filesystem::path(a.path)) != bsa::Status::Ok) continue;  // the game can't read it either
            const uint32_t count = r.header().folderCount;
            bool named = true;
            for (uint32_t i = 0; i < count; ++i) {
                std::string_view name;
                if (r.folder_name(i, name) != bsa::Status::Ok) {
                    named = false;
                    break;
                }
                w->folders_.push_back({FolderKey(name), a.rank});
            }
            a.indexed = named;
            if (!named) w->unindexed_.push_back(a.rank);
            slot.Pace();
        }
        std::sort(w->folders_.begin(), w->folders_.end());
        w->folders_.erase(std::unique(w->folders_.begin(), w->folders_.end()), w->folders_.end());
        w->buildSeconds_ = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        return w;
    }
};

std::shared_ptr<const DataWinners> DataWinners::ForArchive(const std::wstring& bsaPath) {
    if (!IsInGameData(bsaPath)) return nullptr;
    const std::wstring data = bsaPath.substr(0, bsaPath.find_last_of(L"\\/") + 1);
    const std::wstring gameDir = data + L"..\\";
    const std::wstring folderName = GameFolderName(gameDir);
    const Fingerprint fp = MakeFingerprint(data, folderName);

    struct Cached {
        std::wstring fingerprint;
        std::shared_ptr<DataWinners> plan;
    };
    static std::mutex lock;
    static std::map<std::wstring, Cached> cache;
    static std::mutex buildLock;  // one build at a time

    const std::wstring key = LowerW(data);
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = cache.find(key);
        if (it != cache.end() && it->second.fingerprint == fp.text) return it->second.plan;
    }
    std::lock_guard<std::mutex> b(buildLock);
    {
        std::lock_guard<std::mutex> g(lock);
        auto it = cache.find(key);
        if (it != cache.end() && it->second.fingerprint == fp.text) return it->second.plan;
    }
    auto plan = WinnerBuilder::Build(data, folderName);
    std::lock_guard<std::mutex> g(lock);
    cache[key] = {fp.text, plan};
    return plan;
}

bool DataWinners::Loaded(const std::wstring& bsaPath) const {
    const std::wstring l = LowerW(bsaPath.substr(bsaPath.find_last_of(L"\\/") + 1));
    for (const auto& a : loaded_)
        if (a.lowerName == l) return true;
    return false;
}

void DataWinners::Resolve(const std::vector<std::wstring>& relativePaths, std::vector<WinnerResult>* out) const {
    out->assign(relativePaths.size(), WinnerResult{});
    if (!why_.empty()) return;  // Unknown
    std::map<int, std::unique_ptr<bsa::Reader>> readers;  // opened as needed, closed on return
    auto reader = [&](int rank) -> const bsa::Reader* {
        auto it = readers.find(rank);
        if (it == readers.end()) {
            auto r = std::make_unique<bsa::Reader>();
            if (r->open(std::filesystem::path(loaded_[static_cast<size_t>(rank)].path)) != bsa::Status::Ok) r.reset();
            it = readers.emplace(rank, std::move(r)).first;
        }
        return it->second.get();
    };
    for (size_t i = 0; i < relativePaths.size(); ++i) {
        std::wstring rel = relativePaths[i];
        std::replace(rel.begin(), rel.end(), L'/', L'\\');
        WinnerResult& r = (*out)[i];
        if (FileExists(data_ + rel)) {
            r.kind = WinnerResult::Loose;
            r.path = data_ + rel;
            continue;
        }
        r.kind = WinnerResult::None;
        // The names as the archives store them: Windows-1252, or UTF-8 for a few mods.
        std::vector<std::string> spellings{Narrow(rel, 1252)};
        if (!IsAscii(rel)) spellings.push_back(Narrow(rel, CP_UTF8));
        std::vector<int> ranks;
        for (const std::string& s : spellings) {
            const size_t cut = s.find_last_of('\\');
            const std::string f = cut == std::string::npos ? std::string() : s.substr(0, cut);
            const uint64_t key = FolderKey(f);
            for (auto it = std::lower_bound(folders_.begin(), folders_.end(), std::make_pair(key, 0)); it != folders_.end() && it->first == key; ++it)
                ranks.push_back(it->second);
        }
        ranks.insert(ranks.end(), unindexed_.begin(), unindexed_.end());
        std::sort(ranks.begin(), ranks.end(), std::greater<int>());
        ranks.erase(std::unique(ranks.begin(), ranks.end()), ranks.end());
        for (int rank : ranks) {
            const bsa::Reader* rd = reader(rank);
            if (!rd) continue;
            bool found = false;
            for (const std::string& s : spellings) {
                bsa::Entry e;
                if (rd->find(s, e) == bsa::Status::Ok) {
                    found = true;
                    break;
                }
            }
            if (found) {
                r.kind = WinnerResult::Archive;
                r.path = loaded_[static_cast<size_t>(rank)].path;
                break;
            }
        }
    }
}

}  // namespace pse

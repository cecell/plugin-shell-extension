#include "disk_cache.h"

#include <windows.h>
#include <shlobj.h>
#include <bcrypt.h>

#pragma comment(lib, "bcrypt.lib")

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "pattern_value.h"

namespace pse {
namespace {

const std::wstring kVersionLine = [] {
    std::string v = PSE_VERSION_STR;  // ASCII, from CMake
    return L"PSE-CACHE\t" + std::wstring(v.begin(), v.end());
}();

std::mutex g_lock;
bool g_loaded = false;
bool g_fresh = false;  // the file must be (re)created with the version line
std::unordered_map<std::wstring, std::wstring> g_values;

std::wstring CacheDir() {
    PWSTR local = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local))) {
        dir = std::wstring(local) + L"\\Plugin Shell Extension";
    }
    CoTaskMemFree(local);
    return dir;
}

std::wstring CachePath() {
    std::wstring dir = CacheDir();
    return dir.empty() ? dir : dir + L"\\cache.txt";
}

std::string ToUtf8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring FromUtf8(const char* p, size_t n) {
    int len = MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(n), nullptr, 0);
    std::wstring w(len, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, p, static_cast<int>(n), w.data(), len);
    return w;
}

// "<kind>|<path>|..." -> "<kind>|<path>": a newer stamp of the same file
// makes the older line obsolete.
std::wstring FileOf(const std::wstring& key) {
    size_t first = key.find(L'|');
    size_t second = first == std::wstring::npos ? first : key.find(L'|', first + 1);
    return second == std::wstring::npos ? key : key.substr(0, second);
}

bool WriteAll(HANDLE f, const std::string& s) {
    DWORD written = 0;
    return WriteFile(f, s.data(), static_cast<DWORD>(s.size()), &written, nullptr) && written == s.size();
}

void Load() {
    g_loaded = true;
    std::wstring path = CachePath();
    if (path.empty()) return;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        g_fresh = true;
        return;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(f, &size);
    std::string data(static_cast<size_t>(std::min<LONGLONG>(size.QuadPart, 256LL << 20)), '\0');
    DWORD got = 0;
    if (!data.empty()) ReadFile(f, data.data(), static_cast<DWORD>(data.size()), &got, nullptr);
    CloseHandle(f);
    data.resize(got);

    std::unordered_map<std::wstring, std::pair<std::wstring, std::wstring>> latest;  // file -> (key, value)
    size_t lines = 0;
    size_t pos = 0;
    bool first = true;
    while (pos < data.size()) {
        size_t end = data.find('\n', pos);
        if (end == std::string::npos) break;  // a half-written last line
        std::wstring line = FromUtf8(data.data() + pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (first) {
            first = false;
            if (line != kVersionLine) {  // another version's results: start over
                g_fresh = true;
                return;
            }
            continue;
        }
        size_t tab = line.find(L'\t');
        if (tab == std::wstring::npos) continue;
        std::wstring key = line.substr(0, tab), value = line.substr(tab + 1);
        ++lines;
        latest[FileOf(key)] = {key, value};
        g_values[key] = std::move(value);
    }
    if (first) {
        g_fresh = true;
        return;
    }
    // Mostly outdated lines: rewrite with only the newest per file.
    if (lines > 1000 && lines > 2 * latest.size()) {
        std::string out = ToUtf8(kVersionLine) + "\n";
        for (const auto& [file, kv] : latest) out += ToUtf8(kv.first + L"\t" + kv.second) + "\n";
        std::wstring tmp = path + L".tmp";
        HANDLE t = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (t != INVALID_HANDLE_VALUE) {
            bool ok = WriteAll(t, out);
            CloseHandle(t);
            if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
        }
    }
}

int PhysicalCores() {
    DWORD len = 0;
    GetLogicalProcessorInformationEx(RelationProcessorCore, nullptr, &len);
    std::vector<uint8_t> buf(len);
    auto* info = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data());
    if (!len || !GetLogicalProcessorInformationEx(RelationProcessorCore, info, &len)) return 1;
    int cores = 0;
    for (DWORD off = 0; off < len;) {
        auto* e = reinterpret_cast<SYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX*>(buf.data() + off);
        if (e->Relationship == RelationProcessorCore) ++cores;
        off += e->Size;
    }
    return cores ? cores : 1;
}

// The header of every compiled script (.pex) names a user and a computer.
// Bethesda's compiler writes the Windows account and machine name there;
// Compile writes these instead. Each install gets its own random pair the
// first time settings.json is written (from the system's random generator,
// not from any hardware, account or licence), so a user can tell their own
// scripts without the names saying anything about them. These two are only
// the fallback when no random value can be made or stored.
const char kFallbackPapyrusUser[] = "skqe";
const char kFallbackPapyrusComputer[] = "papyrus";

// "<prefix>" + 12 random hex digits; empty when the generator fails.
std::string RandomName(const char* prefix) {
    unsigned char bytes[6] = {};
    if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return {};
    static const char kHex[] = "0123456789abcdef";
    std::string out = prefix;
    for (unsigned char b : bytes) {
        out += kHex[b >> 4];
        out += kHex[b & 15];
    }
    return out;
}

// settings.json as written for a new install, and when the papyrus names are
// missing or empty (the other values are kept).
std::string SettingsText(int64_t workerThreads, const std::string& user, const std::string& computer) {
    std::string out;
    WriteJson(Value::Str(user), &out);
    const std::string userJson = out;
    out.clear();
    WriteJson(Value::Str(computer), &out);
    const std::string computerJson = out;
    return "{\n"
           "  \"workerThreads\": " + std::to_string(workerThreads) + ",\n"
           "  \"_workerThreads\": \"How many background jobs the whole extension runs at once: #ITM checks, extracting, "
           "testing and hashing. 0 = automatic: half the CPU cores, from 1 to 6. Press F5 in Explorer after changing it.\",\n"
           "  \"papyrusUser\": " + userJson + ",\n"
           "  \"papyrusComputer\": " + computerJson + ",\n"
           "  \"_papyrus\": \"The user and computer names Compile writes into the header of every .pex it makes. Bethesda's "
           "compiler puts your Windows account and computer name there, which anyone with the file can read. These start "
           "as random values made for this install, so you can recognise your own scripts without them saying who you "
           "are. Change them to anything: up to 64 plain characters each, anything else is dropped. Empty them to get "
           "new random ones. Takes effect on the next Compile.\"\n"
           "}\n";
}

// A header name: printable ASCII only, trimmed, at most 64 characters; empty
// when there is nothing usable.
std::string HeaderName(const Value* v) {
    std::string out;
    if (v && v->kind() == Value::Kind::String) {
        for (char c : v->AsString())
            if (c >= 0x20 && c <= 0x7E && out.size() < 64) out += c;
        while (!out.empty() && out.back() == ' ') out.pop_back();
        while (!out.empty() && out.front() == ' ') out.erase(out.begin());
    }
    return out;
}

struct Settings {
    int workerThreads = 0;
    std::string papyrusUser = kFallbackPapyrusUser;
    std::string papyrusComputer = kFallbackPapyrusComputer;
};

void WriteSettings(const std::wstring& path, const Settings& s) {
    HANDLE w = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (w == INVALID_HANDLE_VALUE) return;
    WriteAll(w, SettingsText(s.workerThreads, s.papyrusUser, s.papyrusComputer));
    CloseHandle(w);
}

// settings.json, re-read whenever its size or write time changes. Created when
// missing, with random papyrus names; a file that parses but lacks a papyrus
// name, or has an empty one, gets a random one written in (its other values
// kept). A file that doesn't parse is left alone and the fallbacks are used.
Settings CurrentSettings() {
    static std::mutex lock;
    static uint64_t seenStamp = ~0ull;
    static Settings current;
    std::lock_guard<std::mutex> g(lock);
    std::wstring dir = CacheDir();
    if (dir.empty()) return current;
    std::wstring path = dir + L"\\settings.json";
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
        CreateDirectoryW(dir.c_str(), nullptr);
        Settings fresh;
        const std::string user = RandomName("wham-"), computer = RandomName("pc-");
        if (!user.empty() && !computer.empty()) {
            fresh.papyrusUser = user;
            fresh.papyrusComputer = computer;
        }
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            WriteAll(f, SettingsText(fresh.workerThreads, fresh.papyrusUser, fresh.papyrusComputer));
            CloseHandle(f);
        }
        current = fresh;
        seenStamp = ~0ull;  // read it back next time, so the stamp is right
        return current;
    }
    uint64_t stamp = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) |
                     fa.ftLastWriteTime.dwLowDateTime ^ fa.nFileSizeLow;
    if (stamp == seenStamp) return current;  // unchanged since the last read
    seenStamp = stamp;
    current = Settings();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0,
                           nullptr);
    if (f == INVALID_HANDLE_VALUE) return current;
    char text[8192] = {};
    DWORD got = 0;
    ReadFile(f, text, sizeof(text) - 1, &got, nullptr);
    CloseHandle(f);
    Value root;
    if (!ParseJson(std::string_view(text, got), &root, nullptr) || root.kind() != Value::Kind::Map) return current;
    int64_t n = 0;
    const Value* v = root.Find("workerThreads");
    if (!v) v = root.Find("itmThreads");
    if (v && v->ToInt(&n)) current.workerThreads = static_cast<int>(n);
    std::string user = HeaderName(root.Find("papyrusUser"));
    std::string computer = HeaderName(root.Find("papyrusComputer"));
    bool rewrite = false;
    if (user.empty()) {
        user = RandomName("wham-");
        rewrite = !user.empty();
    }
    if (computer.empty()) {
        computer = RandomName("pc-");
        rewrite = rewrite || !computer.empty();
    }
    if (!user.empty()) current.papyrusUser = user;
    if (!computer.empty()) current.papyrusComputer = computer;
    if (rewrite) WriteSettings(path, current);  // the next read sees the new stamp and these values
    return current;
}

}  // namespace

int WorkerThreadCount() {
    const int setting = CurrentSettings().workerThreads;
    if (setting > 0) return std::min(setting, 64);
    return std::clamp(PhysicalCores() / 2, 1, 6);
}

std::string PapyrusHeaderUser() { return CurrentSettings().papyrusUser; }
std::string PapyrusHeaderComputer() { return CurrentSettings().papyrusComputer; }

bool DiskCacheGet(const std::wstring& key, std::wstring* value) {
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_loaded) Load();
    auto it = g_values.find(key);
    if (it == g_values.end()) return false;
    *value = it->second;
    return true;
}

void DiskCachePut(const std::wstring& key, const std::wstring& value) {
    std::lock_guard<std::mutex> g(g_lock);
    if (!g_loaded) Load();
    auto it = g_values.find(key);
    if (it != g_values.end() && it->second == value) return;
    g_values[key] = value;
    std::wstring path = CachePath();
    if (path.empty()) return;
    std::string line = ToUtf8(key + L"\t" + value) + "\n";
    HANDLE f;
    if (g_fresh) {
        CreateDirectoryW(CacheDir().c_str(), nullptr);
        f = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, 0,
                        nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
        line = ToUtf8(kVersionLine) + "\n" + line;
        g_fresh = false;
    } else {
        // Appends are one write each, so lines from several processes do not mix.
        f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                        OPEN_ALWAYS, 0, nullptr);
        if (f == INVALID_HANDLE_VALUE) return;
    }
    WriteAll(f, line);
    CloseHandle(f);
}

}  // namespace pse

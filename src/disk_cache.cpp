#include "disk_cache.h"

#include <windows.h>
#include <shlobj.h>

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

const char kDefaultSettings[] =
    "{\n"
    "  \"itmThreads\": 0,\n"
    "  \"_itmThreads\": \"How many plugins the #ITM column checks at once. 0 = automatic: half the CPU cores, "
    "from 1 to 8. Press F5 in Explorer after changing it.\"\n"
    "}\n";

}  // namespace

int ItmThreadCount() {
    static std::mutex lock;
    static uint64_t seenStamp = ~0ull;
    static int setting = 0;
    std::lock_guard<std::mutex> g(lock);
    std::wstring dir = CacheDir();
    if (!dir.empty()) {
        std::wstring path = dir + L"\\settings.json";
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
            CreateDirectoryW(dir.c_str(), nullptr);
            HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
            if (f != INVALID_HANDLE_VALUE) {
                WriteAll(f, kDefaultSettings);
                CloseHandle(f);
            }
            setting = 0;
        } else {
            uint64_t stamp = (static_cast<uint64_t>(fa.ftLastWriteTime.dwHighDateTime) << 32) |
                             fa.ftLastWriteTime.dwLowDateTime ^ fa.nFileSizeLow;
            if (stamp != seenStamp) {  // changed (or first look): read it again
                seenStamp = stamp;
                setting = 0;
                HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                       OPEN_EXISTING, 0, nullptr);
                if (f != INVALID_HANDLE_VALUE) {
                    char text[4096] = {};
                    DWORD got = 0;
                    ReadFile(f, text, sizeof(text) - 1, &got, nullptr);
                    CloseHandle(f);
                    Value root;
                    int64_t n = 0;
                    if (ParseJson(std::string_view(text, got), &root, nullptr) && root.kind() == Value::Kind::Map) {
                        if (const Value* v = root.Find("itmThreads"); v && v->ToInt(&n)) setting = static_cast<int>(n);
                    }
                }
            }
        }
    }
    if (setting > 0) return std::min(setting, 64);
    return std::clamp(PhysicalCores() / 2, 1, 8);
}

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

// A .bsa opened and turned into a tree of safe names. Shared by the Explorer
// folder and the BSA manager's tools.
#pragma once

#include <windows.h>

#include <bsa/reader.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace pse {

inline std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

inline std::wstring Widen(std::string_view bytes) {
    if (bytes.empty()) return {};
    const int size = static_cast<int>(bytes.size());
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), size, nullptr, 0);
    UINT codepage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (n <= 0) {
        codepage = 1252;
        flags = 0;
        n = MultiByteToWideChar(codepage, flags, bytes.data(), size, nullptr, 0);
    }
    std::wstring out(n, L' ');
    MultiByteToWideChar(codepage, flags, bytes.data(), size, out.data(), n);
    return out;
}

// Names come from the archive, so they are untrusted: a name must not be able to
// point outside the folder it is copied into.
inline bool SafeName(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L".." || name.back() == L'.' || name.back() == L' ') return false;
    for (wchar_t c : name)
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) return false;
    // Windows' device names, with or without an extension.
    std::wstring base = name.substr(0, name.find(L'.'));
    CharUpperBuffW(base.data(), static_cast<DWORD>(base.size()));
    static const wchar_t* const reserved[] = {L"CON", L"PRN", L"AUX", L"NUL", L"COM1", L"COM2", L"COM3", L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9",
                                              L"LPT1", L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8", L"LPT9"};
    for (const wchar_t* r : reserved)
        if (base == r) return false;
    return true;
}

struct Node {
    std::wstring name;
    int parent = -1;
    int64_t entry = -1;  // index into the archive's files; -1 for a folder
    std::vector<int> children;
    bool dir() const { return entry < 0; }
};

class Archive {
public:
    static std::shared_ptr<Archive> Open(const std::wstring& path, HRESULT* hr) {
        static std::mutex lock;
        static std::map<std::wstring, std::weak_ptr<Archive>> open;
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) {
            *hr = HRESULT_FROM_WIN32(GetLastError());
            return nullptr;
        }
        const std::wstring key = Lower(path) + L"|" + std::to_wstring(fa.nFileSizeLow) + L"|" + std::to_wstring(fa.nFileSizeHigh) + L"|" +
                                 std::to_wstring(fa.ftLastWriteTime.dwLowDateTime) + L"|" + std::to_wstring(fa.ftLastWriteTime.dwHighDateTime);
        std::lock_guard<std::mutex> g(lock);
        if (auto found = open[key].lock()) {
            *hr = S_OK;
            return found;
        }
        auto made = std::make_shared<Archive>();
        made->path_ = path;
        if (made->reader_.open(std::filesystem::path(path)) != bsa::Status::Ok || !made->Build()) {
            *hr = HRESULT_FROM_WIN32(ERROR_BAD_FORMAT);
            return nullptr;
        }
        open[key] = made;
        for (auto it = open.begin(); it != open.end();) it = it->second.expired() ? open.erase(it) : std::next(it);
        *hr = S_OK;
        return made;
    }

    const Node& At(int i) const { return nodes_[i]; }
    int NodeCount() const { return static_cast<int>(nodes_.size()); }
    const bsa::Reader& Reader() const { return reader_; }
    const std::wstring& Path() const { return path_; }

    int Child(int node, const wchar_t* name) const {
        const auto& kids = nodes_[node].children;
        auto it = std::lower_bound(kids.begin(), kids.end(), name, [&](int a, const wchar_t* n) { return _wcsicmp(nodes_[a].name.c_str(), n) < 0; });
        return it != kids.end() && _wcsicmp(nodes_[*it].name.c_str(), name) == 0 ? *it : -1;
    }

    std::wstring RelativePath(int node) const {
        std::wstring out;
        for (; node > 0; node = nodes_[node].parent) out = nodes_[node].name + (out.empty() ? L"" : L"\\" + out);
        return out;
    }

    uint64_t Size(int node) const {
        uint32_t size = 0;
        bsa::Entry e;
        if (nodes_[node].dir() || reader_.entry(static_cast<uint32_t>(nodes_[node].entry), e) != bsa::Status::Ok ||
            reader_.uncompressed_size(e, size) != bsa::Status::Ok)
            return 0;
        return size;
    }

    bool Read(int node, std::vector<uint8_t>* out) const {
        bsa::Entry e;
        return !nodes_[node].dir() && reader_.entry(static_cast<uint32_t>(nodes_[node].entry), e) == bsa::Status::Ok &&
               reader_.read(e, *out, size_t{0xFFFFFFFF}) == bsa::Status::Ok;
    }

    size_t Skipped() const { return skipped_; }

private:
    bool Build() {
        nodes_.emplace_back();
        std::unordered_map<std::wstring, int> dirs;
        dirs[L""] = 0;
        const uint32_t count = reader_.header().fileCount;
        for (uint32_t i = 0; i < count; ++i) {
            bsa::Entry e;
            std::string_view folder, name;
            if (reader_.entry(i, e) != bsa::Status::Ok || reader_.folder_name(e.folderIndex, folder) != bsa::Status::Ok ||
                reader_.file_name(e, name) != bsa::Status::Ok) {
                ++skipped_;
                continue;
            }
            std::wstring dir = Widen(folder);
            std::replace(dir.begin(), dir.end(), L'/', L'\\');
            const std::wstring file = Widen(name);
            int at = 0;
            std::wstring soFar;
            bool ok = SafeName(file);
            for (size_t p = 0; ok && p <= dir.size();) {
                size_t slash = dir.find(L'\\', p);
                if (slash == std::wstring::npos) slash = dir.size();
                const std::wstring part = dir.substr(p, slash - p);
                p = slash + 1;
                if (part.empty()) continue;
                if (!SafeName(part)) {
                    ok = false;
                    break;
                }
                soFar += Lower(part) + L"\\";
                auto it = dirs.find(soFar);
                if (it == dirs.end()) {
                    Node n;
                    n.name = part;
                    n.parent = at;
                    nodes_.push_back(std::move(n));
                    const int made = static_cast<int>(nodes_.size()) - 1;
                    nodes_[at].children.push_back(made);
                    it = dirs.emplace(soFar, made).first;
                }
                at = it->second;
            }
            if (!ok) {
                ++skipped_;
                continue;
            }
            Node n;
            n.name = file;
            n.parent = at;
            n.entry = i;
            nodes_.push_back(std::move(n));
            nodes_[at].children.push_back(static_cast<int>(nodes_.size()) - 1);
        }
        for (auto& n : nodes_)
            std::sort(n.children.begin(), n.children.end(), [&](int a, int b) { return _wcsicmp(nodes_[a].name.c_str(), nodes_[b].name.c_str()) < 0; });
        return true;
    }

    std::wstring path_;
    bsa::Reader reader_;
    std::vector<Node> nodes_;
    size_t skipped_ = 0;
};

using ArchivePtr = std::shared_ptr<Archive>;

}  // namespace pse

#include "bsa_tools.h"

#include <bcrypt.h>

#include <algorithm>
#include <map>
#include <set>
#include <unordered_map>

#include "bsa_archive.h"
#include "work_budget.h"

namespace pse {
namespace {

struct KindRule {
    const wchar_t* label;
    std::vector<const wchar_t*> extensions;
};

// In the order the properties dialog lists them.
const std::vector<KindRule>& KindRules() {
    static const std::vector<KindRule> rules = {
        {L"Number of textures (.dds)", {L".dds"}},
        {L"Number of TGA textures", {L".tga"}},
        {L"Number of other images", {L".png", L".bmp", L".jpg", L".jpeg"}},
        {L"Number of meshes (.nif)", {L".nif"}},
        {L"Number of animations (.hkx, .kf)", {L".hkx", L".kf"}},
        {L"Number of FaceGen morphs (.tri)", {L".tri"}},
        {L"Number of scripts (.pex)", {L".pex"}},
        {L"Number of script sources (.psc)", {L".psc"}},
        {L"Number of sounds (.wav, .xwm)", {L".wav", L".xwm"}},
        {L"Number of voice lines (.fuz)", {L".fuz"}},
        {L"Number of lip files (.lip)", {L".lip"}},
        {L"Number of interface files (.swf, .gfx)", {L".swf", L".gfx"}},
        {L"Number of string tables", {L".strings", L".dlstrings", L".ilstrings"}},
        {L"Number of scene files (.seq)", {L".seq"}},
        {L"Number of LOD files (.bto, .btr, .btt)", {L".bto", L".btr", L".btt"}},
        {L"Number of text files (.txt, .xml, .json, .ini)", {L".txt", L".xml", L".json", L".ini"}},
    };
    return rules;
}

std::wstring Extension(const std::wstring& name) {
    const size_t dot = name.rfind(L'.');
    return dot == std::wstring::npos ? std::wstring() : Lower(name.substr(dot));
}

std::wstring LongPath(const std::wstring& p) {
    if (p.compare(0, 4, L"\\\\?\\") == 0) return p;
    if (p.compare(0, 2, L"\\\\") == 0) return L"\\\\?\\UNC\\" + p.substr(2);
    return L"\\\\?\\" + p;
}

std::wstring FullPath(const std::wstring& p) {
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n) return p;
    std::wstring out(n, L' ');
    n = GetFullPathNameW(p.c_str(), n, out.data(), nullptr);
    out.resize(n);
    while (out.size() > 3 && (out.back() == L'\\' || out.back() == L'/')) out.pop_back();
    return out;
}

void MakeFolders(const std::wstring& longDir, std::set<std::wstring>* made) {
    if (made->count(longDir)) return;
    const size_t slash = longDir.find_last_of(L'\\');
    if (slash != std::wstring::npos && slash > 7) MakeFolders(longDir.substr(0, slash), made);
    CreateDirectoryW(longDir.c_str(), nullptr);
    made->insert(longDir);
}

class Bcrypt {
public:
    explicit Bcrypt(const wchar_t* algorithm) {
        if (BCryptOpenAlgorithmProvider(&alg_, algorithm, nullptr, 0) != 0) return;
        DWORD length = 0, got = 0;
        BCryptGetProperty(alg_, BCRYPT_HASH_LENGTH, reinterpret_cast<PUCHAR>(&length), sizeof(length), &got, 0);
        digest_.resize(length);
        if (BCryptCreateHash(alg_, &hash_, nullptr, 0, nullptr, 0, 0) != 0) hash_ = nullptr;
    }
    ~Bcrypt() {
        if (hash_) BCryptDestroyHash(hash_);
        if (alg_) BCryptCloseAlgorithmProvider(alg_, 0);
    }
    bool ok() const { return hash_ != nullptr; }
    void Add(const uint8_t* data, size_t size) { BCryptHashData(hash_, const_cast<PUCHAR>(data), static_cast<ULONG>(size), 0); }
    std::wstring Hex() {
        BCryptFinishHash(hash_, digest_.data(), static_cast<ULONG>(digest_.size()), 0);
        static const wchar_t* digits = L"0123456789abcdef";
        std::wstring out;
        for (uint8_t b : digest_) {
            out += digits[b >> 4];
            out += digits[b & 15];
        }
        return out;
    }

private:
    BCRYPT_ALG_HANDLE alg_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
    std::vector<uint8_t> digest_;
};

uint32_t Crc32(uint32_t crc, const uint8_t* data, size_t size) {
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

std::wstring HumanSize(uint64_t bytes) {
    wchar_t buf[64];
    if (bytes >= 1024 * 1024) swprintf_s(buf, L"%.1f MB", bytes / 1048576.0);
    else if (bytes >= 1024) swprintf_s(buf, L"%.0f KB", bytes / 1024.0);
    else swprintf_s(buf, L"%llu bytes", static_cast<unsigned long long>(bytes));
    return buf;
}

// Duplicates the game finds by exact path can't be merged.
bool PathIsRequired(std::wstring lower) {
    std::replace(lower.begin(), lower.end(), L'/', L'\\');
    return lower.find(L"facegendata\\") != std::wstring::npos || lower.compare(0, 12, L"sound\\voice\\") == 0 ||
           lower.find(L"\\lod\\") != std::wstring::npos || lower.compare(0, 8, L"terrain\\") == 0 ||
           lower.find(L"meshes\\terrain\\") != std::wstring::npos || lower.find(L"textures\\terrain\\") != std::wstring::npos;
}

}  // namespace

const wchar_t* BsaGameName(uint32_t version) {
    switch (version) {
        case 103: return L"Oblivion";
        case 104: return L"Fallout 3, New Vegas or Skyrim (2011)";
        case 105: return L"Skyrim Special Edition";
    }
    return L"Unknown";
}

const wchar_t* BsaCompressionName(const BsaFacts& f) {
    if (!f.compressedFiles) return L"None";
    return f.version == 105 ? L"LZ4 frame" : L"zlib";
}

std::wstring BsaFlagsText(uint32_t a) {
    using namespace bsa::archive_flags;
    std::wstring s;
    auto add = [&](uint32_t bit, const wchar_t* name) {
        if (a & bit) s += (s.empty() ? L"" : L", ") + std::wstring(name);
    };
    add(FolderNames, L"Folder names");
    add(FileNames, L"File names");
    add(Compressed, L"Compressed");
    add(RetainFolderNames, L"Retain folder names");
    add(RetainFileNames, L"Retain file names");
    add(RetainFileNameOffsets, L"Retain name offsets");
    add(Xbox360, L"Xbox 360");
    add(RetainStrings, L"Retain strings");
    add(EmbeddedNames, L"Embedded names");
    add(XMemCodec, L"XMem");
    return s;
}

std::wstring BsaContentText(uint32_t a) {
    using namespace bsa::file_flags;
    std::wstring s;
    auto add = [&](uint32_t bit, const wchar_t* name) {
        if (a & bit) s += (s.empty() ? L"" : L", ") + std::wstring(name);
    };
    add(Meshes, L"Meshes");
    add(Textures, L"Textures");
    add(Menus, L"Menus");
    add(Sounds, L"Sounds");
    add(Voices, L"Voices");
    add(Shaders, L"Shaders");
    add(Trees, L"Trees");
    add(Fonts, L"Fonts");
    add(Misc, L"Misc");
    return s.empty() ? L"None" : s;
}

BsaFacts ReadBsaFacts(const std::wstring& path, const std::atomic<bool>* cancel) {
    BsaFacts f;
    try {
        WorkSlot slot(WorkKind::Interactive, cancel);
        if (!slot.Held()) return f;
        HRESULT hr = E_FAIL;
        ArchivePtr archive = Archive::Open(path, &hr);
        if (!archive) {
            f.error = L"This file can't be read as a BSA archive.";
            return f;
        }
        const bsa::Header& h = archive->Reader().header();
        f.version = h.version;
        f.archiveFlags = h.archiveFlags;
        f.fileFlags = h.fileFlags;
        f.folders = h.folderCount;
        f.files = h.fileCount;
        f.archiveBytes = h.archiveSize;
        f.skippedNames = archive->Skipped();
        for (const auto& r : KindRules()) f.kinds.push_back({r.label, 0});
        f.kinds.push_back({L"Number of other files", 0});
        std::unordered_map<std::wstring, size_t> byExtension;
        for (size_t k = 0; k < KindRules().size(); ++k)
            for (const wchar_t* e : KindRules()[k].extensions) byExtension[e] = k;
        for (uint32_t i = 0; i < h.fileCount; ++i) {
            if (cancel && cancel->load()) return f;
            slot.Pace();
            bsa::Entry e;
            std::string_view name;
            uint32_t size = 0;
            if (archive->Reader().entry(i, e) != bsa::Status::Ok) continue;
            f.compressedFiles += e.compressed;
            if (archive->Reader().uncompressed_size(e, size) == bsa::Status::Ok) f.unpackedBytes += size;
            if (archive->Reader().file_name(e, name) != bsa::Status::Ok) continue;
            const std::wstring wide = Widen(name);
            auto it = byExtension.find(Extension(wide));
            ++f.kinds[it == byExtension.end() ? f.kinds.size() - 1 : it->second].count;
            if (size > f.largestBytes) {
                f.largestBytes = size;
                f.largestName = wide;
            }
        }
        f.ok = true;
    } catch (const std::exception&) {
        f.ok = false;
        f.error = L"Out of memory reading this archive.";
    }
    return f;
}

uint64_t CountExisting(const std::wstring& path, const std::wstring& destination) {
    uint64_t found = 0;
    try {
        HRESULT hr = E_FAIL;
        ArchivePtr archive = Archive::Open(path, &hr);
        if (!archive) return 0;
        const std::wstring root = LongPath(FullPath(destination));
        for (int n = 1; n < archive->NodeCount(); ++n)
            if (!archive->At(n).dir() && GetFileAttributesW((root + L"\\" + archive->RelativePath(n)).c_str()) != INVALID_FILE_ATTRIBUTES) ++found;
    } catch (const std::exception&) {
    }
    return found;
}

ExtractResult ExtractBsa(const std::wstring& path, const std::wstring& destination, ExistingFiles existing, const Progress& progress) {
    ExtractResult out;
    try {
        WorkSlot slot(WorkKind::Interactive);
        HRESULT hr = E_FAIL;
        ArchivePtr archive = Archive::Open(path, &hr);
        if (!archive) {
            out.firstError = L"This file can't be read as a BSA archive.";
            return out;
        }
        out.opened = true;
        out.skippedNames = archive->Skipped();
        const std::wstring root = LongPath(FullPath(destination));
        std::set<std::wstring> made;
        MakeFolders(root, &made);
        std::vector<int> files;
        for (int n = 1; n < archive->NodeCount(); ++n)
            if (!archive->At(n).dir()) files.push_back(n);
        std::vector<uint8_t> data;
        for (size_t i = 0; i < files.size(); ++i) {
            if (progress && !progress(i, files.size())) {
                out.cancelled = true;
                return out;
            }
            slot.Pace();
            const std::wstring target = root + L"\\" + archive->RelativePath(files[i]);
            if (existing == ExistingFiles::Skip && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
                ++out.skippedExisting;
                continue;
            }
            auto fail = [&](const wchar_t* why) {
                ++out.failed;
                if (out.firstError.empty()) out.firstError = archive->RelativePath(files[i]) + L": " + why;
            };
            if (!archive->Read(files[i], &data)) {
                fail(L"couldn't be read from the archive");
                continue;
            }
            MakeFolders(target.substr(0, target.find_last_of(L'\\')), &made);
            HANDLE h = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) {
                fail(L"couldn't be created");
                continue;
            }
            DWORD wrote = 0;
            const BOOL ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &wrote, nullptr) && wrote == data.size();
            CloseHandle(h);
            if (ok) {
                ++out.written;
            } else {
                DeleteFileW(target.c_str());  // never leave half a file
                fail(L"couldn't be written");
            }
        }
        if (progress) progress(files.size(), files.size());
    } catch (const std::exception&) {
        ++out.failed;
        if (out.firstError.empty()) out.firstError = L"Out of memory while extracting.";
    }
    return out;
}

BsaTestReport TestBsa(const std::wstring& path, const Progress& progress) {
    BsaTestReport report;
    try {
        WorkSlot slot(WorkKind::Interactive);
        bsa::Reader reader;
        const bsa::Status opened = reader.open(std::filesystem::path(path));
        if (opened != bsa::Status::Ok) {
            report.text = L"FAILED\r\n\r\nThis archive can't be opened: " + Widen(bsa::to_string(opened)) + L".\r\n";
            return report;
        }
        const bsa::Header& h = reader.header();
        uint64_t hashWrong = 0, lookupWrong = 0, readFailed = 0, unsafe = 0;
        std::wstring problems;
        std::map<std::pair<uint64_t, std::wstring>, std::vector<std::wstring>> byContent;
        std::vector<std::wstring> sources, sourcesScripts;
        std::vector<uint8_t> data;
        for (uint32_t i = 0; i < h.fileCount; ++i) {
            if (progress && !progress(i, h.fileCount)) {
                report.cancelled = true;
                report.status = 3;
                return report;
            }
            slot.Pace();
            bsa::Entry e;
            std::string_view folder, name;
            if (reader.entry(i, e) != bsa::Status::Ok || reader.folder_name(e.folderIndex, folder) != bsa::Status::Ok ||
                reader.file_name(e, name) != bsa::Status::Ok) {
                ++lookupWrong;
                continue;
            }
            std::wstring wideFolder = Widen(folder);
            const std::wstring wideName = Widen(name);
            const std::wstring display = wideFolder + L"\\" + wideName;
            if (bsa::hash_folder(folder, h.version) != e.folderHash || bsa::hash_file(name, h.version) != e.fileHash) {
                if (++hashWrong <= 20) problems += L"WRONG HASH  " + display + L"  (the game won't find this file by name)\r\n";
            }
            bsa::Entry found;
            std::string lookup(folder);
            lookup.append("\\").append(name);
            if (reader.find(lookup, found) != bsa::Status::Ok || found.fileIndex != e.fileIndex) {
                if (++lookupWrong <= 20) problems += L"NOT FOUND BY NAME  " + display + L"\r\n";
            }
            bool safe = SafeName(wideName);
            std::wstring parts = wideFolder;
            std::replace(parts.begin(), parts.end(), L'/', L'\\');
            for (size_t p = 0; safe && p <= parts.size();) {
                size_t slash = parts.find(L'\\', p);
                if (slash == std::wstring::npos) slash = parts.size();
                if (slash > p && !SafeName(parts.substr(p, slash - p))) safe = false;
                p = slash + 1;
            }
            if (!safe && ++unsafe <= 20) problems += L"UNSAFE NAME  " + display + L"\r\n";
            const bsa::Status read = reader.read(e, data, size_t{0xFFFFFFFF});
            if (read != bsa::Status::Ok) {
                if (++readFailed <= 20) problems += L"CAN'T BE READ  " + display + L"  (" + Widen(bsa::to_string(read)) + L")\r\n";
                continue;
            }
            Bcrypt md5(BCRYPT_MD5_ALGORITHM);
            if (md5.ok()) {
                md5.Add(data.data(), data.size());
                byContent[{static_cast<uint64_t>(data.size()), md5.Hex()}].push_back(display);
            }
            if (Extension(wideName) == L".psc") {
                const std::wstring lower = Lower(parts);
                if (lower == L"source\\scripts") sourcesScripts.push_back(Lower(wideName));
                else if (lower == L"source") sources.push_back(Lower(wideName));
            }
        }
        std::wstring duplicates;
        uint64_t optional = 0, required = 0, savable = 0;
        for (const auto& [key, paths] : byContent) {
            if (paths.size() < 2 || !key.first) continue;
            bool mustKeep = true;
            for (const auto& p : paths) mustKeep = mustKeep && PathIsRequired(Lower(p));
            const std::wstring each = HumanSize(key.first);
            duplicates += std::wstring(mustKeep ? L"DUPLICATE (required)  " : L"DUPLICATE  ") + std::to_wstring(paths.size()) + L" copies, " + each + L" each";
            if (!mustKeep) duplicates += L", " + HumanSize(key.first * (paths.size() - 1)) + L" could be saved";
            duplicates += L"\r\n";
            for (const auto& p : paths) duplicates += L"  " + p + L"\r\n";
            duplicates += mustKeep ? L"  The game finds these by exact path. All of them are needed.\r\n\r\n" : L"  Keep one copy and point the records at it.\r\n\r\n";
            if (mustKeep) {
                ++required;
            } else {
                ++optional;
                savable += key.first * (paths.size() - 1);
            }
        }
        std::sort(sources.begin(), sources.end());
        std::sort(sourcesScripts.begin(), sourcesScripts.end());
        std::vector<std::wstring> both;
        std::set_intersection(sources.begin(), sources.end(), sourcesScripts.begin(), sourcesScripts.end(), std::back_inserter(both));

        const uint64_t errors = readFailed + lookupWrong;
        const uint64_t warnings = hashWrong + unsafe + optional + both.size();
        report.status = errors ? 2 : warnings ? 1 : 0;
        std::wstring text = report.status == 0 ? L"PASSED" : report.status == 1 ? L"PASSED WITH WARNINGS" : L"FAILED";
        text += L"\r\n\r\n";
        text += std::to_wstring(h.fileCount) + L" files in " + std::to_wstring(h.folderCount) + L" folders were checked and decoded.\r\n";
        text += L"Can't be read: " + std::to_wstring(readFailed) + L"\r\n";
        text += L"Not found by name: " + std::to_wstring(lookupWrong) + L"\r\n";
        text += L"Wrong name hash: " + std::to_wstring(hashWrong) + L"\r\n";
        text += L"Unsafe names: " + std::to_wstring(unsafe) + L"\r\n";
        text += L"Duplicates that could be merged: " + std::to_wstring(optional);
        if (optional) text += L" (" + HumanSize(savable) + L" could be saved)";
        text += L"\r\nDuplicates the game needs: " + std::to_wstring(required) + L"\r\n";
        if (!problems.empty()) text += L"\r\nPROBLEMS\r\n" + problems;
        if (!both.empty()) {
            text += L"\r\nSCRIPTS IN BOTH source AND source\\scripts\r\n";
            for (const auto& s : both) text += L"  " + s + L"\r\n";
        }
        if (!duplicates.empty()) text += L"\r\n" + duplicates;
        report.text = std::move(text);
        if (progress) progress(h.fileCount, h.fileCount);
    } catch (const std::exception&) {
        report.status = 3;
        report.text = L"FAILED\r\n\r\nOut of memory while testing this archive.\r\n";
    }
    return report;
}

FileHashes ComputeHashes(const std::wstring& path, const std::atomic<bool>* cancel, const Progress& progress) {
    FileHashes out;
    try {
        WorkSlot slot(WorkKind::Interactive, cancel);
        if (!slot.Held()) {
            out.cancelled = true;
            return out;
        }
        HANDLE h = CreateFileW(LongPath(FullPath(path)).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                               FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
            out.error = L"This file can't be opened.";
            return out;
        }
        LARGE_INTEGER size{};
        GetFileSizeEx(h, &size);
        Bcrypt sha(BCRYPT_SHA256_ALGORITHM), md5(BCRYPT_MD5_ALGORITHM);
        uint32_t crc = 0;
        std::vector<uint8_t> buffer(size_t{4} << 20);
        uint64_t done = 0;
        bool good = sha.ok() && md5.ok();
        for (;;) {
            if (cancel && cancel->load()) {
                out.cancelled = true;
                CloseHandle(h);
                return out;
            }
            DWORD got = 0;
            if (!ReadFile(h, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
                good = false;
                break;
            }
            if (!got) break;
            sha.Add(buffer.data(), got);
            md5.Add(buffer.data(), got);
            crc = Crc32(crc, buffer.data(), got);
            done += got;
            if (progress && !progress(done, static_cast<uint64_t>(size.QuadPart))) {
                out.cancelled = true;
                CloseHandle(h);
                return out;
            }
            slot.Pace();
        }
        CloseHandle(h);
        if (!good) {
            out.error = L"This file couldn't be read to the end.";
            return out;
        }
        wchar_t text[16];
        swprintf_s(text, L"%08x", crc);
        out.crc32 = text;
        out.sha256 = sha.Hex();
        out.md5 = md5.Hex();
        out.ok = true;
    } catch (const std::exception&) {
        out.error = L"Out of memory while hashing this file.";
    }
    return out;
}

}  // namespace pse

#include "plugin_parser.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <map>

namespace pp {
namespace {

uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t U32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
bool SigIs(const uint8_t* p, const char* sig) { return std::memcmp(p, sig, 4) == 0; }

class Handle {
public:
    explicit Handle(HANDLE h) : h_(h) {}
    ~Handle() {
        if (h_ && h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return h_; }
    bool valid() const { return h_ && h_ != INVALID_HANDLE_VALUE; }

private:
    HANDLE h_;
};

HANDLE OpenForRead(const std::wstring& path) {
    return CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
}

bool ReadExact(HANDLE h, void* buf, DWORD size) {
    DWORD got = 0;
    return ReadFile(h, buf, size, &got, nullptr) && got == size;
}

std::wstring LastErrorText(DWORD err) {
    wchar_t* msg = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, err, 0, reinterpret_cast<wchar_t*>(&msg), 0, nullptr);
    std::wstring text = msg ? msg : L"Unknown error";
    if (msg) LocalFree(msg);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' ')) text.pop_back();
    return text;
}

bool HasLightExtension(const std::wstring& path) {
    size_t dot = path.find_last_of(L'.');
    if (dot == std::wstring::npos) return false;
    return _wcsicmp(path.c_str() + dot, L".esl") == 0;
}

// Top-level GRUP walk. Runs under SEH because the file is memory-mapped and a
// read can fault (file truncated meanwhile, network share drops). No C++
// objects with destructors may live in this function.
struct RawGroup {
    char label[4];
    uint64_t records;
};

enum WalkResult { kWalkOk = 0, kWalkCorrupt, kWalkTooMany, kWalkFault };

WalkResult WalkGroups(const uint8_t* base, uint64_t fileSize, uint64_t start, RawGroup* out, size_t capacity,
                      size_t* outCount, uint64_t* totalGroups, uint64_t* corruptAt) {
    *outCount = 0;
    *totalGroups = 0;
    __try {
        uint64_t pos = start;
        while (pos + 24 <= fileSize) {
            const uint8_t* g = base + pos;
            if (!SigIs(g, "GRUP")) {
                *corruptAt = pos;
                return kWalkCorrupt;
            }
            uint64_t groupSize = U32(g + 4);
            if (groupSize < 24 || pos + groupSize > fileSize) {
                *corruptAt = pos;
                return kWalkCorrupt;
            }
            ++*totalGroups;

            // Contents of a nested GRUP directly follow its 24-byte header,
            // so one linear pass visits every record at every depth.
            uint64_t records = 0;
            uint64_t end = pos + groupSize;
            uint64_t p = pos + 24;
            while (p < end) {
                if (p + 24 > end) {
                    *corruptAt = p;
                    return kWalkCorrupt;
                }
                const uint8_t* r = base + p;
                uint64_t size = U32(r + 4);
                if (SigIs(r, "GRUP")) {
                    if (size < 24 || p + size > end) {
                        *corruptAt = p;
                        return kWalkCorrupt;
                    }
                    ++*totalGroups;
                    p += 24;
                } else {
                    if (p + 24 + size > end) {
                        *corruptAt = p;
                        return kWalkCorrupt;
                    }
                    ++records;
                    p += 24 + size;
                }
            }

            size_t i = 0;
            for (; i < *outCount; ++i) {
                if (std::memcmp(out[i].label, g + 8, 4) == 0) break;
            }
            if (i == *outCount) {
                if (*outCount == capacity) return kWalkTooMany;
                std::memcpy(out[i].label, g + 8, 4);
                out[i].records = 0;
                ++*outCount;
            }
            out[i].records += records;
            pos = end;
        }
        return kWalkOk;
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
        return kWalkFault;
    }
}

}  // namespace

std::wstring DecodeZString(const uint8_t* data, size_t size) {
    while (size > 0 && data[size - 1] == 0) --size;
    if (size == 0) return std::wstring();
    const char* src = reinterpret_cast<const char*>(data);
    int len = static_cast<int>(std::min<size_t>(size, 0x7fffffff));
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, src, len, nullptr, 0);
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (n <= 0) {
        cp = 1252;
        flags = 0;
        n = MultiByteToWideChar(cp, flags, src, len, nullptr, 0);
        if (n <= 0) return std::wstring();
    }
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(cp, flags, src, len, &out[0], n);
    // Embedded NULs (padding inside the string) would cut the text when drawn.
    std::replace(out.begin(), out.end(), L'\0', L' ');
    return out;
}

std::wstring FlagsText(uint32_t flags) {
    std::wstring s;
    auto add = [&s](const wchar_t* name) {
        if (!s.empty()) s += L", ";
        s += name;
    };
    if (flags & kFlagEsm) add(L"ESM");
    if (flags & kFlagEsl) add(L"ESL");
    if (flags & kFlagLocalized) add(L"Localized");
    return s;
}

std::wstring VersionText(float version) {
    if (!std::isfinite(version)) return L"?";
    wchar_t buf[64];
    swprintf_s(buf, L"%.2f", static_cast<double>(version));
    return buf;
}

PluginHeader ReadPluginHeader(const std::wstring& path) {
    PluginHeader h;
    h.light = HasLightExtension(path);

    Handle file(OpenForRead(path));
    if (!file.valid()) {
        h.error = LastErrorText(GetLastError());
        return h;
    }

    uint8_t rec[24];
    if (!ReadExact(file.get(), rec, sizeof(rec))) {
        h.error = L"File is too small to be a plugin.";
        return h;
    }
    if (!SigIs(rec, "TES4")) {
        h.error = L"Not a Skyrim plugin (no TES4 record).";
        return h;
    }
    uint32_t dataSize = U32(rec + 4);
    h.flags = U32(rec + 8);
    h.light = h.light || (h.flags & kFlagEsl) != 0;
    h.firstGroupOffset = 24ull + dataSize;

    LARGE_INTEGER fileSize{};
    GetFileSizeEx(file.get(), &fileSize);
    uint64_t available = fileSize.QuadPart > 24 ? static_cast<uint64_t>(fileSize.QuadPart) - 24 : 0;
    bool truncated = dataSize > available;
    uint32_t toRead = static_cast<uint32_t>(std::min<uint64_t>(dataSize, available));
    if (toRead > 256u * 1024 * 1024) {
        h.error = L"TES4 record is implausibly large.";
        return h;
    }

    std::vector<uint8_t> data(toRead);
    if (toRead && !ReadExact(file.get(), data.data(), toRead)) {
        h.error = LastErrorText(GetLastError());
        return h;
    }
    h.ok = true;
    if (truncated) h.error = L"File is truncated inside the TES4 record.";

    const uint8_t* p = data.data();
    size_t pos = 0;
    size_t end = data.size();
    bool havePending = false;
    uint32_t pendingSize = 0;
    while (pos + 6 <= end) {
        const uint8_t* sig = p + pos;
        uint32_t size = U16(p + pos + 4);
        pos += 6;
        if (SigIs(sig, "XXXX")) {
            if (pos + 4 <= end) {
                pendingSize = U32(p + pos);
                havePending = true;
            }
            pos += size;
            continue;
        }
        if (havePending) {
            size = pendingSize;
            havePending = false;
        }
        if (pos + size > end) {
            if (h.error.empty()) h.error = L"TES4 subrecord runs past the end of the record.";
            break;
        }
        const uint8_t* payload = p + pos;
        if (SigIs(sig, "HEDR") && size >= 8) {
            uint32_t bits = U32(payload);
            std::memcpy(&h.version, &bits, sizeof(float));
            h.numRecords = static_cast<int32_t>(U32(payload + 4));
            h.hasHedr = true;
        } else if (SigIs(sig, "CNAM")) {
            h.author = DecodeZString(payload, size);
            h.hasAuthor = true;
        } else if (SigIs(sig, "SNAM")) {
            h.description = DecodeZString(payload, size);
            h.hasDescription = true;
        } else if (SigIs(sig, "MAST")) {
            h.masters.push_back(DecodeZString(payload, size));
        }
        pos += size;
    }
    return h;
}

GroupScan ScanGroups(const std::wstring& path, uint64_t firstGroupOffset) {
    GroupScan scan;
    Handle file(OpenForRead(path));
    if (!file.valid()) {
        scan.error = LastErrorText(GetLastError());
        return scan;
    }
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file.get(), &size)) {
        scan.error = LastErrorText(GetLastError());
        return scan;
    }
    uint64_t fileSize = static_cast<uint64_t>(size.QuadPart);
    if (firstGroupOffset >= fileSize) {
        scan.ok = true;  // no GRUPs is legal
        return scan;
    }

    Handle mapping(CreateFileMappingW(file.get(), nullptr, PAGE_READONLY, 0, 0, nullptr));
    if (!mapping.valid()) {
        scan.error = LastErrorText(GetLastError());
        return scan;
    }
    const uint8_t* base = static_cast<const uint8_t*>(MapViewOfFile(mapping.get(), FILE_MAP_READ, 0, 0, 0));
    if (!base) {
        scan.error = LastErrorText(GetLastError());
        return scan;
    }

    std::vector<RawGroup> raw(4096);
    size_t count = 0;
    uint64_t corruptAt = 0;
    WalkResult result = WalkGroups(base, fileSize, firstGroupOffset, raw.data(), raw.size(), &count,
                                   &scan.totalGroups, &corruptAt);
    UnmapViewOfFile(base);

    for (size_t i = 0; i < count; ++i) {
        GroupCount gc;
        for (char c : raw[i].label) gc.label += static_cast<wchar_t>(static_cast<unsigned char>(c));  // Latin-1
        gc.records = raw[i].records;
        scan.totalRecords += gc.records;
        scan.groups.push_back(std::move(gc));
    }
    std::sort(scan.groups.begin(), scan.groups.end(),
              [](const GroupCount& a, const GroupCount& b) { return a.label < b.label; });

    switch (result) {
        case kWalkOk:
            scan.ok = true;
            break;
        case kWalkCorrupt: {
            wchar_t buf[128];
            swprintf_s(buf, L"Malformed data at offset 0x%llX; counts are partial.", corruptAt);
            scan.error = buf;
            break;
        }
        case kWalkTooMany:
            scan.error = L"Too many distinct top-level GRUPs; counts are partial.";
            break;
        case kWalkFault:
            scan.error = L"The file could not be read to the end.";
            break;
    }
    return scan;
}

}  // namespace pp

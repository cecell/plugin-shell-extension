#include "plugin_parser.h"

#include <objidl.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace pse {
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

// Random-access reads from a plugin: a file handle (the dialog) or an IStream
// (the Explorer property handler, which only gets a stream).
class ByteSource {
public:
    virtual ~ByteSource() = default;
    virtual bool Size(uint64_t* size) = 0;
    // Reads up to n bytes at offset; *got is how many arrived.
    virtual bool ReadAt(uint64_t offset, void* buf, size_t n, size_t* got) = 0;
    virtual std::wstring LastError() = 0;
};

class FileSource final : public ByteSource {
public:
    explicit FileSource(HANDLE file) : file_(file) {}
    bool Size(uint64_t* size) override {
        LARGE_INTEGER li{};
        if (!GetFileSizeEx(file_, &li)) return Fail();
        *size = static_cast<uint64_t>(li.QuadPart);
        return true;
    }
    bool ReadAt(uint64_t offset, void* buf, size_t n, size_t* got) override {
        OVERLAPPED ov{};
        ov.Offset = static_cast<DWORD>(offset);
        ov.OffsetHigh = static_cast<DWORD>(offset >> 32);
        DWORD read = 0;
        if (!ReadFile(file_, buf, static_cast<DWORD>(n), &read, &ov) && GetLastError() != ERROR_HANDLE_EOF)
            return Fail();
        *got = read;
        return true;
    }
    std::wstring LastError() override { return LastErrorText(error_); }

private:
    bool Fail() {
        error_ = GetLastError();
        return false;
    }
    HANDLE file_;
    DWORD error_ = 0;
};

class StreamSource final : public ByteSource {
public:
    explicit StreamSource(IStream* stream) : stream_(stream) {}
    bool Size(uint64_t* size) override {
        STATSTG st{};
        HRESULT hr = stream_->Stat(&st, STATFLAG_NONAME);
        if (FAILED(hr)) return Fail(hr);
        *size = st.cbSize.QuadPart;
        return true;
    }
    bool ReadAt(uint64_t offset, void* buf, size_t n, size_t* got) override {
        LARGE_INTEGER move{};
        move.QuadPart = static_cast<LONGLONG>(offset);
        HRESULT hr = stream_->Seek(move, STREAM_SEEK_SET, nullptr);
        if (FAILED(hr)) return Fail(hr);
        ULONG read = 0;
        hr = stream_->Read(buf, static_cast<ULONG>(n), &read);
        if (FAILED(hr)) return Fail(hr);
        *got = read;
        return true;
    }
    std::wstring LastError() override { return LastErrorText(static_cast<DWORD>(hr_)); }

private:
    bool Fail(HRESULT hr) {
        hr_ = hr;
        return false;
    }
    IStream* stream_;
    HRESULT hr_ = S_OK;
};

bool ReadExactAt(ByteSource& src, uint64_t offset, void* buf, size_t n) {
    size_t got = 0;
    return src.ReadAt(offset, buf, n, &got) && got == n;
}

PluginHeader ReadHeader(ByteSource& src, bool lightExtension) {
    PluginHeader h;
    h.light = lightExtension;

    uint8_t rec[24];
    size_t got = 0;
    if (!src.ReadAt(0, rec, sizeof(rec), &got)) {
        h.error = src.LastError();
        return h;
    }
    if (got < sizeof(rec)) {
        h.error = L"File is too small to be a plugin.";
        return h;
    }
    if (!SigIs(rec, "TES4")) {
        h.error = L"Not a Skyrim plugin (no TES4 record).";
        return h;
    }
    uint32_t dataSize = U32(rec + 4);
    h.flags = U32(rec + 8);
    h.formVersion = U16(rec + 20);
    h.light = h.light || (h.flags & kFlagEsl) != 0;
    h.firstGroupOffset = 24ull + dataSize;

    uint64_t fileSize = 0;
    src.Size(&fileSize);
    uint64_t available = fileSize > 24 ? fileSize - 24 : 0;
    bool truncated = dataSize > available;
    uint32_t toRead = static_cast<uint32_t>(std::min<uint64_t>(dataSize, available));
    if (toRead > 256u * 1024 * 1024) {
        h.error = L"TES4 record is implausibly large.";
        return h;
    }

    std::vector<uint8_t> data(toRead);
    if (toRead && !ReadExactAt(src, 24, data.data(), toRead)) {
        h.error = src.LastError();
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

struct RawGroup {
    char label[4];
    uint64_t records;
};

enum WalkResult { kWalkOk = 0, kWalkCorrupt, kWalkTooMany, kWalkFault };

// ESL facts (doc section 10): a record whose FormID's top byte is at least
// the master count is new to this plugin. Overrides and FormID 0 are ignored.
// Both light ranges are tracked; ClassifyEsl picks by HEDR version.
struct EslCheck {
    uint32_t numMasters;
    uint64_t newRecords;
    bool in800;  // every new object ID in 0x800..0xFFF (HEDR < 1.71 range)
    bool in000;  // every new object ID in 0x000..0xFFF (HEDR 1.71+ range)
};

// Deleted-record counts (doc section 11): the types xEdit's "Undelete and
// Disable References" handles for Skyrim SE, the 10 placed-object types plus
// NAVM. A record is deleted when bit 0x20 of its header flags is set.
const char kDeletedTypes[][5] = {"ACHR", "NAVM", "PARW", "PBAR", "PBEA", "PCON",
                                 "PFLA", "PGRE", "PHZD", "PMIS", "REFR"};
constexpr size_t kDeletedTypeCount = sizeof(kDeletedTypes) / sizeof(kDeletedTypes[0]);
constexpr uint32_t kRecordFlagDeleted = 0x20;

struct WalkOutput {
    RawGroup* groups;
    size_t capacity;
    size_t count;
    uint64_t totalGroups;
    EslCheck esl;
    uint64_t deleted[kDeletedTypeCount];
    uint64_t corruptAt;
};

// Readers for WalkCore: At(offset, n) returns n bytes at offset, valid until
// the next call, or nullptr if they cannot be read.
struct MappedReader {
    const uint8_t* base;
    uint64_t size;
    const uint8_t* At(uint64_t offset, size_t n) const { return offset + n <= size ? base + offset : nullptr; }
};

class BufferedReader {
public:
    explicit BufferedReader(ByteSource& src) : src_(src), buf_(256 * 1024) {}
    const uint8_t* At(uint64_t offset, size_t n) {
        if (offset >= start_ && offset + n <= start_ + len_) return buf_.data() + (offset - start_);
        size_t got = 0;
        if (!src_.ReadAt(offset, buf_.data(), buf_.size(), &got) || got < n) {
            len_ = 0;
            return nullptr;
        }
        start_ = offset;
        len_ = got;
        return buf_.data();
    }

private:
    ByteSource& src_;
    std::vector<uint8_t> buf_;
    uint64_t start_ = 0;
    size_t len_ = 0;
};

// Walks every GRUP after the TES4 record. Contents of a nested GRUP directly
// follow its 24-byte header, so one linear pass visits every record at every
// depth: GRUP headers step 24 bytes, records step 24 + data size.
template <class Reader>
WalkResult WalkCore(Reader& reader, uint64_t fileSize, uint64_t start, WalkOutput* out) {
    uint64_t pos = start;
    while (pos + 24 <= fileSize) {
        const uint8_t* g = reader.At(pos, 24);
        if (!g) return kWalkFault;
        if (!SigIs(g, "GRUP")) {
            out->corruptAt = pos;
            return kWalkCorrupt;
        }
        uint64_t groupSize = U32(g + 4);
        char label[4];
        std::memcpy(label, g + 8, 4);  // g is invalidated by the next At()
        if (groupSize < 24 || pos + groupSize > fileSize) {
            out->corruptAt = pos;
            return kWalkCorrupt;
        }
        ++out->totalGroups;

        uint64_t records = 0;
        uint64_t end = pos + groupSize;
        uint64_t p = pos + 24;
        while (p < end) {
            if (p + 24 > end) {
                out->corruptAt = p;
                return kWalkCorrupt;
            }
            const uint8_t* r = reader.At(p, 24);
            if (!r) return kWalkFault;
            uint64_t size = U32(r + 4);
            if (SigIs(r, "GRUP")) {
                if (size < 24 || p + size > end) {
                    out->corruptAt = p;
                    return kWalkCorrupt;
                }
                ++out->totalGroups;
                p += 24;
                continue;
            }
            if (p + 24 + size > end) {
                out->corruptAt = p;
                return kWalkCorrupt;
            }
            ++records;
            uint32_t formId = U32(r + 12);
            if (formId != 0 && (formId >> 24) >= out->esl.numMasters) {
                uint32_t objectId = formId & 0x00FFFFFF;
                ++out->esl.newRecords;
                if (objectId > 0xFFF) out->esl.in000 = out->esl.in800 = false;
                else if (objectId < 0x800) out->esl.in800 = false;
            }
            if (U32(r + 8) & kRecordFlagDeleted) {
                for (size_t t = 0; t < kDeletedTypeCount; ++t) {
                    if (SigIs(r, kDeletedTypes[t])) {
                        ++out->deleted[t];
                        break;
                    }
                }
            }
            p += 24 + size;
        }

        size_t i = 0;
        for (; i < out->count; ++i) {
            if (std::memcmp(out->groups[i].label, label, 4) == 0) break;
        }
        if (i == out->count) {
            if (out->count == out->capacity) return kWalkTooMany;
            std::memcpy(out->groups[i].label, label, 4);
            out->groups[i].records = 0;
            ++out->count;
        }
        out->groups[i].records += records;
        pos = end;
    }
    return kWalkOk;
}

// The mapped walk runs under SEH: a read through the view can fault (file
// truncated meanwhile, network share drops). No objects with destructors here.
WalkResult WalkMapped(const uint8_t* base, uint64_t fileSize, uint64_t start, WalkOutput* out) {
    __try {
        MappedReader reader{base, fileSize};
        return WalkCore(reader, fileSize, start, out);
    } __except (GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER
                                                               : EXCEPTION_CONTINUE_SEARCH) {
        return kWalkFault;
    }
}

void InitWalk(const PluginHeader& header, std::vector<RawGroup>& raw, WalkOutput* out) {
    *out = WalkOutput{};
    out->groups = raw.data();
    out->capacity = raw.size();
    out->esl = EslCheck{static_cast<uint32_t>(header.masters.size()), 0, true, true};
}

void FinishScan(const WalkOutput& out, WalkResult result, GroupScan* scan) {
    scan->totalGroups = out.totalGroups;
    scan->newRecords = out.esl.newRecords;
    scan->newIdsIn800 = out.esl.in800;
    scan->newIdsIn000 = out.esl.in000;
    for (size_t t = 0; t < kDeletedTypeCount; ++t) {  // kDeletedTypes is already alphabetical
        if (!out.deleted[t]) continue;
        GroupCount dc;
        for (size_t c = 0; c < 4; ++c) dc.label += static_cast<wchar_t>(kDeletedTypes[t][c]);
        dc.records = out.deleted[t];
        scan->totalDeleted += dc.records;
        if (dc.label == L"NAVM") scan->deletedNavm = dc.records;
        scan->deleted.push_back(std::move(dc));
    }
    for (size_t i = 0; i < out.count; ++i) {
        GroupCount gc;
        for (char c : out.groups[i].label) gc.label += static_cast<wchar_t>(static_cast<unsigned char>(c));  // Latin-1
        gc.records = out.groups[i].records;
        scan->totalRecords += gc.records;
        scan->groups.push_back(std::move(gc));
    }
    std::sort(scan->groups.begin(), scan->groups.end(),
              [](const GroupCount& a, const GroupCount& b) { return a.label < b.label; });

    switch (result) {
        case kWalkOk:
            scan->ok = true;
            break;
        case kWalkCorrupt: {
            wchar_t buf[128];
            swprintf_s(buf, L"Malformed data at offset 0x%llX; counts are partial.", out.corruptAt);
            scan->error = buf;
            break;
        }
        case kWalkTooMany:
            scan->error = L"Too many distinct top-level GRUPs; counts are partial.";
            break;
        case kWalkFault:
            scan->error = L"The file could not be read to the end.";
            break;
    }
}

// HEDR 1.71+ (CK for game 1.6.1130+) may use object IDs from 0x000 (4,096);
// older headers only from 0x800 (2,048).
bool HasNewIdRange(const PluginHeader& header) {
    return header.hasHedr && std::isfinite(header.version) && std::lround(header.version * 100.0f) >= 171;
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

EslStatus ClassifyEsl(const PluginHeader& header, const GroupScan& scan) {
    if (header.light) return EslStatus::Already;
    if (!scan.ok) return EslStatus::Corrupted;
    uint64_t n = scan.newRecords;
    // Plugins that would need compacting (renumbering) count as False: that
    // is an advanced step, not something to suggest.
    if (HasNewIdRange(header)) return n <= 4096 && scan.newIdsIn000 ? EslStatus::True : EslStatus::False;
    // Header < 1.71. Resaving in the newer CK gives it a 1.71 header, which
    // opens 0x000..0x7FF, so "Save in CK" fixes IDs that already fit there.
    if (n <= 2048 && scan.newIdsIn800) return EslStatus::True;
    if (n <= 4096 && scan.newIdsIn000) return EslStatus::SaveInCk;
    return EslStatus::False;
}

bool EslNeedsNewerGame(const PluginHeader& header, const GroupScan& scan, EslStatus status) {
    switch (status) {
        case EslStatus::True:  // a 1.71 plugin already using IDs below 0x800
            return HasNewIdRange(header) && !scan.newIdsIn800;
        case EslStatus::SaveInCk:  // ends with a 1.71 header and IDs below 0x800
            return true;
        default:
            return false;
    }
}

const wchar_t* EslStatusText(EslStatus status, bool shortForm) {
    switch (status) {
        case EslStatus::Already: return shortForm ? L"Already" : L"Already flagged";
        case EslStatus::True: return L"True";
        case EslStatus::SaveInCk: return L"Save in CK";
        case EslStatus::False: return L"False";
        case EslStatus::Corrupted: break;
    }
    return L"Corrupted";
}

PluginHeader ReadPluginHeader(const std::wstring& path) {
    Handle file(OpenForRead(path));
    if (!file.valid()) {
        PluginHeader h;
        h.error = LastErrorText(GetLastError());
        return h;
    }
    FileSource src(file.get());
    return ReadHeader(src, HasLightExtension(path));
}

PluginHeader ReadPluginHeader(IStream* stream, const std::wstring& fileName) {
    StreamSource src(stream);
    return ReadHeader(src, HasLightExtension(fileName));
}

GroupScan ScanGroups(const std::wstring& path, const PluginHeader& header) {
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
    if (header.firstGroupOffset >= fileSize) {
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
    WalkOutput out;
    InitWalk(header, raw, &out);
    WalkResult result = WalkMapped(base, fileSize, header.firstGroupOffset, &out);
    UnmapViewOfFile(base);
    FinishScan(out, result, &scan);
    return scan;
}

GroupScan ScanGroups(IStream* stream, const PluginHeader& header) {
    GroupScan scan;
    StreamSource src(stream);
    uint64_t fileSize = 0;
    if (!src.Size(&fileSize)) {
        scan.error = src.LastError();
        return scan;
    }
    if (header.firstGroupOffset >= fileSize) {
        scan.ok = true;
        return scan;
    }
    std::vector<RawGroup> raw(4096);
    WalkOutput out;
    InitWalk(header, raw, &out);
    BufferedReader reader(src);
    WalkResult result = WalkCore(reader, fileSize, header.firstGroupOffset, &out);
    FinishScan(out, result, &scan);
    return scan;
}

}  // namespace pse

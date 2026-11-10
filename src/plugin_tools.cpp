#include "plugin_tools.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace pse {
namespace {

// The record header is 24 bytes, then "HEDR", a 2-byte size (12), then the version.
constexpr DWORD kFormVersionOffset = 20, kHedrSignature = 24, kHedrSize = 28, kVersionOffset = 30, kPrefixBytes = 34;
constexpr uint16_t kSeFormVersion = 44;
constexpr uint32_t kVersion170 = 0x3FD9999A, kVersion171 = 0x3FDAE148;  // the floats 1.70 and 1.71

std::wstring NameOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

class File {
public:
    explicit File(HANDLE handle) : handle_(handle) {}
    ~File() {
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    HANDLE get() const { return handle_; }

private:
    HANDLE handle_;
};

bool ReadAt(HANDLE file, DWORD offset, void* data, DWORD size) {
    OVERLAPPED at{};
    at.Offset = offset;
    DWORD done = 0;
    return ReadFile(file, data, size, &done, &at) && done == size;
}

}  // namespace

HeaderUpgradeResult UpgradePluginHeader(const std::wstring& path) {
    HeaderUpgradeResult result;
    const std::wstring name = NameOf(path);
    HANDLE opened = INVALID_HANDLE_VALUE;
    DWORD error = 0;
    // Explorer may be reading the file for a moment to fill in its columns.
    for (int attempt = 0; attempt < 8; ++attempt) {
        opened = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        error = GetLastError();
        if (opened != INVALID_HANDLE_VALUE || error != ERROR_SHARING_VIOLATION) break;
        Sleep(150);
    }
    File file(opened);
    if (opened == INVALID_HANDLE_VALUE) {
        result.text = name + (error == ERROR_ACCESS_DENIED ? L": it's read-only or you don't have permission to change it." : error == ERROR_SHARING_VIOLATION ? L": another program has it open." : L": it couldn't be opened.");
        return result;
    }
    unsigned char prefix[kPrefixBytes] = {};
    uint16_t hedrSize = 0;
    if (!ReadAt(opened, 0, prefix, kPrefixBytes) || memcmp(prefix, "TES4", 4) != 0 || memcmp(prefix + kHedrSignature, "HEDR", 4) != 0 ||
        (memcpy(&hedrSize, prefix + kHedrSize, sizeof(hedrSize)), hedrSize != 12)) {
        result.status = 2;
        result.text = name + L": this isn't a Skyrim plugin. Nothing was changed.";
        return result;
    }
    uint16_t formVersion = 0;
    memcpy(&formVersion, prefix + kFormVersionOffset, sizeof(formVersion));
    if (formVersion < kSeFormVersion) {
        result.status = 2;
        result.formTooOld = true;
        result.text = name + L": it's form version " + std::to_wstring(formVersion) + L". Anything below form version 44 can't be upgraded to 1.71, which is AE. Nothing was changed.";
        return result;
    }
    if (formVersion != kSeFormVersion) {
        result.status = 2;
        result.text = name + L": it's form version " + std::to_wstring(formVersion) + L", not a Skyrim SE plugin. Nothing was changed.";
        return result;
    }
    uint32_t version = 0;
    memcpy(&version, prefix + kVersionOffset, sizeof(version));
    if (version == kVersion171) {
        result.status = 1;
        result.text = name + L": the header is already 1.71.";
        return result;
    }
    if (version != kVersion170) {
        float value = 0;
        memcpy(&value, &version, sizeof(value));
        wchar_t shown[32];
        swprintf_s(shown, L"%.2f", value);
        result.status = 2;
        result.text = name + L": the header is " + shown + L". Only 1.70 can be upgraded, so nothing was changed.";
        return result;
    }
    OVERLAPPED at{};
    at.Offset = kVersionOffset;
    DWORD written = 0;
    if (!WriteFile(opened, &kVersion171, sizeof(kVersion171), &written, &at) || written != sizeof(kVersion171) || !FlushFileBuffers(opened)) {
        result.text = name + L": the new version number couldn't be written.";
        return result;
    }
    uint32_t check = 0;
    if (!ReadAt(opened, kVersionOffset, &check, sizeof(check)) || check != kVersion171) {
        result.text = name + L": the new version number couldn't be confirmed.";
        return result;
    }
    result.status = 0;
    result.text = name + L": upgraded from 1.70 to 1.71.";
    return result;
}

}  // namespace pse

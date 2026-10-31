#include "registration.h"

#include <shlobj.h>
#include <shlwapi.h>

namespace pp {
namespace {

const wchar_t kClasses[] = L"Software\\Classes\\";
const wchar_t kApproved[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Shell Extensions\\Approved";

LSTATUS SetString(HKEY root, const std::wstring& subkey, const wchar_t* name, const std::wstring& value) {
    HKEY key = nullptr;
    LSTATUS st = RegCreateKeyExW(root, subkey.c_str(), 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
    if (st != ERROR_SUCCESS) return st;
    st = RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()),
                        static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
    return st;
}

std::wstring HandlerKey(const wchar_t* ext) {
    return std::wstring(kClasses) + L"SystemFileAssociations\\" + ext + L"\\shellex\\ContextMenuHandlers\\" +
           kHandlerName;
}

// Removes subkey and then its parents up to (not including) stopAt, each only
// while it has no values and no subkeys left, so keys other software created
// or shares are never touched.
void DeleteEmptyKeys(HKEY root, std::wstring subkey, const std::wstring& stopAt) {
    while (subkey.size() > stopAt.size()) {
        HKEY key = nullptr;
        if (RegOpenKeyExW(root, subkey.c_str(), 0, KEY_READ, &key) == ERROR_SUCCESS) {
            DWORD subkeys = 0, values = 0;
            LSTATUS st = RegQueryInfoKeyW(key, nullptr, nullptr, nullptr, &subkeys, nullptr, nullptr, &values,
                                          nullptr, nullptr, nullptr, nullptr);
            RegCloseKey(key);
            if (st != ERROR_SUCCESS || subkeys || values) return;
            if (RegDeleteKeyW(root, subkey.c_str()) != ERROR_SUCCESS) return;
        }
        size_t slash = subkey.find_last_of(L'\\');
        if (slash == std::wstring::npos) return;
        subkey.resize(slash);
    }
}

}  // namespace

HRESULT RegisterHandler(HKEY root, const std::wstring& dllPath) {
    std::wstring clsidKey = std::wstring(kClasses) + L"CLSID\\" + kClsidString;
    LSTATUS st = SetString(root, clsidKey, nullptr, kHandlerDescription);
    if (st == ERROR_SUCCESS) st = SetString(root, clsidKey + L"\\InprocServer32", nullptr, dllPath);
    if (st == ERROR_SUCCESS) st = SetString(root, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    for (const wchar_t* ext : kExtensions) {
        if (st == ERROR_SUCCESS) st = SetString(root, HandlerKey(ext), nullptr, kClsidString);
    }
    // Only consulted (machine-wide) when the EnforceShellExtensionSecurity
    // policy is on; harmless otherwise.
    if (st == ERROR_SUCCESS && root == HKEY_LOCAL_MACHINE)
        st = SetString(root, kApproved, kClsidString, kHandlerDescription);
    if (st != ERROR_SUCCESS) return HRESULT_FROM_WIN32(st);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

HRESULT UnregisterHandler(HKEY root) {
    for (const wchar_t* ext : kExtensions) {
        std::wstring key = HandlerKey(ext);
        RegDeleteTreeW(root, key.c_str());
        RegDeleteKeyW(root, key.c_str());
        DeleteEmptyKeys(root, key.substr(0, key.find_last_of(L'\\')),
                        std::wstring(kClasses) + L"SystemFileAssociations");
    }
    RegDeleteTreeW(root, (std::wstring(kClasses) + L"CLSID\\" + kClsidString).c_str());
    if (root == HKEY_LOCAL_MACHINE) RegDeleteKeyValueW(root, kApproved, kClsidString);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

}  // namespace pp

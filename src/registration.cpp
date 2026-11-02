#include "registration.h"

#include <propsys.h>
#include <shlobj.h>
#include <shlwapi.h>

namespace pse {
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

namespace {
const wchar_t kPropertyHandlers[] = L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\PropertySystem\\PropertyHandlers\\";
}

HRESULT RegisterPropertyHandler(const std::wstring& dllPath, const std::wstring& schemaPath) {
    // The schema first: it fails cleanly (bad XML, not elevated) before any
    // key is written.
    HRESULT hr = PSRegisterPropertySchema(schemaPath.c_str());
    if (FAILED(hr)) return hr;
    // CLSID in HKLM as well: the Windows Search indexer runs without this
    // user's HKCU and also loads property handlers.
    std::wstring clsidKey = std::wstring(kClasses) + L"CLSID\\" + kPropertyHandlerClsidString;
    LSTATUS st = SetString(HKEY_LOCAL_MACHINE, clsidKey, nullptr, L"Plugin Shell Extension property handler");
    if (st == ERROR_SUCCESS) st = SetString(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", nullptr, dllPath);
    if (st == ERROR_SUCCESS)
        st = SetString(HKEY_LOCAL_MACHINE, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Both");
    for (const wchar_t* ext : kExtensions) {
        if (st == ERROR_SUCCESS)
            st = SetString(HKEY_LOCAL_MACHINE, std::wstring(kPropertyHandlers) + ext, nullptr,
                           kPropertyHandlerClsidString);
    }
    if (st != ERROR_SUCCESS) {
        UnregisterPropertyHandler(schemaPath);
        return HRESULT_FROM_WIN32(st);
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

HRESULT UnregisterPropertyHandler(const std::wstring& schemaPath) {
    for (const wchar_t* ext : kExtensions) {
        std::wstring key = std::wstring(kPropertyHandlers) + ext;
        // Only remove the association if it is still ours.
        wchar_t value[64] = {};
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value, &size) ==
                ERROR_SUCCESS &&
            _wcsicmp(value, kPropertyHandlerClsidString) == 0)
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, key.c_str());
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(kClasses) + L"CLSID\\" + kPropertyHandlerClsidString).c_str());
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, (std::wstring(kClasses) + L"CLSID\\" + kPropertyHandlerClsidString).c_str());
    PSUnregisterPropertySchema(schemaPath.c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

}  // namespace pse

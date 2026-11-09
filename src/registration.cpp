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

namespace {
// Windows' own "open this as a folder" command, the one zip files use.
const wchar_t kExecuteFolder[] = L"{11dbb47c-a525-400b-9e80-a54615a090c0}";
const wchar_t kExplorerCommand[] = L"%SystemRoot%\\Explorer.exe /idlist,%I,%L";
const wchar_t kBsaAppName[] = L"Plugin Shell Extension";

// The command holds %SystemRoot%, so it has to be stored as an expandable string, as zip's is.
LSTATUS SetExplorerCommand(HKEY root, const std::wstring& commandKey) {
    return RegSetKeyValueW(root, commandKey.c_str(), nullptr, REG_EXPAND_SZ, kExplorerCommand, sizeof(kExplorerCommand));
}
}  // namespace

namespace {
LSTATUS WriteHandler(HKEY root, const std::wstring& clsidKey, const wchar_t* name, const std::wstring& dllPath) {
    LSTATUS st = SetString(root, clsidKey, nullptr, name);
    if (st == ERROR_SUCCESS) st = SetString(root, clsidKey + L"\\InprocServer32", nullptr, dllPath);
    if (st == ERROR_SUCCESS) st = SetString(root, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    return st;
}
}  // namespace

bool InstallHashMenu(HKEY root, const std::wstring& classes, const std::wstring& dllPath) {
    const std::wstring clsid = classes + L"CLSID\\" + kHashMenuClsidString;
    LSTATUS st = WriteHandler(root, clsid, L"Plugin Shell Extension hashes menu", dllPath);
    if (st == ERROR_SUCCESS) st = SetString(root, classes + L"*\\shellex\\ContextMenuHandlers\\PluginShellExtensionHashes", nullptr, kHashMenuClsidString);
    if (st != ERROR_SUCCESS) RemoveHashMenu(root, classes);
    return st == ERROR_SUCCESS;
}

void RemoveHashMenu(HKEY root, const std::wstring& classes) {
    RegDeleteTreeW(root, (classes + L"*\\shellex\\ContextMenuHandlers\\PluginShellExtensionHashes").c_str());
    RegDeleteKeyW(root, (classes + L"*\\shellex\\ContextMenuHandlers\\PluginShellExtensionHashes").c_str());
    const std::wstring clsid = classes + L"CLSID\\" + kHashMenuClsidString;
    RegDeleteTreeW(root, clsid.c_str());
    RegDeleteKeyW(root, clsid.c_str());
}

bool InstallFolderViewsMenu(HKEY root, const std::wstring& classes, const std::wstring& dllPath) {
    const std::wstring clsid = classes + L"CLSID\\" + kFolderViewsMenuClsidString;
    LSTATUS st = WriteHandler(root, clsid, L"Plugin Shell Extension folder views menu", dllPath);
    if (st == ERROR_SUCCESS) st = SetString(root, classes + L"Directory\\Background\\shellex\\ContextMenuHandlers\\PluginShellExtensionFolderViews", nullptr, kFolderViewsMenuClsidString);
    if (st != ERROR_SUCCESS) RemoveFolderViewsMenu(root, classes);
    return st == ERROR_SUCCESS;
}

void RemoveFolderViewsMenu(HKEY root, const std::wstring& classes) {
    const std::wstring key = classes + L"Directory\\Background\\shellex\\ContextMenuHandlers\\PluginShellExtensionFolderViews";
    RegDeleteTreeW(root, key.c_str());
    RegDeleteKeyW(root, key.c_str());
    const std::wstring clsid = classes + L"CLSID\\" + kFolderViewsMenuClsidString;
    RegDeleteTreeW(root, clsid.c_str());
    RegDeleteKeyW(root, clsid.c_str());
}

bool InstallPapyrusMenu(HKEY root, const std::wstring& classes, const std::wstring& dllPath) {
    const std::wstring clsid = classes + L"CLSID\\" + kPapyrusMenuClsidString;
    LSTATUS st = WriteHandler(root, clsid, L"Plugin Shell Extension Papyrus menu", dllPath);
    for (const wchar_t* ext : {L".pex", L".psc"}) {
        if (st == ERROR_SUCCESS) st = SetString(root, classes + L"SystemFileAssociations\\" + ext + L"\\shellex\\ContextMenuHandlers\\PluginShellExtensionPapyrus", nullptr, kPapyrusMenuClsidString);
    }
    if (st != ERROR_SUCCESS) RemovePapyrusMenu(root, classes);
    return st == ERROR_SUCCESS;
}

void RemovePapyrusMenu(HKEY root, const std::wstring& classes) {
    for (const wchar_t* ext : {L".pex", L".psc"}) {
        const std::wstring key = classes + L"SystemFileAssociations\\" + ext + L"\\shellex\\ContextMenuHandlers\\PluginShellExtensionPapyrus";
        RegDeleteTreeW(root, key.c_str());
        RegDeleteKeyW(root, key.c_str());
    }
    const std::wstring clsid = classes + L"CLSID\\" + kPapyrusMenuClsidString;
    RegDeleteTreeW(root, clsid.c_str());
    RegDeleteKeyW(root, clsid.c_str());
}

bool InstallBsaBrowser(HKEY root, const std::wstring& classes, const std::wstring& ext, const std::wstring& dllPath) {
    const std::wstring clsid = classes + L"CLSID\\" + kBsaFolderClsidString;
    const std::wstring prog = classes + kBsaProgId;
    const std::wstring extKey = classes + ext;
    const std::wstring verb = classes + L"SystemFileAssociations\\" + ext + L"\\shell\\BrowseContents";
    LSTATUS st = SetString(root, clsid, nullptr, L"Plugin Shell Extension BSA folder");
    if (st == ERROR_SUCCESS) st = SetString(root, clsid + L"\\InprocServer32", nullptr, dllPath);
    if (st == ERROR_SUCCESS) st = SetString(root, clsid + L"\\InprocServer32", L"ThreadingModel", L"Apartment");
    if (st == ERROR_SUCCESS) {
        const DWORD attributes = SFGAO_FOLDER | SFGAO_BROWSABLE;
        st = RegSetKeyValueW(root, (clsid + L"\\ShellFolder").c_str(), L"Attributes", REG_DWORD, &attributes, sizeof(attributes));
    }
    // Without this category, Windows never treats the file as a folder.
    if (st == ERROR_SUCCESS) st = SetString(root, clsid + L"\\Implemented Categories\\{00021490-0000-0000-C000-000000000046}", nullptr, L"");
    if (st == ERROR_SUCCESS) st = SetString(root, prog, nullptr, L"Bethesda archive");
    if (st == ERROR_SUCCESS) st = SetString(root, prog + L"\\CLSID", nullptr, kBsaFolderClsidString);
    // The same icon as a zip file. Without it Explorer draws a plain folder.
    if (st == ERROR_SUCCESS) {
        static const wchar_t icon[] = L"%SystemRoot%\\system32\\zipfldr.dll";
        st = RegSetKeyValueW(root, (prog + L"\\DefaultIcon").c_str(), nullptr, REG_EXPAND_SZ, icon, sizeof(icon));
    }
    // Windows would otherwise name the program after ExplorerFrame.dll, which runs the open verb.
    if (st == ERROR_SUCCESS) st = SetString(root, prog + L"\\shell\\open", L"FriendlyAppName", kBsaAppName);
    if (st == ERROR_SUCCESS) st = SetString(root, prog + L"\\Application", L"ApplicationName", kBsaAppName);
    if (st == ERROR_SUCCESS) st = SetExplorerCommand(root, prog + L"\\shell\\open\\command");
    if (st == ERROR_SUCCESS) st = SetString(root, prog + L"\\shell\\open\\command", L"DelegateExecute", kExecuteFolder);
    const std::wstring managerClsid = classes + L"CLSID\\" + kBsaManagerClsidString;
    const std::wstring manager = classes + L"SystemFileAssociations\\" + ext + L"\\shellex\\ContextMenuHandlers\\PluginShellExtensionBsaManager";
    if (st == ERROR_SUCCESS) st = WriteHandler(root, managerClsid, L"Plugin Shell Extension BSA manager", dllPath);
    if (st == ERROR_SUCCESS) st = SetString(root, manager, nullptr, kBsaManagerClsidString);
    if (st == ERROR_SUCCESS) st = SetString(root, verb, nullptr, L"Browse contents");
    if (st == ERROR_SUCCESS) st = SetExplorerCommand(root, verb + L"\\command");
    if (st == ERROR_SUCCESS) st = SetString(root, verb + L"\\command", L"DelegateExecute", kExecuteFolder);
    wchar_t current[256] = {};
    DWORD size = sizeof(current);
    const bool had = RegGetValueW(root, extKey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, current, &size) == ERROR_SUCCESS && current[0];
    if (st == ERROR_SUCCESS && had && _wcsicmp(current, kBsaProgId) != 0) st = SetString(root, prog, kPreviousProgIdValue, current);
    if (st == ERROR_SUCCESS) st = SetString(root, extKey, nullptr, kBsaProgId);
    // A new OpenWithProgids entry makes Windows ask which program to use, so
    // the extension's default is all that gets set. Earlier versions wrote one.
    RegDeleteKeyValueW(root, (extKey + L"\\OpenWithProgids").c_str(), kBsaProgId);
    RegDeleteKeyW(root, (extKey + L"\\OpenWithProgids").c_str());
    if (st != ERROR_SUCCESS) {
        RemoveBsaBrowser(root, classes, ext);
        return false;
    }
    return true;
}

void RemoveBsaBrowser(HKEY root, const std::wstring& classes, const std::wstring& ext) {
    const std::wstring clsid = classes + L"CLSID\\" + kBsaFolderClsidString;
    const std::wstring prog = classes + kBsaProgId;
    const std::wstring extKey = classes + ext;
    const std::wstring verb = classes + L"SystemFileAssociations\\" + ext + L"\\shell\\BrowseContents";
    wchar_t current[256] = {};
    DWORD size = sizeof(current);
    if (RegGetValueW(root, extKey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, current, &size) == ERROR_SUCCESS && _wcsicmp(current, kBsaProgId) == 0) {
        wchar_t previous[256] = {};
        size = sizeof(previous);
        if (RegGetValueW(root, prog.c_str(), kPreviousProgIdValue, RRF_RT_REG_SZ, nullptr, previous, &size) == ERROR_SUCCESS && previous[0])
            SetString(root, extKey, nullptr, previous);
        else
            RegDeleteKeyValueW(root, extKey.c_str(), nullptr);
    }
    RegDeleteKeyValueW(root, (extKey + L"\\OpenWithProgids").c_str(), kBsaProgId);
    const std::wstring manager = classes + L"SystemFileAssociations\\" + ext + L"\\shellex\\ContextMenuHandlers\\PluginShellExtensionBsaManager";
    RegDeleteTreeW(root, manager.c_str());
    RegDeleteKeyW(root, manager.c_str());
    RegDeleteTreeW(root, (classes + L"CLSID\\" + kBsaManagerClsidString).c_str());
    RegDeleteKeyW(root, (classes + L"CLSID\\" + kBsaManagerClsidString).c_str());
    RegDeleteTreeW(root, verb.c_str());
    RegDeleteKeyW(root, verb.c_str());
    RegDeleteTreeW(root, prog.c_str());
    RegDeleteKeyW(root, prog.c_str());
    RegDeleteTreeW(root, clsid.c_str());
    RegDeleteKeyW(root, clsid.c_str());
}

bool InstallDdsHandler(HKEY root, const std::wstring& classes, const std::wstring& handlers, const std::wstring& dllPath) {
    const std::wstring handlerKey = handlers + L".dds";
    const std::wstring clsidKey = classes + L"CLSID\\" + kDdsPropertyHandlerClsidString;
    wchar_t current[64] = {};
    DWORD size = sizeof(current);
    const bool had = RegGetValueW(root, handlerKey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, current, &size) == ERROR_SUCCESS && current[0];
    const bool alreadyOurs = had && _wcsicmp(current, kDdsPropertyHandlerClsidString) == 0;
    LSTATUS st = SetString(root, clsidKey, nullptr, L"Plugin Shell Extension .dds property handler");
    if (st == ERROR_SUCCESS) st = SetString(root, clsidKey + L"\\InprocServer32", nullptr, dllPath);
    if (st == ERROR_SUCCESS) st = SetString(root, clsidKey + L"\\InprocServer32", L"ThreadingModel", L"Both");
    // On a reinstall the handler is already ours: keep what was recorded the first time.
    if (st == ERROR_SUCCESS && had && !alreadyOurs) st = SetString(root, clsidKey, kWrappedHandlerValue, current);
    if (st == ERROR_SUCCESS) st = SetString(root, handlerKey, nullptr, kDdsPropertyHandlerClsidString);
    if (st != ERROR_SUCCESS && !alreadyOurs) {
        RegDeleteTreeW(root, clsidKey.c_str());
        RegDeleteKeyW(root, clsidKey.c_str());
        return false;
    }
    return st == ERROR_SUCCESS;
}

void RemoveDdsHandler(HKEY root, const std::wstring& classes, const std::wstring& handlers) {
    const std::wstring handlerKey = handlers + L".dds";
    const std::wstring clsidKey = classes + L"CLSID\\" + kDdsPropertyHandlerClsidString;
    wchar_t current[64] = {};
    DWORD size = sizeof(current);
    if (RegGetValueW(root, handlerKey.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, current, &size) == ERROR_SUCCESS &&
        _wcsicmp(current, kDdsPropertyHandlerClsidString) == 0) {
        wchar_t wrapped[64] = {};
        size = sizeof(wrapped);
        if (RegGetValueW(root, clsidKey.c_str(), kWrappedHandlerValue, RRF_RT_REG_SZ, nullptr, wrapped, &size) == ERROR_SUCCESS && wrapped[0])
            SetString(root, handlerKey, nullptr, wrapped);
        else
            RegDeleteKeyW(root, handlerKey.c_str());
    }
    RegDeleteTreeW(root, clsidKey.c_str());
    RegDeleteKeyW(root, clsidKey.c_str());
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
    // The handler is initialized with a file path (IInitializeWithFile), which
    // Windows only allows for handlers that run in the calling process.
    if (st == ERROR_SUCCESS) {
        DWORD one = 1;
        st = RegSetKeyValueW(HKEY_LOCAL_MACHINE, clsidKey.c_str(), L"DisableProcessIsolation", REG_DWORD, &one,
                             sizeof(one));
    }
    for (const wchar_t* ext : kExtensions) {
        if (st == ERROR_SUCCESS)
            st = SetString(HKEY_LOCAL_MACHINE, std::wstring(kPropertyHandlers) + ext, nullptr,
                           kPropertyHandlerClsidString);
    }
    // The asset handler reads a stream, so Windows can keep it out of Explorer's
    // process. An extension another handler already owns is left alone.
    const std::wstring assetKey = std::wstring(kClasses) + L"CLSID\\" + kAssetPropertyHandlerClsidString;
    if (st == ERROR_SUCCESS) st = SetString(HKEY_LOCAL_MACHINE, assetKey, nullptr, L"Plugin Shell Extension asset property handler");
    if (st == ERROR_SUCCESS) st = SetString(HKEY_LOCAL_MACHINE, assetKey + L"\\InprocServer32", nullptr, dllPath);
    if (st == ERROR_SUCCESS) st = SetString(HKEY_LOCAL_MACHINE, assetKey + L"\\InprocServer32", L"ThreadingModel", L"Both");
    for (const wchar_t* ext : kAssetExtensions) {
        wchar_t current[64] = {};
        DWORD size = sizeof(current);
        const std::wstring key = std::wstring(kPropertyHandlers) + ext;
        const bool taken = RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, current, &size) == ERROR_SUCCESS &&
                           current[0] && _wcsicmp(current, kAssetPropertyHandlerClsidString) != 0;
        if (st == ERROR_SUCCESS && !taken) st = SetString(HKEY_LOCAL_MACHINE, key, nullptr, kAssetPropertyHandlerClsidString);
    }
    if (st != ERROR_SUCCESS) {
        UnregisterPropertyHandler(schemaPath);
        return HRESULT_FROM_WIN32(st);
    }
    // Not fatal: the plugin columns still work if .dds can't be taken over.
    InstallDdsHandler(HKEY_LOCAL_MACHINE, kClasses, kPropertyHandlers, dllPath);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

HRESULT UnregisterPropertyHandler(const std::wstring& schemaPath) {
    RemoveDdsHandler(HKEY_LOCAL_MACHINE, kClasses, kPropertyHandlers);
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
    for (const wchar_t* ext : kAssetExtensions) {
        std::wstring key = std::wstring(kPropertyHandlers) + ext;
        wchar_t value[64] = {};
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS &&
            _wcsicmp(value, kAssetPropertyHandlerClsidString) == 0)
            RegDeleteKeyW(HKEY_LOCAL_MACHINE, key.c_str());
    }
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(kClasses) + L"CLSID\\" + kAssetPropertyHandlerClsidString).c_str());
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, (std::wstring(kClasses) + L"CLSID\\" + kAssetPropertyHandlerClsidString).c_str());
    RegDeleteTreeW(HKEY_LOCAL_MACHINE, (std::wstring(kClasses) + L"CLSID\\" + kPropertyHandlerClsidString).c_str());
    RegDeleteKeyW(HKEY_LOCAL_MACHINE, (std::wstring(kClasses) + L"CLSID\\" + kPropertyHandlerClsidString).c_str());
    PSUnregisterPropertySchema(schemaPath.c_str());
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

}  // namespace pse

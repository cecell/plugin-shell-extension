// Registry layout of the context-menu handler, used by the DLL
// (DllRegisterServer / DllInstall). installer/PluginShellExtension.iss writes the
// same keys; keep the two in step.
#pragma once

#include <windows.h>
#include <string>

namespace pse {

// {1D4348E0-3A45-489E-8022-6A2A883E9D94}
inline constexpr GUID kClsidContextMenu = {0x1d4348e0, 0x3a45, 0x489e, {0x80, 0x22, 0x6a, 0x2a, 0x88, 0x3e, 0x9d, 0x94}};
inline constexpr wchar_t kClsidString[] = L"{1D4348E0-3A45-489E-8022-6A2A883E9D94}";
inline constexpr wchar_t kHandlerName[] = L"PluginShellExtension";
inline constexpr wchar_t kHandlerDescription[] = L"WHAM context menu";
inline constexpr const wchar_t* kExtensions[] = {L".esp", L".esm", L".esl"};
// The asset property handler. Not .dds, .xwm or .wav: other handlers own those.
inline constexpr const wchar_t* kAssetExtensions[] = {L".tga", L".fuz"};

// root is HKEY_CURRENT_USER (per-user) or HKEY_LOCAL_MACHINE (all users).
HRESULT RegisterHandler(HKEY root, const std::wstring& dllPath);
HRESULT UnregisterHandler(HKEY root);

// Property handler: the Explorer columns / details pane / tooltip fields.
// {E0BA20B2-0391-45F5-80C3-07E306E945E9}
inline constexpr GUID kClsidPropertyHandler = {0xe0ba20b2, 0x0391, 0x45f5, {0x80, 0xc3, 0x07, 0xe3, 0x06, 0xe9, 0x45, 0xe9}};
inline constexpr wchar_t kPropertyHandlerClsidString[] = L"{E0BA20B2-0391-45F5-80C3-07E306E945E9}";

// {D19B8F6E-787D-498A-8D3D-E90A027561D1}
inline constexpr GUID kClsidAssetPropertyHandler = {0xd19b8f6e, 0x787d, 0x498a, {0x8d, 0x3d, 0xe9, 0x0a, 0x02, 0x75, 0x61, 0xd1}};
inline constexpr wchar_t kAssetPropertyHandlerClsidString[] = L"{D19B8F6E-787D-498A-8D3D-E90A027561D1}";

// .dds: passes everything through to the handler that was registered before it,
// and adds Mipmaps.
// {7C1F4E52-93A6-4B0D-A4E7-5D2C8F6B1E30}
inline constexpr GUID kClsidDdsPropertyHandler = {0x7c1f4e52, 0x93a6, 0x4b0d, {0xa4, 0xe7, 0x5d, 0x2c, 0x8f, 0x6b, 0x1e, 0x30}};
inline constexpr wchar_t kDdsPropertyHandlerClsidString[] = L"{7C1F4E52-93A6-4B0D-A4E7-5D2C8F6B1E30}";
// Windows' own .dds handler, the one to wrap if none was recorded.
// {A38B883C-1682-497E-97B0-0A3A9E801682}
inline constexpr GUID kClsidWindowsImageHandler = {0xa38b883c, 0x1682, 0x497e, {0x97, 0xb0, 0x0a, 0x3a, 0x9e, 0x80, 0x16, 0x82}};
// Under our CLSID: the handler we replaced, so it can be called and put back.
inline constexpr wchar_t kWrappedHandlerValue[] = L"WrappedHandler";

// Browsing .bsa as a folder.
// {B0F2F3B6-3E4A-4F1E-9C57-2A8D61C0E4A7}
inline constexpr GUID kClsidBsaFolder = {0xb0f2f3b6, 0x3e4a, 0x4f1e, {0x9c, 0x57, 0x2a, 0x8d, 0x61, 0xc0, 0xe4, 0xa7}};
inline constexpr wchar_t kBsaFolderClsidString[] = L"{B0F2F3B6-3E4A-4F1E-9C57-2A8D61C0E4A7}";
inline constexpr wchar_t kBsaProgId[] = L"PluginShellExtension.BSA";
// Under our type: what the extension pointed at before, to put back on removal.
inline constexpr wchar_t kPreviousProgIdValue[] = L"PreviousProgId";

// The "BSA manager" menu on .bsa files.
// {C4A7B1D2-6E3F-4A58-B9D1-7F20E6C3A914}
inline constexpr GUID kClsidBsaManager = {0xc4a7b1d2, 0x6e3f, 0x4a58, {0xb9, 0xd1, 0x7f, 0x20, 0xe6, 0xc3, 0xa9, 0x14}};
inline constexpr wchar_t kBsaManagerClsidString[] = L"{C4A7B1D2-6E3F-4A58-B9D1-7F20E6C3A914}";
inline constexpr GUID kClsidBsaDrop = {0xa4d3e8b2, 0x71c5, 0x4f09, {0x8e, 0x36, 0xc9, 0xb1, 0x5d, 0x72, 0x0e, 0x44}};
inline constexpr wchar_t kBsaDropClsidString[] = L"{A4D3E8B2-71C5-4F09-8E36-C9B15D720E44}";
// The "Hashes" menu on every file.
// {5E8D2C17-A3B4-4C69-8F12-3B7A90D4E5C6}
inline constexpr GUID kClsidHashMenu = {0x5e8d2c17, 0xa3b4, 0x4c69, {0x8f, 0x12, 0x3b, 0x7a, 0x90, 0xd4, 0xe5, 0xc6}};
inline constexpr wchar_t kHashMenuClsidString[] = L"{5E8D2C17-A3B4-4C69-8F12-3B7A90D4E5C6}";
inline constexpr GUID kClsidPapyrusMenu = {0x3a9e7c41, 0x5b2d, 0x4f86, {0xa1, 0xc3, 0x8e, 0x6d, 0x0f, 0x2b, 0x7a, 0x15}};
inline constexpr wchar_t kPapyrusMenuClsidString[] = L"{3A9E7C41-5B2D-4F86-A1C3-8E6D0F2B7A15}";
inline constexpr GUID kClsidFolderViewsMenu = {0x7b1e4a90, 0x2c6f, 0x4d13, {0x9e, 0x47, 0x15, 0xa8, 0xd3, 0x6c, 0x02, 0xf1}};
inline constexpr wchar_t kFolderViewsMenuClsidString[] = L"{7B1E4A90-2C6F-4D13-9E47-15A8D36C02F1}";

bool InstallHashMenu(HKEY root, const std::wstring& classes, const std::wstring& dllPath);
void RemoveHashMenu(HKEY root, const std::wstring& classes);
// Decompile on .pex and Compile on .psc.
bool InstallPapyrusMenu(HKEY root, const std::wstring& classes, const std::wstring& dllPath);
void RemovePapyrusMenu(HKEY root, const std::wstring& classes);
// Folder views on the background of every folder.
bool InstallFolderViewsMenu(HKEY root, const std::wstring& classes, const std::wstring& dllPath);
void RemoveFolderViewsMenu(HKEY root, const std::wstring& classes);

// Register and remove the BSA folder. `classes` is the registry path of the
// classes root and `ext` the file extension, so a test can use neither the real
// location nor the real extension.
bool InstallBsaBrowser(HKEY root, const std::wstring& classes, const std::wstring& ext, const std::wstring& dllPath);
void RemoveBsaBrowser(HKEY root, const std::wstring& classes, const std::wstring& ext);

// Take over .dds, recording the current handler, and hand it back. `classes` and
// `handlers` are the registry paths, so a test can use a throwaway location.
bool InstallDdsHandler(HKEY root, const std::wstring& classes, const std::wstring& handlers, const std::wstring& dllPath);
void RemoveDdsHandler(HKEY root, const std::wstring& classes, const std::wstring& handlers);
inline constexpr wchar_t kSchemaFileName[] = L"PluginShellExtension.propdesc";

// Machine-wide only: Windows reads property handlers and schemas from HKLM,
// so this needs an elevated caller (regsvr32 /n /i:columns, run as admin).
// schemaPath is the .propdesc registered with PSRegisterPropertySchema.
HRESULT RegisterPropertyHandler(const std::wstring& dllPath, const std::wstring& schemaPath);
HRESULT UnregisterPropertyHandler(const std::wstring& schemaPath);

}  // namespace pse

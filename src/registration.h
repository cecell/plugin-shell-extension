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
inline constexpr wchar_t kHandlerDescription[] = L"Plugin Shell Extension context menu";
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

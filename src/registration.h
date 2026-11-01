// Registry layout of the context-menu handler, used by the DLL
// (DllRegisterServer / DllInstall). installer/PluginProperties.iss writes the
// same keys; keep the two in step.
#pragma once

#include <windows.h>
#include <string>

namespace pp {

// {1D4348E0-3A45-489E-8022-6A2A883E9D94}
inline constexpr GUID kClsidContextMenu = {0x1d4348e0, 0x3a45, 0x489e, {0x80, 0x22, 0x6a, 0x2a, 0x88, 0x3e, 0x9d, 0x94}};
inline constexpr wchar_t kClsidString[] = L"{1D4348E0-3A45-489E-8022-6A2A883E9D94}";
inline constexpr wchar_t kHandlerName[] = L"PluginProperties";
inline constexpr wchar_t kHandlerDescription[] = L"Plugin Properties context menu";
inline constexpr const wchar_t* kExtensions[] = {L".esp", L".esm", L".esl"};

// root is HKEY_CURRENT_USER (per-user) or HKEY_LOCAL_MACHINE (all users).
HRESULT RegisterHandler(HKEY root, const std::wstring& dllPath);
HRESULT UnregisterHandler(HKEY root);

// Property handler: the Explorer columns / details pane / tooltip fields.
// {E0BA20B2-0391-45F5-80C3-07E306E945E9}
inline constexpr GUID kClsidPropertyHandler = {0xe0ba20b2, 0x0391, 0x45f5, {0x80, 0xc3, 0x07, 0xe3, 0x06, 0xe9, 0x45, 0xe9}};
inline constexpr wchar_t kPropertyHandlerClsidString[] = L"{E0BA20B2-0391-45F5-80C3-07E306E945E9}";
inline constexpr wchar_t kSchemaFileName[] = L"PluginProperties.propdesc";

// Machine-wide only: Windows reads property handlers and schemas from HKLM,
// so this needs an elevated caller (regsvr32 /n /i:columns, run as admin).
// schemaPath is the .propdesc registered with PSRegisterPropertySchema.
HRESULT RegisterPropertyHandler(const std::wstring& dllPath, const std::wstring& schemaPath);
HRESULT UnregisterPropertyHandler(const std::wstring& schemaPath);

}  // namespace pp

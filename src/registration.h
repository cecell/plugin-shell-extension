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

}  // namespace pp

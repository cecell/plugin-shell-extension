#include "embedded_patterns.h"

#include <windows.h>

#include <memory>
#include <mutex>
#include <string>

namespace pse {

namespace {
constexpr int kPatternsResource = 101;  // PluginShellExtension.rc
}

const PatternCatalog* EmbeddedPatterns() {
    static std::once_flag once;
    static std::unique_ptr<PatternCatalog> catalog;  // freed when the DLL unloads
    std::call_once(once, [] {
        HMODULE module = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&EmbeddedPatterns), &module);
        HRSRC res = FindResourceW(module, MAKEINTRESOURCEW(kPatternsResource), RT_RCDATA);
        HGLOBAL data = res ? LoadResource(module, res) : nullptr;
        const char* text = data ? static_cast<const char*>(LockResource(data)) : nullptr;
        if (!text) return;
        auto c = std::make_unique<PatternCatalog>();
        if (c->Load(std::string(text, SizeofResource(module, res)), nullptr)) catalog = std::move(c);
    });
    return catalog.get();
}

}  // namespace pse

// Decompiles .pex and compiles .psc with the skqe-papyrus-decompiler library.
#pragma once

#include <memory>
#include <string>

namespace pse {

// Where each command writes. A decompiled script goes next to its .pex. A
// compiled script goes to the game's folder for it when the source is in
// Scripts\Source or Source\Scripts, and next to its .psc otherwise.
std::wstring DecompiledScriptPath(const std::wstring& pex);
std::wstring CompiledScriptPath(const std::wstring& psc);

struct PapyrusResult {
    int status = 2;  // 0 done, 1 done with warnings, 2 failed, 3 the input can't be read
    std::wstring text;
};

// One run of the command: it opens the game's script archives at most once.
class PapyrusWorkspace {
public:
    // The game's Data folder; empty finds it from the Steam install.
    explicit PapyrusWorkspace(const std::wstring& gameData = L"");
    ~PapyrusWorkspace();
    PapyrusResult Decompile(const std::wstring& pex, const std::wstring& target);
    PapyrusResult Compile(const std::wstring& psc, const std::wstring& target);

private:
    PapyrusResult DecompileUnguarded(const std::wstring& pex, const std::wstring& target);
    PapyrusResult CompileUnguarded(const std::wstring& psc, const std::wstring& target);
    PapyrusResult Guarded(bool compile, const std::wstring& input, const std::wstring& target);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace pse

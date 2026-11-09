#include "papyrus_tools.h"

#include <windows.h>

#include <malloc.h>

#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string_view>
#include <vector>

#include <bsa/reader.hpp>

#include "bsa_archive.h"
#include "papyrus/compile.hpp"
#include "papyrus/decompile.hpp"
#include "papyrus/pex.hpp"

namespace pse {
namespace fs = std::filesystem;
namespace pex = papyrus::pex;
namespace psc = papyrus::psc;

namespace {

// The two flags every Skyrim script compiles with, for when the game's own
// TESV_Papyrus_Flags.flg isn't installed.
const char kDefaultFlags[] = "Flag Hidden 0 { Script Property } Flag Conditional 1 { Script Variable }";
const size_t kMaxScriptBytes = 64u * 1024 * 1024;

std::string Utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size, nullptr, nullptr);
    return out;
}

std::wstring FolderOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : path.substr(0, slash);
}

std::wstring LeafOf(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

std::wstring WithExtension(const std::wstring& path, const wchar_t* extension) {
    return fs::path(path).replace_extension(extension).wstring();
}

bool Same(const std::wstring& a, const wchar_t* b) { return _wcsicmp(a.c_str(), b) == 0; }

bool IsFolder(const fs::path& path) {
    std::error_code ec;
    return fs::is_directory(path, ec);
}

void AddFolder(std::vector<fs::path>& list, const fs::path& folder) {
    if (folder.empty() || !IsFolder(folder)) return;
    const fs::path normal = folder.lexically_normal();
    for (const fs::path& known : list) {
        if (_wcsicmp(known.c_str(), normal.c_str()) == 0) return;
    }
    list.push_back(normal);
}

std::wstring FindGameData() {
    const wchar_t* keys[] = {L"SOFTWARE\\WOW6432Node\\Bethesda Softworks\\Skyrim Special Edition", L"SOFTWARE\\Bethesda Softworks\\Skyrim Special Edition"};
    for (const wchar_t* key : keys) {
        wchar_t value[MAX_PATH * 2] = {};
        DWORD size = sizeof(value);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key, L"Installed Path", RRF_RT_REG_SZ, nullptr, value, &size) == ERROR_SUCCESS && value[0]) {
            fs::path data = fs::path(value) / L"Data";
            if (IsFolder(data)) return data.wstring();
        }
    }
    return {};
}

std::wstring WindowsError(DWORD code) {
    wchar_t* message = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                   reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring text = message ? message : L"unknown error";
    if (message) LocalFree(message);
    while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n')) text.pop_back();
    return text;
}

// Replaces the target only after the whole file has been written.
bool WriteFileAtomic(const std::wstring& target, const void* data, size_t size, std::wstring& error) {
    const std::wstring temporary = target + L".pse-tmp";
    {
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            error = WindowsError(GetLastError());
            return false;
        }
        DWORD written = 0;
        const BOOL ok = WriteFile(file, data, static_cast<DWORD>(size), &written, nullptr) && written == size;
        const DWORD code = GetLastError();
        CloseHandle(file);
        if (!ok) {
            DeleteFileW(temporary.c_str());
            error = WindowsError(code);
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        error = WindowsError(GetLastError());
        DeleteFileW(temporary.c_str());
        return false;
    }
    return true;
}

// A damaged or hostile script can recurse past any stack. This runs inside
// Explorer, so that must end the one command and not the whole process.
bool RunGuarded(void (*work)(void*), void* context) noexcept {
    __try {
        work(context);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW || GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        if (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return false;
    }
}

bool SafeScriptName(std::string_view name) {
    if (name.empty()) return false;
    for (char c : name) {
        if (c == '/' || c == '\\' || c == ':' || c == '.' || static_cast<unsigned char>(c) < 32) return false;
    }
    return true;
}

// Compiled scripts by name: the folders first, then the game's archives.
class CompiledScripts final : public pex::ScriptResolver {
public:
    explicit CompiledScripts(std::wstring gameData) : data_(std::move(gameData)) {}

    void SetFolders(std::vector<fs::path> folders) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (folders != folders_) cache_.clear();
        folders_ = std::move(folders);
    }

    const pex::Script* find(std::string_view name) override {
        if (!SafeScriptName(name)) return nullptr;
        std::string key(name);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
        std::lock_guard<std::mutex> lock(mutex_);
        auto known = cache_.find(key);
        if (known != cache_.end()) return known->second.get();
        std::unique_ptr<pex::Script> script = Load(key);
        const pex::Script* result = script.get();
        cache_.emplace(std::move(key), std::move(script));
        return result;
    }

private:
    std::unique_ptr<pex::Script> Load(const std::string& name) {
        auto script = std::make_unique<pex::Script>();
        for (const fs::path& folder : folders_) {
            std::error_code ec;
            const fs::path file = folder / fs::u8path(name + ".pex");
            if (fs::is_regular_file(file, ec) && pex::read_file(file, *script) == pex::Status::Ok) return script;
            *script = pex::Script();
        }
        OpenArchives();
        const std::string inside = "scripts\\" + name + ".pex";
        for (const auto& archive : archives_) {
            bsa::Entry entry;
            std::vector<uint8_t> bytes;
            if (archive->find(inside, entry) != bsa::Status::Ok || archive->read(entry, bytes, kMaxScriptBytes) != bsa::Status::Ok) continue;
            if (pex::read(bytes.data(), bytes.size(), *script) == pex::Status::Ok) return script;
            *script = pex::Script();
        }
        return nullptr;
    }

    void OpenArchives() {
        if (opened_ || data_.empty()) return;
        opened_ = true;
        // Every vanilla script is in Misc. The unofficial patch replaces many of them.
        for (const wchar_t* name : {L"unofficial skyrim special edition patch.bsa", L"Skyrim - Misc.bsa"}) {
            auto archive = std::make_unique<bsa::Reader>();
            if (archive->open(fs::path(data_) / name) == bsa::Status::Ok) archives_.push_back(std::move(archive));
        }
    }

    std::mutex mutex_;
    std::wstring data_;
    std::vector<fs::path> folders_;
    std::map<std::string, std::unique_ptr<pex::Script>> cache_;
    std::vector<std::unique_ptr<bsa::Reader>> archives_;
    bool opened_ = false;
};

// Real sources first. A script that has none is served as a declaration stub
// decompiled from its compiled file, so no script sources need to be installed.
class SourcesThenDeclarations final : public psc::SourceResolver {
public:
    SourcesThenDeclarations(std::unique_ptr<psc::SourceResolver> folders, CompiledScripts& compiled) : folders_(std::move(folders)), compiled_(compiled) {}

    bool find(std::string_view name, psc::SourceFile& out) override {
        if (folders_->find(name, out)) return true;
        const pex::Script* script = compiled_.find(name);
        if (!script || script->objects.empty()) return false;
        pex::DecompileOptions options;
        options.declarationsOnly = true;
        options.optionalObjectParams = true;
        const pex::DecompileResult result = pex::decompile(*script, options);
        out.name = std::string(script->str(script->objects[0].name));
        out.path = out.name + " (compiled)";
        out.text = result.source;
        out.modified = 0;
        return true;
    }

private:
    std::unique_ptr<psc::SourceResolver> folders_;
    CompiledScripts& compiled_;
};

bool ReadText(const fs::path& path, std::string& out) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return false;
    out.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

uint64_t ModifiedTime(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return 0;
    const uint64_t ticks = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    return ticks / 10000000 - 11644473600ull;
}

std::vector<psc::UserFlagDef> LoadFlags(const std::vector<fs::path>& folders) {
    std::vector<psc::UserFlagDef> flags;
    for (const fs::path& folder : folders) {
        std::string text;
        std::error_code ec;
        const fs::path file = folder / L"TESV_Papyrus_Flags.flg";
        if (fs::is_regular_file(file, ec) && ReadText(file, text) && psc::load_flags(text, flags)) return flags;
        flags.clear();
    }
    psc::load_flags(kDefaultFlags, flags);
    return flags;
}

}  // namespace

std::wstring DecompiledScriptPath(const std::wstring& pexPath) { return WithExtension(pexPath, L".psc"); }

std::wstring CompiledScriptPath(const std::wstring& pscPath) {
    const std::wstring folder = FolderOf(pscPath);
    const std::wstring parent = FolderOf(folder);
    const std::wstring name = WithExtension(LeafOf(pscPath), L".pex");
    if (!parent.empty()) {
        if (Same(LeafOf(folder), L"Source") && Same(LeafOf(parent), L"Scripts")) return parent + L"\\" + name;
        if (Same(LeafOf(folder), L"Scripts") && Same(LeafOf(parent), L"Source")) {
            const std::wstring root = FolderOf(parent);
            if (!root.empty()) {
                const fs::path scripts = fs::path(root) / L"Scripts";
                if (IsFolder(scripts)) return (scripts / name).wstring();
            }
        }
    }
    return folder.empty() ? name : folder + L"\\" + name;
}

struct PapyrusWorkspace::Impl {
    explicit Impl(std::wstring data) : gameData(std::move(data)), compiled(gameData) {}
    std::wstring gameData;
    CompiledScripts compiled;
};

PapyrusWorkspace::PapyrusWorkspace(const std::wstring& gameData) : impl_(std::make_unique<Impl>(gameData.empty() ? FindGameData() : gameData)) {}
PapyrusWorkspace::~PapyrusWorkspace() = default;

PapyrusResult PapyrusWorkspace::Guarded(bool compile, const std::wstring& input, const std::wstring& target) {
    struct Call {
        PapyrusWorkspace* self;
        bool compile;
        const std::wstring& input;
        const std::wstring& target;
        PapyrusResult result;
    } call{this, compile, input, target, {}};
    const bool finished = RunGuarded(
        [](void* context) {
            auto& c = *static_cast<Call*>(context);
            c.result = c.compile ? c.self->CompileUnguarded(c.input, c.target) : c.self->DecompileUnguarded(c.input, c.target);
        },
        &call);
    if (finished) return call.result;
    // The failed call never unwound, so its locks may still be held. Abandon its state.
    const std::wstring data = impl_->gameData;
    impl_.release();
    impl_ = std::make_unique<Impl>(data);
    PapyrusResult failed;
    failed.text = LeafOf(input) + L" is too damaged or too deeply nested to process.\r\n";
    return failed;
}

PapyrusResult PapyrusWorkspace::Decompile(const std::wstring& pexPath, const std::wstring& target) { return Guarded(false, pexPath, target); }
PapyrusResult PapyrusWorkspace::Compile(const std::wstring& pscPath, const std::wstring& target) { return Guarded(true, pscPath, target); }

PapyrusResult PapyrusWorkspace::DecompileUnguarded(const std::wstring& pexPath, const std::wstring& target) {
    PapyrusResult result;
    try {
        pex::Script script;
        std::string error;
        if (pex::read_file(fs::path(pexPath), script, &error) != pex::Status::Ok) {
            result.status = 3;
            result.text = LeafOf(pexPath) + L" can't be read: " + Widen(error) + L"\r\n";
            return result;
        }
        std::vector<fs::path> folders;
        AddFolder(folders, FolderOf(pexPath));
        if (!impl_->gameData.empty()) AddFolder(folders, fs::path(impl_->gameData) / L"Scripts");
        impl_->compiled.SetFolders(std::move(folders));

        pex::DecompileOptions options;
        options.resolver = &impl_->compiled;
        const pex::DecompileResult output = pex::decompile(script, options);
        std::wstring writeError;
        if (!WriteFileAtomic(target, output.source.data(), output.source.size(), writeError)) {
            result.text = L"Couldn't write " + target + L": " + writeError + L"\r\n";
            return result;
        }
        std::wstring stubs;
        size_t failed = 0;
        for (const pex::FunctionReport& f : output.functions) {
            if (f.status != pex::FnStatus::Failed) continue;
            ++failed;
            stubs += L"  " + Widen(f.state.empty() ? f.name : f.state + "." + f.name) + L": " + Widen(f.reason) + L"\r\n";
        }
        result.status = failed ? 1 : 0;
        result.text = LeafOf(pexPath) + L" decompiled to " + target + L"\r\n";
        if (failed) {
            result.text += std::to_wstring(failed) + L" of " + std::to_wstring(output.functions.size()) +
                           L" functions couldn't be rebuilt. Each is written as an empty function with its assembly in a comment:\r\n" + stubs;
        }
    } catch (...) {
        result.status = 2;
        result.text = LeafOf(pexPath) + L" couldn't be decompiled because of an internal error.\r\n";
    }
    return result;
}

PapyrusResult PapyrusWorkspace::CompileUnguarded(const std::wstring& pscPath, const std::wstring& target) {
    PapyrusResult result;
    try {
        psc::SourceFile source;
        if (!ReadText(fs::path(pscPath), source.text)) {
            result.status = 3;
            result.text = LeafOf(pscPath) + L" can't be read.\r\n";
            return result;
        }
        source.name = Utf8(fs::path(pscPath).stem().wstring());
        source.path = Utf8(pscPath);
        source.modified = ModifiedTime(pscPath);

        const fs::path folder = FolderOf(pscPath);
        const fs::path data = impl_->gameData;
        std::vector<fs::path> sources;
        AddFolder(sources, folder);
        AddFolder(sources, folder / L"Source");
        AddFolder(sources, folder / L"Source" / L"Scripts");
        AddFolder(sources, folder.parent_path() / L"Source");
        AddFolder(sources, folder.parent_path() / L"Source" / L"Scripts");
        AddFolder(sources, folder.parent_path() / L"Scripts" / L"Source");
        if (!data.empty()) {
            AddFolder(sources, data / L"Scripts" / L"Source");
            AddFolder(sources, data / L"Source" / L"Scripts");
        }
        std::vector<fs::path> compiled;
        AddFolder(compiled, FolderOf(target));
        AddFolder(compiled, folder);
        if (!data.empty()) AddFolder(compiled, data / L"Scripts");
        impl_->compiled.SetFolders(std::move(compiled));

        const std::vector<psc::UserFlagDef> flags = LoadFlags(sources);
        SourcesThenDeclarations resolver(psc::folder_resolver(sources), impl_->compiled);
        psc::CompileOptions options;
        options.flags = flags;
        options.resolver = &resolver;

        pex::Script script;
        std::vector<psc::Diagnostic> diagnostics;
        const bool ok = psc::compile(source, options, script, diagnostics);
        std::wstring messages;
        for (const psc::Diagnostic& d : diagnostics) messages += Widen(d.text()) + L"\r\n";
        if (!ok) {
            result.status = 2;
            result.text = LeafOf(pscPath) + L" didn't compile:\r\n" + messages;
            return result;
        }
        std::vector<uint8_t> bytes;
        std::string error;
        if (pex::write(script, bytes, &error) != pex::Status::Ok) {
            result.status = 2;
            result.text = LeafOf(pscPath) + L" couldn't be written: " + Widen(error) + L"\r\n";
            return result;
        }
        std::wstring writeError;
        if (!WriteFileAtomic(target, bytes.data(), bytes.size(), writeError)) {
            result.status = 2;
            result.text = L"Couldn't write " + target + L": " + writeError + L"\r\n";
            return result;
        }
        result.status = messages.empty() ? 0 : 1;
        result.text = LeafOf(pscPath) + L" compiled to " + target + L"\r\n" + messages;
    } catch (...) {
        result.status = 2;
        result.text = LeafOf(pscPath) + L" couldn't be compiled because of an internal error.\r\n";
    }
    return result;
}

}  // namespace pse

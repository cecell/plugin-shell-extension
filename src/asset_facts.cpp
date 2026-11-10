#include "asset_facts.h"

#include <shlwapi.h>

#include <asset/metadata.hpp>
#include <bsa/reader.hpp>

#include <algorithm>
#include <cwctype>
#include <malloc.h>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string_view>

#include "bsa_archive.h"
#include "bsa_tools.h"
#include "papyrus/pex.hpp"
#include "papyrus_tools.h"
#include "property_handler.h"

namespace pse {
namespace pex = papyrus::pex;
namespace {

constexpr size_t kMaxFullRead = size_t{512} << 20;
constexpr size_t kMaxLipRead = size_t{1} << 20;
constexpr size_t kMaxTexturesText = 3000;
constexpr wchar_t kFactSeparator = L'\x1f', kRecordSeparator = L'\x1e';
constexpr wchar_t kEncodingVersion[] = L"v1";

using Type = AssetFact::Type;

AssetFact Text(DWORD pid, std::wstring text) { return AssetFact{pid, Type::Text, std::move(text), 0}; }
AssetFact Number(DWORD pid, uint64_t value) { return AssetFact{pid, Type::UInt32, L"", value}; }
AssetFact Big(DWORD pid, uint64_t value) { return AssetFact{pid, Type::UInt64, L"", value}; }

std::wstring Extension(const std::wstring& path) {
    std::wstring ext = PathFindExtensionW(path.c_str());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](wchar_t c) { return static_cast<wchar_t>(towlower(c)); });
    return ext;
}

std::wstring YesNo(bool value) { return value ? L"Yes" : L"No"; }

std::wstring NifGame(asset::NifEngine engine) {
    switch (engine) {
        case asset::NifEngine::SkyrimLe: return L"LE";
        case asset::NifEngine::SkyrimSe: return L"SE";
        case asset::NifEngine::SkyrimEither: return L"LE or SE";
        default: return L"";
    }
}

bool FileTimeOf(const std::wstring& path, uint64_t* time) {
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return false;
    *time = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    return true;
}

// "Foo - Textures.bsa" and "Foo.bsa" both belong to Foo.esp, .esm or .esl.
bool HasMatchingPlugin(const std::wstring& path) {
    const std::filesystem::path file(path);
    std::wstring stem = file.stem().wstring();
    const std::vector<std::wstring> names = [&] {
        std::vector<std::wstring> list{stem};
        const size_t dash = stem.rfind(L" - ");
        if (dash != std::wstring::npos) list.push_back(stem.substr(0, dash));
        return list;
    }();
    std::error_code ec;
    for (const std::wstring& name : names) {
        for (const wchar_t* ext : {L".esp", L".esm", L".esl"}) {
            if (std::filesystem::exists(file.parent_path() / (name + ext), ec)) return true;
        }
    }
    return false;
}

struct SourceCounts {
    std::string extends;
    uint64_t functions = 0, properties = 0;
    bool isScript = false;
};

std::string Lower(std::string text) {
    std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return static_cast<char>(tolower(c)); });
    return text;
}

// Takes out comments and joins lines continued with a backslash.
std::string StripSource(const std::string& source) {
    std::string out;
    out.reserve(source.size());
    bool inString = false;
    for (size_t i = 0; i < source.size(); ++i) {
        const char c = source[i];
        if (inString) {
            out += c;
            if (c == '\\' && i + 1 < source.size()) out += source[++i];
            else if (c == '"' || c == '\n') inString = false;
            continue;
        }
        if (c == '"') {
            inString = true;
            out += c;
        } else if (c == ';' && i + 1 < source.size() && source[i + 1] == '/') {
            const size_t end = source.find("/;", i + 2);
            if (end == std::string::npos) break;
            i = end + 1;
        } else if (c == ';') {
            while (i + 1 < source.size() && source[i + 1] != '\n') ++i;
        } else if (c == '{') {
            const size_t end = source.find('}', i + 1);
            if (end == std::string::npos) break;
            i = end;
        } else if (c == '\\') {
            while (i + 1 < source.size() && source[i + 1] != '\n') ++i;
            if (i + 1 < source.size()) ++i;
            out += ' ';
        } else {
            out += c;
        }
    }
    return out;
}

// Enough of the language to count declarations. Functions include events,
// and a full property's Get and Set aren't counted, as in the compiled file.
SourceCounts ScanSource(const std::string& source) {
    SourceCounts counts;
    const std::string text = StripSource(source);
    bool inProperty = false;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::vector<std::string> tokens;
        std::string token;
        for (size_t i = start; i <= end; ++i) {
            const char c = i < end ? text[i] : ' ';
            if (c == ' ' || c == '\t' || c == '\r' || c == '(') {
                if (!token.empty()) tokens.push_back(std::move(token));
                token.clear();
                if (c == '(') break;
            } else {
                token += c;
            }
        }
        start = end + 1;
        if (tokens.empty()) continue;
        const std::string first = Lower(tokens[0]);
        const std::string second = tokens.size() > 1 ? Lower(tokens[1]) : std::string();
        if (first == "endproperty") {
            inProperty = false;
            continue;
        }
        if (inProperty) continue;
        if (first == "scriptname") {
            counts.isScript = true;
            for (size_t i = 2; i + 1 < tokens.size(); ++i) {
                if (Lower(tokens[i]) == "extends") counts.extends = tokens[i + 1];
            }
        } else if (first == "function" || first == "event" || second == "function") {
            ++counts.functions;
        } else if (second == "property") {
            ++counts.properties;
            bool isAuto = false;
            for (size_t i = 2; i < tokens.size(); ++i) {
                const std::string word = Lower(tokens[i]);
                isAuto = isAuto || word == "auto" || word == "autoreadonly";
            }
            inProperty = !isAuto;
        }
    }
    return counts;
}

bool ReadWhole(const std::wstring& path, std::vector<uint8_t>* out) {
    return asset::read_prefix(std::filesystem::path(path), kMaxFullRead, *out) == asset::Status::Ok && !out->empty();
}

void FullNif(const std::wstring& path, std::vector<AssetFact>& facts) {
    std::vector<uint8_t> bytes;
    if (!ReadWhole(path, &bytes)) return;
    asset::NifBody body;
    if (asset::read_nif_body(bytes.data(), bytes.size(), body) != asset::Status::Ok) return;
    facts.push_back(Number(kPidNifShapes, body.shapes + body.shapesUnread));
    // Only exact totals: a shape that couldn't be read would make them too low.
    if (!body.shapesUnread) {
        facts.push_back(Big(kPidNifVertices, body.vertices));
        facts.push_back(Big(kPidNifTriangles, body.triangles));
    }
    std::set<std::wstring> seen;
    std::wstring list;
    for (const asset::NifTexture& t : body.textures) {
        if (t.path.empty()) continue;
        const std::wstring path16 = Widen(t.path);
        if (!seen.insert(path16).second) continue;
        if (list.size() + path16.size() > kMaxTexturesText) break;
        list += (list.empty() ? L"" : L"; ") + path16;
    }
    facts.push_back(Text(kPidNifTextures, list));
    if (body.skinStrips) {
        // A skin partition with strips settles it: only LE can use the mesh.
        facts.push_back(Text(kPidNifGame, L"LE"));
        facts.push_back(Text(kPidNifStrips, L"Yes"));
    }
}

void FullPex(const std::wstring& path, std::vector<AssetFact>& facts) {
    std::vector<uint8_t> bytes;
    if (!ReadWhole(path, &bytes)) return;
    pex::Script script;
    if (pex::read(bytes.data(), bytes.size(), script) != pex::Status::Ok || script.objects.empty()) return;
    const pex::Object& object = script.objects[0];
    facts.push_back(Text(kPidScriptName, Widen(script.str(object.name))));
    if (script.header.compileTime) {
        // Seconds since 1970 to a FILETIME.
        facts.push_back(AssetFact{kPidScriptCompiled, Type::Date, L"", (script.header.compileTime + 11644473600ull) * 10000000ull});
    }
    facts.push_back(Text(kPidScriptExtends, Widen(script.str(object.parent))));
    uint64_t functions = 0, states = 0;
    const bool noParent = script.str(object.parent).empty();
    for (const pex::State& state : object.states) {
        const bool isDefault = script.str(state.name).empty();
        if (!isDefault) ++states;
        for (const pex::NamedFunction& f : state.functions) {
            const std::string name = Lower(std::string(script.str(f.name)));
            // The compiler adds these two, and the state events to a script with no parent.
            if (isDefault && (name == "getstate" || name == "gotostate")) continue;
            if (isDefault && noParent && (name == "onbeginstate" || name == "onendstate")) continue;
            ++functions;
        }
    }
    facts.push_back(Number(kPidScriptFunctions, functions));
    facts.push_back(Number(kPidScriptProperties, object.properties.size()));
    facts.push_back(Number(kPidScriptStates, states));
    std::wstring flags;
    for (const pex::UserFlag& flag : script.userFlags) {
        if (flag.bit < 32 && (object.userFlags & (1u << flag.bit))) flags += (flags.empty() ? L"" : L", ") + Widen(script.str(flag.name));
    }
    facts.push_back(Text(kPidScriptFlags, flags));
}

void FullPsc(const std::wstring& path, std::vector<AssetFact>& facts) {
    std::ifstream file{std::filesystem::path(path), std::ios::binary};
    if (!file) return;
    const std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    const SourceCounts counts = ScanSource(text);
    if (!counts.isScript) return;
    facts.push_back(Text(kPidScriptExtends, Widen(counts.extends)));
    facts.push_back(Number(kPidScriptFunctions, counts.functions));
    facts.push_back(Number(kPidScriptProperties, counts.properties));
}

void FullBsa(const std::wstring& path, std::vector<AssetFact>& facts) {
    const BsaFacts f = ReadBsaFacts(path);
    if (!f.ok) return;
    facts.push_back(Text(kPidBsaCompression, BsaCompressionName(f)));
    facts.push_back(Big(kPidBsaUnpacked, f.unpackedBytes));
}

void FullHkx(const std::wstring& path, std::vector<AssetFact>& facts) {
    std::vector<uint8_t> bytes;
    if (!ReadWhole(path, &bytes)) return;
    asset::HkxInfo h;
    if (asset::read_hkx(bytes.data(), bytes.size(), h) != asset::Status::Ok) return;
    facts.push_back(Text(kPidHkxContent, Widen(asset::to_string(h.content))));
    facts.push_back(Text(kPidHkxGame, h.pointerSize == 8 ? L"SE (64-bit)" : h.pointerSize == 4 ? L"LE (32-bit)" : L""));
    if (h.hasAnimation) {
        facts.push_back(Big(kFactDuration, static_cast<uint64_t>(h.duration * 10000000.0f + 0.5f)));
        if (h.frameCount >= 0) facts.push_back(Number(kPidHkxFrames, static_cast<uint64_t>(h.frameCount)));
        facts.push_back(Number(kPidHkxTracks, static_cast<uint64_t>(h.transformTracks)));
    }
}

void ReadFull(const std::wstring& path, std::vector<AssetFact>& facts) {
    const std::wstring ext = Extension(path);
    if (ext == L".nif") FullNif(path, facts);
    else if (ext == L".pex") FullPex(path, facts);
    else if (ext == L".psc") FullPsc(path, facts);
    else if (ext == L".bsa") FullBsa(path, facts);
    else if (ext == L".hkx") FullHkx(path, facts);
}

struct GuardedCall {
    const std::wstring* path;
    std::vector<AssetFact>* facts;
};

void CallReadFull(void* context) {
    auto* call = static_cast<GuardedCall*>(context);
    try {
        ReadFull(*call->path, *call->facts);
    } catch (...) {
        call->facts->clear();
    }
}

// These run inside Explorer, on files that may be damaged.
bool RunGuarded(void (*work)(void*), void* context) noexcept {
    __try {
        work(context);
        return true;
    } __except (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW || GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        if (GetExceptionCode() == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return false;
    }
}

}  // namespace

bool HasAssetFacts(const std::wstring& path) {
    const std::wstring ext = Extension(path);
    return ext == L".nif" || ext == L".pex" || ext == L".psc" || ext == L".bsa" || ext == L".hkx" || ext == L".lip";
}

std::vector<AssetFact> AssetFactColumns(const std::wstring& path) {
    const std::wstring ext = Extension(path);
    auto text = [](DWORD pid) { return Text(pid, L""); };
    auto number = [](DWORD pid) { return Number(pid, 0); };
    if (ext == L".nif") return {number(kPidNifShapes), Big(kPidNifVertices, 0), Big(kPidNifTriangles, 0), text(kPidNifTextures)};
    if (ext == L".pex") return {text(kPidScriptName), AssetFact{kPidScriptCompiled, Type::Date, L"", 0}, text(kPidScriptExtends), number(kPidScriptFunctions), number(kPidScriptProperties), number(kPidScriptStates), text(kPidScriptFlags)};
    if (ext == L".psc") return {text(kPidScriptExtends), number(kPidScriptFunctions), number(kPidScriptProperties)};
    if (ext == L".bsa") return {text(kPidBsaCompression), Big(kPidBsaUnpacked, 0)};
    if (ext == L".hkx") return {text(kPidHkxContent), text(kPidHkxGame), Big(kFactDuration, 0), number(kPidHkxFrames), number(kPidHkxTracks)};
    return {};
}

std::vector<AssetFact> ReadQuickAssetFacts(const std::wstring& path) {
    std::vector<AssetFact> facts;
    const std::wstring ext = Extension(path);
    try {
        if (ext == L".nif") {
            std::vector<uint8_t> bytes;
            asset::NifInfo n;
            if (asset::read_prefix(std::filesystem::path(path), asset::kNifProbeBytes, bytes) == asset::Status::Ok && asset::read_nif(bytes.data(), bytes.size(), n) == asset::Status::Ok) {
                const std::wstring game = NifGame(n.engine);
                if (!game.empty()) facts.push_back(Text(kPidNifGame, game));
                if (n.hasBsHeader) facts.push_back(Number(kPidNifVersion, n.bsVersion));
                facts.push_back(Number(kPidNifBlocks, n.blockCount));
                facts.push_back(Text(kPidNifStrips, YesNo(n.stripGeometry)));
            }
        } else if (ext == L".psc") {
            const std::wstring target = CompiledScriptPath(path);
            uint64_t source = 0, compiled = 0;
            if (FileTimeOf(path, &source)) {
                if (!FileTimeOf(target, &compiled)) facts.push_back(Text(kPidScriptCompiledState, L"No"));
                else facts.push_back(Text(kPidScriptCompiledState, compiled >= source ? L"Yes" : L"Out of date"));
            }
        } else if (ext == L".bsa") {
            bsa::Reader reader;
            if (reader.open(std::filesystem::path(path)) == bsa::Status::Ok) {
                const bsa::Header& h = reader.header();
                facts.push_back(Text(kPidBsaGame, BsaGameName(h.version)));
                facts.push_back(Number(kPidBsaVersion, h.version));
                facts.push_back(Number(kPidBsaFiles, h.fileCount));
                facts.push_back(Number(kPidBsaFolders, h.folderCount));
                facts.push_back(Text(kPidBsaContains, BsaContentText(h.fileFlags)));
                facts.push_back(Text(kPidBsaHasPlugin, YesNo(HasMatchingPlugin(path))));
            }
        } else if (ext == L".lip") {
            std::vector<uint8_t> bytes;
            asset::LipInfo lip;
            if (asset::read_prefix(std::filesystem::path(path), kMaxLipRead, bytes) == asset::Status::Ok && asset::read_lip(bytes.data(), bytes.size(), lip) == asset::Status::Ok) {
                facts.push_back(Number(kPidLipVersion, lip.version));
                facts.push_back(Number(kPidLipUnits, lip.units));
            }
        }
    } catch (...) {
        facts.clear();
    }
    return facts;
}

std::vector<AssetFact> ReadFullAssetFacts(const std::wstring& path) {
    std::vector<AssetFact> facts;
    GuardedCall call{&path, &facts};
    if (!RunGuarded(&CallReadFull, &call)) return {};
    return facts;
}

std::wstring EncodeAssetFacts(const std::vector<AssetFact>& facts) {
    std::wstring out = kEncodingVersion;
    for (const AssetFact& f : facts) {
        std::wstring text = f.text;
        for (wchar_t& c : text) {
            if (c == L'\t' || c == L'\r' || c == L'\n' || c == kFactSeparator || c == kRecordSeparator) c = L' ';
        }
        out += kRecordSeparator;
        out += std::to_wstring(f.pid) + kFactSeparator + std::to_wstring(static_cast<int>(f.type)) + kFactSeparator + std::to_wstring(f.number) + kFactSeparator + text;
    }
    return out;
}

bool DecodeAssetFacts(const std::wstring& text, std::vector<AssetFact>* facts) {
    facts->clear();
    const std::wstring version = kEncodingVersion;
    if (text.compare(0, version.size(), version) != 0) return false;
    size_t at = version.size();
    while (at < text.size() && text[at] == kRecordSeparator) {
        size_t end = text.find(kRecordSeparator, at + 1);
        if (end == std::wstring::npos) end = text.size();
        const std::wstring record = text.substr(at + 1, end - at - 1);
        const size_t a = record.find(kFactSeparator);
        const size_t b = a == std::wstring::npos ? a : record.find(kFactSeparator, a + 1);
        const size_t c = b == std::wstring::npos ? b : record.find(kFactSeparator, b + 1);
        if (c == std::wstring::npos) return false;
        AssetFact fact;
        fact.pid = static_cast<DWORD>(wcstoul(record.c_str(), nullptr, 10));
        fact.type = static_cast<Type>(_wtoi(record.c_str() + a + 1));
        fact.number = _wcstoui64(record.c_str() + b + 1, nullptr, 10);
        fact.text = record.substr(c + 1);
        facts->push_back(std::move(fact));
        at = end;
    }
    return true;
}

}  // namespace pse

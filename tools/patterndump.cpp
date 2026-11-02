// patterndump <patterns.json> <plugin>...
//
// Decodes every record of each plugin with the pattern engine and writes one
// JSON line per record to stdout:
//   {"p":plugin,"t":RT,"f":formid,"h":header,"s":[[SIG,value],...],"e":[errors]}
// "s" lists the subrecords that have a pattern, in file order. Used to check
// the C++ engine against the reference database (tools/patterncheck.py).
#include <fcntl.h>
#include <io.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "../src/inflate.h"
#include "../src/pattern_engine.h"

using namespace pse;

namespace {

uint32_t U32(const uint8_t* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint16_t U16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

std::string Sig(const uint8_t* p) { return std::string(reinterpret_cast<const char*>(p), 4); }

std::string BaseName(const std::string& path) {
    size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? path : path.substr(s + 1);
}

struct Stats {
    uint64_t records = 0, subrecords = 0, decoded = 0, errors = 0, inflateFailures = 0;
};

void DumpRecord(const std::string& plugin, const PatternCatalog& cat, const uint8_t* rec, const uint8_t* body,
                size_t bodyLen, bool localized, Stats& st, std::string& out) {
    std::string rt = Sig(rec);
    uint32_t formId = U32(rec + 12);
    DecodeContext ctx;
    ctx.rt = rt;
    ctx.formVersion = U16(rec + 20);
    ctx.localized = localized;
    std::vector<std::string> errors;

    out += "{\"p\":";
    WriteJson(Value::Str(plugin), &out);
    out += ",\"t\":";
    WriteJson(Value::Str(rt), &out);
    out += ",\"f\":" + std::to_string(formId) + ",\"h\":";
    if (const Pattern* hp = cat.Find(rt, "Record Header")) {
        ctx.fsig = "Record Header";
        try {
            WriteJson(DecodeSubrecord(*hp, rec, 24, ctx), &out);
        } catch (const PatternError& e) {
            out += "null";
            errors.push_back("Record Header: " + std::string(e.what()));
        }
    } else {
        out += "null";
    }
    out += ",\"s\":[";

    bool first = true;
    int64_t lastEpft = 0;
    size_t pos = 0;
    uint32_t bigSize = 0;
    bool haveBig = false;
    while (pos + 6 <= bodyLen) {
        std::string fs = Sig(body + pos);
        size_t len = U16(body + pos + 4);
        pos += 6;
        if (haveBig) {
            len = bigSize;
            haveBig = false;
        }
        if (pos + len > bodyLen) {
            errors.push_back(fs + ": subrecord runs past the record end");
            break;
        }
        const uint8_t* data = body + pos;
        pos += len;
        if (fs == "XXXX" && len >= 4) {
            bigSize = U32(data);
            haveBig = true;
            continue;
        }
        ++st.subrecords;
        if (rt == "PERK") {
            if (fs == "PRKE") lastEpft = 0;
            else if (fs == "EPFT" && len > 0) lastEpft = data[0];
            ctx.epft = lastEpft;
        }
        const Pattern* p = cat.Find(rt, fs);
        if (!p) continue;
        ctx.fsig = fs;
        try {
            Value v = DecodeSubrecord(*p, data, len, ctx);
            if (!first) out += ',';
            first = false;
            out += '[';
            WriteJson(Value::Str(fs), &out);
            out += ',';
            WriteJson(v, &out);
            out += ']';
            ++st.decoded;
        } catch (const PatternError& e) {
            errors.push_back(fs + ": " + e.what());
        }
    }
    out += "]";
    if (!errors.empty()) {
        st.errors += errors.size();
        out += ",\"e\":[";
        for (size_t i = 0; i < errors.size(); ++i) {
            if (i) out += ',';
            WriteJson(Value::Str(errors[i]), &out);
        }
        out += "]";
    }
    out += "}\n";
}

bool DumpPlugin(const std::string& path, const PatternCatalog& cat, Stats& st) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        fprintf(stderr, "cannot open %s\n", path.c_str());
        return false;
    }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (file.size() < 24 || Sig(file.data()) != "TES4") {
        fprintf(stderr, "%s: not a plugin\n", path.c_str());
        return false;
    }
    bool localized = (U32(file.data() + 8) & 0x80) != 0;
    std::string plugin = BaseName(path);
    std::string out;
    std::vector<uint8_t> inflated;
    size_t pos = 0;
    while (pos + 24 <= file.size()) {
        const uint8_t* h = file.data() + pos;
        if (Sig(h) == "GRUP") {
            pos += 24;  // step into the group; its records follow
            continue;
        }
        size_t size = U32(h + 4);
        uint32_t flags = U32(h + 8);
        if (pos + 24 + size > file.size()) {
            fprintf(stderr, "%s: record at %zu runs past the end\n", plugin.c_str(), pos);
            return false;
        }
        const uint8_t* body = h + 24;
        size_t bodyLen = size;
        ++st.records;
        if ((flags & 0x00040000) && size >= 4) {
            if (!ZlibInflate(body + 4, size - 4, U32(body), &inflated)) {
                ++st.inflateFailures;
                fprintf(stderr, "%s: inflate failed for %08X\n", plugin.c_str(), U32(h + 12));
                pos += 24 + size;
                continue;
            }
            body = inflated.data();
            bodyLen = inflated.size();
        }
        DumpRecord(plugin, cat, h, body, bodyLen, localized, st, out);
        if (out.size() > (1 << 22)) {
            fwrite(out.data(), 1, out.size(), stdout);
            out.clear();
        }
        pos += 24 + size;
    }
    fwrite(out.data(), 1, out.size(), stdout);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: patterndump <patterns.json> <plugin>...\n");
        return 2;
    }
    _setmode(_fileno(stdout), _O_BINARY);
    std::ifstream pf(argv[1], std::ios::binary);
    std::stringstream ss;
    ss << pf.rdbuf();
    PatternCatalog cat;
    std::string err;
    if (!cat.Load(ss.str(), &err)) {
        fprintf(stderr, "patterns: %s\n", err.c_str());
        return 2;
    }
    Stats st;
    int rc = 0;
    for (int i = 2; i < argc; ++i) {
        if (!DumpPlugin(argv[i], cat, st)) rc = 1;
    }
    fprintf(stderr, "patterns %zu, records %llu, subrecords %llu, decoded %llu, decode errors %llu, inflate failures %llu\n",
            cat.size(), st.records, st.subrecords, st.decoded, st.errors, st.inflateFailures);
    return rc;
}

// itmcheck <patterns.json> <plugin> [formid ...]
//
// Lists the plugin's ITM records (xEdit Quick Auto Clean rules: the plugin
// and its masters only). With FormIDs (hex, as stored in the plugin), explains
// for each one why it is or is not ITM. Set PSE_MASTERS to a folder to take the
// masters from there (for plugins kept away from them, e.g. xEdit backups).
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

#include "../src/itm.h"

using namespace pse;

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: itmcheck <patterns.json> <plugin> [formid ...]\n");
        return 2;
    }
    std::ifstream pf(argv[1], std::ios::binary);
    std::stringstream ss;
    ss << pf.rdbuf();
    PatternCatalog cat;
    std::string err;
    if (!cat.Load(ss.str(), &err)) {
        fprintf(stderr, "patterns: %s\n", err.c_str());
        return 2;
    }
    wchar_t masters[MAX_PATH];
    if (GetEnvironmentVariableW(L"PSE_MASTERS", masters, MAX_PATH)) SetItmMastersFolder(masters);
    if (argc > 3) {
        for (int i = 3; i < argc; ++i) {
            uint32_t fid = static_cast<uint32_t>(wcstoul(argv[i], nullptr, 16));
            printf("%08X\n", fid);
            for (const auto& line : ExplainItm(argv[2], cat, fid)) printf("    %s\n", line.c_str());
        }
        return 0;
    }
    ItmResult r = FindItms(argv[2], cat);
    if (!r.ok) {
        fprintf(stderr, "error: %s\n", r.error.c_str());
        return 1;
    }
    for (const auto& m : r.missingMasters) printf("missing master: %s\n", m.c_str());
    for (const auto& it : r.itms) printf("ITM %08X %s\n", it.formId, it.type.c_str());
    printf("overrides %u, ITM %zu\n", r.overrides, r.itms.size());
    return 0;
}

// Console dump of what the dialog shows, for testing the parser.
//   plugininfo.exe <plugin> [<plugin> ...]
#include <windows.h>
#include <cstdio>
#include <io.h>
#include <fcntl.h>

#include "../src/plugin_parser.h"

int wmain(int argc, wchar_t** argv) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    if (argc < 2) {
        fwprintf(stderr, L"usage: plugininfo <plugin> [...]\n");
        return 2;
    }
    int failures = 0;
    for (int i = 1; i < argc; ++i) {
        pp::PluginHeader h = pp::ReadPluginHeader(argv[i]);
        wprintf(L"== %s\n", argv[i]);
        if (!h.ok) {
            wprintf(L"error: %s\n", h.error.c_str());
            ++failures;
            continue;
        }
        if (!h.error.empty()) wprintf(L"warning: %s\n", h.error.c_str());
        wprintf(L"Header version: %s\n", h.hasHedr ? pp::VersionText(h.version).c_str() : L"");
        wprintf(L"Records: %d\n", h.numRecords);
        wprintf(L"Flags: %s (0x%X)\n", pp::FlagsText(h.flags).c_str(), h.flags);
        wprintf(L"Light: %s\n", h.light ? L"True" : L"False");
        wprintf(L"Author - CNAM: %s\n", h.author.c_str());
        wprintf(L"Description - SNAM: %s\n", h.description.c_str());
        wprintf(L"Masters:");
        for (size_t m = 0; m < h.masters.size(); ++m) wprintf(L" [%zu] %s", m, h.masters[m].c_str());
        wprintf(L"\n");

        pp::GroupScan s = pp::ScanGroups(argv[i], h);
        if (!s.error.empty()) wprintf(L"scan error: %s\n", s.error.c_str());
        wprintf(L"ESLable: %s\n", pp::EslStatusText(pp::ClassifyEsl(h, s), true));
        wprintf(L"New records: %llu (in 800-FFF: %d, in 000-FFF: %d)\n", s.newRecords, s.newIdsIn800, s.newIdsIn000);
        wprintf(L"Record types:");
        for (size_t g = 0; g < s.groups.size(); ++g)
            wprintf(L"%s %s (%llu)", g ? L"," : L"", s.groups[g].label.c_str(), s.groups[g].records);
        wprintf(L"\nDeleted forms:");
        for (size_t d = 0; d < s.deleted.size(); ++d)
            wprintf(L"%s %s (%llu)", d ? L"," : L"", s.deleted[d].label.c_str(), s.deleted[d].records);
        wprintf(L"\n");
        uint64_t sum = s.totalRecords + s.totalGroups;
        bool match = h.hasHedr && sum == static_cast<uint64_t>(static_cast<uint32_t>(h.numRecords));
        wprintf(L"Self-check: records %llu + groups %llu = %llu vs HEDR %d -> %s\n", s.totalRecords, s.totalGroups,
                sum, h.numRecords, match ? L"OK" : L"MISMATCH");
        if (!s.ok || !match) ++failures;
    }
    return failures ? 1 : 0;
}

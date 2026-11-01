// Explorer property handler for .esp/.esm/.esl: supplies the plugin fields as
// shell properties, so they can be Details-view columns, details pane entries
// and tooltip lines. The property names and labels live in
// installer/PluginProperties.propdesc.
#pragma once

#include <windows.h>

namespace pp {

// Format ID shared by all plugin properties; PIDs below must match the
// .propdesc schema.
// {14E162A8-957D-4913-B1DD-86439C188DC6}
inline constexpr GUID kPluginFmtid = {0x14e162a8, 0x957d, 0x4913, {0xb1, 0xdd, 0x86, 0x43, 0x9c, 0x18, 0x8d, 0xc6}};

enum PluginPid : DWORD {
    kPidHeaderVersion = 2,  // String  "1.71"
    kPidRecordCount = 3,    // UInt32  HEDR record count
    kPidFlags = 4,          // String  "ESM, ESL, Localized"
    kPidLight = 5,          // String  True / False
    kPidMasterCount = 6,    // UInt32
    kPidDeletedCount = 7,   // UInt32  deleted REFR/ACHR/P***/NAVM, empty when 0 (full scan)
    kPidEslable = 8,        // String  Already / True / False (full scan)
    kPidMasters = 9,        // String vector, shown "a; b; c"
};

HRESULT CreatePropertyHandlerFactory(REFIID riid, void** ppv);

}  // namespace pp

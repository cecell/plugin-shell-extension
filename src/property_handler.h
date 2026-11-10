// Explorer property handler for .esp/.esm/.esl: supplies the plugin fields as
// shell properties, so they can be Details-view columns, details pane entries
// and tooltip lines. The property names and labels live in
// installer/PluginShellExtension.propdesc.
#pragma once

#include <windows.h>

namespace pse {

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
    kPidDeletedCount = 7,   // UInt32  deleted REFR/ACHR/P*** (not NAVM: see 12), empty when 0 (full scan)
    kPidEslable = 8,        // String  Already / True / Save in CK / False / Corrupted (full scan)
    kPidMasters = 9,        // String vector, shown "a; b; c"
    kPidEslNote = 10,       // String  "Requires v1.6.1130 or later" when it applies (full scan)
    kPidFormVersion = 11,   // UInt32  TES4 header form version (44 SE, 43 LE)
    kPidDeletedNavm = 12,   // UInt32  deleted NAVM records, empty when 0 (full scan)
    kPidFormCount = 13,     // UInt32  records new to this plugin, the ESL-limited count (full scan)
    kPidItm = 14,           // String  ITM record count, "MissingMaster", empty when 0 (reads the masters)
    kPidDirty = 15,         // String  True / False (#Deleted or #ITM non-zero), "Working", "MissingMaster"
    kPidTipStatus = 16,     // String  "Dirty: True    Is Light: False", one tooltip line (label hidden)
    kPidMipmaps = 17,       // String  "9 of 12" (.dds, from the asset property handler)
    kPidFuzVersion = 18,    // UInt32  .fuz format version
    kPidAudioFormat = 19,   // String  .fuz: "xWMA" or "WAV", the audio inside
    kPidSampleRate = 20,    // UInt32  .fuz. Windows' own Sample rate isn't offered as a column
    kPidChannels = 21,      // UInt32  .fuz. Nor is Windows' own Channels
    kPidNifGame = 22,
    kPidNifVersion = 23,
    kPidNifBlocks = 24,
    kPidNifStrips = 25,
    kPidNifShapes = 26,
    kPidNifVertices = 27,
    kPidNifTriangles = 28,
    kPidNifTextures = 29,
    kPidScriptName = 30,
    kPidScriptCompiled = 31,
    kPidScriptExtends = 32,
    kPidScriptFunctions = 33,
    kPidScriptProperties = 34,
    kPidScriptStates = 35,
    kPidScriptFlags = 36,
    kPidScriptCompiledState = 37,
    kPidBsaGame = 38,
    kPidBsaVersion = 39,
    kPidBsaFiles = 40,
    kPidBsaFolders = 41,
    kPidBsaContains = 42,
    kPidBsaCompression = 43,
    kPidBsaUnpacked = 44,
    kPidBsaHasPlugin = 45,
    kPidTexFormat = 46,
    kPidTexSrgb = 47,
    kPidTexAlpha = 48,
    kPidTexCubemap = 49,
    kPidHasLip = 50,
    kPidLipVersion = 51,
    kPidLipUnits = 52,
    kPidHkxContent = 53,
    kPidHkxGame = 54,
    kPidHkxFrames = 55,
    kPidHkxTracks = 56,
};

HRESULT CreatePropertyHandlerFactory(REFIID riid, void** ppv);

}  // namespace pse

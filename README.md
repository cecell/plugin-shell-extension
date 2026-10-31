# Plugin Properties

A Windows Explorer shell extension that adds **Plugin properties** to the
right-click menu of Skyrim SE plugins (`.esp`, `.esm`, `.esl`). It opens a
window styled after the Properties dialog's **Details** tab:

![screenshot](docs/screenshot.png)

| Property | Source |
|---|---|
| Header ver | `HEDR` float, rounded to 2 decimals |
| Records | `HEDR` record count (records + GRUPs, as the CK writes it) |
| Flags | TES4 flags as text: `ESM`, `ESL`, `Localized` |
| Light | `.esl` extension, or the ESL flag |
| ESLable | whether the ESL flag could be set without renumbering anything (Vortex's "Could be light", esplugin's rule): every record new to the plugin has an object ID in `0x800..0xFFF`, or `0x000..0xFFF` for HEDR 1.71+. Computed during the GRUP scan |
| Author | `CNAM` (wraps) |
| Description | `SNAM` (wraps, keeps line breaks) |
| Masters | every `MAST`, one per line, in master-index order |
| GRUPs | top-level GRUPs, alphabetical, `LABEL (records)`; the count is every record inside the group at any depth, so `CELL` includes its placed references and `DIAL` its `INFO`s. Duplicate top-level groups (Skyrim.esm has two `HAZD`) are merged |
| Size, Created, Modified, Attributes | formatted by the shell's property system, exactly as the Details tab shows them |

The parsing follows `C:\skqe\db\docs\db\PLUGIN_HEADER_EXTRACTION.md`. The GRUP
walk runs on a background thread over a memory-mapped view, so the window
appears immediately and fills in the GRUP line when the scan finishes
(Skyrim.esm, 250 MB, takes about a second).

Right-click a row to copy its value or the whole table. The Property column
can be resized, and so can the window.

## Install

Run `build\PluginProperties-<version>-setup.exe`, a standard Inno Setup
wizard. It installs for the current user (no admin prompt) into
`%LOCALAPPDATA%\Programs\Plugin Properties`, registers the handler under
`HKCU\Software\Classes`, and adds **Plugin Properties** to **Settings > Apps**
for uninstalling. Installing over an older version works while Explorer has
the DLL loaded: the old file is moved aside rather than Explorer being closed.
Restart Explorer (or sign out) to pick up the new version.

Silent install / uninstall use the usual Inno switches:
`PluginProperties-<version>-setup.exe /VERYSILENT`, and
`unins000.exe /VERYSILENT` in the install folder.

Run setup from Explorer or a normal terminal. A process started from inside a
packaged (MSIX) app, such as a terminal in the Claude desktop app, gets a
private copy of `HKCU`, so the registration lands where Explorer never looks.
The install then appears to succeed and tests run from that same app pass, but
the menu entry never shows up.

On Windows 11 the entry is in the classic menu (**Show more options** or
Shift+right-click). Getting into the new top-level menu needs an
`IExplorerCommand` handler plus a signed sparse MSIX package, which this
project does not do.

## Build

Requires Visual Studio with the MSVC 14.44 (v143) toolset; CMake and Ninja come
from the VS install / PATH. The installer needs Inno Setup 6 or 7
(`winget install JRSoftware.InnoSetup`). Without it the build still produces
the DLL and skips the installer.

```
build.cmd
```

Outputs, in `build\`:

- `bin\PluginProperties.dll`: the shell extension (static CRT, no redistributable needed)
- `PluginProperties-<version>-setup.exe`: the installer (script in `installer\PluginProperties.iss`)
- `plugininfo.exe <plugin>...`: console dump of the same fields, plus a self-check that walked records + GRUPs equal the HEDR count
- `menutest.exe <plugin> [--invoke]`: builds Explorer's context menu for a file and checks the entry is there (and opens)

## Testing without installing

```
rundll32 build\bin\PluginProperties.dll,ShowProperties C:\path\to\Plugin.esp
rundll32 build\bin\PluginProperties.dll,Snapshot "C:\path\to\Plugin.esp" "C:\out.png"
```

`Snapshot` renders the window to a PNG once the GRUP scan finishes (`snap.sh`
wraps it). `regsvr32 build\bin\PluginProperties.dll` registers the build output
for the current user (`/u` to undo).

Set `PLUGINPROPS_LOG=C:\some\file.log` in a process's environment, or the
registry value `HKCU\Software\PluginProperties\LogFile`, to trace the
handler's calls. Create an empty `load_trace.log` next to the DLL to log every
process that loads it.

## Layout

- `src/plugin_parser.*`: TES4 header + GRUP walk (bounds-checked; the mapped walk runs under SEH)
- `src/property_window.*`: the dialog and its custom Property/Value list, needed because a ListView cannot give rows different heights
- `src/context_menu.*`: `IShellExtInit` + `IContextMenu` + class factory
- `src/registration.*`: registry layout for `regsvr32` (the installer writes the same keys from `installer/PluginProperties.iss`)
- `src/dllmain.cpp`: COM exports, `DllRegisterServer`/`DllInstall`, rundll32 entry points
- `installer/PluginProperties.iss`: the Inno Setup installer

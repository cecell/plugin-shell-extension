# Plugin Shell Extension

A Windows Explorer shell extension that adds **Plugin properties** to the
right-click menu of Skyrim SE plugins (`.esp`, `.esm`, `.esl`). It opens a
window styled after the Properties dialog's **Details** tab:

![screenshot](docs/screenshot.png)

| Property | Source |
|---|---|
| Header version | `HEDR` float, rounded to 2 decimals |
| Records | `HEDR` record count (records + GRUPs, as the CK writes it) |
| Flags | TES4 flags as text: `ESM`, `ESL`, `Localized` |
| Light | `.esl` extension, or the ESL flag |
| ESLable | whether the ESL flag could be set without renumbering anything (Vortex's "Could be light", esplugin's rule): every record new to the plugin has an object ID in `0x800..0xFFF`, or `0x000..0xFFF` for HEDR 1.71+. Computed during the GRUP scan. A plugin that already loads as light shows "Already flagged" |
| Author - CNAM | `CNAM` (wraps) |
| Description - SNAM | `SNAM` (wraps, keeps line breaks) |
| Masters | every `MAST`, one per line as `[index] name`, in master-index order (the index is the FormID top byte that refers to that master) |
| Record types | top-level GRUPs, alphabetical, `LABEL (records)`; the count is every record inside the group at any depth, so `CELL` includes its placed references and `DIAL` its `INFO`s. Duplicate top-level groups (Skyrim.esm has two `HAZD`) are merged |
| Deleted forms | records with the Deleted flag (`0x20`) among the types xEdit's Quick Auto Clean undeletes (`REFR`, `ACHR`, `PGRE`, `PMIS`, `PARW`, `PBAR`, `PBEA`, `PCON`, `PFLA`, `PHZD`) plus `NAVM`, as `TYPE (count)`; nonzero types only, "None" if there are none |
| Size, Created, Modified, Attributes | formatted by the shell's property system, exactly as the Details tab shows them |

The parsing follows `C:\skqe\db\docs\db\PLUGIN_HEADER_EXTRACTION.md`. The GRUP
walk runs on a background thread over a memory-mapped view, so the window
appears immediately and fills in the GRUP line when the scan finishes
(Skyrim.esm, 250 MB, takes about a second).

Right-click a row to copy its value or the whole table. The Property column
can be resized, and so can the window.

The window is per-monitor DPI aware: it is laid out for the scale of the
monitor it opens on and re-laid out when dragged to a monitor with a
different scale. It follows the Windows app mode (Settings > Personalization >
Colors). Dark mode gets a dark title bar, tab, list, header, scrollbar and
buttons, and the window switches live when the setting changes. With high
contrast on, it uses the high-contrast system colours instead.

## Install

Run `PluginShellExtension-<version>-setup.exe` (from the GitHub release, or
`build\` after a local build), a standard Inno Setup wizard. It installs for the current user (no admin prompt) into
`%LOCALAPPDATA%\Programs\Plugin Shell Extension`, registers the handler under
`HKCU\Software\Classes`, and adds **Plugin Shell Extension** to **Settings > Apps**
for uninstalling. Installing over an older version works while Explorer has
the DLL loaded: the old file is moved aside rather than Explorer being closed.
Restart Explorer (or sign out) to pick up the new version.

Pre-release builds before 0.90 used a different name. If one is installed,
setup asks you to uninstall it first (Settings > Apps), because both use the
same registry keys.

Silent install / uninstall use the usual Inno switches:
`PluginShellExtension-<version>-setup.exe /VERYSILENT`, and
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

The wizard follows the Windows light/dark mode. It opens on the GPL-3.0 licence, which must be accepted to continue,
and offers to open this README when it finishes.

## Explorer columns, details pane and tooltip

The setup task **Add plugin fields as Explorer columns…** (on by default)
registers a property handler, so plugin fields become ordinary shell
properties:

| Column | Value |
|---|---|
| Header Ver | `HEDR` version, e.g. `1.71` |
| #Records | `HEDR` record count (number, sorts numerically) |
| Flags | `ESM, ESL, Localized` |
| Is Light | True / False (centred) |
| #Masters | number of masters |
| #Deleted | deleted references + navmeshes (the Deleted forms total), a hint that the plugin needs cleaning; blank when there are none |
| ESLable | Already (already light); True (fits as is: header < 1.71 with up to 2,048 new records in 800-FFF, or 1.71+ with up to 4,096 in 000-FFF; overrides only counts as True); Save in CK (header < 1.71, all new IDs in 000-FFF but not the old rules, fits once resaved with a 1.71 header); False (anything else, including plugins that would need compacting, which is not suggested); Corrupted (the file could not be read to the end). When the result needs IDs below 800, the file tooltip, the details pane and the window add "Requires v1.6.1130 or later" |

Turn them on in Details view: right-click a column header > **More…**. The
details pane (Alt+Shift+P) shows all of them plus the masters list, and the
tooltip when hovering a plugin lists `#Records` and its masters.

#Deleted and ESLable need a walk over every record header, so Explorer fills
them in after the other columns. Results are cached per Explorer process by
file name, size and modification time, so refreshing a folder does not rescan.

Windows only accepts property handlers and their schema
(`PluginShellExtension.propdesc`) registered machine-wide, so this task asks for
administrator rights (it runs `regsvr32 /n /i:columns` elevated; `/u` undoes
it, as the uninstaller does). Everything else stays per user. Restart Explorer
after installing so it loads the new schema.

## Licence

GPL-3.0. See [LICENSE](LICENSE).

## Official builds and verification

Release files are built by GitHub Actions (`.github/workflows/build.yml`) on a
clean Windows runner, not on a developer PC:

- the compiler is pinned to MSVC 14.44, and its version is printed in the log
- Inno Setup 7.1.1 is downloaded from its official release and checked
  against a pinned SHA-256 before use
- every run publishes `SHA256SUMS.txt` for the DLL and the installer, both
  in the run summary and in the downloadable artifact
- a GitHub Release publishes one run's artifact unchanged, so its files are
  byte for byte the ones that run hashed; the release notes link the run
- runs since the repository became public also attach signed build
  provenance that ties each file's hash to the commit and the run:

```
gh attestation verify PluginShellExtension-<version>-setup.exe --repo cecell/plugin-shell-extension
```

0.90 was built while the repository was still private, so it has no
provenance attestation; its run log and artifact are public and carry the
same SHA-256 values as the release.

A local build does not reproduce the official hashes byte for byte (the linker
stamps build times), so compare downloads against the published list.

## Build

Requires Visual Studio with the MSVC 14.44 (v143) toolset; CMake and Ninja come
from the VS install / PATH. The installer needs Inno Setup 6.6 or newer
(`winget install JRSoftware.InnoSetup`). Without it the build still produces
the DLL and skips the installer.

```
build.cmd
```

Outputs, in `build\`:

- `bin\PluginShellExtension.dll`: the shell extension (static CRT, no redistributable needed)
- `PluginShellExtension-<version>-setup.exe`: the installer (script in `installer\PluginShellExtension.iss`)
- `plugininfo.exe <plugin>...`: console dump of the same fields, plus a self-check that walked records + GRUPs equal the HEDR count
- `menutest.exe <plugin> [--invoke]`: builds Explorer's context menu for a file and checks the entry is there (and opens)
- `proptest.exe <dll> <plugin>...`: loads the property handler from the DLL without registering it, feeds it a file stream as Explorer does, and prints every column value

## Testing without installing

```
rundll32 build\bin\PluginShellExtension.dll,ShowProperties C:\path\to\Plugin.esp
rundll32 build\bin\PluginShellExtension.dll,Snapshot "C:\path\to\Plugin.esp" "C:\out.png"
```

`Snapshot` renders the window to a PNG once the GRUP scan finishes (`snap.sh`
wraps it). `regsvr32 build\bin\PluginShellExtension.dll` registers the build output
for the current user (`/u` to undo).

Set `PSE_LOG=C:\some\file.log` in a process's environment, or the
registry value `HKCU\Software\PluginShellExtension\LogFile`, to trace the
handler's calls. Create an empty `load_trace.log` next to the DLL to log every
process that loads it.

For screenshots of other looks: `PSE_THEME=dark` or `light` forces the
theme, and `PSE_DPI_UNAWARE=1` renders the 100% layout.

## Layout

- `src/plugin_parser.*`: TES4 header + GRUP walk (bounds-checked; the mapped walk runs under SEH)
- `src/property_window.*`: the dialog and its custom Property/Value list, needed because a ListView cannot give rows different heights
- `src/context_menu.*`: `IShellExtInit` + `IContextMenu` + class factory
- `src/registration.*`: registry layout for `regsvr32` (the installer writes the same keys from `installer/PluginShellExtension.iss`)
- `src/dllmain.cpp`: COM exports, `DllRegisterServer`/`DllInstall`, rundll32 entry points
- `src/property_handler.*`: the Explorer property handler (columns / details pane / tooltip), reading from the stream Explorer passes
- `installer/PluginShellExtension.propdesc`: the property schema (names, labels, types)
- `installer/PluginShellExtension.iss`: the Inno Setup installer

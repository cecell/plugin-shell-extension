; Inno Setup (6 or 7) script for the Plugin Properties shell extension.
; Built by CMake (target "installer"); can also be compiled by hand:
;   ISCC.exe /DAppVersion=1.0.0 /DBinDir=..\build\bin /DOutDir=..\build installer\PluginProperties.iss
;
; Installs per user (no admin prompt) into %LOCALAPPDATA%\Programs\Plugin Properties
; and registers the context-menu handler under HKCU\Software\Classes.

#ifndef AppVersion
  #define AppVersion "1.0.0"
#endif
#ifndef BinDir
  #define BinDir "..\build\bin"
#endif
#ifndef OutDir
  #define OutDir "..\build"
#endif

#define AppName "Plugin Properties"
#define Clsid "{{1D4348E0-3A45-489E-8022-6A2A883E9D94}"
#define ClsidKey "Software\Classes\CLSID\" + Clsid
#define Sfa "Software\Classes\SystemFileAssociations"

[Setup]
AppId={{27A531A2-EEF9-4E30-BDE9-3295D03F4D3E}
AppName={#AppName}
AppVersion={#AppVersion}
AppVerName={#AppName} {#AppVersion}
AppPublisher=Cecell
AppComments=Adds "Plugin properties" to the right-click menu of Skyrim plugins (.esp, .esm, .esl).
VersionInfoVersion={#AppVersion}
VersionInfoDescription={#AppName} Setup
PrivilegesRequired=lowest
DefaultDirName={autopf}\{#AppName}
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
WizardStyle=modern
Compression=lzma2/max
SolidCompression=yes
ChangesAssociations=yes
; Explorer keeps the DLL loaded; the [Code] section moves it aside instead of
; asking the user to close Explorer.
CloseApplications=no
OutputDir={#OutDir}
OutputBaseFilename=PluginProperties-{#AppVersion}-setup
UninstallDisplayName={#AppName}

[Languages]
Name: "english"; MessagesFile: "compiler:Default.isl"

[Messages]
FinishedLabelNoIcons=Setup has finished installing [name] on your computer.%n%nRight-click a .esp, .esm or .esl file and choose "Plugin properties". On Windows 11 it is under "Show more options" (or Shift+right-click).

[Files]
Source: "{#BinDir}\PluginProperties.dll"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; Left behind by the pre-Inno setup program.
Type: files; Name: "{app}\Setup.exe"
Type: files; Name: "{app}\*.old"

[UninstallDelete]
Type: files; Name: "{app}\*.old"

[Registry]
; COM server
Root: HKA; Subkey: "{#ClsidKey}"; ValueType: string; ValueData: "Plugin Properties context menu"; Flags: uninsdeletekey
Root: HKA; Subkey: "{#ClsidKey}\InprocServer32"; ValueType: string; ValueData: "{app}\PluginProperties.dll"
Root: HKA; Subkey: "{#ClsidKey}\InprocServer32"; ValueType: string; ValueName: "ThreadingModel"; ValueData: "Apartment"
; Context-menu handler for each plugin extension. The parent keys are removed
; on uninstall only if nothing else lives in them.
Root: HKA; Subkey: "{#Sfa}\.esp"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esp\shellex"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esp\shellex\ContextMenuHandlers"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esp\shellex\ContextMenuHandlers\PluginProperties"; ValueType: string; ValueData: "{#Clsid}"; Flags: uninsdeletekey
Root: HKA; Subkey: "{#Sfa}\.esm"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esm\shellex"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esm\shellex\ContextMenuHandlers"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esm\shellex\ContextMenuHandlers\PluginProperties"; ValueType: string; ValueData: "{#Clsid}"; Flags: uninsdeletekey
Root: HKA; Subkey: "{#Sfa}\.esl"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esl\shellex"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esl\shellex\ContextMenuHandlers"; Flags: uninsdeletekeyifempty
Root: HKA; Subkey: "{#Sfa}\.esl\shellex\ContextMenuHandlers\PluginProperties"; ValueType: string; ValueData: "{#Clsid}"; Flags: uninsdeletekey
; Uninstall entry written by the pre-Inno setup program.
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Uninstall\PluginProperties"; Flags: deletekey dontcreatekey

[Code]
// A DLL that Explorer has loaded cannot be overwritten or deleted, but it can
// be renamed. Move it aside so the new copy (or the uninstall) can proceed;
// the leftover is removed by the next install or when it is no longer loaded.

procedure MoveAside(const Path, Dir: String);
var
  Target: String;
begin
  if not FileExists(Path) then
    Exit;
  if DeleteFile(Path) then
    Exit;
  Target := AddBackslash(Dir) + 'PluginProperties.' + GetDateTimeString('yyyymmddhhnnsszzz', #0, #0) + '.old';
  if not RenameFile(Path, Target) then
    Log('Could not move aside ' + Path);
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    MoveAside(ExpandConstant('{app}\PluginProperties.dll'), ExpandConstant('{app}'));
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  // Moved to %TEMP% (same volume in the usual case) so the program folder can
  // be removed even while Explorer still holds the DLL.
  if CurUninstallStep = usUninstall then
    MoveAside(ExpandConstant('{app}\PluginProperties.dll'), GetTempDir);
end;

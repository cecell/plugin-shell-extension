; Inno Setup (6.6 or newer) script for the Plugin Properties shell extension.
; Built by CMake (target "installer"); can also be compiled by hand:
;   ISCC.exe /DAppVersion=1.0.0 /DBinDir=..\build\bin /DOutDir=..\build installer\PluginProperties.iss
;
; Installs per user (no admin prompt) into %LOCALAPPDATA%\Programs\Plugin Properties
; and registers the context-menu handler under HKCU\Software\Classes. The
; optional "columns" task additionally registers the property handler and its
; schema machine-wide, which Windows only accepts from an elevated process: it
; runs "regsvr32 /n /i:columns" through a UAC prompt.

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
#define PropHandlerClsid "{E0BA20B2-0391-45F5-80C3-07E306E945E9}"
#define P "Cecell.PluginProperties."
; Details pane (Alt+Shift+P) and hover tooltip contents.
#define PreviewDetails "prop:" + P + "HeaderVersion;" + P + "RecordCount;" + P + "Flags;" + P + "Light;" + P + "MasterCount;" + P + "DeletedCount;" + P + "ESLable;" + P + "Masters;*System.DateModified;*System.Size;*System.DateCreated"
#define InfoTip "prop:System.ItemTypeText;System.Size;System.DateModified;" + P + "RecordCount;" + P + "Masters"

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
; The licence is the first page.
DisableWelcomePage=yes
LicenseFile=..\LICENSE
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
MinVersion=10.0
; Follows the Windows light/dark app mode (Inno Setup 6.6 or newer).
WizardStyle=modern dynamic
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

[Tasks]
Name: "columns"; Description: "Add columns to Explorer, the details pane and files tooltip (requires admin rights)"

[Files]
Source: "{#BinDir}\PluginProperties.dll"; DestDir: "{app}"; Flags: ignoreversion
Source: "PluginProperties.propdesc"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\README.md"; DestDir: "{app}"; Flags: ignoreversion
Source: "..\docs\screenshot.png"; DestDir: "{app}\docs"; Flags: ignoreversion
Source: "..\LICENSE"; DestDir: "{app}"; Flags: ignoreversion

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
; Details pane and tooltip fields (per user; the properties themselves come
; from the machine-wide handler the columns task registers).
Root: HKA; Subkey: "{#Sfa}\.esp"; ValueType: string; ValueName: "PreviewDetails"; ValueData: "{#PreviewDetails}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.esp"; ValueType: string; ValueName: "InfoTip"; ValueData: "{#InfoTip}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.esm"; ValueType: string; ValueName: "PreviewDetails"; ValueData: "{#PreviewDetails}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.esm"; ValueType: string; ValueName: "InfoTip"; ValueData: "{#InfoTip}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.esl"; ValueType: string; ValueName: "PreviewDetails"; ValueData: "{#PreviewDetails}"; Flags: uninsdeletevalue; Tasks: columns
Root: HKA; Subkey: "{#Sfa}\.esl"; ValueType: string; ValueName: "InfoTip"; ValueData: "{#InfoTip}"; Flags: uninsdeletevalue; Tasks: columns
; Uninstall entry written by the pre-Inno setup program.
Root: HKA; Subkey: "Software\Microsoft\Windows\CurrentVersion\Uninstall\PluginProperties"; Flags: deletekey dontcreatekey

[Run]
Filename: "{app}\README.md"; Description: "Open the README"; Flags: postinstall shellexec skipifsilent nowait

[Code]
var
  LicenseCheck: TNewCheckBox;
  AiNotice, InnoCredit: TNewStaticText;

// --- Licence page: a checkbox instead of Inno's accept / decline radios ---

procedure SyncLicense;
begin
  WizardForm.LicenseAcceptedRadio.Checked := LicenseCheck.Checked;
  WizardForm.LicenseNotAcceptedRadio.Checked := not LicenseCheck.Checked;
  WizardForm.NextButton.Enabled := LicenseCheck.Checked;
end;

procedure LicenseCheckClick(Sender: TObject);
begin
  SyncLicense;
end;

// --- Tasks page: a notice under the task list and the Inno Setup credit at
// the bottom of the page ---

function AddPageText(const Caption: String): TNewStaticText;
begin
  Result := TNewStaticText.Create(WizardForm);
  Result.Parent := WizardForm.SelectTasksPage;
  Result.AutoSize := False;
  Result.WordWrap := True;
  Result.Left := WizardForm.TasksList.Left;
  Result.Width := WizardForm.TasksList.Width;
  Result.Caption := Caption;
  Result.AdjustHeight;
end;

procedure AddTasksPageText;
begin
  WizardForm.TasksList.Height := ScaleY(56);
  AiNotice := AddPageText('Plugin Properties was written with the assistance of Claude by Anthropic. ' +
    'Many have negative feelings towards AI but it has been a life saver for people like me who struggle ' +
    'with cognitive disabilities. I hope this software is helpful.');
  AiNotice.Top := WizardForm.TasksList.Top + WizardForm.TasksList.Height + ScaleY(16);
  InnoCredit := AddPageText('Uses Inno Setup, Copyright (C) Jordan Russell, https://jrsoftware.org/');
  InnoCredit.Top := WizardForm.SelectTasksPage.Height - InnoCredit.Height;
end;

procedure InitializeWizard;
begin
  WizardForm.LicenseAcceptedRadio.Visible := False;
  WizardForm.LicenseNotAcceptedRadio.Visible := False;
  LicenseCheck := TNewCheckBox.Create(WizardForm);
  LicenseCheck.Parent := WizardForm.LicensePage;
  LicenseCheck.Left := WizardForm.LicenseAcceptedRadio.Left;
  LicenseCheck.Top := WizardForm.LicenseAcceptedRadio.Top;
  LicenseCheck.Width := WizardForm.LicenseMemo.Width;
  LicenseCheck.Height := WizardForm.LicenseAcceptedRadio.Height;
  LicenseCheck.Caption := 'I agree to the terms of the GNU General Public License v3.0';
  LicenseCheck.OnClick := @LicenseCheckClick;
  AddTasksPageText;
end;

procedure CurPageChanged(CurPageID: Integer);
begin
  if CurPageID = wpLicense then
    SyncLicense;
end;

// --- Moving a loaded DLL aside ---
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

// --- Machine-wide property handler (columns task) ---

function ColumnsRegistered: Boolean;
var
  Value: String;
begin
  Result := RegQueryStringValue(HKLM, 'SOFTWARE\Microsoft\Windows\CurrentVersion\PropertySystem\PropertyHandlers\.esp', '', Value)
    and (CompareText(Value, '{#PropHandlerClsid}') = 0);
end;

// Runs regsvr32 /n /i:columns (or /u ...) on the installed DLL through a UAC
// prompt. Returns False if the prompt was declined or registration failed.
function RunColumnsRegistration(Install: Boolean): Boolean;
var
  Params: String;
  Code: Integer;
begin
  Params := '/s /n /i:columns "' + ExpandConstant('{app}\PluginProperties.dll') + '"';
  if not Install then
    Params := '/u ' + Params;
  Result := ShellExec('runas', ExpandConstant('{sys}\regsvr32.exe'), Params, '', SW_HIDE, ewWaitUntilTerminated, Code)
    and (Code = 0);
  Log(Format('regsvr32 %s -> ok=%d code=%d', [Params, Ord(Result), Code]));
end;

procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssInstall then
    MoveAside(ExpandConstant('{app}\PluginProperties.dll'), ExpandConstant('{app}'));
  if CurStep = ssPostInstall then begin
    if WizardIsTaskSelected('columns') then begin
      if not RunColumnsRegistration(True) then
        SuppressibleMsgBox('The Explorer columns could not be registered (administrator rights were declined or registration failed).' + #13#10#13#10 +
          'The right-click "Plugin properties" window is installed and works without them. Run setup again to retry.',
          mbInformation, MB_OK, IDOK);
    end else if ColumnsRegistered then
      RunColumnsRegistration(False);  // the task was unticked on an update
  end;
end;

procedure CurUninstallStepChanged(CurUninstallStep: TUninstallStep);
begin
  if CurUninstallStep = usUninstall then begin
    // Unregister while the DLL and schema are still in place.
    if ColumnsRegistered then
      RunColumnsRegistration(False);
    // Moved to %TEMP% (same volume in the usual case) so the program folder
    // can be removed even while Explorer still holds the DLL.
    MoveAside(ExpandConstant('{app}\PluginProperties.dll'), GetTempDir);
  end;
end;

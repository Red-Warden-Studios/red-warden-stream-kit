; Red Warden Stream Kit - Windows installer (Inno Setup 6)
;
; One installer, one plugin: rws-stream-kit.dll holds Multistream, Pre-Flight and In-Flight. There are no
; tool components: a tool is switched on or off in the Kit's own settings inside OBS.
; Built by tools\package.ps1, which passes /DAppVersion=... and /DStage=... and the
; signing tool. Do not run ISCC on this file by hand unless you pass the same defines.
;
; The plugin installs where the Kit 1.0.x plugins did: the per-machine plugin folder OBS 28+ scans,
; C:\ProgramData\obs-studio\plugins\rws-stream-kit\bin\64bit + data\locale.
; Upgrading = running a newer installer over the old one (same AppId). The installer removes the files
; of the plugins the Kit replaced (rws-multistream, rws-preflight, rws-inflight, rws-beacon): files by exact
; name, then each folder only if it is empty. Destinations, settings, reports, hotkeys and stream keys
; live elsewhere (OBS profile and plugin_config, Windows Credential Manager) and are never touched.
;
; Unsigned local builds: /DNoSign (without TestRoot) writes ..\dist-unsigned\...-UNSIGNED-TEST-Setup.exe, a
; name that can never be mistaken for the release. tools\package.ps1 -NoSign does this.
;
; TEST BUILDS ONLY: ISCC /DTestRoot=<dir> installs under <dir> instead of ProgramData, needs
; no admin rights, uses its own AppId, looks for a TEST-ONLY legacy AppId (never the real one),
; and never signs. The OBS-running check applies to test builds too. tools\test-installer.ps1
; uses it. Never ship a TestRoot build.

#ifndef AppVersion
  #error AppVersion must be passed: /DAppVersion=1.0.0
#endif
#ifndef Stage
  #error Stage must be passed: /DStage=<folder holding rws-stream-kit\>
#endif

#ifndef MsLogo
  #define MsLogo "..\..\red-warden-multistream\brand\logo.ico"
#endif

#ifdef TestRoot
  #define PluginRoot TestRoot
  #define LegacyAppId "RWS-KIT-TESTLEGACY-DA4CA68C"
#else
  #define PluginRoot "{commonappdata}\obs-studio\plugins"
  ; The AppId of the standalone Red Warden Multistream installer (rws-multistream.iss).
  #define LegacyAppId "{DA4CA68C-DB1B-4C5B-83B5-9BFB615B4E8B}"
#endif

[Setup]
; Never change AppId - it is how a newer installer finds and replaces an older install.
#ifdef TestRoot
AppId=RWS-KIT-TESTROOT-035D8EC0
#else
AppId={{035D8EC0-7626-4A56-BF6D-A31548414EFA}
#endif
AppName=Red Warden Stream Kit
AppVersion={#AppVersion}
#if defined(NoSign) && !defined(TestRoot)
AppVerName=Red Warden Stream Kit {#AppVersion} (UNSIGNED TEST BUILD)
#else
AppVerName=Red Warden Stream Kit {#AppVersion}
#endif
AppPublisher=Red Warden Studios LLC
AppPublisherURL=https://redwardenstudios.com/division/bastion/stream-kit/
AppSupportURL=https://redwardenstudios.com/division/bastion/stream-kit/
AppUpdatesURL=https://redwardenstudios.com/division/bastion/stream-kit/
VersionInfoVersion={#AppVersion}
VersionInfoCompany=Red Warden Studios LLC
VersionInfoDescription=Red Warden Stream Kit installer
; {app} only holds the uninstaller, the licence and the logo. The plugin goes under the
; plugin root, in its own module folder.
#ifdef TestRoot
DefaultDirName={#PluginRoot}\_kit
PrivilegesRequired=lowest
#else
DefaultDirName={commonappdata}\Red Warden Studios\Stream Kit
PrivilegesRequired=admin
#endif
DisableDirPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
LicenseFile=..\LICENSE
SetupIconFile={#MsLogo}
UninstallDisplayIcon={app}\logo.ico
UninstallDisplayName=Red Warden Stream Kit (OBS plugins)
#ifdef TestRoot
OutputDir=..\.testbed
OutputBaseFilename=RedWardenStreamKit-{#AppVersion}-TestRoot-Setup
#elif defined(NoSign)
OutputDir=..\dist-unsigned
OutputBaseFilename=RedWardenStreamKit-{#AppVersion}-UNSIGNED-TEST-Setup
#else
OutputDir=..\dist
OutputBaseFilename=RedWardenStreamKit-{#AppVersion}-Setup
#endif
Compression=lzma2
SolidCompression=yes
WizardStyle=modern
#if !defined(NoSign) && !defined(TestRoot)
SignTool=rws
SignedUninstaller=yes
#endif

[Files]
; The DLL arrives already signed by package.ps1; Inno signs only setup.exe and the uninstaller.
; The .pdb is NOT installed: it is 15 MB, carries the build machine's paths, and users never need it.
; package.ps1 keeps it in dist\symbols for our own crash symbolication.
Source: "{#Stage}\rws-stream-kit\bin\64bit\rws-stream-kit.dll"; DestDir: "{#PluginRoot}\rws-stream-kit\bin\64bit"; Flags: ignoreversion
Source: "{#Stage}\rws-stream-kit\data\*"; DestDir: "{#PluginRoot}\rws-stream-kit\data"; Flags: ignoreversion recursesubdirs createallsubdirs
Source: "..\LICENSE"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "..\NOTICE.txt"; DestDir: "{app}"; Flags: ignoreversion
Source: "{#MsLogo}"; DestDir: "{app}"; Flags: ignoreversion

[InstallDelete]
; The plugins the Kit replaced. Never a whole-directory delete and never plugin_config: a user's own files in
; an old plugin folder must survive. For each old module remove exactly the files an installer or a dev build
; places (dll, pdb, the en-US locale ini), then each folder only if it is empty, bottom-up. A folder that still
; holds anything else stays. (Kit 1.0.x shipped only the dll and the locale ini per plugin.)
; Multistream and Pre-Flight: Kit 1.0.x, and standalone Multistream 1.0.0 once its own uninstaller has run.
Type: files; Name: "{#PluginRoot}\rws-multistream\bin\64bit\rws-multistream.dll"
Type: files; Name: "{#PluginRoot}\rws-multistream\bin\64bit\rws-multistream.pdb"
Type: files; Name: "{#PluginRoot}\rws-multistream\data\locale\en-US.ini"
Type: dirifempty; Name: "{#PluginRoot}\rws-multistream\data\locale"
Type: dirifempty; Name: "{#PluginRoot}\rws-multistream\data"
Type: dirifempty; Name: "{#PluginRoot}\rws-multistream\bin\64bit"
Type: dirifempty; Name: "{#PluginRoot}\rws-multistream\bin"
Type: dirifempty; Name: "{#PluginRoot}\rws-multistream"
Type: files; Name: "{#PluginRoot}\rws-preflight\bin\64bit\rws-preflight.dll"
Type: files; Name: "{#PluginRoot}\rws-preflight\bin\64bit\rws-preflight.pdb"
Type: files; Name: "{#PluginRoot}\rws-preflight\data\locale\en-US.ini"
Type: dirifempty; Name: "{#PluginRoot}\rws-preflight\data\locale"
Type: dirifempty; Name: "{#PluginRoot}\rws-preflight\data"
Type: dirifempty; Name: "{#PluginRoot}\rws-preflight\bin\64bit"
Type: dirifempty; Name: "{#PluginRoot}\rws-preflight\bin"
Type: dirifempty; Name: "{#PluginRoot}\rws-preflight"
; In-Flight was never released on its own; a development build may have been installed.
Type: files; Name: "{#PluginRoot}\rws-inflight\bin\64bit\rws-inflight.dll"
Type: files; Name: "{#PluginRoot}\rws-inflight\bin\64bit\rws-inflight.pdb"
Type: files; Name: "{#PluginRoot}\rws-inflight\data\locale\en-US.ini"
Type: dirifempty; Name: "{#PluginRoot}\rws-inflight\data\locale"
Type: dirifempty; Name: "{#PluginRoot}\rws-inflight\data"
Type: dirifempty; Name: "{#PluginRoot}\rws-inflight\bin\64bit"
Type: dirifempty; Name: "{#PluginRoot}\rws-inflight\bin"
Type: dirifempty; Name: "{#PluginRoot}\rws-inflight"
; Multistream 0.1.x was installed under its working name, "Beacon".
Type: files; Name: "{#PluginRoot}\rws-beacon\bin\64bit\rws-beacon.dll"
Type: files; Name: "{#PluginRoot}\rws-beacon\bin\64bit\rws-beacon.pdb"
Type: files; Name: "{#PluginRoot}\rws-beacon\data\locale\en-US.ini"
Type: dirifempty; Name: "{#PluginRoot}\rws-beacon\data\locale"
Type: dirifempty; Name: "{#PluginRoot}\rws-beacon\data"
Type: dirifempty; Name: "{#PluginRoot}\rws-beacon\bin\64bit"
Type: dirifempty; Name: "{#PluginRoot}\rws-beacon\bin"
Type: dirifempty; Name: "{#PluginRoot}\rws-beacon"

[Messages]
FinishedLabel=Red Warden Stream Kit is installed.%n%nStart OBS and look for the RW Stream Kit dock (Docks menu). Multistream, Pre-Flight and In-Flight are switched on or off in the Kit's settings.

[Code]
const
  // The standalone Red Warden Multistream installer's uninstall entry (its AppId; a test-only id in TestRoot builds).
  LegacyUninstallKey = 'Software\Microsoft\Windows\CurrentVersion\Uninstall\{#LegacyAppId}_is1';
  LegacyFailed = 'Couldn''t remove the old Red Warden Multistream - uninstall it from Windows Settings > Apps, then run this installer again.';

function ObsIsRunning(): Boolean;
var
  Wmi, Procs: Variant;
begin
  Result := False;
  try
    Wmi := CreateOleObject('WbemScripting.SWbemLocator');
    Procs := Wmi.ConnectServer('.', 'root\CIMV2').ExecQuery(
      'SELECT ProcessId FROM Win32_Process WHERE Name = ''obs64.exe''');
    Result := Procs.Count > 0;
  except
    Result := False;  // can't tell - don't block the install on a WMI hiccup
  end;
end;

function WaitForObsToClose(Silent: Boolean): Boolean;
begin
  Result := True;
  while ObsIsRunning() do
  begin
    // A silent run has nobody to click OK: refuse instead of looping forever.
    if Silent then
    begin
      Result := False;
      Exit;
    end;
    if MsgBox('OBS Studio is running. Close OBS, then click OK to continue.' + #13#10 +
              '(Plugins load only when OBS starts.)',
              mbInformation, MB_OKCANCEL) = IDCANCEL then
    begin
      Result := False;
      Exit;
    end;
  end;
end;

function InitializeSetup(): Boolean;
begin
  Result := WaitForObsToClose(WizardSilent());
end;

function InitializeUninstall(): Boolean;
begin
  Result := WaitForObsToClose(UninstallSilent());
end;

// The old standalone Multistream installer registered its own uninstall entry. Two uninstallers
// owning the same plugin folder would fight, so remove the old one first (silently). Its
// destinations and stream keys live outside the plugin folder and are not touched.
function LegacyKeyExists(): Boolean;
begin
  Result := RegKeyExists(HKLM64, LegacyUninstallKey) or RegKeyExists(HKCU, LegacyUninstallKey);
end;

function LegacyUninstallString(): String;
begin
  Result := '';
  if not RegQueryStringValue(HKLM64, LegacyUninstallKey, 'UninstallString', Result) then
    if not RegQueryStringValue(HKCU, LegacyUninstallKey, 'UninstallString', Result) then
      Result := '';
  Result := RemoveQuotes(Result);
end;

function PrepareToInstall(var NeedsRestart: Boolean): String;
var
  Old: String;
  Code, I: Integer;
begin
  Result := '';
  if not LegacyKeyExists() then
    Exit;
  Old := LegacyUninstallString();
  Log('Removing the standalone Multistream install first: ' + Old);
  if (Old = '') or not FileExists(Old) then
  begin
    Result := LegacyFailed;
    Exit;
  end;
  // Require the uninstaller's exit code 0 AND its registry entry gone afterwards.
  if not Exec(Old, '/VERYSILENT /SUPPRESSMSGBOXES /NORESTART', '', SW_HIDE, ewWaitUntilTerminated, Code) or (Code <> 0) then
  begin
    Result := LegacyFailed;
    Exit;
  end;
  // The uninstaller finishes in a second process: wait (up to 60 s) until it has removed its registry
  // entry AND deleted itself. Require both, or abort.
  I := 0;
  while (LegacyKeyExists() or FileExists(Old)) and (I < 300) do
  begin
    Sleep(200);
    I := I + 1;
  end;
  if LegacyKeyExists() or FileExists(Old) then
    Result := LegacyFailed;
end;

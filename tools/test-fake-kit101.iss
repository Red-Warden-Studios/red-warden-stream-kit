; TEST ONLY. A tiny stand-in for the Red Warden Stream Kit 1.0.1 installer (two tool components), so
; tools\test-installer.ps1 can lay down the 1.0.1 state and check that the 1.1.0 installer upgrades it in
; place. It has the same TestRoot AppId as installer\stream-kit.iss, the same component names and the same
; plugin folders and file names as the released 1.0.1 (dll + en-US.ini per plugin, nothing else), with dummy
; contents. It registers an uninstall entry in HKCU (no admin). Compiled into %TEMP% by the test; never shipped.
#ifndef TestRoot
  #error TestRoot must be passed: /DTestRoot=<scratch folder>
#endif

[Setup]
AppId=RWS-KIT-TESTROOT-035D8EC0
AppName=Red Warden Stream Kit
AppVersion=1.0.1
DefaultDirName={#TestRoot}\_kit
PrivilegesRequired=lowest
DisableDirPage=yes
DisableProgramGroupPage=yes
ArchitecturesAllowed=x64compatible
ArchitecturesInstallIn64BitMode=x64compatible
Compression=none
OutputBaseFilename=fake-kit101-setup

[Types]
Name: "full"; Description: "Full installation (both tools)"
Name: "custom"; Description: "Custom installation"; Flags: iscustom

[Components]
Name: "multistream"; Description: "Red Warden Multistream"; Types: full custom
Name: "preflight"; Description: "Red Warden Pre-Flight"; Types: full custom

[Files]
; Any small file will do as a dummy payload: this script itself.
Source: "{#SourcePath}\test-fake-kit101.iss"; DestDir: "{#TestRoot}\rws-multistream\bin\64bit"; DestName: "rws-multistream.dll"; Flags: ignoreversion; Components: multistream
Source: "{#SourcePath}\test-fake-kit101.iss"; DestDir: "{#TestRoot}\rws-multistream\data\locale"; DestName: "en-US.ini"; Flags: ignoreversion; Components: multistream
Source: "{#SourcePath}\test-fake-kit101.iss"; DestDir: "{#TestRoot}\rws-preflight\bin\64bit"; DestName: "rws-preflight.dll"; Flags: ignoreversion; Components: preflight
Source: "{#SourcePath}\test-fake-kit101.iss"; DestDir: "{#TestRoot}\rws-preflight\data\locale"; DestName: "en-US.ini"; Flags: ignoreversion; Components: preflight
Source: "{#SourcePath}\test-fake-kit101.iss"; DestDir: "{app}"; DestName: "LICENSE.txt"; Flags: ignoreversion
Source: "{#SourcePath}\test-fake-kit101.iss"; DestDir: "{app}"; DestName: "NOTICE.txt"; Flags: ignoreversion
Source: "{#SourcePath}\test-fake-kit101.iss"; DestDir: "{app}"; DestName: "logo.ico"; Flags: ignoreversion

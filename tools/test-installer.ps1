<#
  Installer test for the Red Warden Stream Kit (one plugin, rws-stream-kit.dll). package.ps1 runs it as a
  required release gate (signed and -NoSign) after staging the DLL; it can also be run by hand once staging\
  exists:

    powershell -ExecutionPolicy Bypass -File tools\test-installer.ps1

  Everything happens in a scratch install root (<kit>\.testbed\root) with a TestRoot build (ISCC
  /DTestRoot=<root>: installs under that folder instead of C:\ProgramData, no admin, its own AppId, a
  TEST-ONLY legacy AppId, never signs). It never runs a non-TestRoot install, never touches a real OBS or
  C:\ProgramData\obs-studio (it refuses to start if an obs64.exe is running), and uses no real OBS: the
  "OBS is running" case runs a copy of powershell.exe named obs64.exe from %TEMP% (captured PID, stopped in finally).
  Cases:
    - static: release .iss keeps Multistream's real legacy AppId and the 1.0.1 Kit AppId, has no components,
      and its [InstallDelete] holds only "files" and "dirifempty" lines (never plugin_config)
    - legacy removal fails closed: a stale legacy entry that cannot be uninstalled aborts, installs nothing
    - OBS-running block: refuses with a non-zero exit, installs nothing, and leaves an existing install alone
    - clean install: one plugin (DLL + locale, no pdb), no old folders; uninstall leaves nothing of ours
    - upgrade from Kit 1.0.1 (a fake 1.0.1 installer with the same AppId, components multistream /
      preflight / both): old plugin folders gone, new plugin installed, one uninstall entry at the new version
    - upgrade over a development rws-inflight and an old rws-beacon folder
    - a file a user dropped into an old plugin folder survives (dirifempty keeps the folder), and survives
      uninstall; plugin_config sentinels (inside and outside the install root) are untouched by everything
    - legacy standalone Multistream 1.0.0 (fake, test-only AppId) is uninstalled by the Kit installer
  Writes PASS/FAIL lines and a final RESULT line to .testbed\run-installer.txt.
#>
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$bed = Join-Path $root ".testbed"
New-Item -ItemType Directory -Force $bed | Out-Null
$out = Join-Path $bed "run-installer.txt"
Remove-Item $out -ErrorAction SilentlyContinue
$script:fails = 0
function Say($s) { Write-Host $s; Add-Content -Path $out -Value $s -Encoding ASCII }
function Check($cond, $msg) {
  if ($cond) { Say "PASS: $msg" } else { Say "FAIL: $msg"; $script:fails++ }
}

# Tool folders: the Multistream logo is the installer icon. multistream\ inside this repo (public layout) when it
# exists, else the sibling red-warden-multistream folder (development layout).
if (Test-Path (Join-Path $root "multistream")) { $ms = Join-Path $root "multistream" }
else { $ms = Join-Path (Split-Path -Parent $root) "red-warden-multistream" }
$msLogo = Join-Path $ms "brand\logo.ico"
$plugin = Join-Path $root "plugin"
$version = (Get-Content (Join-Path $plugin "buildspec.json") -Raw | ConvertFrom-Json).version
$testRoot = Join-Path $bed "root"
$cfgRoot = Join-Path $bed "fake-obs-config\plugin_config"   # stands in for %APPDATA%\obs-studio\plugin_config
$stage = Join-Path $root "staging"
$iscc = Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"
$testSetup = Join-Path $bed "RedWardenStreamKit-$version-TestRoot-Setup.exe"
$tmp = Join-Path $env:TEMP ("rws-kit-test-" + [guid]::NewGuid().ToString("N"))   # unique per run; only this folder is deleted
$fakeSetup = Join-Path $tmp "fake-legacy-setup.exe"
$fake101Setup = Join-Path $tmp "fake-kit101-setup.exe"
$uninstallBase = "HKCU:\Software\Microsoft\Windows\CurrentVersion\Uninstall"
$legacyKey = "$uninstallBase\RWS-KIT-TESTLEGACY-DA4CA68C_is1"
$kitKey = "$uninstallBase\RWS-KIT-TESTROOT-035D8EC0_is1"
# The AppId Kit 1.0.1 shipped with. It must never change: it is how 1.1.0 finds and replaces 1.0.1.
$kitRealAppId = "{035D8EC0-7626-4A56-BF6D-A31548414EFA}"

$kitDll = Join-Path $testRoot "rws-stream-kit\bin\64bit\rws-stream-kit.dll"
$kitLoc = Join-Path $testRoot "rws-stream-kit\data\locale\en-US.ini"
$stagedDll = Join-Path $stage "rws-stream-kit\bin\64bit\rws-stream-kit.dll"
$uninst = Join-Path $testRoot "_kit\unins000.exe"
$oldMods = "rws-multistream", "rws-preflight", "rws-inflight", "rws-beacon"
function OldPath($mod, $rel) { Join-Path $testRoot "$mod\$rel" }

function ResetRoot {
  # Scratch only: refuse to delete anything that is not inside this project's .testbed.
  if (!$testRoot.StartsWith($bed, [StringComparison]::OrdinalIgnoreCase)) { throw "test root outside .testbed" }
  if ($testRoot -match 'ProgramData|Program Files') { throw "test root looks like a real install location" }
  if (Test-Path $testRoot) { Remove-Item -Recurse -Force $testRoot }
  New-Item -ItemType Directory -Force $testRoot | Out-Null
  foreach ($k in $legacyKey, $kitKey) { if (Test-Path $k) { Remove-Item -Recurse -Force $k } }
  $script:keepers = @()
  $script:keepHash = @{}
}
function Sha($p) { (Get-FileHash $p -Algorithm SHA256).Hash }
function Put($path, $text) {
  New-Item -ItemType Directory -Force (Split-Path -Parent $path) | Out-Null
  Set-Content -Path $path -Value $text -Encoding ASCII
}
# Files that are NOT ours and must survive everything: remembered with their hash.
function Keep($path, $text) {
  Put $path $text
  $script:keepers += $path
  $script:keepHash[$path] = Sha $path
}
function KeepersIntact($label) {
  $bad = @($script:keepers | Where-Object { !(Test-Path $_) -or ((Sha $_) -ne $script:keepHash[$_]) })
  Check ($bad.Count -eq 0) "$label : $($script:keepers.Count) user/config file(s) untouched$(if ($bad.Count) { ' (changed or gone: ' + (($bad | ForEach-Object { Split-Path -Leaf $_ }) -join ', ') + ')' })"
}
# User settings the installer must never touch: plugin_config both outside the install root (the real place)
# and, as a hostile placement, inside it.
function SeedConfig {
  Keep (Join-Path $cfgRoot "rws-multistream\settings.json") '{"destinations":[]}'
  Keep (Join-Path $cfgRoot "rws-multistream\hotkeys.json") '{"hotkeys":1}'
  Keep (Join-Path $cfgRoot "rws-multistream\reports\session-1.json") '{"report":1}'
  Keep (Join-Path $cfgRoot "rws-preflight\settings.json") '{"checks":1}'
  Keep (Join-Path $cfgRoot "rws-inflight\settings.json") '{"lights":1}'
  Keep (Join-Path $cfgRoot "rws-stream-kit\settings.json") '{"tools":1}'
  Keep (Join-Path $testRoot "plugin_config\rws-multistream\settings.json") '{"inside":1}'
}
function RunProc($exe, $argList, $label, [switch]$ExpectFailure) {
  $p = Start-Process -FilePath $exe -ArgumentList $argList -PassThru -WindowStyle Hidden
  if (!$p.WaitForExit(120000)) {
    Stop-Process -Id $p.Id -Force   # captured PID only
    Check $false "$label finished within 120 s"
    return -1
  }
  if ($ExpectFailure) { Check ($p.ExitCode -ne 0) "$label refused with a non-zero exit code (got $($p.ExitCode))" }
  else { Check ($p.ExitCode -eq 0) "$label exit code 0 (got $($p.ExitCode))" }
  return $p.ExitCode
}
function Install($label, [switch]$ExpectFailure) {
  $log = Join-Path $bed "install-$label.log"
  $a = @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/LOG=`"$log`"")
  if ($ExpectFailure) { return RunProc $testSetup $a "install 1.1.0 ($label)" -ExpectFailure }
  return RunProc $testSetup $a "install 1.1.0 ($label)"
}
# The fake Kit 1.0.1 installer lays down the released 1.0.1 state (components as 1.0.1 had them).
function Install101($components, $label) {
  $log = Join-Path $bed "install101-$label.log"
  $a = @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART", "/COMPONENTS=`"$components`"", "/LOG=`"$log`"")
  return RunProc $fake101Setup $a "fake Kit 1.0.1 install ($components)"
}
function AllFiles { @(Get-ChildItem $testRoot -Recurse -File -ErrorAction SilentlyContinue) }
# Files in the install root that are not a protected user/config file.
function OurFiles { @(AllFiles | Where-Object { $script:keepers -notcontains $_.FullName }) }
function NoOldFolders($label) {
  $left = @($oldMods | Where-Object { Test-Path (Join-Path $testRoot $_) })
  Check ($left.Count -eq 0) "$label : no old plugin folder is left ($($left -join ', '))"
}
function NewPluginOk($label) {
  Check ((Test-Path $kitDll) -and ((Sha $kitDll) -eq (Sha $stagedDll))) "$label : rws-stream-kit.dll installed and is the staged DLL"
  Check (Test-Path $kitLoc) "$label : rws-stream-kit locale installed"
  Check ((Test-Path (Join-Path $testRoot "rws-stream-kit\data\manual\index.html")) -and (Test-Path (Join-Path $testRoot "rws-stream-kit\data\manual\levels.html"))) "$label : manual installed (index.html + levels.html)"
  Check (!(Test-Path (Join-Path $testRoot "rws-stream-kit\bin\64bit\rws-stream-kit.pdb"))) "$label : no .pdb installed"
  Check (Test-Path $uninst) "$label : uninstaller present"
}
function KitUninstall($label) {
  RunProc $uninst @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART") "uninstall ($label)" | Out-Null
  for ($i = 0; $i -lt 150 -and (Test-Path $uninst); $i++) { Start-Sleep -Milliseconds 200 }   # second phase deletes itself
  $left = @(OurFiles)
  Check ($left.Count -eq 0) "uninstall ($label): nothing of ours is left in the install root (left: $($left.Count) $(($left | Select-Object -First 5 | ForEach-Object { $_.FullName.Substring($testRoot.Length) }) -join ', '))"
  Check (!(Test-Path (Join-Path $testRoot "rws-stream-kit"))) "uninstall ($label): rws-stream-kit folder gone"
  Check (!(Test-Path (Join-Path $testRoot "_kit"))) "uninstall ($label): installer folder gone"
  Check (!(Test-Path $kitKey)) "uninstall ($label): uninstall entry gone"
  KeepersIntact "uninstall ($label)"
}

$dummyObs = $null
try {
  # --- Safety and preconditions ---
  $running = @(Get-Process -Name obs64 -ErrorAction SilentlyContinue)
  if ($running.Count -gt 0) { throw "an OBS (obs64.exe) is already running; this test will not touch it. Close it and re-run" }
  Check (Test-Path $stagedDll) "staging holds rws-stream-kit.dll"
  Check (Test-Path (Join-Path $stage "rws-stream-kit\data\locale\en-US.ini")) "staging holds the locale"
  Check (Test-Path (Join-Path $stage "rws-stream-kit\data\manual\index.html")) "staging holds the manual"
  Check ($version -match '^\d+\.\d+\.\d+$') "version from plugin\buildspec.json: $version"
  Check (Test-Path $iscc) "Inno Setup compiler found"
  if ($script:fails -gt 0) { throw "preconditions failed (run tools\package.ps1 first)" }

  # --- Static checks on the release script ---
  $issPath = Join-Path $root "installer\stream-kit.iss"
  $iss = Get-Content $issPath -Raw
  $branches = $iss -split '(?m)^#else\s*$'
  $msIssPath = Join-Path $ms "installer\rws-multistream.iss"
  if (Test-Path $msIssPath) {
    $msIss = Get-Content $msIssPath -Raw
    $realId = if ($msIss -match 'AppId=\{\{([0-9A-Fa-f-]+)\}') { $Matches[1] } else { "" }
    Check ($realId.Length -gt 30) "found Multistream's own AppId ($realId)"
    Check ($branches[1] -match [regex]::Escape('#define LegacyAppId "{' + $realId + '}"')) "release build: LegacyAppId is Multistream's real AppId"
  } else {
    Say "SKIP: legacy installer script not present in this layout (static legacy AppId check)"
  }
  Check ($branches[0] -match 'LegacyAppId "RWS-KIT-TESTLEGACY') "TestRoot build: LegacyAppId is the test-only id"
  Check ($iss -match [regex]::Escape("AppId={$kitRealAppId")) "release build: Kit AppId is still the one 1.0.1 shipped ($kitRealAppId), so it upgrades in place"
  $fake101 = Get-Content (Join-Path $root "tools\test-fake-kit101.iss") -Raw
  $idTest = [regex]::Match($iss, '(?m)^AppId=(RWS-KIT-TESTROOT-\S+)').Groups[1].Value
  Check ($idTest.Length -gt 10 -and $fake101 -match [regex]::Escape("AppId=$idTest")) "fake 1.0.1 installer uses the TestRoot Kit AppId ($idTest)"
  Check ($iss -notmatch '(?m)^\[(Components|Types)\]') "no [Components] or [Types]: one plugin, tools are switched in the dock"
  $del = [regex]::Match($iss, '(?ms)^\[InstallDelete\](.*?)^\[').Groups[1].Value
  $delLines = @($del -split "`n" | ForEach-Object { $_.Trim() } | Where-Object { $_ -ne "" -and !$_.StartsWith(";") })
  Check ($delLines.Count -gt 0) "[InstallDelete] has $($delLines.Count) entries"
  Check (@($delLines | Where-Object { $_ -notmatch '^Type: (files|dirifempty);' }).Count -eq 0) "[InstallDelete] holds only 'files' and 'dirifempty' entries (never filesandordirs)"
  Check (@($delLines | Where-Object { $_ -match 'plugin_config|Credential' }).Count -eq 0) "[InstallDelete] never names plugin_config or credentials"
  foreach ($mod in $oldMods) {
    Check (@($delLines | Where-Object { $_ -match [regex]::Escape("\$mod\bin\64bit\$mod.dll") }).Count -eq 1) "[InstallDelete] removes $mod.dll by exact name"
  }

  # --- Compile the TestRoot installers: Kit 1.1.0, fake legacy Multistream, fake Kit 1.0.1 ---
  ResetRoot
  Remove-Item $testSetup -ErrorAction SilentlyContinue
  & $iscc "/DAppVersion=$version" "/DStage=$stage" "/DNoSign" "/DMsLogo=$msLogo" "/DTestRoot=$testRoot" "/Q" "installer\stream-kit.iss"
  Check ($LASTEXITCODE -eq 0 -and (Test-Path $testSetup)) "ISCC compiled the TestRoot installer"
  if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp }
  New-Item -ItemType Directory -Force $tmp | Out-Null
  & $iscc "/DMsLogo=$msLogo" "/DTestRoot=$testRoot" "/O$tmp" "/Q" "tools\test-fake-legacy.iss"
  Check ($LASTEXITCODE -eq 0 -and (Test-Path $fakeSetup)) "ISCC compiled the fake legacy Multistream installer (into %TEMP%)"
  & $iscc "/DTestRoot=$testRoot" "/O$tmp" "/Q" "tools\test-fake-kit101.iss"
  Check ($LASTEXITCODE -eq 0 -and (Test-Path $fake101Setup)) "ISCC compiled the fake Kit 1.0.1 installer (into %TEMP%)"
  if (!(Test-Path $testSetup) -or !(Test-Path $fakeSetup) -or !(Test-Path $fake101Setup)) { throw "could not compile the installers" }
  if (Test-Path $cfgRoot) { Remove-Item -Recurse -Force (Split-Path -Parent $cfgRoot) }

  # --- 1. Stale legacy entry that cannot be uninstalled: abort, install nothing ---
  ResetRoot
  SeedConfig
  New-Item -Path $legacyKey -Force | Out-Null
  New-ItemProperty -Path $legacyKey -Name UninstallString -Value ('"' + (Join-Path $tmp "no-such-unins000.exe") + '"') | Out-Null
  Install "legacyfail" -ExpectFailure | Out-Null
  Check (@(OurFiles).Count -eq 0) "legacy removal failed: nothing was installed"
  Check (Test-Path $legacyKey) "legacy removal failed: the legacy entry is untouched"
  KeepersIntact "legacy removal failed"
  Remove-Item -Recurse -Force $legacyKey

  # --- 2. OBS running (a copy of powershell.exe named obs64.exe, captured PID): refuses, leaves the old install alone ---
  ResetRoot
  SeedConfig
  Install101 "multistream,preflight" "obsrunning" | Out-Null
  $before = @(OurFiles | ForEach-Object { $_.FullName + "|" + $_.Length } | Sort-Object)
  $fakeObsDir = Join-Path $tmp "obs"
  New-Item -ItemType Directory -Force $fakeObsDir | Out-Null
  Copy-Item (Join-Path $env:SystemRoot "System32\WindowsPowerShell\v1.0\powershell.exe") (Join-Path $fakeObsDir "obs64.exe")
  $dummyObs = Start-Process (Join-Path $fakeObsDir "obs64.exe") -ArgumentList @("-NoProfile", "-Command", "Start-Sleep -Seconds 180") -WindowStyle Hidden -PassThru
  Start-Sleep -Seconds 2
  Check (!$dummyObs.HasExited) "stand-in obs64.exe is running (PID $($dummyObs.Id))"
  Install "obsrunning" -ExpectFailure | Out-Null
  $after = @(OurFiles | ForEach-Object { $_.FullName + "|" + $_.Length } | Sort-Object)
  Check (($before -join "`n") -eq ($after -join "`n")) "OBS running: the existing 1.0.1 install was left exactly as it was ($($after.Count) files)"
  Check (!(Test-Path $kitDll)) "OBS running: the new plugin was not installed"
  KeepersIntact "OBS running"
  Stop-Process -Id $dummyObs.Id -Force -ErrorAction SilentlyContinue   # ONLY the captured PID
  Check ($dummyObs.WaitForExit(20000)) "stand-in obs64.exe stopped"
  $dummyObs = $null

  # --- 3. Clean install, re-install over itself, uninstall ---
  ResetRoot
  SeedConfig
  Install "clean" | Out-Null
  NewPluginOk "clean install"
  NoOldFolders "clean install"
  Check (@(OurFiles | Where-Object { $_.FullName -notlike "$testRoot\rws-stream-kit\*" -and $_.FullName -notlike "$testRoot\_kit\*" }).Count -eq 0) "clean install: the installer wrote only under rws-stream-kit\ and its own _kit\ folder"
  $dllHash = Sha $kitDll
  Install "again" | Out-Null
  Check ((Sha $kitDll) -eq $dllHash) "re-running the installer over itself leaves the same DLL"
  Check ((Get-ItemProperty $kitKey -ErrorAction SilentlyContinue).DisplayVersion -eq $version) "uninstall entry says version $version"
  KeepersIntact "clean install"
  KitUninstall "clean"

  # --- 4. Upgrade from Kit 1.0.1, every subset of the old tools ---
  foreach ($set in @("multistream,preflight", "multistream", "preflight")) {
    ResetRoot
    SeedConfig
    Install101 $set "up-$($set -replace ',','-')" | Out-Null
    $hadMs = $set -match 'multistream'; $hadPf = $set -match 'preflight'
    Check (((Test-Path (OldPath "rws-multistream" "bin\64bit\rws-multistream.dll")) -eq $hadMs) -and ((Test-Path (OldPath "rws-preflight" "bin\64bit\rws-preflight.dll")) -eq $hadPf)) "1.0.1 ($set): old state laid down"
    Check ((Get-ItemProperty $kitKey -ErrorAction SilentlyContinue).DisplayVersion -eq "1.0.1") "1.0.1 ($set): registered as version 1.0.1"
    Install "up-$($set -replace ',','-')" | Out-Null
    NewPluginOk "upgrade from 1.0.1 ($set)"
    NoOldFolders "upgrade from 1.0.1 ($set)"
    Check ((Get-ItemProperty $kitKey -ErrorAction SilentlyContinue).DisplayVersion -eq $version) "upgrade from 1.0.1 ($set): the same uninstall entry now says $version (upgraded in place)"
    Check (@(Get-ChildItem $uninstallBase | Where-Object { $_.PSChildName -like 'RWS-KIT-TESTROOT-*' }).Count -eq 1) "upgrade from 1.0.1 ($set): exactly one Kit uninstall entry"
    KeepersIntact "upgrade from 1.0.1 ($set)"
    KitUninstall "after upgrade from 1.0.1 ($set)"
    NoOldFolders "after uninstall of upgrade from 1.0.1 ($set)"
  }

  # --- 5. Upgrade over a development rws-inflight and an old rws-beacon, with no Kit install at all ---
  ResetRoot
  SeedConfig
  Put (OldPath "rws-inflight" "bin\64bit\rws-inflight.dll") "dev dll"
  Put (OldPath "rws-inflight" "bin\64bit\rws-inflight.pdb") "dev pdb"
  Put (OldPath "rws-inflight" "data\locale\en-US.ini") "dev locale"
  Put (OldPath "rws-beacon" "bin\64bit\rws-beacon.dll") "old dll"
  Put (OldPath "rws-beacon" "data\locale\en-US.ini") "old locale"
  Install "devinflight" | Out-Null
  NewPluginOk "dev rws-inflight + rws-beacon"
  NoOldFolders "dev rws-inflight + rws-beacon"
  KeepersIntact "dev rws-inflight + rws-beacon"
  KitUninstall "dev inflight"

  # --- 6. Files the user put into old plugin folders survive; the folders stay; uninstall leaves them ---
  ResetRoot
  SeedConfig
  Install101 "multistream,preflight" "userfiles" | Out-Null
  Put (OldPath "rws-inflight" "bin\64bit\rws-inflight.dll") "dev dll"
  Put (OldPath "rws-inflight" "data\locale\en-US.ini") "dev locale"
  Keep (OldPath "rws-multistream" "my-own-notes.txt") "mine, multistream"
  Keep (OldPath "rws-preflight" "bin\64bit\keep-me.txt") "mine, preflight"
  Keep (OldPath "rws-inflight" "data\locale\de-DE.ini") "mine, inflight locale"
  Install "userfiles" | Out-Null
  NewPluginOk "user files in old folders"
  KeepersIntact "upgrade with user files in the old folders"
  Check (!(Test-Path (OldPath "rws-multistream" "bin\64bit\rws-multistream.dll")) -and !(Test-Path (OldPath "rws-multistream" "data\locale\en-US.ini"))) "user files: Multistream's old dll and locale removed"
  Check (!(Test-Path (OldPath "rws-preflight" "bin\64bit\rws-preflight.dll")) -and !(Test-Path (OldPath "rws-preflight" "data\locale\en-US.ini"))) "user files: Pre-Flight's old dll and locale removed"
  Check (!(Test-Path (OldPath "rws-inflight" "bin\64bit\rws-inflight.dll")) -and !(Test-Path (OldPath "rws-inflight" "data\locale\en-US.ini"))) "user files: In-Flight's dev dll and locale removed"
  Check (!(Test-Path (OldPath "rws-multistream" "bin")) -and !(Test-Path (OldPath "rws-multistream" "data"))) "user files: rws-multistream's now-empty bin and data folders removed"
  Check (!(Test-Path (OldPath "rws-preflight" "data")) -and !(Test-Path (OldPath "rws-inflight" "bin"))) "user files: other now-empty folders removed"
  Check ((Test-Path (OldPath "rws-multistream" "")) -and (Test-Path (OldPath "rws-preflight" "bin\64bit")) -and (Test-Path (OldPath "rws-inflight" "data\locale"))) "user files: the folders that still hold a user file were kept"
  KitUninstall "user files in old folders"
  KeepersIntact "uninstall with user files in the old folders"

  # --- 7. Legacy removal: a fake standalone Multistream 1.0.0 installed first, then the Kit ---
  ResetRoot
  SeedConfig
  RunProc $fakeSetup @("/VERYSILENT", "/SUPPRESSMSGBOXES", "/NORESTART") "fake legacy install" | Out-Null
  Check (Test-Path $legacyKey) "legacy: its uninstall entry is registered"
  Check (Test-Path (OldPath "rws-multistream" "unins000.exe")) "legacy: its uninstaller is in the plugin folder"
  Check (Test-Path (OldPath "rws-multistream" "bin\64bit\rws-multistream.dll")) "legacy: its (dummy) DLL is installed"
  Install "legacy" | Out-Null
  Check (!(Test-Path $legacyKey)) "legacy: its uninstall entry is gone after the Kit install"
  Check (!(Test-Path (OldPath "rws-multistream" "unins000.exe"))) "legacy: its uninstaller is gone"
  NewPluginOk "after legacy removal"
  NoOldFolders "after legacy removal"
  KeepersIntact "legacy removal"
  KitUninstall "after legacy"
} catch {
  Say "FAIL: $($_.Exception.Message)"
  $script:fails++
} finally {
  if ($dummyObs -and !$dummyObs.HasExited) { Stop-Process -Id $dummyObs.Id -Force -ErrorAction SilentlyContinue }   # ONLY the captured PID
  foreach ($k in $legacyKey, $kitKey) { if (Test-Path $k) { Remove-Item -Recurse -Force $k -ErrorAction SilentlyContinue } }
  if (Test-Path $tmp) { Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue }
}
Say ($(if ($script:fails -eq 0) { "RESULT: PASS" } else { "RESULT: FAIL ($($script:fails) failure(s))" }))
if ($script:fails -ne 0) { exit 1 }

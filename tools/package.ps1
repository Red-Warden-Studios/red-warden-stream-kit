<#
  Package the Red Warden Stream Kit: ONE plugin (rws-stream-kit.dll = Multistream + Pre-Flight + In-Flight).

    powershell -ExecutionPolicy Bypass -File tools\package.ps1            # signed release -> dist\
    powershell -ExecutionPolicy Bypass -File tools\package.ps1 -NoSign    # unsigned local test build -> dist-unsigned\

  Builds NOTHING. It takes the plugin's existing RelWithDebInfo DLL, locale and PDB (plugin\build_x64\...)
  and refuses unless:
    - one version everywhere: plugin\buildspec.json (the source of truth) == kKitVersion in
      plugin\src\common\update-manifest.hpp == "latest" in installer\stream-kit.json == the DLL's
      ProductVersion;
    - no source file is newer than the DLL (plugin\src, plugin\data, the three tools' src\);
    - tools\check-test-results.ps1 passes: every unit-test exe and every release harness step has a
      fresh status (newer than the DLL) with exit 0 and result PASS, recorded against the SHA-256 of this
      very DLL; its lists must also equal plugin\tools\run-all.ps1's. run-done.txt is not trusted.
    - the DLL's bytes are compared (SHA-256) with the tested ones again right before it is staged and again
      right before it is signed.
  Then tools\test-installer.ps1 must pass (a TestRoot build in a scratch folder), BEFORE anything is signed.

  Output of a signed run, in dist\: RedWardenStreamKit-<ver>-Setup.exe, RedWardenStreamKit-<ver>.zip,
  SHA256SUMS.txt, symbols\ (the PDB, for our own crash symbolication; NOT shipped). Staging is in staging\.

  -NoSign is for a local test installer only. It writes to dist-unsigned\ (never dist\), names every file
  ...-UNSIGNED-TEST-..., and writes no SHA256SUMS.txt, so it can never be picked up as the release by
  publish-source.ps1 or the release steps. Never upload anything from dist-unsigned\.

  Signing (skipped with -NoSign): SSL.com OV certificate on the YubiKey, one PIN prompt per signature
  (the DLL, uninstaller, setup.exe). Timestamp MUST be RFC 3161 (/tr). Run by the release owner, not by automation.
#>
param(
  [switch]$NoSign,
  [string]$Thumbprint = "FE5CCAC231978A6489E4DBAE2A333E2F633053E4",
  [string]$Config = "RelWithDebInfo"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

function Fail($msg) { Write-Host "[FAIL] $msg" -ForegroundColor Red; exit 1 }

$plugin = Join-Path $root "plugin"
# The three tools' source trees (compiled into the one DLL): sibling folders of the dev layout, or
# multistream\ preflight\ inflight\ inside this repo (public layout).
$projects = Split-Path -Parent $root
function Tool-Dir($dev, $pub) {
  $inRepo = Join-Path $root $pub
  if (Test-Path $inRepo) { return $inRepo }
  return (Join-Path $projects $dev)
}
$ms = Tool-Dir "red-warden-multistream" "multistream"
$pf = Tool-Dir "red-warden-preflight" "preflight"
$inf = Tool-Dir "red-warden-inflight" "inflight"
$msLogo = Join-Path $ms "brand\logo.ico"

# ONE version source: the plugin's buildspec.json. Everything else must agree with it.
$kitVersion = (Get-Content (Join-Path $plugin "buildspec.json") -Raw | ConvertFrom-Json).version
if (!$kitVersion) { Fail "plugin\buildspec.json has no version" }

$outName = if ($NoSign) { "dist-unsigned" } else { "dist" }
$dist = Join-Path $root $outName
$stage = Join-Path $root "staging"
$tag = if ($NoSign) { "-UNSIGNED-TEST" } else { "" }
$iscc = Join-Path $env:LOCALAPPDATA "Programs\Inno Setup 6\ISCC.exe"
$signtool = $null
if (!$NoSign) {
  $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object FullName -like '*\x64\*' | Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
}

# --- Gates: refuse to package anything we can't stand behind --------------------------------------
if (!(Test-Path $iscc)) { Fail "Inno Setup not found at $iscc (winget install JRSoftware.InnoSetup --scope user)" }
# leak_scout: RWS_LEAK_SCOUT, else the sibling Red Warden Studios workspace (relative to this script).
$leakScout = $env:RWS_LEAK_SCOUT
if ([string]::IsNullOrWhiteSpace($leakScout)) {
  $leakScout = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot "..\..\..\Red Warden Studios\tools\leak_scout\leak_scout.py"))
}
if (!(Test-Path $leakScout)) {
  if ($NoSign) { Write-Host "[WARN] leak_scout not found: unsigned test build is not leak-scanned" -ForegroundColor Yellow; $leakScout = $null }
  else { Fail "leak_scout not found. Set RWS_LEAK_SCOUT to leak_scout.py; a signed release is never packaged without the leak scan." }
}
if ($leakScout -and !(Get-Command python -ErrorAction SilentlyContinue)) {
  if ($NoSign) { Write-Host "[WARN] python not found: unsigned test build is not leak-scanned" -ForegroundColor Yellow; $leakScout = $null }
  else { Fail "python is not on PATH; a signed release is never packaged without the leak scan." }
}
foreach ($f in "LICENSE", "NOTICE.txt", "installer\stream-kit.iss", "installer\stream-kit.json") { if (!(Test-Path (Join-Path $root $f))) { Fail "Missing $f" } }
if (!(Test-Path $msLogo)) { Fail "Missing Multistream brand\logo.ico ($msLogo)" }

$manifest = Get-Content (Join-Path $root "installer\stream-kit.json") -Raw | ConvertFrom-Json
if ($manifest.latest -ne $kitVersion) { Fail "installer\stream-kit.json says latest '$($manifest.latest)', plugin\buildspec.json says $kitVersion" }
# The plugin compares the update manifest against a compiled-in kit version: it must be this one.
$hpp = Join-Path $plugin "src\common\update-manifest.hpp"
$hppText = Get-Content $hpp -Raw
$m = [regex]::Match($hppText, 'kKitVersion\s*=\s*"([^"]+)"')
if (!$m.Success) { Fail "$hpp has no kKitVersion" }
if ($m.Groups[1].Value -ne $kitVersion) { Fail "kKitVersion in $hpp is '$($m.Groups[1].Value)', plugin\buildspec.json says $kitVersion" }

$dll = Join-Path $plugin "build_x64\$Config\rws-stream-kit.dll"
$pdb = Join-Path $plugin "build_x64\$Config\rws-stream-kit.pdb"
$locale = Join-Path $plugin "data\locale"
if (!(Test-Path $dll)) { Fail "Build first: $dll not found" }
if (!(Test-Path (Join-Path $locale "en-US.ini"))) { Fail "Missing $locale\en-US.ini" }
$fv = (Get-Item $dll).VersionInfo.ProductVersion
if ($fv -ne $kitVersion) { Fail "$dll says version '$fv' but plugin\buildspec.json says '$kitVersion'. Re-run configure + build." }

# Stale-DLL hole: a DLL built before the last source edit proves nothing. Refuse if ANY file under the
# plugin's src\ or data\, a tool's src\, the plugin CMakeLists.txt / buildspec.json or Multistream's
# embedded logos is newer than the DLL.
$dllTime = (Get-Item $dll).LastWriteTime
$watch = @(Get-ChildItem (Join-Path $plugin "src"), (Join-Path $plugin "data"), (Join-Path $ms "src"), (Join-Path $pf "src"), (Join-Path $inf "src") -Recurse -File -ErrorAction SilentlyContinue) +
         @(Get-Item (Join-Path $plugin "CMakeLists.txt"), (Join-Path $plugin "buildspec.json")) +
         @(Get-ChildItem (Join-Path $ms "brand") -Filter "logo-*.png" -File -ErrorAction SilentlyContinue)
$newer = $watch | Where-Object { $_.LastWriteTime -gt $dllTime } | Sort-Object LastWriteTime | Select-Object -First 1
if ($newer) { Fail "$($newer.FullName) (modified $($newer.LastWriteTime)) is newer than $dll (built $dllTime). Rebuild and re-run the tests." }

# Test gate: the test-results checker requires every unit-test exe and every release harness to have a
# fresh passing result, each result file newer than the DLL. run-done.txt is not consulted.
$checker = Join-Path $PSScriptRoot "check-test-results.ps1"
& powershell -NoProfile -ExecutionPolicy Bypass -File $checker -PluginDir $plugin -Dll $dll
if ($LASTEXITCODE -ne 0) { Fail "The Kit tests did not all pass against its DLL (see above)" }
# The bytes the tests ran against (recorded by the test runner, already matched to this DLL by the checker).
$testedSha = $null
foreach ($l in Get-Content (Join-Path $plugin ".testbed\status-unit.txt")) { if ($l -match '^dll_sha256=([0-9a-f]{64})$') { $testedSha = $Matches[1] } }
if (!$testedSha) { Fail "plugin\.testbed\status-unit.txt records no DLL SHA-256" }
function Assert-Tested($what, $file) {
  $h = (Get-FileHash -LiteralPath $file -Algorithm SHA256).Hash.ToLower()
  if ($h -ne $testedSha) { Fail "$what ($file) has SHA-256 $h, but the tests ran against $testedSha. Rebuild and re-run every test." }
}

if (!$NoSign) {
  if (!$signtool) { Fail "signtool.exe not found (Windows SDK)" }
  if (!(Get-ChildItem Cert:\CurrentUser\My | Where-Object Thumbprint -eq $Thumbprint)) {
    Fail "Code-signing cert $Thumbprint not in the store. Plug in the YubiKey (minidriver installed?)."
  }
}

function Sign($file, $desc) {
  if ($NoSign) { return }
  & $signtool sign /sha1 $Thumbprint /fd sha256 /tr http://ts.ssl.com /td sha256 /d $desc $file
  if ($LASTEXITCODE -ne 0) { Fail "signtool sign failed on $file" }
  & $signtool verify /pa /q $file
  if ($LASTEXITCODE -ne 0) { Fail "signature does not verify on $file" }
  Write-Host "[OK] signed $(Split-Path -Leaf $file)"
}

# --- Stage ----------------------------------------------------------------------------------------
if (Test-Path $dist) { Remove-Item -Recurse -Force $dist }
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
$sym = Join-Path $dist "symbols"
New-Item -ItemType Directory -Force $dist, $sym | Out-Null
$bin = Join-Path $stage "rws-stream-kit\bin\64bit"
$loc = Join-Path $stage "rws-stream-kit\data\locale"
New-Item -ItemType Directory -Force $bin, $loc | Out-Null
Assert-Tested "DLL" $dll
Copy-Item $dll $bin
Assert-Tested "staged DLL" (Join-Path $bin "rws-stream-kit.dll")
# The PDB is kept for our own crash symbolication and is NOT installed or zipped (15 MB, build-machine paths).
if (Test-Path $pdb) { Copy-Item $pdb $sym } else { Write-Host "[warn] no rws-stream-kit.pdb next to the DLL" }
Copy-Item (Join-Path $locale "*.ini") $loc
# The HTML manual (the dock's "?" opens data\manual\index.html, a section through manual\<anchor>.html).
$manualSrc = Join-Path $root "manual"
if (!(Test-Path (Join-Path $manualSrc "index.html"))) { Fail "Missing manual\index.html" }
if (!(Test-Path (Join-Path $manualSrc "levels.html"))) { Fail "Missing manual\levels.html (the Levels panel's ? opens it)" }
Copy-Item $manualSrc (Join-Path $stage "rws-stream-kit\data\manual") -Recurse

# --- Binary leak gate on the installer payload ------------------------------------------------------
# Binaries get a printable-strings scan: this is what catches a PDB path (a build-machine path) compiled
# into the DLL. Refuse unless leak_scout exits 0.
function Assert-LeakClean($what, $target, $logName) {
  if (-not $leakScout -and $NoSign) { return }  # unsigned test build without leak_scout (warned above); a signed run has already failed
  $testbed = Join-Path $root ".testbed"
  New-Item -ItemType Directory -Force $testbed | Out-Null
  $log = Join-Path $testbed $logName
  Remove-Item $log -ErrorAction SilentlyContinue
  $old = $ErrorActionPreference; $ErrorActionPreference = "Continue"
  & python $leakScout $target --profile source --exemptions (Join-Path $root "tools\scan-exemptions.txt") --log $log
  $code = $LASTEXITCODE; $ErrorActionPreference = $old
  if ($code -ne 0) { Fail "leak_scout found build-machine or private text in the $what ($target) or could not run (exit $code). See $log" }
  if (!(Test-Path $log)) { Fail "leak_scout wrote no log for the $what ($target); its result cannot be trusted. Expected $log" }
  Write-Host "[OK] leak_scout: $what is clean"
}

Assert-LeakClean "installer payload" $stage "leak-scan-payload.txt"

# --- Installer tests: a required gate, BEFORE anything is signed or the release Setup is built ------
# The installer test compiles a TestRoot build from the staged (still unsigned) DLL and runs the whole
# install / upgrade / uninstall / legacy-removal / OBS-running matrix in a scratch folder.
$instResult = Join-Path $root ".testbed\run-installer.txt"
Remove-Item $instResult -ErrorAction SilentlyContinue
Write-Host "Running tools\test-installer.ps1 (release gate) ..."
& powershell -ExecutionPolicy Bypass -File (Join-Path $root "tools\test-installer.ps1") | Out-Null
if (!(Test-Path $instResult)) { Fail "test-installer.ps1 wrote no $instResult" }
$instLast = (Get-Content $instResult | Select-Object -Last 1)
if ($instLast -ne "RESULT: PASS") { Fail "installer tests did not pass ($instLast). See $instResult" }
$instTime = (Get-Item $instResult).LastWriteTime
foreach ($dep in @($dll, (Join-Path $root "installer\stream-kit.iss"), (Join-Path $root "tools\test-fake-legacy.iss"), (Join-Path $root "tools\test-installer.ps1"))) {
  if ($instTime -lt (Get-Item $dep).LastWriteTime) { Fail "installer test result is older than $dep" }
}
Write-Host "[OK] installer tests passed"

# --- Sign the staged DLL (skipped with -NoSign) ------------------------------------------------------
# The installer tests took minutes: check the bytes again, right before they are signed.
Assert-Tested "DLL" $dll
Assert-Tested "staged DLL" (Join-Path $bin "rws-stream-kit.dll")
Sign (Join-Path $bin "rws-stream-kit.dll") "Red Warden Stream Kit"

# --- Zip (manual / portable OBS installs) ---------------------------------------------------------
$zipStage = Join-Path $dist "zipstage"
New-Item -ItemType Directory -Force $zipStage | Out-Null
Copy-Item (Join-Path $stage "rws-stream-kit") $zipStage -Recurse
Copy-Item LICENSE (Join-Path $zipStage "LICENSE.txt")
Copy-Item NOTICE.txt $zipStage
@"
Red Warden Stream Kit $kitVersion - manual install

1. Close OBS.
2. If an older Kit (1.0.x) or the standalone Multistream is installed, delete its rws-multistream,
   rws-preflight and rws-inflight folders from C:\ProgramData\obs-studio\plugins\ first (they are
   replaced by this one plugin). Do NOT delete OBS's plugin_config folder: it holds your settings.
3. Copy the rws-stream-kit folder into C:\ProgramData\obs-studio\plugins\   (create the plugins
   folder if it does not exist).
4. Start OBS and look for the RW Stream Kit dock (Docks menu).

The installer (.exe) does all of this for you and is the easier route.
"@ | Set-Content (Join-Path $zipStage "INSTALL.txt") -Encoding ascii
$zip = Join-Path $dist "RedWardenStreamKit-$kitVersion$tag.zip"
Compress-Archive -Path (Join-Path $zipStage "*") -DestinationPath $zip
Remove-Item -Recurse -Force $zipStage

# --- Binary leak gate on the zip ------------------------------------------------------------------
# Same printable-strings scan as the payload gate: catches a PDB path compiled into the DLL inside the zip.
Assert-LeakClean "zip" $zip "leak-scan-zip.txt"

# --- Installer ------------------------------------------------------------------------------------
$isccArgs = @("/DAppVersion=$kitVersion", "/DStage=$stage", "/DMsLogo=$msLogo", "/Q")
# Every quote inside the sign command is Inno's $q token, never a literal ": Windows PowerShell 5.1
# mangles embedded quotes in native arguments, and the signtool path has spaces. ($f arrives quoted.)
if ($NoSign) { $isccArgs += "/DNoSign" }
else { $isccArgs += "/Srws=`$q$signtool`$q sign /sha1 $Thumbprint /fd sha256 /tr http://ts.ssl.com /td sha256 /d `$qRed Warden Stream Kit`$q `$f" }
& $iscc @isccArgs "installer\stream-kit.iss"
if ($LASTEXITCODE -ne 0) { Fail "ISCC failed" }
$setup = Join-Path $dist "RedWardenStreamKit-$kitVersion$tag-Setup.exe"
if (!(Test-Path $setup)) { Fail "ISCC reported success but $setup is missing" }
if (!$NoSign) {
  & $signtool verify /pa /q $setup
  if ($LASTEXITCODE -ne 0) { Fail "setup.exe signature does not verify" }
}
Assert-LeakClean "Setup exe" $setup "leak-scan-setup.txt"

# --- Checksums ------------------------------------------------------------------------------------
$sumsName = if ($NoSign) { "SHA256SUMS-UNSIGNED-TEST.txt" } else { "SHA256SUMS.txt" }
Get-ChildItem $dist -File | ForEach-Object {
  "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
} | Set-Content (Join-Path $dist $sumsName) -Encoding ascii
Write-Host "[OK] Red Warden Stream Kit $kitVersion packaged$(if ($NoSign) {' (UNSIGNED TEST BUILD, never publish) -> dist-unsigned\'} else {' -> dist\'})"
Get-ChildItem $dist | Format-Table Name, Length -AutoSize

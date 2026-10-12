<#
  Red Warden Pre-Flight load test, run against the ONE Red Warden Stream Kit DLL (rws-stream-kit.dll).

  Touches none of your real OBS. It:
    1. uses the Kit testbed's portable OBS (plugin\.testbed\obs of the Kit; first run copies one),
    2. installs exactly ONE plugin DLL, rws-stream-kit.dll (+ pdb + locale); the old rws-multistream /
       rws-preflight / rws-inflight DLLs and data folders are removed from that copy first,
    3. seeds pre-existing settings (shared update preference OFF, Pre-Flight and In-Flight settings with
       recognisable values) and launches the portable OBS HIDDEN with a throwaway profile "PreflightTest",
    4. waits, reads the newest log, checks the "[rws-preflight] loaded version" line and the dock id,
    5. closes OBS with WM_CLOSE (the File > Exit path), then checks the Kit load log (one module, each of the
       three tools once, version 1.1.0, three dock ids in the saved dock state, no "could not register the dock"),
       the update notice, and that the seeded settings files survived.
  Stops ONLY the OBS process it started (captured PID). Output: console (run-all captures it), "RESULT: PASS|FAIL".
#>
param(
  [string]$Config = "RelWithDebInfo",
  [int]$Seconds = 20
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
$kitTools = Join-Path (Split-Path -Parent $root) "plugin\tools"                                          # published layout: <repo>\plugin
if (!(Test-Path $kitTools)) { $kitTools = Join-Path (Split-Path -Parent $root) "red-warden-stream-kit\plugin\tools" }   # development layout
. (Join-Path $kitTools "kit-testbed.ps1")
$kit  = Get-KitTestbed $Config
$bed  = $kit.Bed
$obs  = $kit.Obs
$cfg  = $kit.Cfg
$prof = Join-Path $cfg "basic\profiles\PreflightTest"

function Finish([bool]$ok, [string[]]$lines) {
  $lines | ForEach-Object { Write-Host $_ }
  if ($ok) { Write-Host "RESULT: PASS" } else { Write-Host "RESULT: FAIL" }
  exit $(if ($ok) { 0 } else { 1 })
}
trap { Write-Host "FAIL: $_"; Write-Host "RESULT: FAIL"; exit 1 }

if (!(Test-Path $kit.Dll)) { Finish $false @("rws-stream-kit.dll not found: $($kit.Dll)") }

# 1. portable OBS (first run only); the previous harness's OBS may still be shutting down: wait, never kill
Initialize-KitPortableObs $obs
if (!(Wait-KitObsExit (Join-Path $obs "bin\64bit\obs64.exe") 30)) { Finish $false @("a testbed obs64.exe ($obs) is still running; not touching it") }

# 2. exactly ONE plugin DLL
Install-KitPlugin $kit | ForEach-Object { Write-Host $_ }

# fresh throwaway config every run
Clear-KitConfig $cfg
New-Item -ItemType Directory -Force $prof | Out-Null
New-Item -ItemType Directory -Force (Join-Path $cfg "basic\scenes") | Out-Null

$ini = @"
[General]
FirstRun=false
LastVersion=999999999
EnableAutoUpdates=false
[Basic]
Profile=PreflightTest
ProfileDir=PreflightTest
SceneCollection=PreflightTest
SceneCollectionFile=PreflightTest
[BasicWindow]
ConfirmOnExit=false
WarnBeforeStartingStream=false
WarnBeforeStoppingStream=false
"@
Set-Content (Join-Path $cfg "global.ini") $ini -Encoding UTF8
Set-Content (Join-Path $cfg "user.ini") $ini -Encoding UTF8

# obs-websocket off in the test copy (never clash with a real OBS on the usual port)
$wsDir = Join-Path $cfg "plugin_config\obs-websocket"
New-Item -ItemType Directory -Force $wsDir | Out-Null
Set-Content (Join-Path $wsDir "config.json") '{"alerts_enabled":false,"auth_required":false,"first_load":false,"server_enabled":false,"server_password":"","server_port":4466}' -Encoding UTF8

Set-Content (Join-Path $prof "basic.ini") @"
[General]
Name=PreflightTest
[Output]
Mode=Advanced
[AdvOut]
Encoder=obs_x264
TrackIndex=1
Track1Bitrate=256
[Video]
BaseCX=1280
BaseCY=720
OutputCX=1280
OutputCY=720
FPSType=0
FPSCommon=30
"@ -Encoding UTF8

Set-Content (Join-Path $cfg "basic\scenes\PreflightTest.json") @'
{"name":"PreflightTest","current_scene":"Scene","current_program_scene":"Scene",
 "scene_order":[{"name":"Scene"}],
 "sources":[
  {"id":"color_source_v3","versioned_id":"color_source_v3","name":"Ember","uuid":"b0000000-0000-0000-0000-000000000002","settings":{"color":4282038770,"width":1280,"height":720}},
  {"id":"scene","versioned_id":"scene","name":"Scene","uuid":"b0000000-0000-0000-0000-000000000001","settings":{"id_counter":1,"items":[{"name":"Ember","source_uuid":"b0000000-0000-0000-0000-000000000002","id":1,"visible":true}]}}
 ]}
'@ -Encoding UTF8

# Multistream profile config with two ENABLED destinations at an unreachable local URL (never started): Pre-Flight's probe must
# report their count directly from Multistream's dock, in-process ("multistream probe: ok (2 enabled)"), i.e. real data came back, not just a response.
Set-Content (Join-Path $prof "rws-multistream.json") @"
{"version":2,"destinations":[
 {"id":"00000000-beac-0000-0000-0000000000a1","name":"Load Alpha","platform":"custom","server":"rtmp://127.0.0.1:1/x","enabled":true,"encoder":{"shared":true,"encoder_id":"","video_bitrate":2500,"audio_bitrate":0,"audio_track":-1,"width":0,"height":0},"hotkey":[]},
 {"id":"00000000-beac-0000-0000-0000000000a2","name":"Load Bravo","platform":"custom","server":"rtmp://127.0.0.1:1/x","enabled":true,"encoder":{"shared":true,"encoder_id":"","video_bitrate":2500,"audio_bitrate":0,"audio_track":-1,"width":0,"height":0},"hotkey":[]}
]}
"@ -Encoding UTF8
# Kit: pre-existing settings of an upgrading user (shared update preference OFF + Pre-Flight + In-Flight files)
Write-KitSeedSettings $cfg

# 3. run OBS hidden (test env var keeps the update check off the network)
$env:RWS_PREFLIGHT_TEST = "1"
$proc = Start-KitObs $obs "PreflightTest" "PreflightTest"
$obsPid = $proc.Id
$text = ""
$log = $null
$alive = $false
$failure = $null
$visible = @()
try {
  Start-Sleep -Seconds $Seconds
  $log = Get-KitLogFile $cfg
  $text = Get-KitLogText $cfg
  $alive = -not $proc.HasExited
} catch {
  $failure = "$_"
} finally {
  # 5. close the normal way; only the captured PID is ever stopped
  $closed = $false
  try { $closed = Close-KitObs $proc 15 } catch { }
  Remove-Item Env:\RWS_PREFLIGHT_TEST -ErrorAction SilentlyContinue
}
[void](Wait-KitObsExit (Join-Path $obs "bin\64bit\obs64.exe") 20)
$final = Get-KitLogText $cfg

$lines = @("pid=$obsPid still_running_at_check=$alive", "log=$(if ($log) { $log.FullName } else { 'NONE' })")
if ($failure) { $lines += "error during check: $failure" }
$loaded = $text -match '\[rws-preflight\] loaded version'
$dock = $text -match '\[rws-stream-kit\] dock registered \(id rws-stream-kit\)'
$lines += "loaded_line=$loaded dock_id=$dock"

# Kit assertions (new with the merge)
$visible = @(Stop-KitWindowWatch)   # background sampler (every 250 ms from OBS start through exit)
$kc = @()
$kc += Test-KitLoadLog $final $cfg @("multistream", "preflight", "inflight")
$kc += Test-KitUpdateNotice $final
# Pre-Flight's probe asks Multistream's dock directly (in-process, no obs-websocket); Multistream is now in the same DLL, so it is found
# (pre-merge, with no Multistream installed, the log said "multistream probe: not found" for the whole run)
$kc += New-KitCheck ($final -match '\[rws-preflight\] multistream probe: ok \(2 enabled\)') "Pre-Flight's Multistream probe returns the seeded destination data: 'multistream probe: ok (2 enabled)'"
$kc += Test-KitSeedLoaded $final @("preflight", "inflight")
$kc += Test-KitUpdateSource
$kc += Test-KitSeedSettings $cfg
$kc += Test-KitHidden $visible
$kc += Test-KitClosePath
$kc += New-KitCheck ($proc.ExitCode -eq 0 -and $closed) "OBS exited cleanly on WM_CLOSE (exit code $($proc.ExitCode))"
$lines += "--- Kit checks (one DLL, three tools, update notice, seeded settings) ---"
$lines += Get-KitCheckLines $kc
$lines += "--- plugin lines ---"
$lines += @($final -split "`r?`n" | Where-Object { $_ -match 'rws-preflight|rws-inflight|rws-multistream|Stream Kit|Red Warden' })
Finish ($loaded -and $dock -and $alive -and -not $failure -and (Test-KitChecksOk $kc)) $lines

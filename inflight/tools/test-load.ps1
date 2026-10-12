<#
  Red Warden In-Flight load test.

  Touches none of your real OBS. It:
    1. makes a portable OBS copy in .testbed\obs (first run only) by copying the
       Multistream testbed's portable OBS,
    2. installs the freshly built rws-inflight.dll (+ .pdb) and locale into it,
    3. launches that portable OBS with a throwaway profile/collection "InflightTest",
    4. waits, reads the newest log, checks for the "[rws-inflight] loaded" line and the dock id,
    5. stops ONLY the OBS process it started (captured PID).
  Result: .testbed\run-load.txt (PASS / FAIL), exit code 0 / 1.
#>
param(
  [string]$Config = "RelWithDebInfo",
  [int]$Seconds = 20,
  # -NoWebsocket: obs-websocket.dll is renamed away in the portable copy (restored afterwards, even on failure).
  # CHANGED (direct bridge): In-Flight no longer asks Multistream through obs-websocket, it asks Multistream's dock
  # in-process, so with no obs-websocket Multistream is STILL found (ms=ok), exactly as with it. What this run proves is
  # that Multistream says "remote control disabled" (Test-KitLoadLog -NoVendor) and In-Flight does not care.
  [switch]$NoWebsocket
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "testbed-common.ps1")
$kitTools = Join-Path (Split-Path -Parent $root) "plugin\tools"                                          # published layout: <repo>\plugin
if (!(Test-Path $kitTools)) { $kitTools = Join-Path (Split-Path -Parent $root) "red-warden-stream-kit\plugin\tools" }   # development layout
. (Join-Path $kitTools "kit-testbed.ps1")
$kit  = Get-KitTestbed $Config
$bed  = $kit.Bed
$obs  = $kit.Obs
$cfg  = $kit.Cfg
$prof = Join-Path $cfg "basic\profiles\InflightTest"
$dll  = $kit.Dll
$pdb  = $kit.Pdb
New-Item -ItemType Directory -Force $bed | Out-Null

function Finish([bool]$ok, [string[]]$lines) {
  $lines | ForEach-Object { Write-Host $_ }
  if ($ok) { Write-Host "RESULT: PASS" } else { Write-Host "RESULT: FAIL" }
  exit $(if ($ok) { 0 } else { 1 })
}
trap { Restore-KitObsWebsocket $kit.Obs; Write-Host "FAIL: $_"; Write-Host "RESULT: FAIL"; exit 1 }

if (!(Test-Path $dll)) { Finish $false @("rws-stream-kit.dll not found: $dll") }

# 1+2. portable OBS, and exactly ONE plugin DLL (Kit)
Initialize-KitPortableObs $obs
# The previous harness's OBS may still be shutting down: wait for it, never kill it.
if (!(Wait-KitObsExit (Join-Path $obs "bin\64bit\obs64.exe") 30)) { Finish $false @("a testbed obs64.exe ($obs) is still running; not touching it") }
# exactly ONE plugin DLL: rws-stream-kit.dll (+pdb) and its locale; the old standalone DLLs and data folders are removed first
Install-KitPlugin $kit | ForEach-Object { Write-Host $_ }
if ($NoWebsocket) { Disable-KitObsWebsocket $obs; Write-Host "obs-websocket.dll renamed away for this run (restored afterwards)" }

# fresh throwaway config every run (retries while CEF helpers still hold handles)
Clear-KitConfig $cfg
New-Item -ItemType Directory -Force $prof | Out-Null
New-Item -ItemType Directory -Force (Join-Path $cfg "basic\scenes") | Out-Null

$ini = @"
[General]
FirstRun=false
LastVersion=999999999
EnableAutoUpdates=false
[Basic]
Profile=InflightTest
ProfileDir=InflightTest
SceneCollection=InflightTest
SceneCollectionFile=InflightTest
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
Name=InflightTest
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

# Kit: pre-existing shared update preference (OFF) and Pre-Flight's settings file (what "Copy my choices from
# Pre-Flight" reads). In-Flight has no settings of its own, so it copies Pre-Flight's choices by itself (no click).
Write-KitSeedSettings $cfg @("kit", "preflight")
Set-Content (Join-Path $cfg "basic\scenes\InflightTest.json") @'
{"name":"InflightTest","current_scene":"Scene","current_program_scene":"Scene",
 "scene_order":[{"name":"Scene"}],
 "sources":[
  {"id":"color_source_v3","versioned_id":"color_source_v3","name":"Ember","uuid":"b0000000-0000-0000-0000-000000000002","settings":{"color":4282038770,"width":1280,"height":720}},
  {"id":"scene","versioned_id":"scene","name":"Scene","uuid":"b0000000-0000-0000-0000-000000000001","settings":{"id_counter":1,"items":[{"name":"Ember","source_uuid":"b0000000-0000-0000-0000-000000000002","id":1,"visible":true}]}}
 ]}
'@ -Encoding UTF8

# 3. run OBS (test env var keeps the update check off the network)
$env:RWS_INFLIGHT_TEST = "1"
$env:RWS_PREFLIGHT_TEST = "1"   # Pre-Flight logs its light states only under its test env var: needed to see that the seeded settings were loaded
$proc = Start-KitObs $obs "InflightTest" "InflightTest"
$visible = @()
$obsPid = $proc.Id
$text = ""
$log = $null
$alive = $false
$failure = $null
try {
  Start-Sleep -Seconds $Seconds

  # 4. read the newest log (OBS holds it open, so open with sharing)
  $logDir = Join-Path $cfg "logs"
  $log = Get-ChildItem $logDir -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if ($log) {
    $fs = [IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite')
    try {
      $sr = New-Object IO.StreamReader($fs)
      $text = $sr.ReadToEnd()
    } finally {
      $fs.Close()
    }
  }
  $alive = -not $proc.HasExited
} catch {
  $failure = "$_"
} finally {
  # 5. stop ONLY the captured PID, even if reading the log threw (never by image name)
  try {
    $p = Get-Process -Id $obsPid -ErrorAction SilentlyContinue
    if ($p -and -not $p.HasExited) {
      [void](Send-KitWmClose $p)
      if (!$p.WaitForExit(15000)) { Stop-Process -Id $obsPid -Force -ErrorAction SilentlyContinue }
    }
  } catch { }
  Remove-Item Env:\RWS_INFLIGHT_TEST -ErrorAction SilentlyContinue
  Remove-Item Env:\RWS_PREFLIGHT_TEST -ErrorAction SilentlyContinue
}

# Kit assertions (new with the merge): read after OBS has exited, so the saved dock state is final
[void](Wait-KitObsExit (Join-Path $obs "bin\64bit\obs64.exe") 20)
Restore-KitObsWebsocket $obs
$final = Get-KitLogText $cfg
$visible = @(Stop-KitWindowWatch)   # background sampler (every 250 ms from OBS start through exit)
$kc = @()
$kc += Test-KitLoadLog $final $cfg @("multistream", "preflight", "inflight") -NoVendor:$NoWebsocket
$kc += Test-KitUpdateNotice $final
# In-Flight asks Multistream's dock directly (in-process, UI thread; no obs-websocket, no worker thread), so it finds Multistream
# with or without obs-websocket (pre-merge, with no Multistream installed: "multistream live probe: not found ... ms=notfound")
$kc += New-KitCheck ($final -match '\[rws-inflight\] multistream live probe: ok \(any live: no\)') "In-Flight's Multistream live probe returned data$(if ($NoWebsocket) { ' even with no obs-websocket' }): 'multistream live probe: ok (any live: no)'"
$kc += New-KitCheck ($final -match '\[rws-inflight\] probe: live=\w+ main=\d+ ms=ok ') "In-Flight's status line reports ms=ok$(if ($NoWebsocket) { ' (not ms=notfound) with no obs-websocket' })"
$kc += Test-KitSeedLoaded $final @("preflight")
$kc += Test-KitSeedSettings $cfg @("kit", "preflight")
# Auto-copy: In-Flight was not set up and Pre-Flight's file has a mic, so In-Flight took the choices by itself, once, and saved them.
$ac = ([regex]::Matches($final, 'inflight: auto-copied choices from Pre-Flight \(setup complete\)')).Count
$kc += New-KitCheck ($ac -eq 1) "In-Flight auto-copied Pre-Flight's choices once, setup complete (log 'inflight: auto-copied choices from Pre-Flight (setup complete)' x$ac)"
$ifSaved = Read-KitSeedJson $cfg "inflight"
$kc += New-KitCheck ($ifSaved -ne $null -and $ifSaved.setup_done -eq $true -and $ifSaved.mic_uuid -eq "kit-seed-mic") "In-Flight's saved settings.json after the auto-copy: setup_done=$(if ($ifSaved) { $ifSaved.setup_done } else { 'FILE MISSING/INVALID' }), mic_uuid=$(if ($ifSaved) { $ifSaved.mic_uuid } else { '-' }) (want true / Pre-Flight's kit-seed-mic)"
# "Copy my choices from Pre-Flight": In-Flight reads <its config folder>\..\rws-preflight\settings.json. There is no log
# line or test hook for the button itself (it needs a click), so this asserts the file at that exact path is
# still there, unchanged, and that In-Flight's folder resolves next to it; the import parsing is unit-tested
# (rws-inflight-settings-test).
$ifDir = Join-Path $cfg "plugin_config\rws-inflight"
$pfViaIf = [IO.Path]::GetFullPath((Join-Path $ifDir "..\rws-preflight\settings.json"))
$pfSeedTxt = if (Test-Path $pfViaIf) { [IO.File]::ReadAllText($pfViaIf) } else { "" }
$kc += New-KitCheck ($pfSeedTxt -eq $script:KitSeed.preflight) "Pre-Flight's settings.json is at <In-Flight folder>\..\rws-preflight\settings.json after the run, unchanged (the file Copy my choices reads)"
$kc += Test-KitHidden $visible
$kc += Test-KitClosePath
$kc += New-KitCheck ($proc.HasExited -and $proc.ExitCode -eq 0) "OBS exited cleanly on WM_CLOSE (exit code $(if ($proc.HasExited) { $proc.ExitCode } else { 'still running' }))"
$lines = @("pid=$obsPid still_running_at_check=$alive", "log=$(if ($log) { $log.FullName } else { 'NONE' })")
if ($failure) { $lines += "error during check: $failure" }
$loaded = $text -match '\[rws-inflight\] loaded'
$dock = $text -match '\[rws-stream-kit\] dock registered \(id rws-stream-kit\)'
$lines += "loaded_line=$loaded dock_id=$dock"
# Probe checks (the test env var RWS_INFLIGHT_TEST is set above)
$probeLine = $text -match 'probe: live='
$msDirect = $text -match 'multistream-live direct-bridge'
$lines += "probe_line=$probeLine"
$lines += "multistream_live_direct_bridge=$msDirect"
$lines += "--- Kit checks (one DLL, three tools, update notice, seeded settings, hidden) ---"
$lines += Get-KitCheckLines $kc
$lines += "--- plugin lines ---"
$lines += @($text -split "`r?`n" | Where-Object { $_ -match 'rws-inflight|Red Warden In' })
Finish ($loaded -and $dock -and $alive -and -not $failure -and $probeLine -and $msDirect -and (Test-KitChecksOk $kc)) $lines
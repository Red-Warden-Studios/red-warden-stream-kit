<#
  Red Warden Pre-Flight state test (OBS probe + controller).

  Touches none of your real OBS. Same portable copy as test-load.ps1 (.testbed\obs). It:
    1. installs the freshly built rws-preflight.dll into the portable OBS (Multistream removed),
    2. writes a throwaway profile + scene collection "PreflightState":
       scenes "Start" and "Other", one muted audio input "Test Mic" (wasapi_input_capture,
       device does_not_exist), current scene "Start", all with fixed UUIDs,
    3. writes the plugin's settings.json (setup done, main stream + Multistream, mic = Test Mic,
       start scene = Start, no recording),
    4. launches that OBS with --portable (env RWS_PREFLIGHT_TEST=1), waits ~50 s so the 30 s frame
       window can fill, reads the newest log, and asserts the "[rws-preflight] state ..." lines,
    5. stops ONLY the OBS process it started (captured PID, never by image name).
  Result: .testbed\run-state.txt (PASS / FAIL + matched lines), exit code 0 / 1.
#>
param(
  [string]$Config = "RelWithDebInfo",
  [int]$Seconds = 50,
  # -NoWebsocket: obs-websocket.dll is renamed away in the portable copy (restored afterwards, even on failure).
  # CHANGED (direct bridge): Pre-Flight no longer asks Multistream through obs-websocket, it asks Multistream's dock
  # in-process, so with no obs-websocket Multistream is STILL found (Red, "none turned on"), exactly as with it. What
  # this run proves is that Multistream says "remote control disabled" (Test-KitLoadLog -NoVendor) and Pre-Flight does not care.
  [switch]$NoWebsocket
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
$prof = Join-Path $cfg "basic\profiles\PreflightState"
$dll  = $kit.Dll
$pdb  = $kit.Pdb
New-Item -ItemType Directory -Force $bed | Out-Null

# fixed UUIDs
$uStart = "c0000000-0000-0000-0000-000000000001"
$uOther = "c0000000-0000-0000-0000-000000000002"
$uMic   = "c0000000-0000-0000-0000-000000000003"
$uColor = "c0000000-0000-0000-0000-000000000004"

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
Profile=PreflightState
ProfileDir=PreflightState
SceneCollection=PreflightState
SceneCollectionFile=PreflightState
[BasicWindow]
ConfirmOnExit=false
WarnBeforeStartingStream=false
WarnBeforeStoppingStream=false
"@
Set-Content (Join-Path $cfg "global.ini") $ini -Encoding UTF8
Set-Content (Join-Path $cfg "user.ini") $ini -Encoding UTF8

# obs-websocket server off in the test copy (never clash with a real OBS on the usual port).
# (Pre-Flight asks Multistream directly, in-process; it does not use obs-websocket at all.)
$wsDir = Join-Path $cfg "plugin_config\obs-websocket"
New-Item -ItemType Directory -Force $wsDir | Out-Null
Set-Content (Join-Path $wsDir "config.json") '{"alerts_enabled":false,"auth_required":false,"first_load":false,"server_enabled":false,"server_password":"","server_port":4466}' -Encoding UTF8

Set-Content (Join-Path $prof "basic.ini") @"
[General]
Name=PreflightState
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

# Why the old fixture's "Test Mic" never matched: OBS only keeps a source alive while something holds a
# reference to it (a scene item or a global audio slot). After loading the collection OBS releases its
# own refs, so a source that sits in "sources" but in no scene is destroyed at once - it was not even
# in the collection OBS saved back - and obs_get_source_by_uuid() finds nothing (MicMuted = "missing",
# the settings dialog says "(choose again)"). So the mic is now a scene item of "Start", and it is an
# ffmpeg_source over a silent WAV (always loads, has the audio flag, needs no hardware), set muted.
. (Join-Path $PSScriptRoot "testbed-common.ps1")
$wav = Join-Path $bed "silent.wav"
New-SilentWav $wav 1.0
$wavJson = $wav.Replace('\', '/')
$scenes = @"
{"name":"PreflightState","current_scene":"Start","current_program_scene":"Start",
 "scene_order":[{"name":"Start"},{"name":"Other"}],
 "sources":[
  {"id":"color_source_v3","versioned_id":"color_source_v3","name":"Ember","uuid":"$uColor","settings":{"color":4282038770,"width":1280,"height":720}},
  {"id":"ffmpeg_source","versioned_id":"ffmpeg_source","name":"Test Mic","uuid":"$uMic","muted":true,"volume":1.0,"mixers":255,"enabled":true,"settings":{"is_local_file":true,"local_file":"$wavJson","looping":true,"close_when_inactive":false}},
  {"id":"scene","versioned_id":"scene","name":"Start","uuid":"$uStart","settings":{"id_counter":2,"items":[{"name":"Ember","source_uuid":"$uColor","id":1,"visible":true},{"name":"Test Mic","source_uuid":"$uMic","id":2,"visible":true}]}},
  {"id":"scene","versioned_id":"scene","name":"Other","uuid":"$uOther","settings":{"id_counter":0,"items":[]}}
 ]}
"@
Set-Content (Join-Path $cfg "basic\scenes\PreflightState.json") $scenes -Encoding UTF8

# 3. the plugin's own settings (module config folder of the portable OBS)
$plugCfg = Join-Path $cfg "plugin_config\rws-preflight"
New-Item -ItemType Directory -Force $plugCfg | Out-Null
$settings = '{"setup_done":true,"use_main_stream":true,"use_multistream":true,"mic_uuid":"' + $uMic + '","start_scene_uuid":"' + $uStart + '","record_intent":false,"check_updates":false}'
Set-Content (Join-Path $plugCfg "settings.json") $settings -Encoding UTF8
# Kit: pre-existing shared update preference (OFF) and an In-Flight settings file next to Pre-Flight's
Write-KitSeedSettings $cfg @("kit", "inflight")

# 4. run OBS
$env:RWS_PREFLIGHT_TEST = "1"
$proc = Start-KitObs $obs "PreflightState" "PreflightState"
$visible = @()
$obsPid = $proc.Id
$text = ""
$log = $null
$alive = $false
$failure = $null
try {
  Start-Sleep -Seconds $Seconds
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
  # 5. stop ONLY the captured PID (never by image name)
  try {
    $p = Get-Process -Id $obsPid -ErrorAction SilentlyContinue
    if ($p -and -not $p.HasExited) {
      [void](Send-KitWmClose $p)
      if (!$p.WaitForExit(15000)) { Stop-Process -Id $obsPid -Force -ErrorAction SilentlyContinue }
    }
  } catch { }
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
$kc += Test-KitSeedLoaded $final @("inflight")
$kc += New-KitCheck ($final -match '\[rws-preflight\] state Setup=Hidden') "seeded Pre-Flight settings were loaded: 'state Setup=Hidden' (setup_done=true; default without settings is Amber)"
$kc += Test-KitSeedSettings $cfg @("kit", "inflight")
$kc += Test-KitHidden $visible
$kc += Test-KitClosePath
$kc += New-KitCheck ($proc.HasExited -and $proc.ExitCode -eq 0) "OBS exited cleanly on WM_CLOSE (exit code $(if ($proc.HasExited) { $proc.ExitCode } else { 'still running' }))"
$pfs = Read-KitSeedJson $cfg "preflight"
$kc += New-KitCheck ($pfs -ne $null -and $pfs.setup_done -eq $true -and $pfs.mic_uuid -eq $uMic -and $pfs.start_scene_uuid -eq $uStart) "Pre-Flight settings.json intact after exit (setup_done, mic and start scene still the seeded ones)"
$all = @($text -split "`r?`n")
$plug = @($all | Where-Object { $_ -match '\[rws-preflight\]' })
$stateLines = @($plug | Where-Object { $_ -match '\] state ' })

# the last logged state of one check (states only log on change)
function Last-State([string]$name) {
  $m = @($stateLines | Where-Object { $_ -match ("state " + $name + "=") })
  if ($m.Count -eq 0) { return $null }
  if ($m[-1] -match ("state " + $name + "=(\w+)")) { return $Matches[1] }
  return $null
}
$mic   = Last-State "MicMuted"
$scene = Last-State "Scene"
$multi = Last-State "Multistream"
$perf  = Last-State "Performance"
$threadOk = ($plug | Where-Object { $_ -match 'multistream-probe direct-bridge' }).Count -gt 0
$leaks = @($plug | Where-Object { $_ -match '(?i)key' -or $_ -match 'rtmp://' })

# The mic must really be matched: Red because it is MUTED, not Red because it is "missing".
$msgLines = @($plug | Where-Object { $_ -match 'message MicMuted: ' })
$micMsg = if ($msgLines.Count -gt 0) { $msgLines[-1] } else { "" }
$micMsgOk = ($micMsg -match 'is muted') -and ($micMsg -notmatch 'missing')
$savedOk = $false
try { $savedOk = ((Get-Content (Join-Path $cfg "basic\scenes\PreflightState.json") -Raw) -match [regex]::Escape($uMic)) } catch { }

$checks = @()
$checks += "MicMuted message contains 'muted' (not 'missing')=$micMsgOk : $micMsg"
$checks += "Test Mic uuid present in the collection OBS saved back=$savedOk (want True)"
$checks += "MicMuted=$mic (want Red)"
$checks += "Scene=$scene (want Green)"
# CHANGED with the merge (was: Multistream=Amber, "wasn't found"): Multistream is now part of the same DLL, so Pre-Flight
# finds it. It has no destinations in this fixture, so the light is Red with the "none turned on" message.
$msgMs = @($plug | Where-Object { $_ -match 'message Multistream: ' })
$msMsg = if ($msgMs.Count -gt 0) { $msgMs[-1] } else { "" }
# CHANGED again (direct bridge): with obs-websocket removed Multistream is still found through the in-process bridge, so
# -NoWebsocket expects the same Red / "none turned on" as the normal run (it used to expect Amber "wasn't found").
$multiWant = "Red"
$msMsgOk = $msMsg -match 'No Multistream destinations are turned on'
$checks += "Multistream=$multi (want ${multiWant}: found through the in-process bridge$(if ($NoWebsocket) { ', even with no obs-websocket' }), no destination turned on)"
$checks += "Multistream message (no destinations are turned on)=$msMsgOk : $msMsg"
$checks += "Performance=$perf (want Green or Amber)"
$checks += "direct-bridge line (no worker thread)=$threadOk"
$checks += "key/rtmp lines from rws-preflight=$($leaks.Count) (want 0)"

$ok = ($mic -eq "Red") -and $micMsgOk -and $savedOk -and ($scene -eq "Green") -and ($multi -eq $multiWant) -and $msMsgOk -and
      (($perf -eq "Green") -or ($perf -eq "Amber")) -and $threadOk -and ($leaks.Count -eq 0) -and
      $alive -and -not $failure -and (Test-KitChecksOk $kc)

$lines = @("pid=$obsPid still_running_at_check=$alive", "log=$(if ($log) { $log.FullName } else { 'NONE' })")
if ($failure) { $lines += "error during check: $failure" }
$lines += $checks
$lines += "--- Kit checks (one DLL, three tools, update notice, seeded settings) ---"
$lines += Get-KitCheckLines $kc
$lines += "--- rws-preflight log lines ---"
$lines += $plug
Finish $ok $lines

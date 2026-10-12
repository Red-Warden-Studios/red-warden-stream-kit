<#
  Red Warden Pre-Flight integration test: Pre-Flight talking to the REAL Red Warden Multistream plugin.

  Touches none of your real OBS and never streams. It:
    1. uses the same portable OBS copy as the other tests (.testbed\obs),
    2. installs the one Kit DLL (rws-stream-kit.dll) with its locale,
    3. writes a throwaway profile + scene collection "PreflightInteg", obs-websocket server DISABLED
       (the in-process vendor API still works), Multistream's own config with two enabled
       destinations that point at an unreachable local URL (rtmp://127.0.0.1:1/x; nothing is ever
       started, no key is stored), Pre-Flight settings with Multistream + "I record" on, and a
       recording folder = .testbed\rec (Simple output, existing folder),
    4. launches OBS with --portable (RWS_PREFLIGHT_TEST=1, RWS_PREFLIGHT_TEST_HAMMER=1 so Pre-Flight asks
       Multistream on every tick, right up to exit), waits until Multistream is Green and
       Recording is Green/Amber,
    5. closes OBS with CloseMainWindow and requires the process to exit within 10 s (else Stop-Process
       on the captured PID and FAIL), then asserts the log: both destination names listed, no
       "detached at exit", no crash. (The probe is a direct in-process call now: there is no worker to stop, so the
       old "multistream probe stopped in N ms" check is gone.)
  Stops ONLY the OBS process it started (captured PID). Result: .testbed\run-integration.txt.
#>
param(
  [string]$Config = "RelWithDebInfo",
  [int]$WaitStatesSeconds = 75
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
$profName = "PreflightInteg"
$prof = Join-Path $cfg "basic\profiles\$profName"
$dll  = $kit.Dll
$pdb  = $kit.Pdb
$recDir = Join-Path $bed "rec"
New-Item -ItemType Directory -Force $bed | Out-Null

$uStart = "c0000000-0000-0000-0000-000000000001"
$uOther = "c0000000-0000-0000-0000-000000000002"
$uMic   = "c0000000-0000-0000-0000-000000000003"
$uColor = "c0000000-0000-0000-0000-000000000004"

$pbin  = Join-Path $obs "obs-plugins\64bit"
$msData = Join-Path $obs "data\obs-plugins\rws-multistream"

function Finish([bool]$ok, [string[]]$lines) {
  $lines | ForEach-Object { Write-Host $_ }
  if ($ok) { Write-Host "RESULT: PASS" } else { Write-Host "RESULT: FAIL" }
  exit $(if ($ok) { 0 } else { 1 })
}
trap { Write-Host "FAIL: $_"; Write-Host "RESULT: FAIL"; exit 1 }

if (!(Test-Path $dll)) { Finish $false @("rws-stream-kit.dll not found: $dll") }

# 1+2. portable OBS, and exactly ONE plugin DLL (Kit)
Initialize-KitPortableObs $obs
# The previous harness's OBS may still be shutting down: wait for it, never kill it.
if (!(Wait-KitObsExit (Join-Path $obs "bin\64bit\obs64.exe") 30)) { Finish $false @("a testbed obs64.exe ($obs) is still running; not touching it") }
# exactly ONE plugin DLL: rws-stream-kit.dll (+pdb) and its locale; the old standalone DLLs and data folders are removed first
Install-KitPlugin $kit | ForEach-Object { Write-Host $_ }

# fresh throwaway config
Clear-KitConfig $cfg
New-Item -ItemType Directory -Force $prof | Out-Null
New-Item -ItemType Directory -Force (Join-Path $cfg "basic\scenes") | Out-Null
New-Item -ItemType Directory -Force $recDir | Out-Null
$recIni = $recDir.Replace('\', '/')

$ini = @"
[General]
FirstRun=false
LastVersion=999999999
EnableAutoUpdates=false
[Basic]
Profile=$profName
ProfileDir=$profName
SceneCollection=$profName
SceneCollectionFile=$profName
[BasicWindow]
ConfirmOnExit=false
WarnBeforeStartingStream=false
WarnBeforeStoppingStream=false
RecordWhenStreaming=true
"@
Set-Content (Join-Path $cfg "global.ini") $ini -Encoding UTF8
Set-Content (Join-Path $cfg "user.ini") $ini -Encoding UTF8

# obs-websocket server DISABLED (Pre-Flight asks Multistream in-process, not through obs-websocket)
$wsDir = Join-Path $cfg "plugin_config\obs-websocket"
New-Item -ItemType Directory -Force $wsDir | Out-Null
Set-Content (Join-Path $wsDir "config.json") '{"alerts_enabled":false,"auth_required":false,"first_load":false,"server_enabled":false,"server_password":"","server_port":4466}' -Encoding UTF8

Set-Content (Join-Path $prof "basic.ini") @"
[General]
Name=$profName
[Output]
Mode=Simple
[SimpleOutput]
FilePath=$recIni
[Video]
BaseCX=1280
BaseCY=720
OutputCX=1280
OutputCY=720
FPSType=0
FPSCommon=30
"@ -Encoding UTF8

# Multistream's own config: two enabled destinations at an unreachable local URL. Never started.
Set-Content (Join-Path $prof "rws-multistream.json") @"
{"version":2,"destinations":[
 {"id":"00000000-beac-0000-0000-0000000000c1","name":"Alpha test","platform":"custom","server":"rtmp://127.0.0.1:1/x","enabled":true,"encoder":{"shared":true,"encoder_id":"","video_bitrate":2500,"audio_bitrate":0,"audio_track":-1,"width":0,"height":0},"hotkey":[]},
 {"id":"00000000-beac-0000-0000-0000000000c2","name":"Bravo test","platform":"custom","server":"rtmp://127.0.0.1:1/x","enabled":true,"encoder":{"shared":true,"encoder_id":"","video_bitrate":2500,"audio_bitrate":0,"audio_track":-1,"width":0,"height":0},"hotkey":[]}
]}
"@ -Encoding UTF8
$msCfg = Join-Path $cfg "plugin_config\rws-multistream"
New-Item -ItemType Directory -Force $msCfg | Out-Null
Set-Content (Join-Path $msCfg "settings.json") '{"upload_mbps":0.0,"upload_source":"","upload_when":"","check_updates":true}' -Encoding UTF8

# Same fixture as test-state.ps1 (the mic must be a scene item, see the note there)
. (Join-Path $PSScriptRoot "testbed-common.ps1")
$wav = Join-Path $bed "silent.wav"
New-SilentWav $wav 1.0
$wavJson = $wav.Replace('\', '/')
$scenes = @"
{"name":"$profName","current_scene":"Start","current_program_scene":"Start",
 "scene_order":[{"name":"Start"},{"name":"Other"}],
 "sources":[
  {"id":"color_source_v3","versioned_id":"color_source_v3","name":"Ember","uuid":"$uColor","settings":{"color":4282038770,"width":1280,"height":720}},
  {"id":"ffmpeg_source","versioned_id":"ffmpeg_source","name":"Test Mic","uuid":"$uMic","muted":true,"volume":1.0,"mixers":255,"enabled":true,"settings":{"is_local_file":true,"local_file":"$wavJson","looping":true,"close_when_inactive":false}},
  {"id":"scene","versioned_id":"scene","name":"Start","uuid":"$uStart","settings":{"id_counter":2,"items":[{"name":"Ember","source_uuid":"$uColor","id":1,"visible":true},{"name":"Test Mic","source_uuid":"$uMic","id":2,"visible":true}]}},
  {"id":"scene","versioned_id":"scene","name":"Other","uuid":"$uOther","settings":{"id_counter":0,"items":[]}}
 ]}
"@
Set-Content (Join-Path $cfg "basic\scenes\$profName.json") $scenes -Encoding UTF8

# Pre-Flight settings: Multistream only, "I record" on (folder comes from the profile: .testbed\rec)
$plugCfg = Join-Path $cfg "plugin_config\rws-preflight"
New-Item -ItemType Directory -Force $plugCfg | Out-Null
$settings = '{"setup_done":true,"use_main_stream":false,"use_multistream":true,"mic_uuid":"' + $uMic + '","start_scene_uuid":"' + $uStart + '","record_intent":true,"check_updates":false}'
Set-Content (Join-Path $plugCfg "settings.json") $settings -Encoding UTF8

# Legacy-only start: no shared Kit settings file exists; Multistream says check_updates=true, Pre-Flight says false.
$kitSharedAbsentBefore = -not (Test-Path (Join-Path $cfg "plugin_config\rws-stream-kit\settings.json"))

function Read-Log([string]$logDir) {
  $l = Get-ChildItem $logDir -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
  if (!$l) { return @("", $null) }
  $fs = [IO.File]::Open($l.FullName, 'Open', 'Read', 'ReadWrite')
  try { $sr = New-Object IO.StreamReader($fs); $t = $sr.ReadToEnd() } finally { $fs.Close() }
  return @($t, $l)
}

# 4. run OBS
$env:RWS_PREFLIGHT_TEST = "1"
$env:RWS_PREFLIGHT_TEST_HAMMER = "1"
$proc = Start-KitObs $obs $profName $profName
$visible = @()
$obsPid = $proc.Id
$logDir = Join-Path $cfg "logs"
$failure = $null
$statesOk = $false
$exitedInTime = $false
$exitSeconds = -1.0
$exitCode = $null
$killed = $false
$text = ""
$log = $null
try {
  $deadline = (Get-Date).AddSeconds($WaitStatesSeconds)
  while ((Get-Date) -lt $deadline) {
    Start-Sleep -Seconds 3
    if ($proc.HasExited) { $failure = "OBS exited on its own before the checks (exit code $($proc.ExitCode))"; break }
    $r = Read-Log $logDir
    $t = [string]$r[0]
    if (($t -match 'state Multistream=Green') -and ($t -match 'state Recording=(Green|Amber)')) { $statesOk = $true; break }
  }
  Start-Sleep -Seconds 2  # let the every-tick probing run a moment longer
} catch {
  $failure = "$_"
} finally {
  # 5. close the normal way and time it. Only the captured PID is ever stopped.
  try {
    # $proc is the object Start-Process returned (it keeps the exit code); it is the captured PID.
    $proc.Refresh()
    if (-not $proc.HasExited) {
      $sw = [Diagnostics.Stopwatch]::StartNew()
      [void](Send-KitWmClose $proc)
      $exitedInTime = $proc.WaitForExit(10000)
      $exitSeconds = [math]::Round($sw.Elapsed.TotalSeconds, 1)
      if (!$exitedInTime) {
        $killed = $true
        Stop-Process -Id $obsPid -Force -ErrorAction SilentlyContinue
      } else {
        $exitCode = $proc.ExitCode
      }
    }
  } catch { if (!$failure) { $failure = "closing OBS: $_" } }
  Remove-Item Env:\RWS_PREFLIGHT_TEST -ErrorAction SilentlyContinue
  Remove-Item Env:\RWS_PREFLIGHT_TEST_HAMMER -ErrorAction SilentlyContinue
}
Start-Sleep -Seconds 1
$r = Read-Log $logDir
$text = [string]$r[0]
$log = $r[1]

# Kit assertions (new with the merge). Multistream is now part of the same DLL: the old harness copied a second
# DLL (rws-multistream.dll) in for this run; now there is exactly one DLL and Multistream's tool must load once.
$final = $text
[void](Wait-KitObsExit (Join-Path $obs "bin\64bit\obs64.exe") 20)
$visible = @(Stop-KitWindowWatch)   # background sampler (every 250 ms from OBS start through exit)
$kc = @()
$kc += Test-KitLoadLog $final $cfg @("multistream", "preflight", "inflight")
$kc += Test-KitUpdateNotice $final
$kc += New-KitCheck ($final -match '\[rws-preflight\] state Setup=Hidden') "seeded Pre-Flight settings were loaded: 'state Setup=Hidden' (setup_done=true; default without settings is Amber)"
$kc += New-KitCheck ($final -match '\[rws-preflight\] message Multistream: Multistream destinations ready: Alpha test, Bravo test\.') "Multistream's seeded profile config was loaded: Pre-Flight's probe got both destination names back through the vendor API"
$kc += New-KitCheck $kitSharedAbsentBefore "legacy-only start: plugin_config\rws-stream-kit\settings.json did NOT exist before launch (so what is there after exit was created by the run)"
$kc += Test-KitHidden $visible
$kc += Test-KitClosePath
$kitSettings = Read-KitSeedJson $cfg "kit"
$kc += New-KitCheck ($kitSettings -ne $null -and $kitSettings.check_updates -eq $false) "migration: with ONLY legacy per-tool values (Multistream settings check_updates=true, Pre-Flight settings check_updates=false) the run created plugin_config\rws-stream-kit\settings.json with check_updates=false (either-false rule; default without any value would be true)"
$msJson = Join-Path $prof "rws-multistream.json"
$msTxt = if (Test-Path $msJson) { Get-Content $msJson -Raw } else { "" }
$kc += New-KitCheck (($msTxt -match 'Alpha test') -and ($msTxt -match 'Bravo test')) "Multistream's profile config rws-multistream.json read back and still holds both seeded destinations after exit"
$pfs = Read-KitSeedJson $cfg "preflight"
$kc += New-KitCheck ($pfs -ne $null -and $pfs.setup_done -eq $true -and $pfs.record_intent -eq $true -and $pfs.mic_uuid -eq $uMic) "Pre-Flight settings.json intact after exit (setup_done, record_intent, mic)"
$all = @($text -split "`r?`n")
$plug = @($all | Where-Object { $_ -match '\[rws-preflight\]' })
function Last-State([string]$name) {
  $m = @($plug | Where-Object { $_ -match ("state " + $name + "=") })
  if ($m.Count -eq 0) { return $null }
  if ($m[-1] -match ("state " + $name + "=(\w+)")) { return $Matches[1] }
  return $null
}
$multi = Last-State "Multistream"
$rec   = Last-State "Recording"
$msgLine = @($plug | Where-Object { $_ -match 'message Multistream: ' })
$msgText = if ($msgLine.Count -gt 0) { $msgLine[-1] } else { "" }
$namesOk = ($msgText -match 'Alpha test') -and ($msgText -match 'Bravo test')
$detached = @($plug | Where-Object { $_ -match 'detached at exit' }).Count
$crashDir = Join-Path $cfg "crashes"
$crashFiles = @(Get-ChildItem $crashDir -ErrorAction SilentlyContinue)
$crashLog = @($all | Where-Object { $_ -match '(?i)unhandled exception|crash(ed)? ' -and $_ -notmatch '(?i)crash handler|crash_handler' })
$msLoaded = @($all | Where-Object { $_ -match 'rws-multistream' }).Count -gt 0
$leaks = @($plug | Where-Object { $_ -match '(?i)key' -or $_ -match 'rtmp://' })

$checks = @()
$checks += "Multistream plugin loaded (log mentions rws-multistream)=$msLoaded"
$checks += "Multistream=$multi (want Green)"
$checks += "Multistream message lists 'Alpha test' and 'Bravo test'=$namesOk : $msgText"
$checks += "Recording=$rec (want Green or Amber, not Unknown)"
$checks += "states reached within ${WaitStatesSeconds}s=$statesOk"
$checks += "OBS exited within 10 s of CloseMainWindow=$exitedInTime (took $exitSeconds s; killed=$killed)"
$checks += "OBS exit code=$exitCode (want 0)"
$checks += "'detached at exit' log lines=$detached (want 0)"
$checks += "crash files=$($crashFiles.Count), crash log lines=$($crashLog.Count) (want 0 / 0)"
$checks += "key/rtmp lines from rws-preflight=$($leaks.Count) (want 0)"

$ok = $msLoaded -and ($multi -eq "Green") -and $namesOk -and (($rec -eq "Green") -or ($rec -eq "Amber")) -and $statesOk -and
      $exitedInTime -and (-not $killed) -and ($exitCode -eq 0) -and ($detached -eq 0) -and
      ($crashFiles.Count -eq 0) -and ($crashLog.Count -eq 0) -and ($leaks.Count -eq 0) -and (-not $failure) -and (Test-KitChecksOk $kc)

$lines = @("pid=$obsPid", "log=$(if ($log) { $log.FullName } else { 'NONE' })")
if ($failure) { $lines += "error during check: $failure" }
$lines += $checks
$lines += "--- Kit checks (one DLL, three tools, update notice, hidden) ---"
$lines += Get-KitCheckLines $kc
$lines += "--- rws-preflight log lines ---"
$lines += $plug
Finish $ok $lines

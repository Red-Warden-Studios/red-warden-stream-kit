<#
  Red Warden In-Flight lights-UI test.

  Touches none of your real OBS. Same testbed and fixture as test-audio.ps1 (portable OBS in .testbed\obs,
  a fresh throwaway config, obs-websocket off, hidden window, only the captured PID is stopped; Tone mic,
  Hush desktop, setup done, force-live), plus RWS_INFLIGHT_TEST_SHOT_DIR=.testbed\shots. About 6 s after
  OBS finishes loading, the plugin grabs two parentless, never-shown LightsPanel widgets offscreen:
  ui-live.png (the controller's latest real snapshot) and ui-demo.png (one light of each state, details
  open, session log open), plus levels-demo.png (the Levels panel with demo numbers: no meters, no timer). No dock is floated or shown. It then grabs the settings dialog (parentless,
  Qt::WA_DontShowOnScreen, one PNG per tab: settings-watch/scenes/lights/advanced.png, plus settings-advanced-end.png (scrolled to the bottom),
  settings-note.png with the "I adjusted a few values" note, and settings-setupcard.png).
  The fixture settings.json holds mic Tone, desktop Hush and one holding scene "Starting Soon".
  With RWS_INFLIGHT_TEST_APPLY=1 the plugin then drives the REAL settings-dialog path offscreen (open(), not
  exec(); WA_DontShowOnScreen): it sets mic amber 30 and turns the Network light off, presses OK, and checks
  settings.json (amber 30, network off, the holding scene's "future_key" kept) and that the controller got
  it ("apply-test ok", "settings saved", "apply-test controller amber=30 network=0"); then changes a value,
  presses Cancel and checks nothing changed ("cancel-test ok").
  PASS needs: every "ui-shot saved" / "settings-shot saved" log line, every PNG larger than 2 KB,
  no "ui-shot failed" / "settings-shot failed", OBS alive.
  Result: .testbed\run-ui.txt (PASS / FAIL + one name=True/False line per check), exit code 0 / 1.
#>
param(
  [string]$Config = "RelWithDebInfo",
  [int]$MaxSeconds = 90
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
$shots  = Join-Path $bed "shots"
New-Item -ItemType Directory -Force $bed | Out-Null

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

# fixture audio
$toneWav = Join-Path $bed "tone-1k-20db.wav"
$hushWav = Join-Path $bed "silence.wav"
New-SineWav $toneWav 2.0
New-SilentWav $hushWav 2.0
$toneJson = $toneWav.Replace('\', '/')
$hushJson = $hushWav.Replace('\', '/')
$toneUuid = "b0000000-0000-0000-0000-000000000003"
$hushUuid = "b0000000-0000-0000-0000-000000000004"
$micUuid  = $toneUuid

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

# The plugin's settings: setup done, mic = Tone (or Hush for the fail-proof), desktop = Hush.
# Written without a BOM (Set-Content -Encoding UTF8 on Windows PowerShell 5 adds one).
$plugCfg = Join-Path $cfg "plugin_config\rws-inflight"
New-Item -ItemType Directory -Force $plugCfg | Out-Null
$sceneUuid = "b0000000-0000-0000-0000-000000000001"
$settingsJson = '{"setup_done":true,"mic_uuid":"' + $micUuid + '","desktop_uuid":"' + $hushUuid + '","check_updates":false,"levels_open":true,"levels_mode":"lufs","levels_true_peak":true,"levels_lufs_ref_on":true,"levels_lufs_ref":-18.5,' +
  '"levels_sources":[{"uuid":"' + $toneUuid + '","future_key":"keep"},{"uuid":"missing-uuid"}],' +
  '"holding_scenes":[{"scene_uuid":"' + $sceneUuid + '","limit_seconds":300,"label":"Starting Soon","future_key":"keep-me"}]}'
[IO.File]::WriteAllText((Join-Path $plugCfg "settings.json"), $settingsJson, (New-Object Text.UTF8Encoding($false)))

Write-KitSeedSettings $cfg @("kit")
$ffTone = '{"is_local_file":true,"local_file":"' + $toneJson + '","looping":true,"restart_on_activate":true}'
$ffHush = '{"is_local_file":true,"local_file":"' + $hushJson + '","looping":true,"restart_on_activate":true}'
$scene = @"
{"name":"InflightTest","current_scene":"Scene","current_program_scene":"Scene",
 "scene_order":[{"name":"Scene"}],
 "sources":[
  {"id":"color_source_v3","versioned_id":"color_source_v3","name":"Ember","uuid":"b0000000-0000-0000-0000-000000000002","settings":{"color":4282038770,"width":1280,"height":720}},
  {"id":"ffmpeg_source","versioned_id":"ffmpeg_source","name":"Tone","uuid":"$toneUuid","mixers":255,"volume":1.0,"muted":false,"enabled":true,"settings":$ffTone},
  {"id":"ffmpeg_source","versioned_id":"ffmpeg_source","name":"Hush","uuid":"$hushUuid","mixers":255,"volume":1.0,"muted":false,"enabled":true,"settings":$ffHush},
  {"id":"scene","versioned_id":"scene","name":"Scene","uuid":"b0000000-0000-0000-0000-000000000001","settings":{"id_counter":3,"items":[
    {"name":"Ember","source_uuid":"b0000000-0000-0000-0000-000000000002","id":1,"visible":true},
    {"name":"Tone","source_uuid":"$toneUuid","id":2,"visible":true},
    {"name":"Hush","source_uuid":"$hushUuid","id":3,"visible":true}]}}
 ]}
"@
[IO.File]::WriteAllText((Join-Path $cfg "basic\scenes\InflightTest.json"), $scene, (New-Object Text.UTF8Encoding($false)))

# 3. run OBS (test env vars: no update check, and the plugin treats the session as live)
$env:RWS_INFLIGHT_TEST = "1"
$env:RWS_INFLIGHT_TEST_FORCE_LIVE = "1"
$env:RWS_INFLIGHT_TEST_SHOT_DIR = $shots
$env:RWS_INFLIGHT_TEST_APPLY = "1"
New-Item -ItemType Directory -Force $shots | Out-Null
Remove-Item (Join-Path $shots "ui-*.png") -ErrorAction SilentlyContinue
Remove-Item (Join-Path $shots "levels-*.png") -ErrorAction SilentlyContinue
Remove-Item (Join-Path $shots "settings-*.png") -ErrorAction SilentlyContinue
$proc = Start-KitObs $obs "InflightTest" "InflightTest"
$visible = @()
$obsPid = $proc.Id
$text = ""
$log = $null
$alive = $false
$failure = $null
try {
  Start-Sleep -Seconds 12

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
  for ($w = 12; $w -lt $MaxSeconds -and (([regex]::Matches($text, 'ui-shot (saved|failed)')).Count -lt 3 -or
      ([regex]::Matches($text, 'settings-shot (saved|failed)')).Count -lt 7 -or $text -notmatch '(apply-test|cancel-test) (ok|FAIL)' -or $text -notmatch 'cancel-test (ok|FAIL)'); $w += 3) {
    Start-Sleep -Seconds 3
    $fs = [IO.File]::Open($log.FullName, 'Open', 'Read', 'ReadWrite')
    try { $text = (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() }
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
  Remove-Item Env:\RWS_INFLIGHT_TEST_FORCE_LIVE -ErrorAction SilentlyContinue
  Remove-Item Env:\RWS_INFLIGHT_TEST_SHOT_DIR -ErrorAction SilentlyContinue
  Remove-Item Env:\RWS_INFLIGHT_TEST_APPLY -ErrorAction SilentlyContinue
}

# Kit assertions (new with the merge): read after OBS has exited, so the saved dock state is final
[void](Wait-KitObsExit (Join-Path $obs "bin\64bit\obs64.exe") 20)
$final = Get-KitLogText $cfg
$visible = @(Stop-KitWindowWatch)   # background sampler (every 250 ms from OBS start through exit)
$kc = @()
$kc += Test-KitLoadLog $final $cfg @("multistream", "preflight", "inflight")
$kc += Test-KitUpdateNotice $final
$kc += Test-KitSeedLoaded $final @("inflight")
$kc += Test-KitSeedSettings $cfg @("kit")
$ifs = Read-KitSeedJson $cfg "inflight"
$kc += New-KitCheck ($ifs -ne $null -and $ifs.setup_done -eq $true -and $ifs.mic_uuid -eq $micUuid -and $ifs.desktop_uuid -eq $hushUuid -and @($ifs.holding_scenes).Count -eq 1 -and $ifs.holding_scenes[0].future_key -eq "keep-me") "In-Flight settings.json intact after exit (setup_done, mic, desktop, holding scene and its unknown key)"
$kc += Test-KitHidden $visible
$kc += Test-KitClosePath
$kc += New-KitCheck ($proc.HasExited -and $proc.ExitCode -eq 0) "OBS exited cleanly on WM_CLOSE (exit code $(if ($proc.HasExited) { $proc.ExitCode } else { 'still running' }))"
$lines = @("pid=$obsPid still_running_at_check=$alive", "log=$(if ($log) { $log.FullName } else { 'NONE' })")
if ($failure) { $lines += "error during check: $failure" }
$okAll = $true
foreach ($n in "ui-live.png", "ui-demo.png", "levels-demo.png", "levels-lufs-demo.png", "settings-watch.png", "settings-scenes.png", "settings-lights.png",
               "settings-advanced.png", "settings-advanced-end.png", "settings-note.png", "settings-setupcard.png") {
  $kind = if ($n -like 'settings-*') { 'settings-shot' } else { 'ui-shot' }
  $saved = $text -match ($kind + ' saved ' + [regex]::Escape($n) + ' \(\d+x\d+\)')
  $f = Join-Path $shots $n
  $size = if (Test-Path $f) { (Get-Item $f).Length } else { 0 }
  $big = $size -gt 2048
  $lines += "saved_$n=$saved"
  $lines += "size_$n=$size big_enough=$big"
  $okAll = $okAll -and $saved -and $big
}
foreach ($pat in 'apply-test ok', 'levels-roundtrip ok', 'cancel-test ok', 'settings saved', 'apply-test controller amber=30 network=0') {
  $hit = $text -match [regex]::Escape($pat)
  $lines += "found_$($pat -replace '[^A-Za-z0-9]+','_')=$hit"
  $okAll = $okAll -and $hit
}
# Levels labels (new): "Now" / "3s avg" with their plain-word tips, the "?" link (logs its URL under the test hook), and the
# Reference value label, present only while Reference is on.
$lv = @(
  @('lufs_labels_now_3s_avg', "levels-state lufs-ref-on: mode=lufs now='Now -18\.4' avg='3s avg -19\.2'"),
  @('lufs_tips_plain_words', "levels-state lufs-ref-on: .* nowTip='Loudness over the last 0\.4 seconds \(EBU momentary\)' avgTip='Average loudness over the last 3 seconds \(EBU short-term\)\. Best number for judging your mix\.' tpTip='True peak: the highest point of the waveform, including between samples\. Keep it below -1 dBTP\.'"),
  @('ref_label_on_when_reference_on', "levels-state lufs-ref-on: .* ref='-16' refWord=line-colour help='\?' helpTip='What these numbers mean'"),
  @('ref_label_none_when_reference_off', "levels-state lufs-ref-off: .* ref='none' refWord=line-colour"),
  @('help_link_logs_web_manual_url', "levels: help would open https://redwardenstudios\.com/division/bastion/stream-kit/manual/#levels")
)
foreach ($x in $lv) {
  $n = ([regex]::Matches($text, $x[1])).Count
  $lines += "found_$($x[0])=$($n -eq 1) (x$n)"
  $okAll = $okAll -and ($n -eq 1)
}
$noApplyFail = -not ($text -match '(apply|cancel)-test FAIL|levels-roundtrip FAIL')
$lines += "no_apply_test_fail=$noApplyFail"
$okAll = $okAll -and $noApplyFail
$noFail = -not ($text -match '(ui|settings)-shot failed')
$lines += "no_ui_shot_failed=$noFail"
$lines += "obs_alive=$alive"
$lines += "--- Kit checks (one DLL, three tools, update notice, seeded settings, hidden) ---"
$lines += Get-KitCheckLines $kc
$lines += "--- plugin lines ---"
$lines += @($text -split "`r?`n" | Where-Object { $_ -match 'rws-inflight|Red Warden In|ui-shot|settings-shot' })
Finish ($okAll -and $noFail -and $alive -and -not $failure -and (Test-KitChecksOk $kc)) $lines
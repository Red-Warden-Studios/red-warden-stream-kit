<#
  Red Warden In-Flight audio-watch test.

  Touches none of your real OBS. Same testbed as test-load.ps1 (portable OBS in .testbed\obs, a fresh
  throwaway config, obs-websocket off, hidden window, only the captured PID is stopped), plus:
    - two ffmpeg_source inputs in the scene: "Tone" (1 kHz sine, -20 dBFS peak) and "Hush" (digital silence),
    - the plugin's settings.json written before launch (setup done, mic = Tone, desktop = Hush),
    - RWS_INFLIGHT_TEST and RWS_INFLIGHT_TEST_FORCE_LIVE set, so the plugin treats the session as live.
  PASS needs: both volmeters attached, the mic's first reading between PeakLo and PeakHi dBFS, the
  desktop's first reading exactly -inf, and no "attach ... failed".
  Result: .testbed\run-audio.txt (PASS / FAIL + one name=True/False line per check), exit code 0 / 1.
  -MicSource Hush / -PeakLo / -PeakHi exist only so the check can be proven to fail.
#>
param(
  [string]$Config = "RelWithDebInfo",
  [int]$Seconds = 40,   # the offscreen gate test (tabbed + floating steps) takes several real seconds; the LUFS switch
                        # happens ~11.5 s after the meters start and logs 5 s later
  [ValidateSet("Tone", "Hush")][string]$MicSource = "Tone",
  [double]$PeakLo = -26.0,
  [double]$PeakHi = -14.0,
  # Levels panel live check (mic Tone only): Tone's held peak and smoothed average windows. Parameters exist
  # only so the check can be proven to fail (e.g. -LevelsPeakLo -5 -LevelsPeakHi -4).
  [double]$LevelsPeakLo = -21.0,
  [double]$LevelsPeakHi = -19.0,
  [double]$LevelsAvgLo = -27.0,
  [double]$LevelsAvgHi = -20.0,
  # Levels panel responsiveness: Tone's first fresh reading must reach the display within this many ms of the
  # meters being started (the gate opening). Parameter exists only so the check can be proven to fail.
  [int]$FirstReadingMaxMs = 400,
  # LUFS mode live check (RWS_INFLIGHT_TEST_LUFS): the dock switches to LUFS with true peak on after the dB checks.
  # Prediction for Tone: libobs upmixes a mono source to the stereo mixer with gain 1 per channel (the matrix
  # {1,1} in audio-resampler-ffmpeg.c), so both channels carry a 1 kHz sine of -20 dBFS peak: BS.1770 gives
  # -0.691 + 10*log10(2 * 0.01/2 * 10^(0.0698)) = -19.99 LUFS for M and S (EBU Tech 3341 case 1 at -20), and the
  # true peak of a -20 dBFS sine is -20.0 dBTP. Windows: M and S +-0.5 LU, true peak +0.2/-0.4 dB.
  # Parameters exist only so the check can be proven to fail (e.g. -LufsLo -10 -LufsHi -9).
  [double]$LufsLo = -20.5,
  [double]$LufsHi = -19.5,
  [double]$LufsTpLo = -20.4,
  [double]$LufsTpHi = -19.8,
  # Volume move (RWS_INFLIGHT_TEST_LUFS): Tone's volume set to 0.5 reads -19.99 - 6.02 = -26.01 LUFS.
  [double]$VolLo = -26.5,
  [double]$VolHi = -25.5
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
$micUuid  = if ($MicSource -eq "Tone") { $toneUuid } else { $hushUuid }

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
$settingsJson = '{"setup_done":true,"mic_uuid":"' + $micUuid + '","desktop_uuid":"' + $hushUuid + '","check_updates":false}'
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
$env:RWS_INFLIGHT_TEST_LEVELS_GATE = "1"   # offscreen QMainWindow with tabified docks: the visibility gate test
$env:RWS_INFLIGHT_TEST_LEVELS = "1"   # the dock opens its Levels panel by itself and logs one line per source
$env:RWS_INFLIGHT_TEST_LUFS = "1"     # ... then switches to LUFS mode with true peak on and logs "lufs:" lines
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
  Remove-Item Env:\RWS_INFLIGHT_TEST_FORCE_LIVE -ErrorAction SilentlyContinue
  Remove-Item Env:\RWS_INFLIGHT_TEST_LEVELS -ErrorAction SilentlyContinue
  Remove-Item Env:\RWS_INFLIGHT_TEST_LEVELS_GATE -ErrorAction SilentlyContinue
  Remove-Item Env:\RWS_INFLIGHT_TEST_LUFS -ErrorAction SilentlyContinue
}

# Kit assertions (new with the merge): read after OBS has exited, so the saved dock state is final
[void](Wait-KitObsExit (Join-Path $obs "bin\64bit\obs64.exe") 20)
$final = Get-KitLogText $cfg
$visible = @(Stop-KitWindowWatch)   # background sampler (every 250 ms from OBS start through exit)
$kc = @()
$kc += Test-KitLoadLog $final $cfg @("multistream", "preflight", "inflight")
$kc += Test-KitUpdateNotice $final
$kc += New-KitCheck (($final -match '\[rws-inflight\] audio-watch: attach mic ok') -and ($final -match '\[rws-inflight\] audio-watch: attach desktop ok')) "seeded In-Flight settings were loaded: mic_uuid (Tone) and desktop_uuid (Hush) -> 'audio-watch: attach mic ok' / 'attach desktop ok'"
$kc += Test-KitSeedSettings $cfg @("kit")
$ifs = Read-KitSeedJson $cfg "inflight"
$kc += New-KitCheck ($ifs -ne $null -and $ifs.setup_done -eq $true -and $ifs.mic_uuid -eq $micUuid -and $ifs.desktop_uuid -eq $hushUuid) "In-Flight settings.json intact after exit (setup_done, mic, desktop)"
$kc += Test-KitHidden $visible
$kc += Test-KitClosePath
$kc += New-KitCheck ($proc.HasExited -and $proc.ExitCode -eq 0) "OBS exited cleanly on WM_CLOSE (exit code $(if ($proc.HasExited) { $proc.ExitCode } else { 'still running' }))"
$lines = @("pid=$obsPid still_running_at_check=$alive", "log=$(if ($log) { $log.FullName } else { 'NONE' })", "mic_source=$MicSource window=[$PeakLo,$PeakHi]")
if ($failure) { $lines += "error during check: $failure" }
$attachMic  = $text -match 'audio-watch: attach mic ok'
$attachDesk = $text -match 'audio-watch: attach desktop ok'
$micPeak = $null
$micFirst = $false
$inWindow = $false
if ($text -match 'audio-watch: mic first reading peak=(-inf|-?\d+(\.\d+)?)') {
  $micFirst = $true
  $micPeak = $Matches[1]
  if ($micPeak -ne '-inf') {
    $v = [double]::Parse($micPeak, [Globalization.CultureInfo]::InvariantCulture)
    $inWindow = ($v -ge $PeakLo -and $v -le $PeakHi)
  }
}
$deskSilent = $text -match 'audio-watch: desktop first reading peak=-inf'
$noFail = -not ($text -match 'audio-watch: attach \w+ failed')
$lines += "attach_mic_ok=$attachMic"
$lines += "attach_desktop_ok=$attachDesk"
$lines += "mic_first_reading_seen=$micFirst (peak=$micPeak)"
$lines += "mic_peak_in_window=$inWindow"
$lines += "desktop_first_reading_is_minus_inf=$deskSilent"
$lines += "no_attach_failed=$noFail"
$lines += "obs_alive=$alive"

# Levels panel (live): the real dock opened its panel; one "levels: <name> peak=<x> avg=<y>" line per source.
# Tone: held peak in [LevelsPeakLo,LevelsPeakHi], smoothed average in [LevelsAvgLo,LevelsAvgHi].
# Hush: peak and average exactly -inf, and its gap line reads "Hush is silent". Only meaningful with the
# Tone mic (-MicSource Hush has no Tone row, so the check is skipped there to keep that fail-proof pure).
$levelsOk = $true
if ($MicSource -eq "Tone") {
  $lv = [regex]::Matches($text, 'levels: (Tone|Hush) peak=(-inf|-?\d+(?:\.\d+)?) avg=(-inf|-?\d+(?:\.\d+)?)')
  $tone = $null; $hush = $null
  foreach ($m in $lv) { if ($m.Groups[1].Value -eq 'Tone') { $tone = $m } else { $hush = $m } }
  $inv = [Globalization.CultureInfo]::InvariantCulture
  $tonePeakOk = $false; $toneAvgOk = $false
  if ($tone) {
    if ($tone.Groups[2].Value -ne '-inf') { $v = [double]::Parse($tone.Groups[2].Value, $inv); $tonePeakOk = ($v -ge $LevelsPeakLo -and $v -le $LevelsPeakHi) }
    if ($tone.Groups[3].Value -ne '-inf') { $v = [double]::Parse($tone.Groups[3].Value, $inv); $toneAvgOk = ($v -ge $LevelsAvgLo -and $v -le $LevelsAvgHi) }
  }
  $hushOk = ($hush -ne $null) -and $hush.Groups[2].Value -eq '-inf' -and $hush.Groups[3].Value -eq '-inf'
  $gapOk = $text -match 'levels: gap Hush is silent'
  $once = ($lv.Count -eq 2)
  $lines += "levels_tone_line=$(if ($tone) { $tone.Value } else { 'MISSING' })"
  $lines += "levels_hush_line=$(if ($hush) { $hush.Value } else { 'MISSING' })"
  $lines += "levels_tone_peak_in_window=$tonePeakOk window=[$LevelsPeakLo,$LevelsPeakHi]"
  $lines += "levels_tone_avg_in_window=$toneAvgOk window=[$LevelsAvgLo,$LevelsAvgHi]"
  $lines += "levels_hush_peak_avg_minus_inf=$hushOk"
  $lines += "levels_gap_hush_is_silent=$gapOk"
  $lines += "levels_logged_once_per_source=$once"
  $gateOk = ($text -match 'levels-gate-test ok') -and -not ($text -match 'levels-gate-test FAIL')
  $lines += "levels_gate_test_ok=$gateOk"
  # The floating-dock steps must be PRESENT and ok (an absent step must not pass silently).
  $floatSteps = 'float-shown-before-gate', 'float-minimize', 'float-restore', 'float-lost-premise-stopped', 'float-lost-signal-1hz'
  $floatOk = $true
  foreach ($s in $floatSteps) { if (-not ($text -match ('levels-gate-test step ' + [regex]::Escape($s) + ': .* -> ok'))) { $floatOk = $false } }
  $lines += "levels_gate_float_steps_ok=$floatOk"
  # Before the gate opens (panel open while suspended, meters cannot attach) the rows read their real names.
  $preOk = $text -match 'levels: pre-gate label mic=Tone desktop=Hush running=0'
  $lines += "levels_pregate_labels_are_real_names=$preOk"
  # Responsiveness: "levels: first reading after <ms> ms (Tone)" is logged by the panel (test hook) at the fast
  # tick that had the row's first fresh reading, measured from the moment the meters were started. The first
  # such line in the log is the dock's own panel (the gate-test fixtures start later).
  $fr = [regex]::Match($text, 'levels: first reading after (\d+) ms \(Tone\)')
  $firstMs = 'MISSING'
  $firstOk = $false
  if ($fr.Success) { $firstMs = [int]$fr.Groups[1].Value; $firstOk = ($firstMs -lt $FirstReadingMaxMs) }
  $lines += "levels_first_reading_ms=$firstMs limit=$FirstReadingMaxMs ok=$firstOk"
  # Cost note (no threshold yet): the fast-tick time line must be present once.
  $ft = [regex]::Matches($text, 'levels: fast tick mean=[\d.]+ us max=[\d.]+ us n=\d+')
  $ftOk = ($ft.Count -eq 1)
  $lines += "levels_fast_tick_logged_once=$ftOk ($(if ($ft.Count -gt 0) { $ft[0].Value } else { 'MISSING' }))"
  # (The old real-dock-gate-test hook was retired: the Kit dock's Levels gate is covered by test-kit-dock.ps1 phase E.)
  # LUFS mode (RWS_INFLIGHT_TEST_LUFS): one "lufs: <name> M=<x> S=<y> TP=<z>" line per source, the worker cost
  # line once, and ring overflow 0. Tone M/S inside [LufsLo,LufsHi], TP inside [LufsTpLo,LufsTpHi]; Hush all -inf.
  $lf = [regex]::Matches($text, 'lufs: (Tone|Hush) M=(-inf|--|-?\d+(?:\.\d+)?) S=(-inf|--|-?\d+(?:\.\d+)?) TP=(-inf|--|off|-?\d+(?:\.\d+)?)')
  $lTone = $null; $lHush = $null
  foreach ($m in $lf) { if ($m.Groups[1].Value -eq 'Tone') { $lTone = $m } else { $lHush = $m } }
  $lToneOk = $false
  if ($lTone) {
    $okm = $false; $oks = $false; $oktp = $false
    if ($lTone.Groups[2].Value -match '^-?\d') { $v = [double]::Parse($lTone.Groups[2].Value, $inv); $okm = ($v -ge $LufsLo -and $v -le $LufsHi) }
    if ($lTone.Groups[3].Value -match '^-?\d') { $v = [double]::Parse($lTone.Groups[3].Value, $inv); $oks = ($v -ge $LufsLo -and $v -le $LufsHi) }
    if ($lTone.Groups[4].Value -match '^-?\d') { $v = [double]::Parse($lTone.Groups[4].Value, $inv); $oktp = ($v -ge $LufsTpLo -and $v -le $LufsTpHi) }
    $lToneOk = $okm -and $oks -and $oktp
  }
  $lHushOk = ($lHush -ne $null) -and $lHush.Groups[2].Value -eq '-inf' -and $lHush.Groups[3].Value -eq '-inf' -and $lHush.Groups[4].Value -eq '-inf'
  $lOnce = ($lf.Count -eq 2)
  $lw = [regex]::Matches($text, 'lufs: worker mean=[\d.]+ us max=[\d.]+ us per 10 ms n=\d+')
  $lwOk = ($lw.Count -eq 1)
  $lOv = $text -match 'lufs: ring overflow=0(\D|$)'
  $lines += "lufs_tone_line=$(if ($lTone) { $lTone.Value } else { 'MISSING' })"
  $lines += "lufs_hush_line=$(if ($lHush) { $lHush.Value } else { 'MISSING' })"
  $lines += "lufs_tone_in_window=$lToneOk M_S_window=[$LufsLo,$LufsHi] tp_window=[$LufsTpLo,$LufsTpHi]"
  $lines += "lufs_hush_minus_inf=$lHushOk"
  $lines += "lufs_logged_once_per_source=$lOnce"
  $lines += "lufs_worker_cost_logged_once=$lwOk ($(if ($lw.Count -gt 0) { $lw[0].Value } else { 'MISSING' }))"
  $lines += "lufs_ring_overflow_zero=$lOv"
  # Per-source drop counts (item 4): one line each for Tone and Hush, both 0.
  $ld = [regex]::Matches($text, 'lufs: drops (Tone|Hush) blocks=0 frames=0')
  $ldOk = ($ld.Count -eq 2)
  $lines += "lufs_drops_zero_per_source=$ldOk"
  # Volume move: the dock halves Tone's volume with obs_source_set_volume (-6.02 dB) after the first reading.
  # M must be about -26.0 1 s later and M and S (a 3 s window) 3.8 s later; then the volume goes back to 1.0.
  # Proves the signal-driven gain path (the capture callback never reads libobs's volume itself).
  $v1 = [regex]::Match($text, 'lufs: volume-move Tone \+1s M=(-?\d+(?:\.\d+)?)')
  $v2 = [regex]::Match($text, 'lufs: volume-move Tone \+3\.8s M=(-?\d+(?:\.\d+)?) S=(-?\d+(?:\.\d+)?)')
  $v1Ok = $false; $v2Ok = $false
  if ($v1.Success) { $a = [double]::Parse($v1.Groups[1].Value, $inv); $v1Ok = ($a -ge $VolLo -and $a -le $VolHi) }
  if ($v2.Success) {
    $a = [double]::Parse($v2.Groups[1].Value, $inv); $b = [double]::Parse($v2.Groups[2].Value, $inv)
    $v2Ok = ($a -ge $VolLo -and $a -le $VolHi -and $b -ge $VolLo -and $b -le $VolHi)
  }
  $vRestored = $text -match 'lufs: volume-move Tone restored to 1\.0'
  $lines += "lufs_volume_move_1s=$(if ($v1.Success) { $v1.Value } else { 'MISSING' }) in_window=$v1Ok window=[$VolLo,$VolHi]"
  $lines += "lufs_volume_move_3.8s=$(if ($v2.Success) { $v2.Value } else { 'MISSING' }) in_window=$v2Ok"
  $lines += "lufs_volume_restored=$vRestored"
  $lufsOk = $lToneOk -and $lHushOk -and $lOnce -and $lwOk -and $lOv -and $ldOk -and $v1Ok -and $v2Ok -and $vRestored
  $levelsOk = $tonePeakOk -and $toneAvgOk -and $hushOk -and $gapOk -and $once -and $gateOk -and $floatOk -and $preOk -and $firstOk -and $ftOk -and $lufsOk
}
$lines += "--- Kit checks (one DLL, three tools, update notice, seeded settings, hidden) ---"
$lines += Get-KitCheckLines $kc
$lines += "--- plugin lines ---"
$lines += @($text -split "`r?`n" | Where-Object { $_ -match 'rws-inflight|Red Warden In|audio-watch|levels:|levels-gate-test|lufs:' })
Finish ($attachMic -and $attachDesk -and $micFirst -and $inWindow -and $deskSilent -and $noFail -and $levelsOk -and $alive -and -not $failure -and (Test-KitChecksOk $kc)) $lines

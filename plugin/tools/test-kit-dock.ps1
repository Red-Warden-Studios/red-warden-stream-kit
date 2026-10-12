<#
  Red Warden Stream Kit - the one dock ("RW Stream Kit"). ASCII only.

  Touches none of your real OBS. Uses the Kit testbed (plugin\.testbed\obs, a portable OBS copy) and its single DLL. Four
  hidden OBS runs, each asserted and closed the normal way (WM_CLOSE to the captured PID only):
    A. fresh config, nothing seeded: the dock is registered once, FIRST RUN docks it right and shows it (log line
       "kit dock: first run, docked right"), the Kit settings file records first_run_done, and the test hook saves
       OFFSCREEN screenshots (QWidget::grab, nothing is shown or floated) to .testbed\shots\kit-dock-*.png:
       before-live (Pre-Flight page), show-all, in-flight, ended + live preview, Multistream collapsed, settings tabs.
    B. second start on the same config: the dock is NOT docked again (that log line is absent) and is still present once.
    C. tools switched off in the Kit settings (Multistream and In-Flight): neither module loads (no panel, no vendor
       registration), Pre-Flight and the dock still do.
    D. In-Flight forced live (its test hook): the stage machine starts a session and the dock moves to In-Flight.
  Result: exit code 0 / 1 and a final "RESULT: PASS" / "RESULT: FAIL" line (run-all.ps1 -Steps kit-dock writes it to
  .testbed\run-kit-dock.txt).
#>
param(
  [string]$Config = "RelWithDebInfo",
  [int]$ShotSeconds = 30,
  [int]$Seconds = 22,
  # Mic test numbers (run F). The fixture mic is In-Flight's "Tone": a 1 kHz sine of -20 dBFS peak, mono, which libobs
  # upmixes to the stereo mixer with gain 1 per channel. The volmeter's peak is the sine's peak = -20.0 dBFS, its magnitude
  # is the RMS = peak - 3.01 dB = -23.01 dBFS on every callback, so Loudest = -20.0 and Typical = -23.0 (power mean of equal
  # values). Windows +-0.3 dB / +-0.5 dB. Parameters exist only so the check can be proven to fail.
  [double]$MicLoudestLo = -20.3,
  [double]$MicLoudestHi = -19.7,
  [double]$MicTypicalLo = -23.5,
  [double]$MicTypicalHi = -22.5
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "kit-testbed.ps1")
. (Join-Path (Get-KitToolDir "inflight") "tools\testbed-common.ps1")   # New-SineWav (the Tone fixture)
$kit = Get-KitTestbed $Config
$bed = $kit.Bed
$obs = $kit.Obs
$cfg = $kit.Cfg
$prof = Join-Path $cfg "basic\profiles\KitDockTest"
$shots = Join-Path $bed "shots"
$obsExe = Join-Path $obs "bin\64bit\obs64.exe"

function Finish([bool]$ok, [string[]]$lines) {
  $lines | ForEach-Object { Write-Host $_ }
  if ($ok) { Write-Host "RESULT: PASS" } else { Write-Host "RESULT: FAIL" }
  exit $(if ($ok) { 0 } else { 1 })
}
trap { Restore-KitObsWebsocket $kit.Obs; Write-Host "FAIL: $_"; Write-Host "RESULT: FAIL"; exit 1 }

if (!(Test-Path $kit.Dll)) { Finish $false @("rws-stream-kit.dll not found: $($kit.Dll)") }
Initialize-KitPortableObs $obs
if (!(Wait-KitObsExit $obsExe 30)) { Finish $false @("a testbed obs64.exe ($obs) is still running; not touching it") }
Install-KitPlugin $kit | ForEach-Object { Write-Host $_ }

# A fresh throwaway config: profile, one scene with a colour source, obs-websocket server off. NO Kit files.
function New-KitDockConfig {
  Clear-KitConfig $cfg
  New-Item -ItemType Directory -Force $prof | Out-Null
  New-Item -ItemType Directory -Force (Join-Path $cfg "basic\scenes") | Out-Null
  $ini = @"
[General]
FirstRun=false
LastVersion=999999999
EnableAutoUpdates=false
[Basic]
Profile=KitDockTest
ProfileDir=KitDockTest
SceneCollection=KitDockTest
SceneCollectionFile=KitDockTest
[BasicWindow]
ConfirmOnExit=false
WarnBeforeStartingStream=false
WarnBeforeStoppingStream=false
"@
  Set-Content (Join-Path $cfg "global.ini") $ini -Encoding UTF8
  Set-Content (Join-Path $cfg "user.ini") $ini -Encoding UTF8
  $wsDir = Join-Path $cfg "plugin_config\obs-websocket"
  New-Item -ItemType Directory -Force $wsDir | Out-Null
  Set-Content (Join-Path $wsDir "config.json") '{"alerts_enabled":false,"auth_required":false,"first_load":false,"server_enabled":false,"server_password":"","server_port":4466}' -Encoding UTF8
  Set-Content (Join-Path $prof "basic.ini") @"
[General]
Name=KitDockTest
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
  Set-Content (Join-Path $cfg "basic\scenes\KitDockTest.json") @'
{"name":"KitDockTest","current_scene":"Scene","current_program_scene":"Scene",
 "scene_order":[{"name":"Scene"}],
 "sources":[
  {"id":"color_source_v3","versioned_id":"color_source_v3","name":"Ember","uuid":"b0000000-0000-0000-0000-000000000002","settings":{"color":4282038770,"width":1280,"height":720}},
  {"id":"scene","versioned_id":"scene","name":"Scene","uuid":"b0000000-0000-0000-0000-000000000001","settings":{"id_counter":1,"items":[{"name":"Ember","source_uuid":"b0000000-0000-0000-0000-000000000002","id":1,"visible":true}]}}
 ]}
'@ -Encoding UTF8
}

# One OBS run: env set for the child, OBS started hidden, left running $Secs seconds, log read, closed via WM_CLOSE.
function Invoke-KitDockRun([string]$Name, [int]$Secs, [hashtable]$Env) {
  foreach ($k in $Env.Keys) { Set-Item -Path "Env:\$k" -Value $Env[$k] }
  $proc = Start-KitObs $obs "KitDockTest" "KitDockTest"
  $res = @{ Name = $Name; Pid = $proc.Id }
  try {
    Start-Sleep -Seconds $Secs
    $res.Alive = -not $proc.HasExited
    $res.Text = Get-KitLogText $cfg
  } finally {
    $res.Closed = $false
    try { $res.Closed = Close-KitObs $proc 20 } catch { }
    foreach ($k in $Env.Keys) { Remove-Item -Path "Env:\$k" -ErrorAction SilentlyContinue }
  }
  [void](Wait-KitObsExit $obsExe 30)
  $res.Final = Get-KitLogText $cfg
  $res.Visible = @(Stop-KitWindowWatch)
  $res.Exit = $proc.ExitCode
  $res.ClosePosted = $script:KitLastPosted
  return $res
}
function Count-Lines([string]$Text, [string]$Rx) { return ([regex]::Matches($Text, $Rx)).Count }
# The dock must fit a 360 px wide dock: no page may force the scroll content wider (the shot hook logs the widths).
function Test-KitFitsWidth($run, [string]$Tag) {
  $m = [regex]::Matches($run.Final, 'kit dock shot \S+: minimum widths content=(\d+)')
  $max = 0
  foreach ($x in $m) { $v = [int]$x.Groups[1].Value; if ($v -gt $max) { $max = $v } }
  return New-KitCheck ($m.Count -gt 0 -and $max -le 360) "${Tag}: the dock's minimum content width is at most 360 px in all $($m.Count) shots (widest $max)"
}
function Common-Checks($run, [string[]]$Tools) {
  $c = @()
  $c += Test-KitLoadLog $run.Final $cfg $Tools
  $c += Test-KitUpdateNotice $run.Final
  $h = Test-KitHidden $run.Visible
  $h.Msg = "[$($run.Name)] " + $h.Msg   # which of the five runs showed a window
  $c += $h
  if (-not $h.Ok) {   # diagnosis for a visible-window failure: the Kit / dock lines of that run, with the OBS clock
    Write-Host "DIAG [$($run.Name)] visible: $($run.Visible -join ' | ')"
    foreach ($l in @($run.Final -split "`r?`n" | Where-Object { $_ -match 'kit dock|dock|rws-stream-kit' } | Select-Object -First 40)) { Write-Host "DIAG [$($run.Name)] $l" }
  }
  $c += New-KitCheck ($run.ClosePosted -gt 0) "close path: WM_CLOSE was posted to $($run.ClosePosted) OBS window(s)"
  $c += New-KitCheck ($run.Closed -and $run.Exit -eq 0) "OBS exited cleanly on WM_CLOSE (exit code $($run.Exit))"
  return $c
}
$lines = @()
$all = @()

# ---- A. fresh config: first run -------------------------------------------------------------------------------------
New-Item -ItemType Directory -Force $shots | Out-Null
Get-ChildItem $shots -Filter "kit-dock-*.png" -ErrorAction SilentlyContinue | Remove-Item -Force
New-KitDockConfig
$a = Invoke-KitDockRun "A first run" $ShotSeconds @{ RWS_KIT_TEST = "1"; RWS_KIT_TEST_SHOT_DIR = $shots; RWS_PREFLIGHT_TEST = "1" }
$kc = @(Common-Checks $a @("multistream", "preflight", "inflight"))
$n = Count-Lines $a.Final '(?m)^\d\d:\d\d:\d\d\.\d+: \[rws-stream-kit\] kit dock: first run, docked right\r?$'
$kc += New-KitCheck ($n -eq 1) "A: first run docks the Kit dock right, exactly once (log 'kit dock: first run, docked right' x$n)"
$n = Count-Lines $a.Final '\[rws-stream-kit\] kit dock: dock state floating=0 visible=1'
$kc += New-KitCheck ($n -eq 1) "A: after the first-run docking the dock is not floating and is visible (x$n)"
$n = Count-Lines $a.Final '\[rws-stream-kit\] kit dock: dock state settled floating=0 visible=1'
$kc += New-KitCheck ($n -eq 1) "A: two seconds later (after OBS's own layout pass) the dock is still docked and visible (x$n)"
$ks = Join-Path $cfg "plugin_config\rws-stream-kit\settings.json"
$kj = $null
if (Test-Path $ks) { try { $kj = Get-Content $ks -Raw | ConvertFrom-Json } catch { } }
$kc += New-KitCheck ($kj -ne $null -and $kj.first_run_done -eq $true) "A: plugin_config\rws-stream-kit\settings.json records first_run_done=true (file: $(if ($kj) { 'valid' } else { 'MISSING/INVALID' }))"
$kc += New-KitCheck ($kj -ne $null -and $kj.tools.multistream -eq $true -and $kj.tools.preflight -eq $true -and $kj.tools.inflight -eq $true -and $kj.auto_switch_on_live -eq $true) "A: the saved Kit settings hold the defaults (all tools on, auto-switch on)"
$kc += Test-KitFitsWidth $a "A"
$n = Count-Lines $a.Final 'kit dock shots done'
$kc += New-KitCheck ($n -eq 1) "A: the offscreen screenshot hook ran to the end (x$n)"
# Levels labels (LUFS "Now" / "3s avg", Reference value label, "?" help link) on demo panels at 360 and 300 px of dock width.
foreach ($w in 360, 300) {
  $n = Count-Lines $a.Final ("kit dock levels labels levels-lufs-labels-$w`: dock width $w, panel minimum width \d+, fits=1")
  $kc += New-KitCheck ($n -eq 1) "A: the Levels panel's minimum width fits a ${w} px dock in LUFS mode (x$n)"
}
$n = Count-Lines $a.Final "levels-state levels-lufs-labels-360: mode=lufs now='Now -18\.4' avg='3s avg -19\.2' nowTip='Loudness over the last 0\.4 seconds \(EBU momentary\)' avgTip='Average loudness over the last 3 seconds \(EBU short-term\)\. Best number for judging your mix\.' tpTip='True peak: the highest point of the waveform, including between samples\. Keep it below -1 dBTP\.' ref='-16' refWord=line-colour help='\?' helpTip='What these numbers mean'"
$kc += New-KitCheck ($n -eq 1) "A: LUFS labels read 'Now' / '3s avg' with the plain-word tooltips, the Reference value label is '-16' while Reference is on, the '?' button is there (x$n)"
$n = Count-Lines $a.Final "levels-state levels-lufs-labels-360-refoff: .* ref='none'"
$kc += New-KitCheck ($n -eq 1) "A: with Reference off there is no Reference value label (x$n)"
$n = Count-Lines $a.Final 'kit dock: manual would open \S+#levels'
$kc += New-KitCheck ($n -eq 1) "A: the '?' link asks the dock to open the manual's #levels section; in a test run it only logs the URL (x$n)"
foreach ($f in "kit-dock-levels-lufs-labels-360", "kit-dock-levels-lufs-labels-300", "kit-dock-levels-lufs-labels-360-refoff", "kit-dock-levels-lufs-labels-360-dark") {
  $p = Join-Path $shots "$f.png"
  $len = if (Test-Path $p) { (Get-Item $p).Length } else { 0 }
  $kc += New-KitCheck ($len -gt 4000) "A: screenshot $f.png saved ($len bytes)"
}
foreach ($f in "kit-dock-before-live", "kit-dock-show-all", "kit-dock-in-flight", "kit-dock-ended-live-preview", "kit-dock-ms-collapsed", "kit-dock-settings-tools", "kit-dock-settings-updates", "kit-dock-settings-about") {
  $p = Join-Path $shots "$f.png"
  $len = if (Test-Path $p) { (Get-Item $p).Length } else { 0 }
  $kc += New-KitCheck ($len -gt 4000) "A: screenshot $f.png saved ($len bytes)"
}
$lines += "--- A. fresh config, first run ---"
$lines += Get-KitCheckLines $kc
$all += $kc
$lines += "--- A. kit lines ---"
$lines += @($a.Final -split "`r?`n" | Where-Object { $_ -match 'kit dock|rws-stream-kit' })

# ---- B. second start: no re-dock ------------------------------------------------------------------------------------
$b = Invoke-KitDockRun "B second start" $Seconds @{ RWS_PREFLIGHT_TEST = "1" }
$kc = @(Common-Checks $b @("multistream", "preflight", "inflight"))
$n = Count-Lines $b.Final 'kit dock: first run, docked right'
$kc += New-KitCheck ($n -eq 0) "B: the second start does not dock the dock again (log line absent, x$n)"
$kc += New-KitCheck ((Count-Lines $b.Final 'kit dock: dock state floating') -eq 0) "B: no first-run docking happened at all"
$lines += "--- B. second start ---"
$lines += Get-KitCheckLines $kc
$all += $kc

# ---- B2. the first-run marker was lost (a failed save), but OBS has a saved layout for the dock --------------------------
# Run A / B left OBS's own layout (user.ini DockState, written at exit) with the Kit dock in it. Take first_run_done out of the
# Kit settings, as a failed write would have left it: the next launch must NOT drag the dock back to the right-hand area.
$ui = Join-Path $cfg "user.ini"
$hasDockState = (Test-Path $ui) -and ((Get-Content $ui -Raw) -match 'DockState=')
$d = Join-Path $cfg "plugin_config\rws-stream-kit"
New-Item -ItemType Directory -Force $d | Out-Null
[IO.File]::WriteAllText((Join-Path $d "settings.json"), '{"check_updates":false}', (New-Object Text.UTF8Encoding($false)))
$b2 = Invoke-KitDockRun "B2 lost marker" $Seconds @{ RWS_PREFLIGHT_TEST = "1" }
$kc = @(Common-Checks $b2 @("multistream", "preflight", "inflight"))
$kc += New-KitCheck $hasDockState "B2: precondition: OBS saved a DockState in user.ini when run A / B closed"
$n = Count-Lines $b2.Final 'kit dock: first_run_done is not set, but OBS has a saved layout for the dock: not forcing it into place'
$kc += New-KitCheck ($n -eq 1) "B2: marker lost + saved layout for the dock: the Kit says it is not forcing the dock (x$n)"
$n = Count-Lines $b2.Final 'kit dock: first run, docked right'
$kc += New-KitCheck ($n -eq 0) "B2: ...and does not dock it (log line absent, x$n)"
$kc += New-KitCheck ((Count-Lines $b2.Final 'kit dock: dock state floating') -eq 0) "B2: no first-run docking happened at all"
$n = Count-Lines $b2.Final 'kit dock: first-run marker saved and read back'
$kc += New-KitCheck ($n -eq 1) "B2: the marker is written again and verified by a read-back (x$n)"
$kj = $null
if (Test-Path (Join-Path $d "settings.json")) { try { $kj = Get-Content (Join-Path $d "settings.json") -Raw | ConvertFrom-Json } catch { } }
$kc += New-KitCheck ($kj -ne $null -and $kj.first_run_done -eq $true) "B2: settings.json says first_run_done=true again"
$kc += New-KitCheck ((Count-Lines $a.Final 'kit dock: first-run marker saved and read back') -eq 1) "A: the first run saved its marker and read it back (x$(Count-Lines $a.Final 'kit dock: first-run marker saved and read back'))"
$lines += "--- B2. lost first-run marker with a saved layout ---"
$lines += Get-KitCheckLines $kc
$all += $kc
$lines += @($b2.Final -split "`r?`n" | Where-Object { $_ -match 'kit dock' })

# ---- C. tools off in the Kit settings --------------------------------------------------------------------------------
$d = Join-Path $cfg "plugin_config\rws-stream-kit"
New-Item -ItemType Directory -Force $d | Out-Null
[IO.File]::WriteAllText((Join-Path $d "settings.json"), '{"check_updates":false,"first_run_done":true,"tools":{"multistream":false,"preflight":true,"inflight":false}}', (New-Object Text.UTF8Encoding($false)))
$c = Invoke-KitDockRun "C tools off" $Seconds @{ RWS_PREFLIGHT_TEST = "1" }
$kc = @(Common-Checks $c @("preflight"))
$kc += New-KitCheck ((Count-Lines $c.Final 'Multistream is switched off in the Kit settings: not loaded') -eq 1) "C: Multistream off: the module says it is not loaded"
$kc += New-KitCheck ((Count-Lines $c.Final 'In-Flight is switched off in the Kit settings: not loaded') -eq 1) "C: In-Flight off: the module says it is not loaded"
$kc += New-KitCheck ((Count-Lines $c.Final '\[rws-multistream\]') -eq 0) "C: Multistream off: no [rws-multistream] line at all (no widget, no outputs, no vendor)"
$kc += New-KitCheck ((Count-Lines $c.Final '\[rws-inflight\]') -eq 0) "C: In-Flight off: no [rws-inflight] line at all (no controller, no probe)"
$kc += New-KitCheck ((Count-Lines $c.Final 'obs-websocket vendor "RedWardenMultistream" registered') -eq 0) "C: Multistream off: its obs-websocket vendor is not registered"
$kj = $null
if (Test-Path (Join-Path $d "settings.json")) { try { $kj = Get-Content (Join-Path $d "settings.json") -Raw | ConvertFrom-Json } catch { } }
$kc += New-KitCheck ($kj -ne $null -and $kj.check_updates -eq $false -and $kj.tools.multistream -eq $false -and $kj.tools.inflight -eq $false) "C: the Kit settings survived the run (tools off, check_updates still false)"
$lines += "--- C. Multistream and In-Flight switched off ---"
$lines += Get-KitCheckLines $kc
$all += $kc

# ---- D. In-Flight forced live: the stage machine reacts --------------------------------------------------------------
[IO.File]::WriteAllText((Join-Path $d "settings.json"), '{"check_updates":false,"first_run_done":true}', (New-Object Text.UTF8Encoding($false)))
$e = Invoke-KitDockRun "D forced live" $Seconds @{ RWS_INFLIGHT_TEST = "1"; RWS_INFLIGHT_TEST_FORCE_LIVE = "1"; RWS_PREFLIGHT_TEST = "1" }
$kc = @(Common-Checks $e @("multistream", "preflight", "inflight"))
$n = Count-Lines $e.Final '\[rws-stream-kit\] kit dock: session started \(stage In-Flight\)'
$kc += New-KitCheck ($n -eq 1) "D: In-Flight reports Live, the stage machine starts a session and switches to In-Flight (x$n)"
$kc += New-KitCheck ((Count-Lines $e.Final 'kit dock: session ended') -eq 0) "D: no session end while forced live"
$lines += "--- D. forced live ---"
$lines += Get-KitCheckLines $kc
$all += $kc
$lines += "--- D. kit lines ---"
$lines += @($e.Final -split "`r?`n" | Where-Object { $_ -match 'kit dock' })

# ---- E. seeded config: configured Pre-Flight and In-Flight, two Multistream destinations (screenshots only) ---------------
# Same offscreen hook, file prefix kit-dock-seeded-: the Pre-Flight checklist (not the setup card), In-Flight with its lights,
# and the pinned Multistream section with destination rows.
New-KitDockConfig
Write-KitSeedSettings $cfg @("preflight", "inflight")
$d = Join-Path $cfg "plugin_config\rws-stream-kit"
New-Item -ItemType Directory -Force $d | Out-Null
[IO.File]::WriteAllText((Join-Path $d "settings.json"), '{"check_updates":false}', (New-Object Text.UTF8Encoding($false)))
[IO.File]::WriteAllText((Join-Path $prof "rws-multistream.json"), '{"version":1,"destinations":[{"id":"kd000001","name":"YouTube","platform":"custom","server":"rtmp://127.0.0.1:1/live","enabled":true,"encoder":{"shared":true}},{"id":"kd000002","name":"Kick","platform":"custom","server":"rtmp://127.0.0.1:2/live","enabled":false,"encoder":{"shared":true}}]}', (New-Object Text.UTF8Encoding($false)))
Get-ChildItem $shots -Filter "kit-dock-seeded-*.png" -ErrorAction SilentlyContinue | Remove-Item -Force
$g = Invoke-KitDockRun "E seeded shots" $ShotSeconds @{ RWS_KIT_TEST = "1"; RWS_KIT_TEST_SHOT_DIR = $shots; RWS_KIT_TEST_SHOT_PREFIX = "kit-dock-seeded"; RWS_PREFLIGHT_TEST = "1" }
$kc = @(Common-Checks $g @("multistream", "preflight", "inflight"))
$kc += Test-KitFitsWidth $g "E"
# In-Flight is already set up here (different mic than Pre-Flight's): it must NOT auto-copy, and its choices stay as seeded.
$kc += New-KitCheck ((Count-Lines $g.Final 'auto-copied choices from Pre-Flight') -eq 0) "E: In-Flight already set up: no auto-copy from Pre-Flight (log line absent)"
$kc += Test-KitSeedSettings $cfg @("inflight")
$n = Count-Lines $g.Final 'kit dock shots done'
$kc += New-KitCheck ($n -eq 1) "E: the offscreen screenshot hook ran to the end (x$n)"
$lv = @(
  @("preflight-open",      'stage=Pre-Flight shown=1 open=1 hostShown=1 gate=\d running=\d button=1', "Levels is reachable in the Pre-Flight stage: section shown, expanded, the gate's host input is on, the Pre-Flight Levels button is there"),
  @("preflight-collapsed", 'stage=Pre-Flight shown=0 open=0 hostShown=0 gate=\d running=0 button=1',  "Levels collapsed in the Pre-Flight stage: the section header is HIDDEN (the Levels button in Pre-Flight's row is the way in), the gate's host input is OFF and the meters are not running (changed from shown=1: the duplicate header is gone)"),
  @("inflight-collapsed",  'stage=In-Flight shown=1 open=0 hostShown=0 gate=\d running=0 button=1',   "Levels collapsed on the In-Flight stage page: the section header is still shown, so it can be opened there; host input OFF, meters not running"),
  @("showall-collapsed",   'stage=In-Flight shown=1 open=0 hostShown=0 gate=\d running=0 button=1',   "Levels collapsed in Show all: the section header is still shown; host input OFF, meters not running"),
  @("inflight-open",       'stage=In-Flight shown=1 open=1 hostShown=1 gate=\d running=\d button=1',  "Levels open on the In-Flight stage page: shown, host input on (the stage change did not lose it)"),
  @("section-hidden",      'stage=In-Flight shown=0 open=1 hostShown=0 gate=\d running=0 button=1',    "Levels section not shown (even while expanded): the gate's host input is OFF and the meters are not running"),
  @("restored",            'stage=Pre-Flight shown=0 open=0 hostShown=0 gate=\d running=0 button=1',   "test hook put the state back (Pre-Flight page, Levels collapsed and its header hidden, host input off)"),
  @("scroll-start",        'stage=Pre-Flight shown=1 open=1 hostShown=1 gate=1 running=1 button=1',     "Levels open and scrolled into the Kit dock's viewport: host input on, the gate open, the meters running"),
  @("scrolled-out",        'stage=Pre-Flight shown=1 open=1 hostShown=0 gate=0 running=0 button=1',     "Levels section shown and expanded but scrolled ENTIRELY out of the scroll viewport (viewport 100 px, scrolled to the top): host input OFF, gate 0, meters NOT running (a regression case)"),
  @("scrolled-in",         'stage=Pre-Flight shown=1 open=1 hostShown=1 gate=1 running=1 button=1',     "scrolled back into view (by the scroll event alone, nothing re-asked the gate): host input on, gate 1, meters running again"),
  @("scroll-restored",     'stage=Pre-Flight shown=0 open=0 hostShown=0 gate=0 running=0 button=1',     "test hook put the viewport and Levels back")
)
foreach ($l in $lv) {
  $n = Count-Lines $g.Final ('kit dock levels: ' + $l[0] + ' ' + $l[1])
  $kc += New-KitCheck ($n -eq 1) ("E: " + $l[2] + " (x$n)")
}
foreach ($f in "kit-dock-seeded-before-live", "kit-dock-seeded-show-all", "kit-dock-seeded-in-flight", "kit-dock-seeded-ended-live-preview", "kit-dock-seeded-preflight-levels", "kit-dock-seeded-settings-tools") {
  $p = Join-Path $shots "$f.png"
  $len = if (Test-Path $p) { (Get-Item $p).Length } else { 0 }
  $kc += New-KitCheck ($len -gt 4000) "E: screenshot $f.png saved ($len bytes)"
}
$lines += "--- E. seeded config, screenshots ---"
$lines += Get-KitCheckLines $kc
$all += $kc

# ---- F. Pre-Flight mic test numbers -----------------------------------------------------------------------------------
# Mic = "Tone" (a 1 kHz sine, -20 dBFS peak). A test hook in Pre-Flight starts the 5 s mic test, the Kit grabs the whole dock
# (offscreen) when the numbers appear, and 3 s later the Reset path clears them.
New-KitDockConfig
$toneWav = Join-Path $bed "tone-1k-20db.wav"
New-SineWav $toneWav 2.0
$toneJson = $toneWav.Replace('\', '/')
$toneUuid = "b0000000-0000-0000-0000-000000000003"
$sceneUuid = "b0000000-0000-0000-0000-000000000001"
$ffTone = '{"is_local_file":true,"local_file":"' + $toneJson + '","looping":true,"restart_on_activate":true}'
$scene = @"
{"name":"KitDockTest","current_scene":"Scene","current_program_scene":"Scene",
 "scene_order":[{"name":"Scene"}],
 "sources":[
  {"id":"color_source_v3","versioned_id":"color_source_v3","name":"Ember","uuid":"b0000000-0000-0000-0000-000000000002","settings":{"color":4282038770,"width":1280,"height":720}},
  {"id":"ffmpeg_source","versioned_id":"ffmpeg_source","name":"Tone","uuid":"$toneUuid","mixers":255,"volume":1.0,"muted":false,"enabled":true,"settings":$ffTone},
  {"id":"scene","versioned_id":"scene","name":"Scene","uuid":"$sceneUuid","settings":{"id_counter":2,"items":[
    {"name":"Ember","source_uuid":"b0000000-0000-0000-0000-000000000002","id":1,"visible":true},
    {"name":"Tone","source_uuid":"$toneUuid","id":2,"visible":true}]}}
 ]}
"@
[IO.File]::WriteAllText((Join-Path $cfg "basic\scenes\KitDockTest.json"), $scene, (New-Object Text.UTF8Encoding($false)))
$pfDir = Join-Path $cfg "plugin_config\rws-preflight"
New-Item -ItemType Directory -Force $pfDir | Out-Null
[IO.File]::WriteAllText((Join-Path $pfDir "settings.json"), ('{"setup_done":true,"use_main_stream":true,"use_multistream":false,"mic_uuid":"' + $toneUuid + '","start_scene_uuid":"' + $sceneUuid + '","record_intent":false,"check_updates":false}'), (New-Object Text.UTF8Encoding($false)))
$d = Join-Path $cfg "plugin_config\rws-stream-kit"
New-Item -ItemType Directory -Force $d | Out-Null
[IO.File]::WriteAllText((Join-Path $d "settings.json"), '{"check_updates":false}', (New-Object Text.UTF8Encoding($false)))
Remove-Item (Join-Path $shots "kit-dock-mic-numbers.png") -ErrorAction SilentlyContinue
$mf = Invoke-KitDockRun "F mic numbers" 30 @{ RWS_KIT_TEST_MICSHOT = "1"; RWS_KIT_TEST_SHOT_DIR = $shots; RWS_PREFLIGHT_TEST = "1"; RWS_PREFLIGHT_TEST_MICTEST = "6"; RWS_PREFLIGHT_TEST_MICTEST_RESET = "1" }
$kc = @(Common-Checks $mf @("multistream", "preflight", "inflight"))
$n = Count-Lines $mf.Final 'mic test: samples=\d+'
$kc += New-KitCheck ($n -eq 1) "F: the 5 s mic test ran once and decided as before (log 'mic test: samples=..' x$n)"
$mm = [regex]::Match($mf.Final, 'mic test numbers: loudest=(-?\d+\.\d|-inf) typical=(-?\d+\.\d|-inf)')
$kc += New-KitCheck $mm.Success "F: the mic test logged its numbers (mic test numbers: loudest=$($mm.Groups[1].Value) typical=$($mm.Groups[2].Value))"
$loud = [double]::NaN; $typ = [double]::NaN
if ($mm.Success) { [void][double]::TryParse($mm.Groups[1].Value, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$loud); [void][double]::TryParse($mm.Groups[2].Value, [Globalization.NumberStyles]::Float, [Globalization.CultureInfo]::InvariantCulture, [ref]$typ) }
$kc += New-KitCheck ((-not [double]::IsNaN($loud)) -and $loud -ge $MicLoudestLo -and $loud -le $MicLoudestHi) "F: Loudest for the -20 dBFS sine is $loud (want $MicLoudestLo .. $MicLoudestHi; the sine's peak)"
$kc += New-KitCheck ((-not [double]::IsNaN($typ)) -and $typ -ge $MicTypicalLo -and $typ -le $MicTypicalHi) "F: Typical for the -20 dBFS sine is $typ (want $MicTypicalLo .. $MicTypicalHi; the sine's RMS, peak - 3.01 dB)"
$sm = [regex]::Match($mf.Final, 'mic numbers shown: Loudest (\S+) dB Typical (\S+) dB')
$kc += New-KitCheck ($sm.Success -and $mm.Success -and $sm.Groups[1].Value -eq $mm.Groups[1].Value -and $sm.Groups[2].Value -eq $mm.Groups[2].Value) "F: the Mic test row shows the same two numbers under it ('mic numbers shown: Loudest $($sm.Groups[1].Value) dB Typical $($sm.Groups[2].Value) dB')"
$p = Join-Path $shots "kit-dock-mic-numbers.png"
$len = if (Test-Path $p) { (Get-Item $p).Length } else { 0 }
$kc += New-KitCheck ($len -gt 4000) "F: screenshot kit-dock-mic-numbers.png saved ($len bytes)"
$iShown = $mf.Final.IndexOf('mic numbers shown: Loudest')
$iReset = $mf.Final.IndexOf('test hook: reset after the mic test')
$iCleared = $mf.Final.IndexOf('mic numbers shown: cleared')
$kc += New-KitCheck ($iShown -ge 0 -and $iReset -gt $iShown -and $iCleared -gt $iReset) "F: Reset clears the numbers (shown, then reset, then 'mic numbers shown: cleared')"
$lines += "--- F. Pre-Flight mic test numbers ---"
$lines += Get-KitCheckLines $kc
$all += $kc
$lines += @($mf.Final -split "`r?`n" | Where-Object { $_ -match 'mic test|mic numbers|test hook' })

Restore-KitObsWebsocket $obs
Finish (Test-KitChecksOk $all) $lines

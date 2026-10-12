<#
  Red Warden Pre-Flight UI test (dock UI + settings dialog).

  Same portable OBS copy and throwaway scene collection / settings as test-state.ps1 (.testbed\obs),
  plus two custom checklist items. Sets RWS_PREFLIGHT_TEST=1,
  RWS_PREFLIGHT_TEST_SHOT=.testbed\ui-dock.png and RWS_PREFLIGHT_TEST_SETTINGS_SHOT=.testbed\ui-settings.png,
  launches OBS with --portable, waits ~50 s, then asserts the log lines
  "[rws-preflight] ui-shot saved" and "[rws-preflight] settings-shot saved" and that both PNGs
  exist and are larger than 2 KB. Stops ONLY the OBS process it started (captured PID).
  Result: .testbed\run-ui.txt (PASS / FAIL + details), exit code 0 / 1.
#>
param(
  [string]$Config = "RelWithDebInfo",
  [int]$Seconds = 50
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
$shotDock = Join-Path $bed "ui-dock.png"
$shotSet  = Join-Path $bed "ui-settings.png"
New-Item -ItemType Directory -Force $bed | Out-Null
Remove-Item $shotDock, $shotSet -ErrorAction SilentlyContinue

$uStart = "c0000000-0000-0000-0000-000000000001"
$uOther = "c0000000-0000-0000-0000-000000000002"
$uMic   = "c0000000-0000-0000-0000-000000000003"
$uColor = "c0000000-0000-0000-0000-000000000004"

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

# Same fixture as test-state.ps1 (see the note there): the mic must be a scene item or OBS drops it.
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

$plugCfg = Join-Path $cfg "plugin_config\rws-preflight"
New-Item -ItemType Directory -Force $plugCfg | Out-Null
$settings = '{"setup_done":true,"use_main_stream":true,"use_multistream":true,"mic_uuid":"' + $uMic + '","start_scene_uuid":"' + $uStart + '","record_intent":false,"check_updates":false,"custom_items":[{"text":"Close the door"},{"text":"Phone on silent"}]}'
Set-Content (Join-Path $plugCfg "settings.json") $settings -Encoding UTF8
Write-KitSeedSettings $cfg @("kit", "inflight")

$env:RWS_PREFLIGHT_TEST = "1"
$env:RWS_PREFLIGHT_TEST_SHOT = $shotDock
$env:RWS_PREFLIGHT_TEST_SETTINGS_SHOT = $shotSet
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
  try {
    $p = Get-Process -Id $obsPid -ErrorAction SilentlyContinue
    if ($p -and -not $p.HasExited) {
      [void](Send-KitWmClose $p)
      if (!$p.WaitForExit(15000)) { Stop-Process -Id $obsPid -Force -ErrorAction SilentlyContinue }
    }
  } catch { }
  Remove-Item Env:\RWS_PREFLIGHT_TEST -ErrorAction SilentlyContinue
  Remove-Item Env:\RWS_PREFLIGHT_TEST_SHOT -ErrorAction SilentlyContinue
  Remove-Item Env:\RWS_PREFLIGHT_TEST_SETTINGS_SHOT -ErrorAction SilentlyContinue
}

# Kit assertions (new with the merge): read after OBS has exited, so the saved dock state is final
[void](Wait-KitObsExit (Join-Path $obs "bin\64bit\obs64.exe") 20)
$final = Get-KitLogText $cfg
$visible = @(Stop-KitWindowWatch)   # background sampler (every 250 ms from OBS start through exit)
$kc = @()
$kc += Test-KitLoadLog $final $cfg @("multistream", "preflight", "inflight")
$kc += Test-KitUpdateNotice $final
$kc += Test-KitSeedLoaded $final @("preflight", "inflight")
$kc += Test-KitSeedSettings $cfg @("kit", "inflight")
# This harness cannot stay fully hidden: its own screenshot hook (RWS_PREFLIGHT_TEST_SHOT, in the plugin) floats and shows the
# Pre-Flight dock at 360x720 for the grab (a docked widget cannot be resized), and the settings-dialog hook shows the dialog for
# 1.5 s. The main OBS window is hidden; the only visible top-level window allowed is that floated 'Pre-Flight' dock. Reported.
# Continuous sampling sees TWO windows from Pre-Flight's own hooks: the floated 'Pre-Flight' dock (40 s) and the 'Pre-Flight settings'
# dialog (shown ~1.5 s at 5 s). Both come from the plugin's screenshot hooks; nothing else may be visible.
$kc += Test-KitHidden $visible "'Pre-Flight*"
$kc += Test-KitClosePath
$kc += New-KitCheck ($proc.HasExited -and $proc.ExitCode -eq 0) "OBS exited cleanly on WM_CLOSE (exit code $(if ($proc.HasExited) { $proc.ExitCode } else { 'still running' }))"
$pfs = Read-KitSeedJson $cfg "preflight"
$kc += New-KitCheck ($pfs -ne $null -and $pfs.setup_done -eq $true -and $pfs.mic_uuid -eq $uMic -and @($pfs.custom_items).Count -eq 2 -and $pfs.custom_items[0].text -eq "Close the door" -and $pfs.custom_items[1].text -eq "Phone on silent") "Pre-Flight settings.json intact after exit (mic and the two custom checklist items)"
$all = @($text -split "`r?`n")
$plug = @($all | Where-Object { $_ -match '\[rws-preflight\]' })
$dockLine = ($plug | Where-Object { $_ -match 'ui-shot saved' }).Count -gt 0
$setLine  = ($plug | Where-Object { $_ -match 'settings-shot saved' }).Count -gt 0
$dockSize = if (Test-Path $shotDock) { (Get-Item $shotDock).Length } else { 0 }
$setSize  = if (Test-Path $shotSet) { (Get-Item $shotSet).Length } else { 0 }

# PNG width/height live big-endian at byte 16 and 20 of the file.
function Png-Size([string]$p) {
  if (!(Test-Path $p)) { return @(0, 0) }
  $b = [IO.File]::ReadAllBytes($p)
  if ($b.Length -lt 24) { return @(0, 0) }
  $w = ([int]$b[16] * 16777216) + ([int]$b[17] * 65536) + ([int]$b[18] * 256) + [int]$b[19]
  $h = ([int]$b[20] * 16777216) + ([int]$b[21] * 65536) + ([int]$b[22] * 256) + [int]$b[23]
  return @($w, $h)
}
$dockWH = Png-Size $shotDock
# The test hook floats the dock at about 360x720 logical px, so the whole dock is in the picture.
# Display scaling multiplies the grabbed pixels (150% gives 540x1080), so check the logical size
# at any common scale factor rather than raw pixels.
$sizeOk = $false
foreach ($s in 1.0, 1.25, 1.5, 1.75, 2.0) {
  $w = $dockWH[0] / $s; $h = $dockWH[1] / $s
  if (($w -ge 300) -and ($w -le 460) -and ($h -ge 600) -and ($h -le 800)) { $sizeOk = $true }
}

$checks = @(
  "ui-shot saved line=$dockLine",
  "settings-shot saved line=$setLine",
  "ui-dock.png bytes=$dockSize (want > 2048)",
  "ui-dock.png size=$($dockWH[0])x$($dockWH[1]) (want about 360x720 logical at 100-200% scaling)=$sizeOk",
  "ui-settings.png bytes=$setSize (want > 2048)"
)
$ok = $dockLine -and $setLine -and ($dockSize -gt 2048) -and $sizeOk -and ($setSize -gt 2048) -and $alive -and -not $failure -and (Test-KitChecksOk $kc)

$lines = @("pid=$obsPid still_running_at_check=$alive", "log=$(if ($log) { $log.FullName } else { 'NONE' })")
if ($failure) { $lines += "error during check: $failure" }
$lines += $checks
$lines += "--- Kit checks (one DLL, three tools, update notice, seeded settings, hidden) ---"
$lines += Get-KitCheckLines $kc
$lines += "--- rws-preflight log lines ---"
$lines += $plug
Finish $ok $lines

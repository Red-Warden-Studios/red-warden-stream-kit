<#
  Red Warden Stream Kit - what does the Kit cost in CPU while streaming? ASCII only. OPTIONAL: not part of any release list and not
  read by check-test-results.ps1 (run-all.ps1 -Steps cost; smoke test: -Steps cost -CostMinutes 2).

  Target: Kit overhead under 0.5 % of ONE CPU core while streaming, with every tool on and the Levels panel open (worst case) and
  closed (normal case).

  Touches none of your real OBS. Uses the Kit testbed (plugin\.testbed\obs, a portable OBS copy), started hidden, closed with
  WM_CLOSE to the captured PID only. Every run streams the same small x264 stream (640x360, 30 fps, ultrafast, 800 kbps CBR, a colour
  source, a noise "Mic" and a tone "Desktop Audio" as looping WAVs) to a local ffmpeg receiver, then measures obs64's total CPU time
  ((Get-Process -Id <pid>).TotalProcessorTime) over a fixed window after a warm-up. The runs, in this order (-Runs):
    A  Kit NOT installed (rws-stream-kit.dll parked outside the plugins folder); OBS's main stream only
    B  Kit installed, all tools on, Levels panel closed;           main stream only
    C  Kit installed, all tools on, Levels panel open in LUFS mode; main stream only (the Levels state is seeded in In-Flight's
       settings: levels_open + levels_mode lufs, no hook and no command file, so no test timer runs in the measured OBS)
    A  again, B again (alternation, to show the run-to-run noise)
    D  Kit installed, all tools on, Levels closed, TWO Multistream destinations (shared encoder, local receivers). Reported as it
       is, never subtracted.
  The Kit runs (B, C, D) set RWS_KIT_TEST_COST=1: the Kit's in-process timing counters (src\common\kit-cost.hpp) log one line per
  counter every 60 s and at exit ("kit cost: <name> calls=<n> total_us=<n> max_us=<n>", cumulative). The harness differences two of
  those batches (nearest to the start and to the end of the measuring window) and reports, per counter, the average microseconds
  per call, the calls per second and the share of one core. The counters are wall time inside the hooks (preemption included),
  summed over the UI-thread timers, the audio-thread callbacks and the loudness worker thread.
  Report: CPU seconds and % of one core (= cpu_s / wall_s * 100) per run, B-A and C-A, the spread of the repeated A and B, D,
  and the counter tables. RESULT: PASS when the in-process total (sum of all counters, case C) is under 0.5 % of one core and every
  run was valid (streaming for the whole window, Levels really running in C, hidden launch, clean exit); else RESULT: FAIL. A
  "process-level verdict" line is informational only: an A/B of whole-process CPU time can be noisier than the effect.
  Parameters: -Minutes (default 30) measuring window per run, -WarmupSeconds (default 60) from OBS start to the window.
  Progress: .testbed\cost-progress.txt (appended as it goes); the final report is also saved to .testbed\cost-result.txt (run-all.ps1
  deletes run-*.txt at the start of every run). Needs ffmpeg on PATH.
#>
param(
  [string]$Config = "RelWithDebInfo",
  [int]$Minutes = 30,
  [int]$WarmupSeconds = 60,
  [string]$Runs = "A,B,C,A,B,D"
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "kit-testbed.ps1")
$kit = Get-KitTestbed $Config
$bed = $kit.Bed
$obs = $kit.Obs
$cfg = $kit.Cfg
$prof = Join-Path $cfg "basic\profiles\CostTest"
$obsExe = Join-Path $obs "bin\64bit\obs64.exe"
$progressFile = Join-Path $bed "cost-progress.txt"
$resultFile = Join-Path $bed "cost-result.txt"
$parked = Join-Path $bed "cost-parked"
$utf8 = New-Object Text.UTF8Encoding($false)
$seq = @($Runs -split ',' | ForEach-Object { $_.Trim().ToUpper() } | Where-Object { $_ })
$costEnv = @("RWS_KIT_TEST_COST")

function Progress([string]$m) {
  $l = "$(Get-Date -Format 'HH:mm:ss') $m"
  Write-Host $l
  Add-Content -Encoding ASCII $progressFile $l
}
function Finish([bool]$ok, [string[]]$lines) {
  $lines += $(if ($ok) { "RESULT: PASS" } else { "RESULT: FAIL" })
  $lines | Set-Content -Encoding ASCII $resultFile
  $lines | ForEach-Object { Write-Host $_ }
  exit $(if ($ok) { 0 } else { 1 })
}
$script:credIds = @()
$script:sinks = @()
$script:proc = $null
function Cleanup-Run {
  foreach ($s in @($script:sinks)) { try { if (!$s.HasExited) { $s.Kill() } } catch { } }
  $script:sinks = @()
  foreach ($id in $script:credIds) { try { [void][RWS.CostCred]::CredDeleteW("RedWardenStudios/Multistream/$id", 1, 0) } catch { } }
  $script:credIds = @()
  foreach ($k in $costEnv) { Remove-Item -Path "Env:\$k" -ErrorAction SilentlyContinue }
  Restore-KitObsWebsocket $kit.Obs
}
trap {
  try { if ($script:proc -and !$script:proc.HasExited) { [void](Close-KitObs $script:proc 30) } } catch { }
  Cleanup-Run
  Write-Host "FAIL: $_"; Write-Host "RESULT: FAIL"; exit 1
}

if (!(Test-Path $kit.Dll)) { Finish $false @("rws-stream-kit.dll not found: $($kit.Dll)") }
if (!(Get-Command ffmpeg -ErrorAction SilentlyContinue)) { Finish $false @("ffmpeg is not on PATH") }
Initialize-KitPortableObs $obs
if (!(Wait-KitObsExit $obsExe 30)) { Finish $false @("a testbed obs64.exe ($obs) is still running; not touching it") }
Remove-Item $progressFile -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force $parked | Out-Null

Add-Type -Namespace RWS -Name CostCred -MemberDefinition @'
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct CREDENTIAL { public int Flags; public int Type; public string TargetName; public string Comment;
  public long LastWritten; public int CredentialBlobSize; public System.IntPtr CredentialBlob; public int Persist;
  public int AttributeCount; public System.IntPtr Attributes; public string TargetAlias; public string UserName; }
[DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool CredWriteW(ref CREDENTIAL c, int flags);
[DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool CredDeleteW(string target, int type, int flags);
'@ -ErrorAction SilentlyContinue
function Set-Key($id, $key) {
  $bytes = [Text.Encoding]::UTF8.GetBytes($key)
  $ptr = [Runtime.InteropServices.Marshal]::AllocHGlobal($bytes.Length)
  [Runtime.InteropServices.Marshal]::Copy($bytes, 0, $ptr, $bytes.Length)
  $c = New-Object RWS.CostCred+CREDENTIAL
  $c.Type = 1; $c.TargetName = "RedWardenStudios/Multistream/$id"; $c.UserName = "stream-key"
  $c.CredentialBlobSize = $bytes.Length; $c.CredentialBlob = $ptr; $c.Persist = 2
  if (![RWS.CostCred]::CredWriteW([ref]$c, 0)) { throw "CredWrite failed" }
  [Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
}

# ---- fixtures: two looping WAVs (ffmpeg) ---------------------------------------------------------------------------------
$micWav = Join-Path $bed "cost-mic.wav"
$deskWav = Join-Path $bed "cost-desktop.wav"
& ffmpeg -hide_banner -loglevel error -y -f lavfi -i "anoisesrc=color=pink:amplitude=0.25:sample_rate=48000:duration=12" -ac 1 -c:a pcm_s16le $micWav
& ffmpeg -hide_banner -loglevel error -y -f lavfi -i "sine=frequency=261:sample_rate=48000:duration=12" -af "volume=0.25" -ac 2 -c:a pcm_s16le $deskWav
if (!(Test-Path $micWav) -or !(Test-Path $deskWav)) { Finish $false @("could not write the fixture WAVs with ffmpeg") }
$micUuid = "d0000000-0000-0000-0000-0000000000a1"
$deskUuid = "d0000000-0000-0000-0000-0000000000a2"
$bgUuid = "d0000000-0000-0000-0000-0000000000b1"
$scUuid = "d0000000-0000-0000-0000-000000000011"
function J([string]$p) { $p.Replace('\', '/') }

# ---- one throwaway config -------------------------------------------------------------------------------------------------
function New-CostConfig([bool]$LevelsOpen, [string[]]$DestIds, [bool]$KitSettings) {
  Clear-KitConfig $cfg
  New-Item -ItemType Directory -Force $prof | Out-Null
  New-Item -ItemType Directory -Force (Join-Path $cfg "basic\scenes") | Out-Null
  $ini = @"
[General]
FirstRun=false
LastVersion=999999999
EnableAutoUpdates=false
[Basic]
Profile=CostTest
ProfileDir=CostTest
SceneCollection=CostTest
SceneCollectionFile=CostTest
[BasicWindow]
ConfirmOnExit=false
WarnBeforeStartingStream=false
WarnBeforeStoppingStream=false
"@
  Set-Content (Join-Path $cfg "global.ini") $ini -Encoding UTF8
  Set-Content (Join-Path $cfg "user.ini") $ini -Encoding UTF8
  $wsDir = Join-Path $cfg "plugin_config\obs-websocket"
  New-Item -ItemType Directory -Force $wsDir | Out-Null
  Set-Content (Join-Path $wsDir "config.json") '{"alerts_enabled":false,"auth_required":false,"first_load":false,"server_enabled":true,"server_password":"","server_port":4466}' -Encoding UTF8
  Set-Content (Join-Path $prof "basic.ini") @"
[General]
Name=CostTest
[Output]
Mode=Advanced
[AdvOut]
Encoder=obs_x264
TrackIndex=1
Track1Bitrate=128
ApplyServiceSettings=true
[Video]
BaseCX=640
BaseCY=360
OutputCX=640
OutputCY=360
FPSType=0
FPSCommon=30
"@ -Encoding UTF8
  Set-Content (Join-Path $prof "streamEncoder.json") '{"rate_control":"CBR","bitrate":800,"keyint_sec":2,"preset":"ultrafast"}' -Encoding UTF8
  Set-Content (Join-Path $prof "service.json") '{"type":"rtmp_custom","settings":{"server":"rtmp://127.0.0.1:19360/live","key":"k","use_auth":false}}' -Encoding UTF8
  $items = '[{"name":"Background","source_uuid":"' + $bgUuid + '","id":1,"visible":true},{"name":"Mic","source_uuid":"' + $micUuid + '","id":2,"visible":true},{"name":"Desktop Audio","source_uuid":"' + $deskUuid + '","id":3,"visible":true}]'
  $scene = '{"id":"scene","versioned_id":"scene","name":"Live","uuid":"' + $scUuid + '","settings":{"id_counter":3,"items":' + $items + '}}'
  $col = '{"id":"color_source_v3","versioned_id":"color_source_v3","name":"Background","uuid":"' + $bgUuid + '","settings":{"color":4281545523,"width":640,"height":360}}'
  $mic = '{"id":"ffmpeg_source","versioned_id":"ffmpeg_source","name":"Mic","uuid":"' + $micUuid + '","mixers":255,"volume":1.0,"muted":false,"enabled":true,"settings":{"is_local_file":true,"local_file":"' + (J $micWav) + '","looping":true,"restart_on_activate":true}}'
  $desk = '{"id":"ffmpeg_source","versioned_id":"ffmpeg_source","name":"Desktop Audio","uuid":"' + $deskUuid + '","mixers":255,"volume":1.0,"muted":false,"enabled":true,"settings":{"is_local_file":true,"local_file":"' + (J $deskWav) + '","looping":true,"restart_on_activate":true}}'
  $json = '{"name":"CostTest","current_scene":"Live","current_program_scene":"Live","scene_order":[{"name":"Live"}],"sources":[' + $col + ',' + $mic + ',' + $desk + ',' + $scene + ']}'
  [IO.File]::WriteAllText((Join-Path $cfg "basic\scenes\CostTest.json"), $json, $utf8)
  if ($KitSettings) {
    $kd = Join-Path $cfg "plugin_config\rws-stream-kit"
    New-Item -ItemType Directory -Force $kd | Out-Null
    [IO.File]::WriteAllText((Join-Path $kd "settings.json"), '{"check_updates":false,"first_run_done":true}', $utf8)
    $useMs = if ($DestIds.Count -gt 0) { "true" } else { "false" }
    $d = Join-Path $cfg "plugin_config\rws-preflight"; New-Item -ItemType Directory -Force $d | Out-Null
    [IO.File]::WriteAllText((Join-Path $d "settings.json"), ('{"setup_done":true,"use_main_stream":true,"use_multistream":' + $useMs + ',"mic_uuid":"' + $micUuid + '","desktop_uuid":"' + $deskUuid + '","start_scene_uuid":"' + $scUuid + '","record_intent":false,"check_updates":false}'), $utf8)
    $lv = if ($LevelsOpen) { ',"levels_open":true,"levels_mode":"lufs"' } else { '' }
    $d = Join-Path $cfg "plugin_config\rws-inflight"; New-Item -ItemType Directory -Force $d | Out-Null
    [IO.File]::WriteAllText((Join-Path $d "settings.json"), ('{"setup_done":true,"mic_uuid":"' + $micUuid + '","desktop_uuid":"' + $deskUuid + '","check_updates":false' + $lv + '}'), $utf8)
    if ($DestIds.Count -gt 0) {
      $dest = @()
      $port = 19361
      $n = 1
      foreach ($id in $DestIds) {
        $dest += '{"id":"' + $id + '","name":"Cost dest ' + $n + '","platform":"custom","server":"rtmp://127.0.0.1:' + $port + '/live","enabled":true,"encoder":{"shared":true}}'
        Set-Key $id "k"
        $port++; $n++
      }
      [IO.File]::WriteAllText((Join-Path $prof "rws-multistream.json"), ('{"version":1,"destinations":[' + ($dest -join ',') + ']}'), $utf8)
    }
  }
}

# obs-websocket v5 client (raw request JSON in, reply text out), as test-net-light.ps1. Used only to stop the stream before the
# window is closed (a hidden OBS that is closed while streaming waits on its "exit while streaming?" question).
function Invoke-ObsWsRaw([string[]]$Requests, [int]$Port = 4466) {
  $ws = New-Object System.Net.WebSockets.ClientWebSocket
  $ws.Options.AddSubProtocol("obswebsocket.json")
  $ct = [Threading.CancellationToken]::None
  $ws.ConnectAsync([Uri]"ws://127.0.0.1:$Port", $ct).Wait()
  $buf = New-Object byte[] 65536
  $recv = { $r = $ws.ReceiveAsync((New-Object ArraySegment[byte] -ArgumentList (,$buf)), $ct).Result; [Text.Encoding]::UTF8.GetString($buf, 0, $r.Count) }
  $send = { param($t) $b = [Text.Encoding]::UTF8.GetBytes($t); $ws.SendAsync((New-Object ArraySegment[byte] -ArgumentList (,$b)), "Text", $true, $ct).Wait() }
  [void](& $recv)
  & $send '{"op":1,"d":{"rpcVersion":1,"eventSubscriptions":0}}'
  [void](& $recv)
  $out = @()
  foreach ($rq in $Requests) { & $send $rq; $out += (& $recv) }
  $ws.Dispose()
  return $out
}

function Start-Sink([int]$Port, [string]$LogPath) {
  $p = Start-Process ffmpeg -PassThru -WindowStyle Hidden -RedirectStandardError $LogPath -ArgumentList @(
    "-hide_banner", "-loglevel", "info", "-stats", "-stats_period", "5", "-y", "-listen", "1",
    "-i", "rtmp://127.0.0.1:$Port/live/k", "-c", "copy", "-f", "null", "-")
  $script:sinks += $p
  return $p
}
# Last "time=HH:MM:SS" ffmpeg reported for a receiver (seconds), -1 if none.
function Get-SinkSeconds([string]$LogPath) {
  if (!(Test-Path $LogPath)) { return -1 }
  $fs = [IO.File]::Open($LogPath, 'Open', 'Read', 'ReadWrite')
  try { $t = (New-Object IO.StreamReader($fs)).ReadToEnd() } finally { $fs.Close() }
  $m = [regex]::Matches($t, 'time=(\d\d):(\d\d):(\d\d)')
  if ($m.Count -eq 0) { return -1 }
  $l = $m[$m.Count - 1]
  return [int]$l.Groups[1].Value * 3600 + [int]$l.Groups[2].Value * 60 + [int]$l.Groups[3].Value
}

# ---- the "kit cost:" batches of one OBS log ----------------------------------------------------------------------------------
$slotCount = 12   # kitcost::SlotCount
function Get-CostBatches([string]$Text) {
  $ms = [regex]::Matches($Text, '(?m)^(\d\d):(\d\d):(\d\d)\.(\d+): \[rws-stream-kit\] kit cost: (\S+) calls=(\d+) total_us=(\d+) max_us=(\d+)')
  $entries = @(); $off = 0.0; $prev = -1.0
  foreach ($m in $ms) {
    $t = [int]$m.Groups[1].Value * 3600 + [int]$m.Groups[2].Value * 60 + [int]$m.Groups[3].Value + [double]::Parse("0." + $m.Groups[4].Value, [Globalization.CultureInfo]::InvariantCulture)
    if ($prev -ge 0 -and ($t + $off) -lt ($prev - 43200)) { $off += 86400 }
    $prev = $t + $off
    $entries += [pscustomobject]@{ T = $t + $off; Name = $m.Groups[5].Value; Calls = [double]$m.Groups[6].Value; TotalUs = [double]$m.Groups[7].Value; MaxUs = [double]$m.Groups[8].Value }
  }
  $batches = @()
  for ($i = 0; $i + $slotCount -le $entries.Count; $i += $slotCount) {
    $batches += ,@($entries[$i..($i + $slotCount - 1)])
  }
  return $batches
}
# Counter rows between the batch nearest to $T0 and the batch nearest to $T1 (seconds of day, unwrapped like the log).
function Get-CostRows($Batches, [double]$T0, [double]$T1) {
  if ($Batches.Count -lt 2) { return $null }
  $first = $Batches[0][0].T
  $c0 = $T0; if ($c0 -lt $first - 43200) { $c0 += 86400 }
  $c1 = $T1; if ($c1 -lt $first - 43200) { $c1 += 86400 }
  $best = { param($t) $bi = 0; $bd = [double]::MaxValue; for ($i = 0; $i -lt $Batches.Count; $i++) { $d = [Math]::Abs($Batches[$i][0].T - $t); if ($d -lt $bd) { $bd = $d; $bi = $i } }; $bi }
  $i0 = & $best $c0
  $i1 = & $best $c1
  if ($i1 -le $i0) { return $null }
  $interval = $Batches[$i1][0].T - $Batches[$i0][0].T
  $rows = @()
  for ($k = 0; $k -lt $slotCount; $k++) {
    $a = $Batches[$i0][$k]; $b = $Batches[$i1][$k]
    $calls = $b.Calls - $a.Calls; $tot = $b.TotalUs - $a.TotalUs
    $rows += [pscustomobject]@{
      Name = $b.Name; Calls = $calls; TotalUs = $tot; MaxUs = $b.MaxUs
      AvgUs = $(if ($calls -gt 0) { $tot / $calls } else { 0.0 })
      CallsPerSec = $calls / $interval
      Pct = $tot / ($interval * 1e6) * 100
    }
  }
  return @{ Rows = $rows; Interval = $interval; Total = (($rows | Measure-Object -Property Pct -Sum).Sum); Batch0 = $i0; Batch1 = $i1; Batches = $Batches.Count }
}
function Fmt([double]$v, [int]$d = 3) { $v.ToString("F$d", [Globalization.CultureInfo]::InvariantCulture) }

# ---- one run -------------------------------------------------------------------------------------------------------------------
function Invoke-CostRun([string]$Label, [string]$Kind) {
  $kitOn = $Kind -ne "A"
  $levelsOpen = $Kind -eq "C"
  $destIds = @()
  if ($Kind -eq "D") {
    $rr = -join ((1..6) | ForEach-Object { '{0:x}' -f (Get-Random -Maximum 16) })
    $destIds = @("co${rr}1", "co${rr}2")
    $script:credIds = $destIds
  }
  Progress "[$Label] kind ${Kind}: preparing (Kit installed=$kitOn, Levels open=$levelsOpen, destinations=$($destIds.Count))"
  if (!(Wait-KitObsExit $obsExe 30)) { throw "a testbed obs64.exe is still running; not touching it" }
  New-CostConfig $levelsOpen $destIds $kitOn
  Install-KitPlugin $kit | Out-Null
  if (!$kitOn) {
    # A: the Kit is not installed. Park the DLL (and its pdb and locale) outside the plugins folder.
    $pbin = Join-Path $obs "obs-plugins\64bit"
    foreach ($f in "rws-stream-kit.dll", "rws-stream-kit.pdb") {
      if (Test-Path (Join-Path $pbin $f)) { Move-Item (Join-Path $pbin $f) (Join-Path $parked $f) -Force }
    }
    $kd = Join-Path $obs "data\obs-plugins\rws-stream-kit"
    if (Test-Path $kd) { Remove-Item $kd -Recurse -Force }
    if (@(Get-ChildItem $pbin -Filter "rws-*" -ErrorAction SilentlyContinue).Count -ne 0) { throw "A: an rws file is still in the plugins folder" }
  }
  $outDir = Join-Path $bed "received-cost"
  New-Item -ItemType Directory -Force $outDir | Out-Null
  $mainLog = Join-Path $outDir "$Label-main.log"
  [void](Start-Sink 19360 $mainLog)
  $destLogs = @()
  for ($i = 0; $i -lt $destIds.Count; $i++) { $dl = Join-Path $outDir "$Label-dest$($i + 1).log"; [void](Start-Sink (19361 + $i) $dl); $destLogs += $dl }
  Start-Sleep -Seconds 2
  foreach ($k in $costEnv) { Remove-Item -Path "Env:\$k" -ErrorAction SilentlyContinue }
  if ($kitOn) { $env:RWS_KIT_TEST_COST = "1" }

  $proc = Start-KitObs $obs "CostTest" "CostTest" @("--startstreaming")
  $script:proc = $proc
  $obsPid = $proc.Id
  $startClock = Get-Date
  Progress "[$Label] OBS started, pid $obsPid; warming up $WarmupSeconds s"
  while (((Get-Date) - $startClock).TotalSeconds -lt $WarmupSeconds) {
    if ($proc.HasExited) { throw "[$Label] OBS exited during the warm-up (exit code $($proc.ExitCode))" }
    Start-Sleep -Milliseconds 500
  }
  $streaming = (Get-KitLogText $cfg) -match 'Streaming Start'
  $proc.Refresh()
  $cpu0 = $proc.TotalProcessorTime
  $sw = [Diagnostics.Stopwatch]::StartNew()
  $t0tod = (Get-Date).TimeOfDay.TotalSeconds
  Progress "[$Label] window start: cpu so far $(Fmt $cpu0.TotalSeconds) s, streaming started=$streaming; measuring $Minutes min"
  $windowSec = $Minutes * 60
  $nextNote = 300
  while ($sw.Elapsed.TotalSeconds -lt $windowSec) {
    if ($proc.HasExited) { break }
    Start-Sleep -Seconds 5
    if ($sw.Elapsed.TotalSeconds -ge $nextNote) {
      $proc.Refresh()
      $c = ($proc.TotalProcessorTime - $cpu0).TotalSeconds
      Progress "[$Label] +$([int]$sw.Elapsed.TotalSeconds) s: cpu $(Fmt $c) s = $(Fmt ($c / $sw.Elapsed.TotalSeconds * 100)) % of one core so far"
      $nextNote += 300
    }
  }
  $exitedEarly = $proc.HasExited
  $cpu = 0.0; $wall = $sw.Elapsed.TotalSeconds
  if (!$exitedEarly) {
    $proc.Refresh()
    $cpu1 = $proc.TotalProcessorTime
    $wall = $sw.Elapsed.TotalSeconds
    $cpu = ($cpu1 - $cpu0).TotalSeconds
  }
  $t1tod = (Get-Date).TimeOfDay.TotalSeconds
  $textEnd = Get-KitLogText $cfg
  $stillStreaming = ($textEnd -match 'Streaming Start') -and ($textEnd -notmatch 'Streaming Stop')
  $sinkSec = Get-SinkSeconds $mainLog
  $destSec = @($destLogs | ForEach-Object { Get-SinkSeconds $_ })
  Progress "[$Label] window end: cpu $(Fmt $cpu) s over $(Fmt $wall 1) s = $(Fmt ($cpu / $wall * 100)) % of one core; still streaming=$stillStreaming; receiver time=$sinkSec s"
  $closed = $false
  if (!$exitedEarly) {
    try { [void](Invoke-ObsWsRaw @('{"op":6,"d":{"requestType":"StopStream","requestId":"1"}}')) } catch { Write-Warning "websocket: $_" }
    Start-Sleep -Seconds 4
    try { $closed = Close-KitObs $proc 40 } catch { }
  }
  [void](Wait-KitObsExit $obsExe 30)
  $final = Get-KitLogText $cfg
  Set-Content -Encoding ASCII (Join-Path $bed "cost-run-$Label.log") $final
  $visible = @(Stop-KitWindowWatch)
  foreach ($s in @($script:sinks)) { try { if (!$s.HasExited) { $s.Kill() } } catch { } }
  $script:sinks = @()
  foreach ($id in $script:credIds) { try { [void][RWS.CostCred]::CredDeleteW("RedWardenStudios/Multistream/$id", 1, 0) } catch { } }
  $script:credIds = @()
  foreach ($k in $costEnv) { Remove-Item -Path "Env:\$k" -ErrorAction SilentlyContinue }
  $script:proc = $null

  $kc = @()
  $kc += New-KitCheck (!$exitedEarly) "OBS ran for the whole window"
  $kc += New-KitCheck ($streaming -and $stillStreaming) "streaming from the end of the warm-up to the end of the window (started=$streaming, no stop=$stillStreaming)"
  $kc += New-KitCheck ($sinkSec -ge ($windowSec * 0.9)) "the main receiver saw $sinkSec s of stream (window $windowSec s)"
  foreach ($i in 0..($destLogs.Count - 1)) { if ($destLogs.Count -gt 0) { $kc += New-KitCheck ($destSec[$i] -ge ($windowSec * 0.9)) "destination $($i + 1)'s receiver saw $($destSec[$i]) s of stream" } }
  $kitLoaded = ([regex]::Matches($final, '\[rws-stream-kit\] Red Warden Stream Kit loaded')).Count
  $kc += New-KitCheck ((($kitOn -and $kitLoaded -eq 1) -or (!$kitOn -and $kitLoaded -eq 0 -and $final -notmatch 'rws-stream-kit'))) "Kit loaded exactly when it should be (Kit installed=$kitOn, load lines=$kitLoaded)"
  if ($kitOn) {
    foreach ($t in "multistream", "preflight", "inflight") {
      $rx = switch ($t) { "multistream" { '\[rws-multistream\] Red Warden Multistream loaded' } "preflight" { '\[rws-preflight\] loaded version' } default { '\[rws-inflight\] loaded \(version' } }
      $kc += New-KitCheck (([regex]::Matches($final, $rx)).Count -eq 1) "$t tool loaded"
    }
  }
  if ($destIds.Count -gt 0) {
    $liveN = ([regex]::Matches($final, '(?m)\[rws-multistream\] \[Cost dest \d\] live\r?$')).Count
    $kc += New-KitCheck ($liveN -ge 2) "both Multistream destinations went live (x$liveN)"
  } elseif ($kitOn) {
    $kc += New-KitCheck (([regex]::Matches($final, '\[rws-multistream\] \[Cost dest')).Count -eq 0) "no Multistream destination in this run (main stream only)"
  }
  $kc += Test-KitHidden $visible
  $kc += New-KitCheck ($closed -and $proc.HasExited -and $proc.ExitCode -eq 0) "OBS exited cleanly on WM_CLOSE (exit code $(if ($proc.HasExited) { $proc.ExitCode } else { 'still running' }))"

  $costInfo = $null
  if ($kitOn) {
    $batches = Get-CostBatches $final
    $costInfo = Get-CostRows $batches $t0tod $t1tod
    $kc += New-KitCheck ($costInfo -ne $null) "the 'kit cost:' counters were logged (batches: $($batches.Count)) and span the window"
    if ($costInfo) {
      $r = @{}; foreach ($x in $costInfo.Rows) { $r[$x.Name] = $x }
      $kc += New-KitCheck ($r["inflight-tick"].Calls -gt 0 -and $r["preflight-tick"].Calls -gt 0 -and $r["multistream-stats"].Calls -gt 0 -and $r["kit-tick"].Calls -gt 0) "the 1 Hz timers of all four tools ran in the window"
      $kc += New-KitCheck ($r["volmeter-watch"].Calls -gt 0) "In-Flight's volmeter callbacks ran (x$($r['volmeter-watch'].Calls))"
      if ($levelsOpen) {
        $kc += New-KitCheck ($r["levels-fast"].Calls -gt 0 -and $r["lufs-capture"].Calls -gt 0 -and $r["lufs-worker"].Calls -gt 0) "Levels was really running in LUFS mode: fast timer x$($r['levels-fast'].Calls), capture callbacks x$($r['lufs-capture'].Calls), worker passes x$($r['lufs-worker'].Calls)"
      } else {
        $kc += New-KitCheck ($r["levels-fast"].Calls -eq 0 -and $r["lufs-capture"].Calls -eq 0) "Levels was closed: no Levels timer or capture callback ran"
      }
    }
  }
  return [pscustomobject]@{
    Label = $Label; Kind = $Kind; Cpu = $cpu; Wall = $wall; Pct = $(if ($wall -gt 0) { $cpu / $wall * 100 } else { 0.0 })
    Cost = $costInfo; Checks = $kc; Log = $final
  }
}

# ---- the sequence ---------------------------------------------------------------------------------------------------------------
New-Item -ItemType Directory -Force (Join-Path $bed "received-cost") | Out-Null
$results = @()
$count = @{}
foreach ($kind in $seq) {
  if (@("A", "B", "C", "D") -notcontains $kind) { Finish $false @("unknown run '$kind' in -Runs") }
  $count[$kind] = 1 + [int]$count[$kind]
  $label = "$kind$($count[$kind])"
  $res = Invoke-CostRun $label $kind
  $results += $res
  $ok = Test-KitChecksOk $res.Checks
  Progress "[$label] done: $(Fmt $res.Pct) % of one core ($(Fmt $res.Cpu) s / $(Fmt $res.Wall 1) s); checks ok=$ok"
  Start-Sleep -Seconds 3
}
Cleanup-Run

# ---- the report -----------------------------------------------------------------------------------------------------------------
function Runs-Of([string]$k) { @($results | Where-Object { $_.Kind -eq $k }) }
function Mean-Pct($rs) { if ($rs.Count -eq 0) { return $null }; return (($rs | Measure-Object -Property Pct -Average).Average) }
$lines = @()
$lines += "Red Warden Stream Kit - CPU cost while streaming (window $Minutes min per run after $WarmupSeconds s warm-up; sequence $($seq -join ','); started $(Get-Date -Format 's'))"
$lines += "stream: 640x360 30 fps, x264 ultrafast, 800 kbps CBR, noise Mic + tone Desktop Audio (looping WAVs); machine: $((Get-CimInstance Win32_Processor | Select-Object -First 1).Name), $([Environment]::ProcessorCount) logical CPUs"
$lines += ""
$lines += "--- process-level: obs64 total CPU time over the window ---"
$lines += ("{0,-4} {1,-44} {2,10} {3,10} {4,12}" -f "run", "case", "cpu_s", "wall_s", "% of 1 core")
$caseName = @{ A = "Kit not installed (main stream)"; B = "Kit, all tools, Levels closed (main stream)"; C = "Kit, all tools, Levels open LUFS (main stream)"; D = "Kit, all tools, Levels closed, 2 destinations" }
foreach ($r in $results) { $lines += ("{0,-4} {1,-44} {2,10} {3,10} {4,12}" -f $r.Label, $caseName[$r.Kind], (Fmt $r.Cpu 2), (Fmt $r.Wall 1), (Fmt $r.Pct 3)) }
$A = Runs-Of "A"; $B = Runs-Of "B"; $C = Runs-Of "C"; $D = Runs-Of "D"
$aM = Mean-Pct $A; $bM = Mean-Pct $B; $cM = Mean-Pct $C
$spread = @()
$lines += ""
if ($A.Count -ge 2) { $sA = [Math]::Abs($A[0].Pct - $A[1].Pct); $spread += $sA; $lines += "spread of the repeated A: |A1-A2| = $(Fmt $sA 3) % of one core (A1 $(Fmt $A[0].Pct 3), A2 $(Fmt $A[1].Pct 3))" }
if ($B.Count -ge 2) { $sB = [Math]::Abs($B[0].Pct - $B[1].Pct); $spread += $sB; $lines += "spread of the repeated B: |B1-B2| = $(Fmt $sB 3) % of one core (B1 $(Fmt $B[0].Pct 3), B2 $(Fmt $B[1].Pct 3))" }
if ($aM -ne $null) { $lines += "A mean = $(Fmt $aM 3) %" }
if ($aM -ne $null -and $bM -ne $null) { $lines += "B-A = $(Fmt ($bM - $aM) 3) % of one core (B mean $(Fmt $bM 3) minus A mean $(Fmt $aM 3); first pair only: $(Fmt ($B[0].Pct - $A[0].Pct) 3))" }
if ($aM -ne $null -and $cM -ne $null) { $lines += "C-A = $(Fmt ($cM - $aM) 3) % of one core (C $(Fmt $cM 3) minus A mean $(Fmt $aM 3))" }
if ($D.Count -ge 1) { $lines += "D (two Multistream destinations, Levels closed) = $(Fmt (Mean-Pct $D) 3) % of one core, reported as it is (not subtracted: it also carries the destinations' own outputs and network)" }
$noise = if ($spread.Count -gt 0) { ($spread | Measure-Object -Maximum).Maximum } else { $null }
$pv = "process-level verdict (informational only): "
if ($noise -eq $null -or $aM -eq $null -or $bM -eq $null) {
  $pv += "not enough runs for an A/B (needs A, B and the repeats)"
} else {
  $parts = @()
  $dB = $bM - $aM
  $parts += "B-A $(Fmt $dB 3) % is " + $(if ([Math]::Abs($dB) -le $noise) { "INSIDE the noise (spread $(Fmt $noise 3) %)" } else { "outside the noise (spread $(Fmt $noise 3) %)" }) + $(if ($dB -lt 0.5) { ", under the 0.5 % target" } else { ", NOT under the 0.5 % target" })
  if ($cM -ne $null) {
    $dC = $cM - $aM
    $parts += "C-A $(Fmt $dC 3) % is " + $(if ([Math]::Abs($dC) -le $noise) { "INSIDE the noise" } else { "outside the noise" }) + $(if ($dC -lt 0.5) { ", under the 0.5 % target" } else { ", NOT under the 0.5 % target" })
  }
  $pv += ($parts -join "; ")
  if ([Math]::Abs($dB) -le $noise -or ($cM -ne $null -and [Math]::Abs($cM - $aM) -le $noise)) { $pv += ". Where a difference is inside the noise, this A/B cannot resolve it: the in-process counters below are the measurement." }
}
$lines += $pv

function Add-CounterTable([string]$Title, $Res) {
  $script:lines += ""
  if (!$Res -or !$Res.Cost) { $script:lines += "--- $Title : no counters ---"; return }
  $ci = $Res.Cost
  $script:lines += "--- $Title : in-process counters, run $($Res.Label), interval $(Fmt $ci.Interval 1) s (log batches $($ci.Batch0 + 1) to $($ci.Batch1 + 1) of $($ci.Batches)) ---"
  $script:lines += ("{0,-18} {1,12} {2,12} {3,10} {4,12} {5,13}" -f "counter", "calls", "avg us/call", "calls/s", "max us (run)", "% of 1 core")
  foreach ($x in $ci.Rows) { $script:lines += ("{0,-18} {1,12} {2,12} {3,10} {4,12} {5,13}" -f $x.Name, [int64]$x.Calls, (Fmt $x.AvgUs 2), (Fmt $x.CallsPerSec 2), [int64]$x.MaxUs, (Fmt $x.Pct 5)) }
  $script:lines += ("{0,-18} {1,12} {2,12} {3,10} {4,12} {5,13}" -f "TOTAL", "", "", "", "", (Fmt $ci.Total 5))
}
$cRes = $C | Select-Object -First 1
Add-CounterTable "case C (Levels open, LUFS)" $cRes
$bRes = $B | Select-Object -First 1
Add-CounterTable "case B (Levels closed)" $bRes
if ($B.Count -ge 2) { Add-CounterTable "case B repeat" $B[1] }
Add-CounterTable "case D (two destinations)" ($D | Select-Object -First 1)

$lines += ""
$lines += "--- run checks ---"
$allOk = $true
foreach ($r in $results) {
  foreach ($c in @($r.Checks)) {
    $lines += $(if ($c.Ok) { "ok:   [$($r.Label)] $($c.Msg)" } else { "FAIL: [$($r.Label)] $($c.Msg)" })
    if (!$c.Ok) { $allOk = $false }
  }
}
$lines += ""
$inproc = $null
if ($cRes -and $cRes.Cost) { $inproc = $cRes.Cost.Total }
if ($inproc -eq $null) {
  $lines += "in-process total (case C): not available (no C run with counters)"
  $verdictOk = $false
} else {
  $bTot = if ($bRes -and $bRes.Cost) { "; case B: $(Fmt $bRes.Cost.Total 5) %" } else { "" }
  $lines += "in-process total (sum of all counters, case C): $(Fmt $inproc 5) % of one core (target < 0.5 %)$bTot"
  $verdictOk = ($inproc -lt 0.5)
}
Finish ($verdictOk -and $allOk) $lines

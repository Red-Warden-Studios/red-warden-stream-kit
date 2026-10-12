<#
  Red Warden Stream Kit exit-stress test: closes the portable OBS N times in a row with all three tools live, and
  requires every exit to be clean.

  Why: one run of the Multistream harness crashed at OBS exit with 0xC0000374 (heap corruption, ntdll)
  and did not reproduce in 20 reruns. This loop exists to catch it (and anything like it) in the merged DLL.

  Each cycle (default 20):
    - fresh throwaway config; ONE plugin DLL (rws-stream-kit.dll, old DLLs removed); three docks registered
      (Multistream, Pre-Flight, In-Flight); a scene with a color source and two audio sources (In-Flight's Tone and
      Hush fixtures, reused from the In-Flight folder's tools\testbed-common.ps1); Pre-Flight and In-Flight set up;
      Pre-Flight's test hammer on (RWS_PREFLIGHT_TEST_HAMMER: it asks Multistream, in-process, on every tick up to exit).
    - cycles 1-10 ("busy"): In-Flight's Levels panel is opened by its test hook with meters and the LUFS worker
      running (RWS_INFLIGHT_TEST_LEVELS + _LEVELS_GATE + _LUFS, force-live), and a short LOCAL stream runs: OBS's main
      stream plus one Multistream destination, each into a local ffmpeg RTMP receiver (the same way test-local.ps1
      does it). Cycles 11-20 ("idle"): docks only.
    - waits until the Kit load line is in the log (and, in busy cycles, until the stream started), a few seconds more
      (2-5, varying per cycle), then posts WM_CLOSE (File > Exit) and requires exit code 0 within 20 s.
  Every exit code is recorded. A crash-dump folder is configured for obs64.exe (Windows LocalDumps):
  <plugin>\.testbed\crashdumps\ (git-ignored; point LocalDumps there). On every non-zero exit (or hang, or new .dmp) the cycle's OBS log is copied
  there BEFORE the next cycle wipes the config: <dump>.log beside a new dump, else exitstress-cycleNN-exit<code>.log.
  Only OBS processes this script started are ever touched (captured PID); receivers by their captured PIDs.
  -ProcDump: opt-in. ProcDump attaches to the OBS pid (child of nobody: OBS is still started hidden by Start-KitObs, so the
  WM_CLOSE path and the visible-window watch keep seeing the real OBS pid) with
    procdump64.exe -accepteula -ma -e -f C0000374,C0000409 -n 1 <pid> <dumpdir>\obs64_cycleNN_<pid>_EXCEPTIONCODE_YYMMDD_HHMMSS.dmp
  -e (WITHOUT the 1) = unhandled (second-chance) exceptions only; -f = only those two codes (heap corruption, fail-fast).
  Ordinary first-chance exceptions never dump. Output: console table + .testbed\exit-stress-table.txt; "RESULT: PASS|FAIL"; exit code 0 / 1.
#>
param(
  [string]$Config = "RelWithDebInfo",
  [int]$Cycles = 20,
  [int]$BusyCycles = 10,
  [int]$ExitLimitSeconds = 20,
  # Demo switches (only to prove the assertions can fail): -NoStream runs the "busy" cycles WITHOUT starting the stream
  # (the cycle must FAIL: no Streaming Start / Multistream output); -DemoSkipClose ends OBS WITHOUT posting WM_CLOSE (the
  # close-path assertion must FAIL).
  [switch]$NoStream,
  [switch]$DemoSkipClose,
  # -ProcDump: attach Sysinternals ProcDump (plugin\.testbed\tools\procdump\procdump64.exe) to each cycle's OBS (by the OBS
  # process id, right after it is started hidden) so a heap-corruption / fail-fast exception writes a FULL dump to
  # <plugin>\.testbed\crashdumps\ (the cycle's OBS log is copied beside it as <dump>.log).
  [switch]$ProcDump
)
$ErrorActionPreference = "Stop"
$pluginRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $PSScriptRoot "kit-testbed.ps1")
. (Join-Path (Get-KitToolDir "inflight") "tools\testbed-common.ps1")      # New-SineWav / New-SilentWav (the In-Flight fixtures)
$kit  = Get-KitTestbed $Config
$bed  = $kit.Bed
$obs  = $kit.Obs
$cfg  = $kit.Cfg
$prof = Join-Path $cfg "basic\profiles\ExitStress"
$dumps = $kit.Dumps
New-Item -ItemType Directory -Force $dumps | Out-Null
$table = Join-Path $bed "exit-stress-table.txt"
$procdumpExe = Join-Path $bed "tools\procdump\procdump64.exe"
trap { Write-Host "FAIL: $_"; Write-Host "RESULT: FAIL"; exit 1 }

if ($ProcDump -and !(Test-Path $procdumpExe)) { throw "-ProcDump needs $procdumpExe (see the header)" }
if (!(Test-Path $kit.Dll)) { throw "rws-stream-kit.dll not found: $($kit.Dll)" }
Initialize-KitPortableObs $obs
if (!(Wait-KitObsExit (Join-Path $obs "bin\64bit\obs64.exe") 30)) { throw "a testbed obs64.exe ($obs) is still running; not touching it" }
Install-KitPlugin $kit | ForEach-Object { Write-Host $_ }

# fixtures: the In-Flight tone and silence sources
$toneWav = Join-Path $bed "tone-1k-20db.wav"
$hushWav = Join-Path $bed "silence.wav"
New-SineWav $toneWav 2.0
New-SilentWav $hushWav 2.0
$toneJson = $toneWav.Replace('\', '/')
$hushJson = $hushWav.Replace('\', '/')
$toneUuid = "b0000000-0000-0000-0000-000000000003"
$hushUuid = "b0000000-0000-0000-0000-000000000004"
$sceneUuid = "b0000000-0000-0000-0000-000000000001"

# Multistream stream key in Credential Manager, written the way the plugin writes it (as test-local.ps1 does)
Add-Type -Namespace RWS -Name Cred -MemberDefinition @'
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct CREDENTIAL { public int Flags; public int Type; public string TargetName; public string Comment;
  public long LastWritten; public int CredentialBlobSize; public System.IntPtr CredentialBlob; public int Persist;
  public int AttributeCount; public System.IntPtr Attributes; public string TargetAlias; public string UserName; }
[DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool CredWriteW(ref CREDENTIAL c, int flags);
[DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool CredDeleteW(string target, int type, int flags);
'@ -ErrorAction SilentlyContinue
# run-unique Credential Manager id: this run writes (and deletes) exactly one target, RedWardenStudios/Multistream/<this id>
$destId = "00000000-beac-0000-0000-" + ("{0:x12}" -f (Get-Random -Maximum 281474976710655))
$keyWritten = $false
function Set-Key($id, $key) {
  $bytes = [Text.Encoding]::UTF8.GetBytes($key)
  $ptr = [Runtime.InteropServices.Marshal]::AllocHGlobal($bytes.Length)
  [Runtime.InteropServices.Marshal]::Copy($bytes, 0, $ptr, $bytes.Length)
  $c = New-Object RWS.Cred+CREDENTIAL
  $c.Type = 1; $c.TargetName = "RedWardenStudios/Multistream/$id"; $c.UserName = "stream-key"
  $c.CredentialBlobSize = $bytes.Length; $c.CredentialBlob = $ptr; $c.Persist = 2
  if (![RWS.Cred]::CredWriteW([ref]$c, 0)) { throw "CredWrite failed" }
  [Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
}

function Write-CycleConfig([bool]$busy, [int]$mainPort, [int]$msPort) {
  New-Item -ItemType Directory -Force $prof | Out-Null
  New-Item -ItemType Directory -Force (Join-Path $cfg "basic\scenes") | Out-Null
  $ini = @"
[General]
FirstRun=false
LastVersion=999999999
EnableAutoUpdates=false
ConfirmOnExit=false
[Basic]
Profile=ExitStress
ProfileDir=ExitStress
SceneCollection=ExitStress
SceneCollectionFile=ExitStress
[BasicWindow]
ConfirmOnExit=false
WarnBeforeStartingStream=false
WarnBeforeStoppingStream=false
"@
  Set-Content (Join-Path $cfg "global.ini") $ini -Encoding UTF8
  Set-Content (Join-Path $cfg "user.ini") $ini -Encoding UTF8
  # obs-websocket server off (never clash with a real OBS on the usual port); the in-process vendor API still works
  $wsDir = Join-Path $cfg "plugin_config\obs-websocket"
  New-Item -ItemType Directory -Force $wsDir | Out-Null
  Set-Content (Join-Path $wsDir "config.json") '{"alerts_enabled":false,"auth_required":false,"first_load":false,"server_enabled":false,"server_password":"","server_port":4466}' -Encoding UTF8
  Set-Content (Join-Path $prof "basic.ini") @"
[General]
Name=ExitStress
[Output]
Mode=Advanced
[AdvOut]
Encoder=obs_x264
TrackIndex=1
Track1Bitrate=128
ApplyServiceSettings=true
[Video]
BaseCX=1280
BaseCY=720
OutputCX=1280
OutputCY=720
FPSType=0
FPSCommon=30
"@ -Encoding UTF8
  Set-Content (Join-Path $prof "streamEncoder.json") '{"rate_control":"CBR","bitrate":1000,"keyint_sec":2,"preset":"veryfast"}' -Encoding UTF8
  Set-Content (Join-Path $prof "service.json") ('{"type":"rtmp_custom","settings":{"server":"rtmp://127.0.0.1:' + $mainPort + '/live","key":"k","use_auth":false}}') -Encoding UTF8
  $ffTone = '{"is_local_file":true,"local_file":"' + $toneJson + '","looping":true,"restart_on_activate":true}'
  $ffHush = '{"is_local_file":true,"local_file":"' + $hushJson + '","looping":true,"restart_on_activate":true}'
  $scene = @"
{"name":"ExitStress","current_scene":"Scene","current_program_scene":"Scene",
 "scene_order":[{"name":"Scene"}],
 "sources":[
  {"id":"color_source_v3","versioned_id":"color_source_v3","name":"Ember","uuid":"b0000000-0000-0000-0000-000000000002","settings":{"color":4282038770,"width":1280,"height":720}},
  {"id":"ffmpeg_source","versioned_id":"ffmpeg_source","name":"Tone","uuid":"$toneUuid","mixers":255,"volume":1.0,"muted":false,"enabled":true,"settings":$ffTone},
  {"id":"ffmpeg_source","versioned_id":"ffmpeg_source","name":"Hush","uuid":"$hushUuid","mixers":255,"volume":1.0,"muted":false,"enabled":true,"settings":$ffHush},
  {"id":"scene","versioned_id":"scene","name":"Scene","uuid":"$sceneUuid","settings":{"id_counter":3,"items":[
    {"name":"Ember","source_uuid":"b0000000-0000-0000-0000-000000000002","id":1,"visible":true},
    {"name":"Tone","source_uuid":"$toneUuid","id":2,"visible":true},
    {"name":"Hush","source_uuid":"$hushUuid","id":3,"visible":true}]}}
 ]}
"@
  [IO.File]::WriteAllText((Join-Path $cfg "basic\scenes\ExitStress.json"), $scene, (New-Object Text.UTF8Encoding($false)))
  # the three tools set up (their docks show live content, not a setup card)
  $pf = '{"setup_done":true,"use_main_stream":true,"use_multistream":true,"mic_uuid":"' + $toneUuid + '","start_scene_uuid":"' + $sceneUuid + '","record_intent":false,"check_updates":false}'
  $if = '{"setup_done":true,"mic_uuid":"' + $toneUuid + '","desktop_uuid":"' + $hushUuid + '","check_updates":false,"levels_open":true,"levels_mode":"lufs","levels_true_peak":true}'
  foreach ($pair in @(@("rws-preflight", $pf), @("rws-inflight", $if), @("rws-multistream", '{"upload_mbps":0.0,"upload_source":"","upload_when":"","check_updates":false}'))) {
    $d = Join-Path $cfg ("plugin_config\" + $pair[0])
    New-Item -ItemType Directory -Force $d | Out-Null
    [IO.File]::WriteAllText((Join-Path $d "settings.json"), $pair[1], (New-Object Text.UTF8Encoding($false)))
  }
  Write-KitSeedSettings $cfg @("kit")
  $dest = '{"version":2,"destinations":[{"id":"' + $destId + '","name":"Stress dest","platform":"custom","server":"rtmp://127.0.0.1:' + $msPort + '/live","enabled":' + $(if ($busy) { 'true' } else { 'false' }) + ',"encoder":{"shared":true,"encoder_id":"","video_bitrate":1000,"audio_bitrate":0,"audio_track":-1,"width":0,"height":0},"hotkey":[]}]}'
  [IO.File]::WriteAllText((Join-Path $prof "rws-multistream.json"), $dest, (New-Object Text.UTF8Encoding($false)))
}

function Start-Receiver([int]$port) {
  Start-Process ffmpeg -PassThru -WindowStyle Hidden -ArgumentList @(
    "-hide_banner", "-loglevel", "error", "-y", "-listen", "1",
    "-i", "rtmp://127.0.0.1:$port/live/k", "-c", "copy", "-f", "null", "-")
}

New-Item -ItemType Directory -Force $dumps | Out-Null
$dumpsBefore = @(Get-ChildItem $dumps -Filter *.dmp -ErrorAction SilentlyContinue | ForEach-Object { $_.FullName })
$rows = @()
$allOk = $true

for ($n = 1; $n -le $Cycles; $n++) {
  $busy = ($n -le $BusyCycles)
  $mode = if ($busy) { "busy" } else { "idle" }
  $nn = "{0:D2}" -f $n
  $mainPort = 19450; $msPort = 19451
  if (!(Wait-KitObsExit (Join-Path $obs "bin\64bit\obs64.exe") 30)) { throw "a testbed obs64.exe ($obs) is still running before cycle $n; not touching it" }
  Clear-KitConfig $cfg
  Write-CycleConfig $busy $mainPort $msPort
  $receivers = @()
  $script:KitLastPosted = -1
  $envNames = @("RWS_PREFLIGHT_TEST", "RWS_PREFLIGHT_TEST_HAMMER")
  $env:RWS_PREFLIGHT_TEST = "1"; $env:RWS_PREFLIGHT_TEST_HAMMER = "1"
  $extra = @()
  if ($busy) {
    foreach ($e in "RWS_INFLIGHT_TEST", "RWS_INFLIGHT_TEST_FORCE_LIVE", "RWS_INFLIGHT_TEST_LEVELS", "RWS_INFLIGHT_TEST_LEVELS_GATE", "RWS_INFLIGHT_TEST_LUFS") {
      Set-Item "Env:$e" "1"; $envNames += $e
    }
    Set-Key $destId "k"; $keyWritten = $true
    if (!$NoStream) {
      $receivers += Start-Receiver $mainPort
      $receivers += Start-Receiver $msPort
      Start-Sleep -Seconds 2
      $extra = @("--startstreaming")
    }
  }
  $note = @()
  $code = $null
  $secs = -1.0
  $exited = $false
  $proc = $null
  $pd = $null
  $logCopy = ""
  try {
    $proc = Start-KitObs $obs "ExitStress" "ExitStress" $extra
    if ($ProcDump) {
      $dmpName = "obs64_cycle${nn}_$($proc.Id)_EXCEPTIONCODE_YYMMDD_HHMMSS.dmp"
      $pdLog = Join-Path $bed "procdump-cycle$nn.txt"
      $pd = Start-Process $procdumpExe -PassThru -WindowStyle Hidden -RedirectStandardOutput $pdLog -ArgumentList @(
        "-accepteula", "-ma", "-e", "-f", "C0000374,C0000409", "-n", "1", "$($proc.Id)", ("`"" + (Join-Path $dumps $dmpName) + "`""))   # Start-Process does not quote: the path has spaces
    }
    # wait until the Kit load line is in the log (up to 40 s), and in busy cycles until the stream started (up to 30 s)
    $loaded = $false
    $deadline = (Get-Date).AddSeconds(40)
    while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
      Start-Sleep -Milliseconds 500
      $t = Get-KitLogText $cfg
      if ($t -match 'Red Warden Stream Kit loaded') { $loaded = $true; break }
    }
    if (!$loaded) { $note += "Kit load line never appeared" }
    $streamed = $false
    if ($busy -and $loaded) {
      $deadline = (Get-Date).AddSeconds(30)
      while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
        $t = Get-KitLogText $cfg
        if ($t -match 'Streaming Start') { $streamed = $true; break }
        Start-Sleep -Milliseconds 500
      }
      if (!$streamed) { $note += "stream never started" }
    }
    if ($busy -and $loaded) {
      # the Levels panel + LUFS worker hooks log their readings ~14 s after the meters start: wait for them (up to 45 s) so the close lands while
      # the loudness worker and the meters are really running, with readings logged
      $deadline = (Get-Date).AddSeconds(45)
      while ((Get-Date) -lt $deadline -and -not $proc.HasExited) {
        if ((Get-KitLogText $cfg) -match '\[rws-inflight\] lufs: Tone M=') { break }
        Start-Sleep -Milliseconds 500
      }
    }
    Start-Sleep -Seconds (2 + ($n % 4))   # 2..5 s of everything running before the close
    $sw = [Diagnostics.Stopwatch]::StartNew()
    $posted = 0
    if ($DemoSkipClose) {
      $note += "DEMO: WM_CLOSE deliberately not posted; OBS stopped by pid"
      Start-Sleep -Seconds 1; $proc.Kill()
    } else {
      for ($i = 0; $i -lt 10 -and $posted -eq 0 -and -not $proc.HasExited; $i++) {
        $posted = Send-KitWmClose $proc
        if ($posted -eq 0) { Start-Sleep -Seconds 1 }
      }
    }
    # Wait for the exit; if a window becomes visible meanwhile (a "really exit?" dialog of a hidden OBS would sit on a
    # screen nobody is looking at, and nothing may pop up on the user's screen) stop waiting and stop this pid at once.
    $exited = $false
    $dlg = ""
    while ($sw.Elapsed.TotalSeconds -lt $ExitLimitSeconds) {
      if ($proc.WaitForExit(500)) { $exited = $true; break }
      $vw = @(Get-KitVisibleWindows $proc)
      if ($vw.Count -gt 0) { $dlg = $vw -join ", "; break }
    }
    $secs = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    if ($exited) { $code = $proc.ExitCode } else {
      $all = @(); try { $all = @(Get-KitVisibleWindows $proc) } catch { }
      $note += $(if ($dlg) { "OBS did not exit: a window became visible after WM_CLOSE ($dlg); stopped by pid" } else { "did not exit within ${ExitLimitSeconds}s; stopped by pid" })
      $proc.Kill(); [void]$proc.WaitForExit(10000); $code = "hang"
    }
  } finally {
    if ($pd) { try { if (-not $pd.WaitForExit(15000)) { $pd.Kill() } } catch { } }   # ProcDump ends with its target; this pid only
    foreach ($r in $receivers) { try { if ($r -and -not $r.HasExited) { $r.Kill() } } catch { } }
    if ($keyWritten) { [void][RWS.Cred]::CredDeleteW("RedWardenStudios/Multistream/$destId", 1, 0); $keyWritten = $false }
    foreach ($e in $envNames) { Remove-Item "Env:$e" -ErrorAction SilentlyContinue }
  }
  [void](Wait-KitObsExit (Join-Path $obs "bin\64bit\obs64.exe") 20)
  Start-Sleep -Seconds 2   # let WER finish writing a dump, if any
  $final = Get-KitLogText $cfg
  # Evidence this cycle really ran its workload. Missing evidence FAILS the cycle (it is not a note).
  $watched = @(Stop-KitWindowWatch)
  $kitLine = [bool]($final -match '(?m)^\d\d:\d\d:\d\d\.\d+: \[rws-stream-kit\] Red Warden Stream Kit loaded \(version ')
  $evReq = [ordered]@{ "Kit load line" = $kitLine }
  if ($busy) {
    $evReq["stream started ('==== Streaming Start')"] = [bool]($final -match '==== Streaming Start')
    $evReq["Multistream output started ('[Stress dest] live')"] = [bool]($final -match '\[rws-multistream\] \[Stress dest\] live')
    $evReq["Levels meters attached ('levels: meter attached (2 now)')"] = [bool]($final -match '\[rws-inflight\] levels: meter attached \(2 now\)')
    $evReq["Levels readings logged ('levels: Tone peak=')"] = [bool]($final -match '\[rws-inflight\] levels: Tone peak=')
    $evReq["LUFS capture started ('levels: lufs capture started (2 sources')"] = [bool]($final -match '\[rws-inflight\] levels: lufs capture started \(2 sources')
    $evReq["LUFS worker readings logged ('lufs: Tone M=')"] = [bool]($final -match '\[rws-inflight\] lufs: Tone M=')
  }
  $missing = @($evReq.Keys | Where-Object { -not $evReq[$_] })
  if ($missing.Count -gt 0) { $note += "MISSING EVIDENCE: " + ($missing -join "; ") }
  $ev = "evidence(" + ($evReq.Keys.Count - $missing.Count) + "/" + $evReq.Keys.Count + ")"
  $note = @($ev) + $note
  # load checks of this cycle (all three tools once, dock ids saved at exit): a clean exit with a broken load is no pass
  $kc = @(Test-KitLoadLog $final $cfg @("multistream", "preflight", "inflight"))
  $badKc = @($kc | Where-Object { -not $_.Ok } | ForEach-Object { $_.Msg })
  if ($badKc.Count -gt 0) { $note += "load checks failed: " + ($badKc -join " | ") }
  $newDumps = @(Get-ChildItem $dumps -Filter *.dmp -ErrorAction SilentlyContinue | Where-Object { $dumpsBefore -notcontains $_.FullName })
  $crashFiles = @(Get-ChildItem (Join-Path $cfg "crashes") -ErrorAction SilentlyContinue)
  $closePathOk = ($script:KitLastPosted -gt 0) -and -not $DemoSkipClose
  if (!$closePathOk) { $note += "close path: WM_CLOSE posted to $(if ($DemoSkipClose) { 0 } else { $script:KitLastPosted }) OBS window(s) (must be at least 1)" }
  # Any visible window of the OBS process fails the cycle (no exemption).
  $otherWin = @($watched)
  if ($otherWin.Count -gt 0) { $note += "visible window(s) seen: " + ($otherWin -join ", ") }
  $cycleOk = ($missing.Count -eq 0) -and $closePathOk -and ($otherWin.Count -eq 0) -and ($code -eq 0) -and ($badKc.Count -eq 0) -and ($newDumps.Count -eq 0) -and ($crashFiles.Count -eq 0)
  if (!$cycleOk) {
    # Preserve this cycle's OBS log before the next cycle wipes the config.
    $lf = Get-KitLogFile $cfg
    if ($lf) {
      if ($newDumps.Count -gt 0) {
        foreach ($d in $newDumps) { Copy-Item $lf.FullName ($d.FullName + ".log") -Force; $logCopy += ($d.FullName + ".log ") }
      } else {
        $dest = Join-Path $dumps ("exitstress-cycle$nn-exit$code.log")
        Copy-Item $lf.FullName $dest -Force; $logCopy = $dest
      }
    }
    foreach ($cf in $crashFiles) { Copy-Item $cf.FullName (Join-Path $dumps ("exitstress-cycle$nn-" + $cf.Name)) -Force }
    $allOk = $false
  }
  $dumpsBefore += @($newDumps | ForEach-Object { $_.FullName })
  $hex = if ($code -is [int]) { "0x{0:X8}" -f $code } else { "$code" }
  $rows += [pscustomobject]@{ Cycle = $n; Mode = $mode; Exit = "$code ($hex)"; Secs = $secs; Dump = $(if ($newDumps.Count -gt 0) { ($newDumps | ForEach-Object { $_.Name }) -join "," } else { "none" }); Ok = $cycleOk; Note = (($note + $(if ($logCopy) { "log kept: $logCopy" })) -join "; ") }
  Write-Host ("cycle {0,2} {1,-4} exit={2} secs={3} dump={4} ok={5} {6}" -f $n, $mode, "$code ($hex)", $secs, $rows[-1].Dump, $cycleOk, $rows[-1].Note)
}

$out = @("cycle | mode | exit code | exit secs | new dump | ok | note")
foreach ($r in $rows) { $out += ("{0,5} | {1,-4} | {2} | {3} | {4} | {5} | {6}" -f $r.Cycle, $r.Mode, $r.Exit, $r.Secs, $r.Dump, $r.Ok, $r.Note) }
$zero = @($rows | Where-Object { $_.Exit -like "0 *" }).Count
$out += "exit code 0 in $zero of $Cycles cycles; new dumps: $(@($rows | Where-Object { $_.Dump -ne 'none' }).Count)"
$out | Set-Content -Encoding ASCII $table
Write-Host "--- exit-stress table (also in $table) ---"
$out | ForEach-Object { Write-Host $_ }
if ($allOk) { Write-Host "RESULT: PASS" } else { Write-Host "RESULT: FAIL" }
exit $(if ($allOk) { 0 } else { 1 })

<#
  Red Warden Stream Kit - the Network light judges EVERY outgoing stream. ASCII only.

  Touches none of your real OBS. Uses the Kit testbed (plugin\.testbed\obs, a portable OBS copy) and its single DLL, started
  hidden, closed with WM_CLOSE to the captured PID only. One stream, three outgoing streams:
    main stream  -> ffmpeg receiver on 19360                       (healthy)
    YouTube test -> ffmpeg receiver on 19361, shared encoder       (healthy)
    Twitch test  -> a throttling relay (this script) on 19362 -> ffmpeg receiver on 19363. While the flag file exists the
                    relay lets only ~60 KB/s through, so OBS cannot send the destination's ~190 KB/s and drops its frames
                    (a congested uplink to one platform). The main stream and YouTube are untouched.
  The harness sets RWS_INFLIGHT_TEST_NET: In-Flight's test hook waits for the Network light to name a bad destination for
  three ticks, opens the Network details, asks the Kit dock for an offscreen grab (QWidget::grab, nothing is shown), and 2.5 s
  later presses the REAL "Turn off <destination>" button once. Asserted from the OBS log:
    1. the Network light goes RED naming "Twitch test" (evaluator log line light=R, pct30 over 2.0; the panel's own shown state
       is logged by the test hook at the grab and asserted, no pixel colour), and in the same line
       the main stream and YouTube are listed as fine; the per-output views carry the three figures
    2. Multistream's row badge was set ("drop badge: Twitch test=...")
    3. the details showed the button "Turn off Twitch test" and it was clicked; the Twitch output stopped (and only that one)
    4. the Network light is green again within 5 s of the click; the incident ended "turned off"
    5. the destination switched back on mid-stream (obs-websocket SetDestination) reconnects (second "live"), is measured again
       (its view goes U then G) and is not red again; the healthy outputs kept streaming throughout
    6. the Kit dock grab kit-dock-net-red.png exists and shows the red light and the amber badge
    7. the Kit load checks, a hidden launch, a clean exit
  Not covered here: the amber/red boundary (0.5 .. 2.0 %). OBS's frame dropping under a throttled uplink is not proportional to
  the throttle (it drops in bursts once the buffer limit is hit), so a relay setting that reliably lands in 0.5..2 % does not
  exist; the boundary values are unit-checked in inflight-eval-test.cpp ("network exactly 0.5 %" / "exactly 2.0 %").
  Result: exit code 0 / 1 and a final "RESULT: PASS" / "RESULT: FAIL" line (run-all.ps1 -Steps net-light writes it to
  .testbed\run-net-light.txt). Needs ffmpeg and ffprobe on PATH (as the other Multistream harnesses do).
#>
param(
  [string]$Config = "RelWithDebInfo",
  [int]$Seconds = 130
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "kit-testbed.ps1")
$kit = Get-KitTestbed $Config
$bed = $kit.Bed
$obs = $kit.Obs
$cfg = $kit.Cfg
$prof = Join-Path $cfg "basic\profiles\NetLightTest"
$shots = Join-Path $bed "shots"
$obsExe = Join-Path $obs "bin\64bit\obs64.exe"
$dll = $kit.Dll

function Finish([bool]$ok, [string[]]$lines) {
  $lines | ForEach-Object { Write-Host $_ }
  if ($ok) { Write-Host "RESULT: PASS" } else { Write-Host "RESULT: FAIL" }
  exit $(if ($ok) { 0 } else { 1 })
}
$script:credIds = @()
$script:sinks = @{}
$script:relayUp = $false
function Cleanup-Run {
  foreach ($s in @($script:sinks.Values)) { try { if (!$s.HasExited) { $s.Kill() } } catch { } }
  if ($script:relayUp) { try { [RwsThrottle]::Stop() } catch { } }
  foreach ($id in $script:credIds) { try { [void][RWS.NlCred]::CredDeleteW("RedWardenStudios/Multistream/$id", 1, 0) } catch { } }
  Restore-KitObsWebsocket $kit.Obs
}
trap { Cleanup-Run; Write-Host "FAIL: $_"; Write-Host "RESULT: FAIL"; exit 1 }

if (!(Test-Path $dll)) { Finish $false @("rws-stream-kit.dll not found: $dll") }
Initialize-KitPortableObs $obs
if (!(Wait-KitObsExit $obsExe 30)) { Finish $false @("a testbed obs64.exe ($obs) is still running; not touching it") }
Install-KitPlugin $kit | ForEach-Object { Write-Host $_ }

# ---- the throttling relay (C# 5, Windows PowerShell 5.1) ---------------------------------------------------------------
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Threading;
public static class RwsThrottle {
  static TcpListener lst;
  static volatile bool stop;
  static string flag;
  static int kbPerSec;
  static int target;
  public static void Start(int listenPort, int targetPort, string flagPath, int kbps) {
    flag = flagPath; kbPerSec = kbps; target = targetPort; stop = false;
    lst = new TcpListener(IPAddress.Loopback, listenPort);
    lst.Start();
    Thread t = new Thread(Accept);
    t.IsBackground = true;
    t.Start();
  }
  public static void Stop() {
    stop = true;
    try { lst.Stop(); } catch (Exception) { }
  }
  static void Accept() {
    while (!stop) {
      TcpClient c;
      try { c = lst.AcceptTcpClient(); } catch (Exception) { break; }
      TcpClient cc = c;
      Thread h = new Thread(delegate() { Handle(cc); });
      h.IsBackground = true;
      h.Start();
    }
  }
  static void Handle(TcpClient c) {
    TcpClient t = new TcpClient();
    try { t.Connect("127.0.0.1", target); } catch (Exception) { try { c.Close(); } catch (Exception) { } return; }
    c.ReceiveBufferSize = 4096;
    t.SendBufferSize = 4096;
    NetworkStream cs = c.GetStream();
    NetworkStream ts = t.GetStream();
    Thread back = new Thread(delegate() { Pump(ts, cs, false); });
    back.IsBackground = true;
    back.Start();
    Pump(cs, ts, true);
    try { c.Close(); } catch (Exception) { }
    try { t.Close(); } catch (Exception) { }
  }
  static void Pump(Stream from, Stream to, bool throttle) {
    byte[] buf = new byte[2048];
    try {
      while (!stop) {
        int n = from.Read(buf, 0, buf.Length);
        if (n <= 0) break;
        if (throttle && File.Exists(flag)) Thread.Sleep((int)(1000L * n / (kbPerSec * 1024L)));
        to.Write(buf, 0, n);
      }
    } catch (Exception) { }
  }
}
'@
Add-Type -Namespace RWS -Name NlCred -MemberDefinition @'
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct CREDENTIAL { public int Flags; public int Type; public string TargetName; public string Comment;
  public long LastWritten; public int CredentialBlobSize; public System.IntPtr CredentialBlob; public int Persist;
  public int AttributeCount; public System.IntPtr Attributes; public string TargetAlias; public string UserName; }
[DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool CredWriteW(ref CREDENTIAL c, int flags);
[DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool CredDeleteW(string target, int type, int flags);
'@ -ErrorAction SilentlyContinue

# ---- throwaway config --------------------------------------------------------------------------------------------------
Clear-KitConfig $cfg
New-Item -ItemType Directory -Force $prof | Out-Null
New-Item -ItemType Directory -Force (Join-Path $cfg "basic\scenes") | Out-Null
$ini = @"
[General]
FirstRun=false
LastVersion=999999999
EnableAutoUpdates=false
[Basic]
Profile=NetLightTest
ProfileDir=NetLightTest
SceneCollection=NetLightTest
SceneCollectionFile=NetLightTest
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
Name=NetLightTest
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
Set-Content (Join-Path $prof "streamEncoder.json") '{"rate_control":"CBR","bitrate":1500,"keyint_sec":2,"preset":"veryfast"}' -Encoding UTF8
Set-Content (Join-Path $prof "service.json") '{"type":"rtmp_custom","settings":{"server":"rtmp://127.0.0.1:19360/live","key":"k","use_auth":false}}' -Encoding UTF8
Set-Content (Join-Path $cfg "basic\scenes\NetLightTest.json") @'
{"name":"NetLightTest","current_scene":"Scene","current_program_scene":"Scene",
 "scene_order":[{"name":"Scene"}],
 "sources":[
  {"id":"color_source_v3","versioned_id":"color_source_v3","name":"Ember","uuid":"b0000000-0000-0000-0000-000000000002","settings":{"color":4282038770,"width":1280,"height":720}},
  {"id":"scene","versioned_id":"scene","name":"Scene","uuid":"b0000000-0000-0000-0000-000000000001","settings":{"id_counter":1,"items":[{"name":"Ember","source_uuid":"b0000000-0000-0000-0000-000000000002","id":1,"visible":true}]}}
 ]}
'@ -Encoding UTF8

# Kit: no update check, first run done. In-Flight: set up (no mic/desktop chosen: those lights stay grey; the Network light is on by default).
$run = -join ((1..6) | ForEach-Object { '{0:x}' -f (Get-Random -Maximum 16) })
$idA = "nl${run}a"      # Twitch test (through the throttling relay)
$idB = "nl${run}b"      # YouTube test (healthy)
$script:credIds = @($idA, $idB)
$kd = Join-Path $cfg "plugin_config\rws-stream-kit"
New-Item -ItemType Directory -Force $kd | Out-Null
[IO.File]::WriteAllText((Join-Path $kd "settings.json"), '{"check_updates":false,"first_run_done":true}', (New-Object Text.UTF8Encoding($false)))
$idir = Join-Path $cfg "plugin_config\rws-inflight"
New-Item -ItemType Directory -Force $idir | Out-Null
[IO.File]::WriteAllText((Join-Path $idir "settings.json"), '{"setup_done":true}', (New-Object Text.UTF8Encoding($false)))
[IO.File]::WriteAllText((Join-Path $prof "rws-multistream.json"), (@"
{"version":1,"destinations":[
 {"id":"$idA","name":"Twitch test","platform":"twitch","server":"rtmp://127.0.0.1:19362/live","enabled":true,
  "encoder":{"shared":false,"encoder_id":"obs_x264","video_bitrate":1500,"width":1280,"height":720}},
 {"id":"$idB","name":"YouTube test","platform":"youtube","server":"rtmp://127.0.0.1:19361/live","enabled":true,"encoder":{"shared":true}}
]}
"@), (New-Object Text.UTF8Encoding($false)))

# Stream keys, written exactly the way the plugin writes them (run-unique targets; only these are deleted afterwards)
function Set-Key($id, $key) {
  $bytes = [Text.Encoding]::UTF8.GetBytes($key)
  $ptr = [Runtime.InteropServices.Marshal]::AllocHGlobal($bytes.Length)
  [Runtime.InteropServices.Marshal]::Copy($bytes, 0, $ptr, $bytes.Length)
  $c = New-Object RWS.NlCred+CREDENTIAL
  $c.Type = 1; $c.TargetName = "RedWardenStudios/Multistream/$id"; $c.UserName = "stream-key"
  $c.CredentialBlobSize = $bytes.Length; $c.CredentialBlob = $ptr; $c.Persist = 2
  if (![RWS.NlCred]::CredWriteW([ref]$c, 0)) { throw "CredWrite failed" }
  [Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
}
Set-Key $idA "k"
Set-Key $idB "k"

# ---- receivers and the relay -------------------------------------------------------------------------------------------
$outDir = Join-Path $bed "received-netlight"
New-Item -ItemType Directory -Force $outDir | Out-Null
Get-ChildItem $outDir | Remove-Item -Force
function Start-Sink($p, $name) {
  Start-Process ffmpeg -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $outDir "$name.log") -ArgumentList @(
    "-hide_banner", "-loglevel", "warning", "-y", "-listen", "1",
    "-i", "rtmp://127.0.0.1:$p/live/k", "-c", "copy", ('"' + (Join-Path $outDir "$name.mkv") + '"'))
}
$flag = Join-Path $outDir "throttle.on"
if (Test-Path $flag) { Remove-Item $flag -Force }
$script:sinks["main"] = Start-Sink 19360 "main"
$script:sinks["youtube"] = Start-Sink 19361 "youtube"
$script:sinks["twitch1"] = Start-Sink 19363 "twitch1"
[RwsThrottle]::Start(19362, 19363, $flag, 60)
$script:relayUp = $true
Start-Sleep -Seconds 2

# obs-websocket v5 client (raw request JSON in, reply text out)
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
function Wait-Log([string]$Rx, [int]$Sec, [int]$MinCount = 1) {
  $end = (Get-Date).AddSeconds($Sec)
  while ((Get-Date) -lt $end) {
    if (([regex]::Matches((Get-KitLogText $cfg), $Rx)).Count -ge $MinCount) { return $true }
    Start-Sleep -Milliseconds 700
  }
  return $false
}

# ---- the run -----------------------------------------------------------------------------------------------------------
New-Item -ItemType Directory -Force $shots | Out-Null
Get-ChildItem $shots -Filter "kit-dock-net-red.png" -ErrorAction SilentlyContinue | Remove-Item -Force
$env:RWS_INFLIGHT_TEST = "1"
$env:RWS_INFLIGHT_TEST_NET = "1"
$env:RWS_KIT_TEST_NET = "1"
$env:RWS_KIT_TEST_SHOT_DIR = $shots
$proc = Start-KitObs $obs "NetLightTest" "NetLightTest"
$t0 = Get-Date
$steps = @()
$reEnabled = $false
try {
  Start-Sleep -Seconds 11
  [void](Invoke-ObsWsRaw @('{"op":6,"d":{"requestType":"StartStream","requestId":"1"}}'))
  Start-Sleep -Seconds 3
  New-Item -ItemType File -Force $flag | Out-Null          # Twitch's uplink gets congested
  $steps += "throttle on at T+$([int]((Get-Date) - $t0).TotalSeconds) s"
  # wait for the hook to press the real button (red verdict needs a full 30 s window, then 3 ticks, then 2.5 s)
  $clicked = Wait-Log 'TEST net: Turn off button clicked=1' 100
  $steps += "button clicked=$clicked at T+$([int]((Get-Date) - $t0).TotalSeconds) s"
  if ($clicked) {
    [void](Wait-Log '\[Twitch test\] stopped' 15)
    # the uplink recovers, then the user switches the destination back on
    Remove-Item $flag -Force -ErrorAction SilentlyContinue
    Start-Sleep -Seconds 6
    $script:sinks["twitch2"] = Start-Sink 19363 "twitch2"
    Start-Sleep -Seconds 2
    $steps += "switch back on at T+$([int]((Get-Date) - $t0).TotalSeconds) s"
    $reply = Invoke-ObsWsRaw @(('{"op":6,"d":{"requestType":"CallVendorRequest","requestId":"2","requestData":{"vendorName":"RedWardenMultistream","requestType":"SetDestination","requestData":{"destination":"' + $idA + '","enabled":true}}}}'))
    $steps += "SetDestination reply: $($reply -join ' ')"
    $reEnabled = $true
    # live again, then a full window of its own (30 s) so it is judged again
    [void](Wait-Log '\[Twitch test\] live' 40 2)
    [void](Wait-Log ("views=[^\r\n]*" + [regex]::Escape($idA) + ":G") 70)
  }
  Write-Host "Stopping the stream via obs-websocket..."
  try { [void](Invoke-ObsWsRaw @('{"op":6,"d":{"requestType":"StopStream","requestId":"3"}}')) } catch { Write-Warning "websocket: $_" }
  Start-Sleep -Seconds 4
} finally {
  try { [void](Close-KitObs $proc 25) } catch { }
  foreach ($k in "RWS_INFLIGHT_TEST", "RWS_INFLIGHT_TEST_NET", "RWS_KIT_TEST_NET", "RWS_KIT_TEST_SHOT_DIR") { Remove-Item -Path "Env:\$k" -ErrorAction SilentlyContinue }
}
[void](Wait-KitObsExit $obsExe 30)
$final = Get-KitLogText $cfg
$visible = @(Stop-KitWindowWatch)
Start-Sleep -Seconds 1
Cleanup-Run
$script:relayUp = $false

# ---- verdict -----------------------------------------------------------------------------------------------------------
$ts = '(?m)^(\d\d):(\d\d):(\d\d)\.(\d+): '
function LineTime($m) { ([int]$m.Groups[1].Value * 3600 + [int]$m.Groups[2].Value * 60 + [int]$m.Groups[3].Value) * 1000 + [int]$m.Groups[4].Value }
function Count-Lines([string]$Rx) { ([regex]::Matches($final, $Rx)).Count }
$kc = @()
$kc += Test-KitLoadLog $final $cfg @("multistream", "preflight", "inflight")
$kc += Test-KitUpdateNotice $final
$kc += New-KitCheck ((Count-Lines '\[rws-stream-kit\] network light: judging OBS''s stream and every Multistream destination') -eq 1) "the Kit wired the Network light to Multistream's destinations (in-process bridge, once)"

# 1. red/amber naming Twitch, main and YouTube fine
$redRx = $ts + '\[rws-inflight\] net: light=([AR]) reason=drops out=Twitch test pct30=([0-9.]+) session=(-?[0-9.]+) fine=([^\r\n]*?) views=(\S+)'
$red = [regex]::Matches($final, $redRx)
$kc += New-KitCheck ($red.Count -ge 1) "the Network light went AMBER/RED because of drops, naming 'Twitch test' (x$($red.Count) change lines)"
if ($red.Count -ge 1) {
  # The first red line can come a second before YouTube's own window has filled (it joined a second after Twitch);
  # the verdict that matters is the first red line in which every OTHER output has been judged too.
  $firstRed = $red[0]
  foreach ($m in $red) { if ($m.Groups[8].Value -match 'YouTube test') { $firstRed = $m; break } }
  $pct30 = [double]::Parse($firstRed.Groups[6].Value, [Globalization.CultureInfo]::InvariantCulture)
  $fine = $firstRed.Groups[8].Value
  $views = $firstRed.Groups[9].Value
  $kc += New-KitCheck ($pct30 -gt 0.5) "its 30 s figure is over the amber threshold (pct30=$pct30)"
  # The Red threshold itself: an A-or-R line with pct30 > 0.5 would also pass for a mere amber. The evaluator's
  # own log lines must contain a Network RED verdict on Twitch test with a 30 s figure over 2.0 %.
  $rLines = [regex]::Matches($final, $ts + '\[rws-inflight\] net: light=R reason=drops out=Twitch test pct30=([0-9.]+) ')
  $rMax = 0.0
  foreach ($m in $rLines) { $v = [double]::Parse($m.Groups[5].Value, [Globalization.CultureInfo]::InvariantCulture); if ($v -gt $rMax) { $rMax = $v } }
  $kc += New-KitCheck ($rLines.Count -ge 1 -and $rMax -gt 2.0) "the evaluator's own line says light=R (red, not amber) on Twitch test with pct30 over 2.0 (x$($rLines.Count) lines, max pct30=$rMax)"
  # The light's state is read straight from the test hook (the state the Network light in the panel shows at the moment of the
  # grab), never inferred from pixel colour (the red Turn-off button would satisfy a colour test).
  $shown = [regex]::Match($final, '\[rws-inflight\] TEST net: light shown at grab=([A-Za-z]) reason=(\w+) pct30=([0-9.]+) out=Twitch test')
  $shownPct = if ($shown.Success) { [double]::Parse($shown.Groups[3].Value, [Globalization.CultureInfo]::InvariantCulture) } else { 0.0 }
  $kc += New-KitCheck ($shown.Success -and $shown.Groups[1].Value -ceq 'R' -and $shown.Groups[2].Value -eq 'drops' -and $shownPct -gt 2.0) "the Network light the panel showed at the grab was R (drops) on Twitch test, pct30=$shownPct (hook line: $($shown.Value))"
  $kc += New-KitCheck ($fine -match '(^|,)main(,|$)' -and $fine -match 'YouTube test') "the same line says the main stream and YouTube test are fine (fine=$fine)"
  $kc += New-KitCheck ($views -match 'main:G:' -and $views -match ([regex]::Escape($idB) + ':G:') -and $views -match ([regex]::Escape($idA) + ':[AR]:')) "the per-output views: main G, YouTube G, Twitch A/R (views=$views)"
  $kc += New-KitCheck ($firstRed.Groups[7].Value -ne "-1.0") "a session total is reported for the named destination (session=$($firstRed.Groups[7].Value))"
}
$kc += New-KitCheck ((Count-Lines 'net: incident started Twitch test') -ge 1) "the session log's Started line names 'Twitch test'"
# 2. Multistream's row badge
$kc += New-KitCheck ((Count-Lines '\[rws-multistream\] drop badge: Twitch test=[0-9.]+') -ge 1) "Multistream's row got the DROPPED badge for Twitch test"
# 3. the button
$kc += New-KitCheck ((Count-Lines 'TEST net: details open, button visible=1 label="Turn off Twitch test"') -eq 1) "the Network details showed the button 'Turn off Twitch test'"
$click = [regex]::Match($final, $ts + '\[rws-inflight\] network: turning off Twitch test')
$kc += New-KitCheck ((Count-Lines 'TEST net: Turn off button clicked=1') -eq 1 -and $click.Success) "the real button was clicked and took the Turn-off path"
if ($click.Success) {
  $tClick = LineTime $click
  $after = $final.Substring($click.Index)
  $stopLine = [regex]::Match($after, $ts + '\[rws-multistream\] \[Twitch test\] stopped')
  $kc += New-KitCheck $stopLine.Success "Multistream stopped the Twitch test output after the click"
  # 4. green within 5 s
  $g = [regex]::Match($after, $ts + '\[rws-inflight\] net: light=G ')
  $dt = if ($g.Success) { (LineTime $g) - $tClick } else { 999999 }
  $kc += New-KitCheck ($g.Success -and $dt -le 5000) "the Network light was green again $dt ms after the click on Turn off (limit 5000)"
  $kc += New-KitCheck ((Count-Lines 'net: incident ended Twitch test \(turned off\)') -ge 1) "the incident ended as 'turned off' (not as recovered)"
  # 5. back on
  $liveN = Count-Lines '(?m)\[rws-multistream\] \[Twitch test\] live\r?$'
  $kc += New-KitCheck ($reEnabled -and $liveN -ge 2) "switched back on mid-stream: Twitch test reconnected (live x$liveN)"
  $u = [regex]::Match($after, 'views=[^\r\n]*' + [regex]::Escape($idA) + ':U:')
  $gg = [regex]::Match($after, 'views=[^\r\n]*' + [regex]::Escape($idA) + ':G:')
  $kc += New-KitCheck ($u.Success -and $gg.Success -and $gg.Index -gt $u.Index) "and it is measured again, then judged: its view goes U (measuring) then G"
  $from = if ($stopLine.Success) { $stopLine.Index } else { 0 }
  $lateRed = [regex]::Matches($after.Substring($from), 'net: light=[AR] [^\r\n]*out=Twitch test')
  $kc += New-KitCheck ($lateRed.Count -eq 0) "no red/amber on Twitch test after the turn-off (the uplink had recovered) (x$($lateRed.Count))"
}
# healthy outputs were never disturbed
function FrameCount($f) { if (!(Test-Path $f)) { return 0 }; [int](ffprobe -v error -count_packets -select_streams v -show_entries stream=nb_read_packets -of csv=p=0 $f) }
$fm = FrameCount (Join-Path $outDir "main.mkv")
$fy = FrameCount (Join-Path $outDir "youtube.mkv")
$kc += New-KitCheck ($fm -gt 600) "the main stream kept streaming: $fm frames received"
$kc += New-KitCheck ($fy -gt 600 -and ($fm - $fy) -lt 150) "YouTube test kept streaming alongside it: $fy frames vs main $fm"
$kc += New-KitCheck ((Count-Lines '\[rws-multistream\] \[YouTube test\] stopped with error|\[rws-multistream\] \[YouTube test\] unstable') -eq 0) "YouTube test never failed or flapped"
# 6. the shot
$shot = Join-Path $shots "kit-dock-net-red.png"
$len = if (Test-Path $shot) { (Get-Item $shot).Length } else { 0 }
$kc += New-KitCheck ($len -gt 4000) "offscreen grab kit-dock-net-red.png saved ($len bytes)"
$kc += New-KitCheck ((Count-Lines 'kit dock shot saved kit-dock-net-red.png') -eq 1) "the Kit dock logged the shot once"
if ($len -gt 4000) {
  Add-Type -AssemblyName System.Drawing
  $bmp = New-Object Drawing.Bitmap $shot
  $redPx = 0; $amberPx = 0
  for ($y = 0; $y -lt $bmp.Height; $y += 1) { for ($x = 0; $x -lt $bmp.Width; $x += 1) {
    $px = $bmp.GetPixel($x, $y)
    if ([Math]::Abs($px.R - 0xC0) -lt 12 -and [Math]::Abs($px.G - 0x39) -lt 12 -and [Math]::Abs($px.B - 0x2B) -lt 12) { $redPx++ }
    if ([Math]::Abs($px.R - 0xF2) -lt 12 -and [Math]::Abs($px.G - 0xA6) -lt 12 -and [Math]::Abs($px.B - 0x3B) -lt 12) { $amberPx++ }
  } }
  $bmp.Dispose()
  # (the old red-pixel check is gone: the red Turn-off button satisfied it. The light's state is asserted from the hook line above.)
  $kc += New-KitCheck ($amberPx -gt 30) "the grab holds the amber DROPPED badge colour ($amberPx pixels)"
}
# 6b. the destination list is as tall as its rows need: a row that grew (LIVE + DROPPED badge + "n frames dropped" wrapping onto
# a second line) must not clip the next row (found in this very shot: the YouTube row's last line was cut off)
$gm = [regex]::Match($final, 'list geometry kit-dock-net-red\.png \(after the grab\): area=(\d+)x(\d+) hint\.h=\d+ hfw\(width\)=(\d+) viewport=\d+x\d+ content=\d+x(\d+) contentHfw\(contentWidth\)=(\d+) rows:(.*)')
$kc += New-KitCheck $gm.Success "the Kit dock logged the destination list's geometry for the grab"
if ($gm.Success) {
  $areaH = [int]$gm.Groups[2].Value; $needH = [int]$gm.Groups[3].Value; $contentH = [int]$gm.Groups[4].Value
  $rowH = @([regex]::Matches($gm.Groups[6].Value, '\[\d+x(\d+) need \d+\]') | ForEach-Object { [int]$_.Groups[1].Value })
  $kc += New-KitCheck ($rowH.Count -ge 2 -and $rowH[0] -gt $rowH[1]) "the fixture has a row that grew (row heights: $($rowH -join ', '); the Twitch row's status line wraps)"
  $kc += New-KitCheck ($areaH -ge $needH -and $areaH -ge $contentH) "the destination list is as tall as its rows need: area $areaH px, rows need $needH px, content $contentH px (was 158 vs 175 before the fix: the last line of the next row was cut off)"
}
# 7. hygiene
$kc += Test-KitHidden $visible
$kc += Test-KitClosePath
$kc += New-KitCheck ($proc.HasExited -and $proc.ExitCode -eq 0) "OBS exited cleanly on WM_CLOSE (exit code $(if ($proc.HasExited) { $proc.ExitCode } else { 'still running' }))"

$lines = @("steps: " + ($steps -join " | "))
$lines += Get-KitCheckLines $kc
$lines += "--- Network light lines ---"
$lines += @($final -split "`r?`n" | Where-Object { $_ -match 'rws-inflight\] (net:|TEST net|network:)|rws-multistream\] (drop badge|\[Twitch test\]|\[YouTube test\]|list geometry)|kit dock shot saved kit-dock-net-red|network light' } | Select-Object -First 400)
Finish (Test-KitChecksOk $kc) $lines

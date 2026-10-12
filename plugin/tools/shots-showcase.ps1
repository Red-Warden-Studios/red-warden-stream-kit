<#
  Red Warden Stream Kit - the CLEAN SHOWCASE screenshots for the manual and the website. ASCII only. OPTIONAL: not part of
  any release list and not read by check-test-results.ps1 (run-all.ps1 -Steps showcase).

  The release harnesses seed recognisable test data ("Seed item", "kit-seed-mic", "Twitch test" ...). These shots are for
  customers, so this script builds a tidy, realistic setup instead and photographs the Kit dock OFFSCREEN (QWidget render, nothing
  is ever shown; hidden OBS as in every other harness, closed with WM_CLOSE to the captured PID only):
    - scene collection "Showcase": scenes Starting Soon / Live / Be Right Back / Ending; a microphone source named "Mic" and
      "Desktop Audio" (looping WAVs this script writes: a voice-like loop with pauses, a music-like loop - so the mic test passes
      and the Levels show moving numbers); Pre-Flight's starting scene is Starting Soon and is the live scene
    - OBS stream settings filled (a local rtmp address; the dock never shows it), so "Main stream" is green
    - Multistream destinations YouTube, Twitch and Kick pointing at local ffmpeg receivers
    - Pre-Flight checklist "Water bottle" / "Phone on silent", both ticked by the dock's showcase command
  The dock obeys a command file (plugin\src\kit\kit-dock.cpp pollShowcase; only when RWS_KIT_TEST and RWS_KIT_TEST_SHOWCASE are set in a .testbed OBS)
  and renders at RWS_KIT_TEST_SHOT_DPR=2, so a 360 px wide dock becomes a 720 px wide file. Four OBS runs:
    A  Pre-Flight, not live  -> dock-pre-flight.png, show-all-tools.png, settings-tools.png
    B  Pre-Flight mic test   -> dock-pre-flight-mic-test.png (the existing RWS_PREFLIGHT_TEST_MICTEST hook)
    C  live on three destinations + the main stream -> dock-in-flight.png, levels-lufs.png, then Twitch's uplink is throttled
       (the net-light relay) -> dock-in-flight-network.png (the existing RWS_INFLIGHT_TEST_NET hook)
    D  Pre-Flight set up, In-Flight not -> in-flight-preflight-note.png ("Using your audio choices from Pre-Flight.")
  Output: plugin\.testbed\shots\showcase\*.png. Needs ffmpeg on PATH (run C). Result: exit 0 / 1 and a final RESULT: PASS / FAIL
  line (run-all.ps1 -Steps showcase writes it to .testbed\run-showcase.txt). Copying the files into manual\img\ and the website
  hand-off folder is a manual step: look at every PNG first (the images are not checked by any scanner).
  -Runs picks runs (default "A,B,C,D"); -Dpr is the pixel ratio (default 2).
#>
param(
  [string]$Config = "RelWithDebInfo",
  [string]$Runs = "A,B,C,D",
  [double]$Dpr = 2.0
)
$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "kit-testbed.ps1")
$kit = Get-KitTestbed $Config
$bed = $kit.Bed
$obs = $kit.Obs
$cfg = $kit.Cfg
$prof = Join-Path $cfg "basic\profiles\Showcase"
$shots = Join-Path $bed "shots\showcase"
$obsExe = Join-Path $obs "bin\64bit\obs64.exe"
$want = @($Runs -split ',' | ForEach-Object { $_.Trim().ToUpper() } | Where-Object { $_ })

$script:credIds = @()
$script:sinks = @{}
$script:relayUp = $false
$script:proc = $null
$script:envKeys = @()
$script:checks = @()

function Finish([bool]$ok, [string[]]$lines) {
  $lines | ForEach-Object { Write-Host $_ }
  if ($ok) { Write-Host "RESULT: PASS" } else { Write-Host "RESULT: FAIL" }
  exit $(if ($ok) { 0 } else { 1 })
}
function Cleanup-Run {
  foreach ($s in @($script:sinks.Values)) { try { if (!$s.HasExited) { $s.Kill() } } catch { } }
  $script:sinks = @{}
  if ($script:relayUp) { try { [RwsShowThrottle]::Stop() } catch { }; $script:relayUp = $false }
  foreach ($id in $script:credIds) { try { [void][RWS.ShowCred]::CredDeleteW("RedWardenStudios/Multistream/$id", 1, 0) } catch { } }
  Restore-KitObsWebsocket $kit.Obs
}
trap {
  try { if ($script:proc -and !$script:proc.HasExited) { [void](Close-KitObs $script:proc 25) } } catch { }
  foreach ($k in $script:envKeys) { Remove-Item -Path "Env:\$k" -ErrorAction SilentlyContinue }
  Cleanup-Run; Write-Host "FAIL: $_"; Write-Host "RESULT: FAIL"; exit 1
}

if (!(Test-Path $kit.Dll)) { Finish $false @("rws-stream-kit.dll not found: $($kit.Dll)") }
Initialize-KitPortableObs $obs
if (!(Wait-KitObsExit $obsExe 30)) { Finish $false @("a testbed obs64.exe ($obs) is still running; not touching it") }
Install-KitPlugin $kit | ForEach-Object { Write-Host $_ }

# ---- helpers in C#: the audio loops, the throttling relay (as test-net-light.ps1), stream keys ---------------------------
Add-Type -TypeDefinition @'
using System;
using System.IO;
public static class RwsShowAudio {
  const int Rate = 48000;
  static void Save(string path, double[] x) {
    int n = x.Length;
    using (FileStream fs = new FileStream(path, FileMode.Create))
    using (BinaryWriter bw = new BinaryWriter(fs)) {
      bw.Write(System.Text.Encoding.ASCII.GetBytes("RIFF")); bw.Write(36 + n * 2);
      bw.Write(System.Text.Encoding.ASCII.GetBytes("WAVE")); bw.Write(System.Text.Encoding.ASCII.GetBytes("fmt "));
      bw.Write(16); bw.Write((short)1); bw.Write((short)1); bw.Write(Rate); bw.Write(Rate * 2); bw.Write((short)2); bw.Write((short)16);
      bw.Write(System.Text.Encoding.ASCII.GetBytes("data")); bw.Write(n * 2);
      for (int i = 0; i < n; i++) {
        double v = Math.Max(-1.0, Math.Min(1.0, x[i]));
        bw.Write((short)Math.Round(v * 32767.0));
      }
    }
  }
  // A voice-like loop: a room-noise floor and syllables (a 100-130 Hz voice with harmonics) in long phrases with short pauses (so the Levels never sit in a gap).
  public static void Voice(string path, double seconds) {
    int n = (int)(Rate * seconds);
    double[] x = new double[n];
    Random r = new Random(11);
    double lp = 0;
    for (int i = 0; i < n; i++) { double w = r.NextDouble() * 2 - 1; lp += 0.15 * (w - lp); x[i] = lp * 0.004; }
    double t = 0.35;
    int inPhrase = 0;
    while (t < seconds - 0.8) {
      double dur = 0.13 + r.NextDouble() * 0.12;
      double f0 = 105 + r.NextDouble() * 28;
      double peakDb = -9.0 + r.NextDouble() * 4.5;
      int s0 = (int)(t * Rate), s1 = Math.Min(n, (int)((t + dur) * Rate));
      double ph = 0;
      double[] seg = new double[s1 - s0];
      double mx = 1e-9;
      for (int i = s0; i < s1; i++) {
        double u = (double)(i - s0) / (s1 - s0);
        double env = Math.Pow(Math.Sin(Math.PI * u), 1.4);
        double f = f0 * (1.0 + 0.10 * (u - 0.5)) * (1.0 + 0.012 * Math.Sin(2 * Math.PI * 5.5 * (double)i / Rate));
        ph += 2 * Math.PI * f / Rate;
        double v = 0;
        for (int k = 1; k <= 30; k++) { if (k * f > 5200) break; v += Math.Sin(k * ph) / Math.Pow(k, 1.05); }
        v += (r.NextDouble() * 2 - 1) * 0.35;
        seg[i - s0] = v * env;
        if (Math.Abs(seg[i - s0]) > mx) mx = Math.Abs(seg[i - s0]);
      }
      double g = Math.Pow(10.0, peakDb / 20.0) / mx;
      for (int i = s0; i < s1; i++) x[i] += seg[i - s0] * g;
      inPhrase++;
      if (inPhrase >= 8 + r.Next(5)) { t += dur + 0.30 + r.NextDouble() * 0.15; inPhrase = 0; }
      else t += dur + 0.01 + r.NextDouble() * 0.04;
    }
    Save(path, x);
  }
  // A music-like loop (seamless: whole cycles over the duration): a four-note chord with a slow swell and a soft pulse.
  public static void Music(string path, double seconds) {
    int n = (int)(Rate * seconds);
    double[] x = new double[n];
    Random r = new Random(5);
    double[] fr = new double[] { 196, 247, 294, 392 };
    double[] lfo = new double[] { 2.0 / 12, 3.0 / 12, 4.0 / 12, 5.0 / 12 };
    double lp = 0;
    double mx = 1e-9;
    for (int i = 0; i < n; i++) {
      double tt = (double)i / Rate;
      double v = 0;
      for (int k = 0; k < 4; k++) {
        double a = 0.6 + 0.4 * Math.Sin(2 * Math.PI * lfo[k] * tt);
        v += a * (Math.Sin(2 * Math.PI * fr[k] * tt) + 0.4 * Math.Sin(2 * Math.PI * 2 * fr[k] * tt));
      }
      double beat = tt - Math.Floor(tt / 0.5) * 0.5;
      v += 2.2 * Math.Exp(-beat * 14.0) * Math.Sin(2 * Math.PI * 62 * beat);
      double w = r.NextDouble() * 2 - 1; lp += 0.08 * (w - lp);
      v += lp * 1.2;
      x[i] = v;
      if (Math.Abs(v) > mx) mx = Math.Abs(v);
    }
    double g = Math.Pow(10.0, -12.0 / 20.0) / mx;
    for (int i = 0; i < n; i++) x[i] *= g;
    Save(path, x);
  }
}
'@
Add-Type -TypeDefinition @'
using System;
using System.IO;
using System.Net;
using System.Net.Sockets;
using System.Threading;
public static class RwsShowThrottle {
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
Add-Type -Namespace RWS -Name ShowCred -MemberDefinition @'
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
  $c = New-Object RWS.ShowCred+CREDENTIAL
  $c.Type = 1; $c.TargetName = "RedWardenStudios/Multistream/$id"; $c.UserName = "stream-key"
  $c.CredentialBlobSize = $bytes.Length; $c.CredentialBlob = $ptr; $c.Persist = 2
  if (![RWS.ShowCred]::CredWriteW([ref]$c, 0)) { throw "CredWrite failed" }
  [Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)
}

# ---- the fixed fixture ids and files -------------------------------------------------------------------------------------
$micUuid = "c0000000-0000-0000-0000-0000000000a1"
$deskUuid = "c0000000-0000-0000-0000-0000000000a2"
$bgUuid = "c0000000-0000-0000-0000-0000000000b1"
$scUuid = @{ "Starting Soon" = "c0000000-0000-0000-0000-000000000011"; "Live" = "c0000000-0000-0000-0000-000000000012"
             "Be Right Back" = "c0000000-0000-0000-0000-000000000013"; "Ending" = "c0000000-0000-0000-0000-000000000014" }
$run = -join ((1..6) | ForEach-Object { '{0:x}' -f (Get-Random -Maximum 16) })
$idYt = "sc${run}1"; $idTw = "sc${run}2"; $idKk = "sc${run}3"
$script:credIds = @($idYt, $idTw, $idKk)
New-Item -ItemType Directory -Force $shots | Out-Null
# Only the shots of the runs asked for are replaced; the others stay.
$runShots = @{ A = @("dock-pre-flight", "show-all-tools", "settings-tools"); B = @("dock-pre-flight-mic-test"); C = @("dock-in-flight", "levels-lufs", "dock-in-flight-network"); D = @("in-flight-preflight-note") }
foreach ($k in $want) { foreach ($n in $runShots[$k]) { Remove-Item (Join-Path $shots "$n.png") -ErrorAction SilentlyContinue } }
Get-ChildItem $shots -Filter "*-net-red.png" -ErrorAction SilentlyContinue | Remove-Item -Force
Get-ChildItem $shots -Filter "mic-*.png" -ErrorAction SilentlyContinue | Remove-Item -Force
Remove-Item (Join-Path $shots "showcase-cmd.txt*") -ErrorAction SilentlyContinue
$voiceWav = Join-Path $bed "showcase-voice.wav"
$musicWav = Join-Path $bed "showcase-music.wav"
[RwsShowAudio]::Voice($voiceWav, 12.0)
[RwsShowAudio]::Music($musicWav, 12.0)
$utf8 = New-Object Text.UTF8Encoding($false)
function J([string]$p) { $p.Replace('\', '/') }

# Throwaway config. -PreFlight / -InFlight: the plugin_config JSON for that tool (or $null = no file). -Live: obs-websocket on.
function New-ShowcaseConfig([string]$PreFlight, [string]$InFlight, [bool]$Live) {
  Clear-KitConfig $cfg
  New-Item -ItemType Directory -Force $prof | Out-Null
  New-Item -ItemType Directory -Force (Join-Path $cfg "basic\scenes") | Out-Null
  $ini = @"
[General]
FirstRun=false
LastVersion=999999999
EnableAutoUpdates=false
[Basic]
Profile=Showcase
ProfileDir=Showcase
SceneCollection=Showcase
SceneCollectionFile=Showcase
[BasicWindow]
ConfirmOnExit=false
WarnBeforeStartingStream=false
WarnBeforeStoppingStream=false
"@
  Set-Content (Join-Path $cfg "global.ini") $ini -Encoding UTF8
  Set-Content (Join-Path $cfg "user.ini") $ini -Encoding UTF8
  $wsDir = Join-Path $cfg "plugin_config\obs-websocket"
  New-Item -ItemType Directory -Force $wsDir | Out-Null
  $wsOn = if ($Live) { "true" } else { "false" }
  Set-Content (Join-Path $wsDir "config.json") ('{"alerts_enabled":false,"auth_required":false,"first_load":false,"server_enabled":' + $wsOn + ',"server_password":"","server_port":4466}') -Encoding UTF8
  Set-Content (Join-Path $prof "basic.ini") @"
[General]
Name=Showcase
[Output]
Mode=Advanced
[AdvOut]
Encoder=obs_x264
TrackIndex=1
Track1Bitrate=160
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
  Set-Content (Join-Path $prof "service.json") '{"type":"rtmp_custom","settings":{"server":"rtmp://127.0.0.1:19360/live","key":"showcase","use_auth":false}}' -Encoding UTF8
  # scenes: a colour background, the mic and the desktop audio in every scene (so their audio is active whichever scene is live)
  $items = '[{"name":"Background","source_uuid":"' + $bgUuid + '","id":1,"visible":true},{"name":"Mic","source_uuid":"' + $micUuid + '","id":2,"visible":true},{"name":"Desktop Audio","source_uuid":"' + $deskUuid + '","id":3,"visible":true}]'
  $scenes = @()
  foreach ($nm in "Starting Soon", "Live", "Be Right Back", "Ending") {
    $scenes += '{"id":"scene","versioned_id":"scene","name":"' + $nm + '","uuid":"' + $scUuid[$nm] + '","settings":{"id_counter":3,"items":' + $items + '}}'
  }
  $col = '{"id":"color_source_v3","versioned_id":"color_source_v3","name":"Background","uuid":"' + $bgUuid + '","settings":{"color":4281545523,"width":1280,"height":720}}'
  $mic = '{"id":"ffmpeg_source","versioned_id":"ffmpeg_source","name":"Mic","uuid":"' + $micUuid + '","mixers":255,"volume":1.0,"muted":false,"enabled":true,"settings":{"is_local_file":true,"local_file":"' + (J $voiceWav) + '","looping":true,"restart_on_activate":true}}'
  $desk = '{"id":"ffmpeg_source","versioned_id":"ffmpeg_source","name":"Desktop Audio","uuid":"' + $deskUuid + '","mixers":255,"volume":1.0,"muted":false,"enabled":true,"settings":{"is_local_file":true,"local_file":"' + (J $musicWav) + '","looping":true,"restart_on_activate":true}}'
  $json = '{"name":"Showcase","current_scene":"Starting Soon","current_program_scene":"Starting Soon","scene_order":[{"name":"Starting Soon"},{"name":"Live"},{"name":"Be Right Back"},{"name":"Ending"}],"sources":[' + $col + ',' + $mic + ',' + $desk + ',' + ($scenes -join ',') + ']}'
  [IO.File]::WriteAllText((Join-Path $cfg "basic\scenes\Showcase.json"), $json, $utf8)
  # Multistream destinations (stream keys in the Credential Manager like the plugin writes them; removed again at the end)
  [IO.File]::WriteAllText((Join-Path $prof "rws-multistream.json"), (@"
{"version":1,"destinations":[
 {"id":"$idYt","name":"YouTube","platform":"youtube","server":"rtmp://127.0.0.1:19361/live","enabled":true,"encoder":{"shared":true}},
 {"id":"$idTw","name":"Twitch","platform":"twitch","server":"rtmp://127.0.0.1:19362/live","enabled":true,
  "encoder":{"shared":false,"encoder_id":"obs_x264","video_bitrate":1500,"width":1280,"height":720}},
 {"id":"$idKk","name":"Kick","platform":"kick","server":"rtmp://127.0.0.1:19364/live","enabled":true,"encoder":{"shared":true}}
]}
"@), $utf8)
  foreach ($id in $script:credIds) { Set-Key $id "showcase" }
  $kd = Join-Path $cfg "plugin_config\rws-stream-kit"
  New-Item -ItemType Directory -Force $kd | Out-Null
  [IO.File]::WriteAllText((Join-Path $kd "settings.json"), '{"check_updates":false}', $utf8)
  if ($PreFlight) {
    $d = Join-Path $cfg "plugin_config\rws-preflight"; New-Item -ItemType Directory -Force $d | Out-Null
    [IO.File]::WriteAllText((Join-Path $d "settings.json"), $PreFlight, $utf8)
  }
  if ($InFlight) {
    $d = Join-Path $cfg "plugin_config\rws-inflight"; New-Item -ItemType Directory -Force $d | Out-Null
    [IO.File]::WriteAllText((Join-Path $d "settings.json"), $InFlight, $utf8)
  }
}
$pfJson = '{"setup_done":true,"use_main_stream":true,"use_multistream":true,"mic_uuid":"' + $micUuid + '","desktop_uuid":"' + $deskUuid + '","start_scene_uuid":"' + $scUuid["Starting Soon"] + '","record_intent":false,"check_updates":false,"custom_items":[{"text":"Water bottle"},{"text":"Phone on silent"}]}'
$ifJson = '{"setup_done":true,"mic_uuid":"' + $micUuid + '","desktop_uuid":"' + $deskUuid + '","check_updates":false,"levels_mode":"lufs","levels_true_peak":true,"levels_lufs_ref_on":true,"levels_lufs_ref":-16,"preflight_note_seen":true,"holding_scenes":[{"scene_uuid":"' + $scUuid["Be Right Back"] + '","limit_seconds":300,"label":"Be Right Back"}]}'

# ---- one OBS run ---------------------------------------------------------------------------------------------------------
function Start-ShowRun([hashtable]$EnvVars) {
  $EnvVars["RWS_KIT_TEST"] = "1"
  $EnvVars["RWS_KIT_TEST_SHOWCASE"] = "1"
  $EnvVars["RWS_KIT_TEST_SHOT_DIR"] = $shots
  $EnvVars["RWS_KIT_TEST_SHOT_DPR"] = "$Dpr"
  foreach ($k in $EnvVars.Keys) { Set-Item -Path "Env:\$k" -Value $EnvVars[$k] }
  $script:envKeys = @($EnvVars.Keys)
  $script:proc = Start-KitObs $obs "Showcase" "Showcase"
}
function Stop-ShowRun([string]$Name, [string[]]$Tools = @("multistream", "preflight", "inflight")) {
  try { [void](Close-KitObs $script:proc 30) } catch { }
  foreach ($k in $script:envKeys) { Remove-Item -Path "Env:\$k" -ErrorAction SilentlyContinue }
  $script:envKeys = @()
  [void](Wait-KitObsExit $obsExe 30)
  $final = Get-KitLogText $cfg
  Set-Content -Encoding ASCII (Join-Path $bed "showcase-run-$Name.log") $final
  $vis = @(Stop-KitWindowWatch)
  $h = Test-KitHidden $vis; $h.Msg = "[$Name] " + $h.Msg
  $script:checks += $h
  $script:checks += New-KitCheck ($script:proc.HasExited -and $script:proc.ExitCode -eq 0) "[$Name] OBS exited cleanly on WM_CLOSE (exit code $($script:proc.ExitCode))"
  $script:checks += Test-KitLoadLog $final $cfg $Tools
  return $final
}
function Wait-Log([string]$Rx, [int]$Sec, [int]$MinCount = 1) {
  $end = (Get-Date).AddSeconds($Sec)
  while ((Get-Date) -lt $end) {
    if (([regex]::Matches((Get-KitLogText $cfg), $Rx)).Count -ge $MinCount) { return $true }
    Start-Sleep -Milliseconds 700
  }
  return $false
}
# Writes the command file (atomically), waits until the dock took it and, with -Shot, until the shot was saved.
function Send-Cmd([string[]]$Lines, [string]$Shot = "") {
  $f = Join-Path $shots "showcase-cmd.txt"
  [IO.File]::WriteAllText("$f.tmp", (($Lines -join "`n") + "`n"), $utf8)
  Move-Item "$f.tmp" $f -Force
  $end = (Get-Date).AddSeconds(20)
  while ((Test-Path $f) -and (Get-Date) -lt $end) { Start-Sleep -Milliseconds 300 }
  if ($Shot) { return (Wait-Log ("kit dock shot saved " + [regex]::Escape($Shot) + "\.png") 40) }
  return (-not (Test-Path $f))
}
function Wait-Ready { return (Wait-Log 'kit dock: showcase command file is watched' 90) }
function Note([bool]$ok, [string]$msg) { $script:checks += New-KitCheck $ok $msg }

# obs-websocket v5 client (raw request JSON in, reply text out), as test-net-light.ps1
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

# ---- A. Pre-Flight, not live ---------------------------------------------------------------------------------------------
if ($want -contains "A") {
  Write-Host "--- run A: Pre-Flight, not live ---"
  New-ShowcaseConfig $pfJson $ifJson $false
  Start-ShowRun @{}
  Note (Wait-Ready) "A: the dock is watching the showcase command file"
  Start-Sleep -Seconds 40   # the first-run docking, and OBS's "keeping up" check needs its 30 s window to turn green
  [void](Send-Cmd @("stage=pre", "showall=0", "levels=0", "tick=Water bottle|Phone on silent"))
  Start-Sleep -Seconds 4
  Note (Send-Cmd @("name=dock-pre-flight") "dock-pre-flight") "A: dock-pre-flight.png saved"
  [void](Send-Cmd @("showall=1"))
  Start-Sleep -Seconds 3
  Note (Send-Cmd @("name=show-all-tools") "show-all-tools") "A: show-all-tools.png saved"
  [void](Send-Cmd @("showall=0"))
  Note (Send-Cmd @("name=settings-tools", "target=settings-tools") "settings-tools") "A: settings-tools.png saved"
  [void](Stop-ShowRun "A")
}

# ---- B. the mic test with numbers ----------------------------------------------------------------------------------------
if ($want -contains "B") {
  Write-Host "--- run B: Pre-Flight mic test ---"
  New-ShowcaseConfig $pfJson $ifJson $false
  Start-ShowRun @{ RWS_KIT_TEST_MICSHOT = "1"; RWS_KIT_TEST_SHOT_PREFIX = "mic"; RWS_PREFLIGHT_TEST = "1"; RWS_PREFLIGHT_TEST_MICTEST = "32" }
  Note (Wait-Ready) "B: the dock is watching the showcase command file"
  Start-Sleep -Seconds 12
  [void](Send-Cmd @("stage=pre", "levels=0", "tick=Water bottle|Phone on silent"))
  Note (Wait-Log 'kit dock shot saved mic-mic-numbers\.png' 90) "B: the mic test ran and the dock saved its numbers shot"
  $fin = Stop-ShowRun "B"
  $mm = [regex]::Match($fin, 'mic test numbers: loudest=(-?\d+\.\d|-inf) typical=(-?\d+\.\d|-inf)')
  Note $mm.Success "B: mic test numbers: $($mm.Value)"
  Note ([regex]::IsMatch($fin, 'mic test: samples=\d+ .* result=Passed')) "B: the mic test PASSED (a voice-like loop with pauses)"
  $src = Join-Path $shots "mic-mic-numbers.png"
  if (Test-Path $src) { Move-Item $src (Join-Path $shots "dock-pre-flight-mic-test.png") -Force }
}

# ---- C. live: lights, Levels, then the congested destination -------------------------------------------------------------
if ($want -contains "C") {
  Write-Host "--- run C: live ---"
  New-ShowcaseConfig $pfJson $ifJson $true
  $outDir = Join-Path $bed "received-showcase"
  New-Item -ItemType Directory -Force $outDir | Out-Null
  Get-ChildItem $outDir | Remove-Item -Force
  function Start-Sink($p, $name) {
    Start-Process ffmpeg -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $outDir "$name.log") -ArgumentList @(
      "-hide_banner", "-loglevel", "warning", "-y", "-listen", "1",
      "-i", "rtmp://127.0.0.1:$p/live/showcase", "-c", "copy", ('"' + (Join-Path $outDir "$name.mkv") + '"'))
  }
  $flag = Join-Path $outDir "throttle.on"
  if (Test-Path $flag) { Remove-Item $flag -Force }
  $script:sinks["main"] = Start-Sink 19360 "main"
  $script:sinks["youtube"] = Start-Sink 19361 "youtube"
  $script:sinks["twitch"] = Start-Sink 19363 "twitch"
  $script:sinks["kick"] = Start-Sink 19364 "kick"
  [RwsShowThrottle]::Start(19362, 19363, $flag, 60)
  $script:relayUp = $true
  Start-Sleep -Seconds 2
  Start-ShowRun @{ RWS_INFLIGHT_TEST = "1"; RWS_INFLIGHT_TEST_NET = "1"; RWS_KIT_TEST_NET = "1"; RWS_KIT_TEST_SHOT_PREFIX = "net" }
  try {
    Note (Wait-Ready) "C: the dock is watching the showcase command file"
    Start-Sleep -Seconds 8
    [void](Invoke-ObsWsRaw @('{"op":6,"d":{"requestType":"StartStream","requestId":"1"}}'))
    Note (Wait-Log '\[YouTube\] live' 40) "C: YouTube went live"
    Note (Wait-Log '\[Twitch\] live' 40) "C: Twitch went live"
    Note (Wait-Log '\[Kick\] live' 40) "C: Kick went live"
    [void](Invoke-ObsWsRaw @('{"op":6,"d":{"requestType":"SetCurrentProgramScene","requestId":"2","requestData":{"sceneName":"Live"}}}'))
    Start-Sleep -Seconds 50    # the Network light needs a full 30 s window before it is judged
    [void](Send-Cmd @("stage=in", "levels=0"))
    Start-Sleep -Seconds 4
    Note (Send-Cmd @("name=dock-in-flight") "dock-in-flight") "C: dock-in-flight.png saved"
    [void](Send-Cmd @("stage=in", "levels=1"))
    Start-Sleep -Seconds 9      # 3 s loudness average, the short-term window and the voice loop's phrases
    Note (Send-Cmd @("name=levels-lufs", "target=levels") "levels-lufs") "C: levels-lufs.png saved"
    [void](Send-Cmd @("levels=0"))
    Start-Sleep -Seconds 3
    New-Item -ItemType File -Force $flag | Out-Null   # Twitch's uplink gets congested
    Note (Wait-Log 'TEST net: Turn off button clicked=1' 120) "C: the Network light went red on Twitch and the hook grabbed the dock"
    [void](Wait-Log 'kit dock shot saved net-net-red\.png' 30)
    Remove-Item $flag -Force -ErrorAction SilentlyContinue
    try { [void](Invoke-ObsWsRaw @('{"op":6,"d":{"requestType":"StopStream","requestId":"3"}}')) } catch { Write-Warning "websocket: $_" }
    Start-Sleep -Seconds 4
  } finally {
    $fin = Stop-ShowRun "C"
    Start-Sleep -Seconds 1
    Cleanup-Run
  }
  $r = [regex]::Match($fin, 'net: light=R reason=drops out=(\S[^\r\n]*?) pct30=')
  Note ($r.Success -and $r.Groups[1].Value -eq "Twitch") "C: the red Network light names Twitch ($($r.Value))"
  $src = Join-Path $shots "net-net-red.png"
  if (Test-Path $src) { Move-Item $src (Join-Path $shots "dock-in-flight-network.png") -Force }
}

# ---- D. In-Flight right after it took Pre-Flight's choices ---------------------------------------------------------------
if ($want -contains "D") {
  Write-Host "--- run D: In-Flight note ---"
  New-ShowcaseConfig $pfJson $null $false
  Start-ShowRun @{}
  Note (Wait-Ready) "D: the dock is watching the showcase command file"
  Start-Sleep -Seconds 14
  [void](Send-Cmd @("stage=in", "levels=0"))
  Start-Sleep -Seconds 4
  Note (Send-Cmd @("name=in-flight-preflight-note") "in-flight-preflight-note") "D: in-flight-preflight-note.png saved"
  $fin = Stop-ShowRun "D"
  Note ([regex]::IsMatch($fin, 'inflight: auto-copied choices from Pre-Flight \(setup complete\)')) "D: In-Flight auto-copied Pre-Flight's choices (setup complete)"
}

# ---- verdict ---------------------------------------------------------------------------------------------------------------
Cleanup-Run
$expect = @{
  A = @("dock-pre-flight", "show-all-tools", "settings-tools"); B = @("dock-pre-flight-mic-test")
  C = @("dock-in-flight", "levels-lufs", "dock-in-flight-network"); D = @("in-flight-preflight-note")
}
Add-Type -AssemblyName System.Drawing
# The dock is photographed at a fixed tall size; the empty dock background under the last widget is cut off (a little is kept).
function Trim-Shot([string]$Path) {
  $bmp = New-Object Drawing.Bitmap $Path
  $bg = $bmp.GetPixel(4, $bmp.Height - 4)
  $last = 0
  for ($y = $bmp.Height - 1; $y -ge 0 -and $last -eq 0; $y--) {
    for ($x = 0; $x -lt $bmp.Width; $x += 2) {
      $p = $bmp.GetPixel($x, $y)
      if ([Math]::Abs($p.R - $bg.R) -gt 6 -or [Math]::Abs($p.G - $bg.G) -gt 6 -or [Math]::Abs($p.B - $bg.B) -gt 6) { $last = $y; break }
    }
  }
  $newH = [Math]::Min($bmp.Height, $last + [int](14 * $Dpr))
  if ($newH -lt $bmp.Height -and $newH -gt 200) {
    $crop = $bmp.Clone((New-Object Drawing.Rectangle 0, 0, $bmp.Width, $newH), $bmp.PixelFormat)
    $bmp.Dispose()
    $crop.Save($Path + ".tmp", [Drawing.Imaging.ImageFormat]::Png); $crop.Dispose()
    Move-Item ($Path + ".tmp") $Path -Force
  } else { $bmp.Dispose() }
}
foreach ($n in "dock-pre-flight", "dock-pre-flight-mic-test", "show-all-tools", "dock-in-flight", "dock-in-flight-network", "in-flight-preflight-note") {
  $p = Join-Path $shots "$n.png"
  if (Test-Path $p) { Trim-Shot $p }
}
$lines = @()
foreach ($k in $want) {
  foreach ($n in $expect[$k]) {
    $p = Join-Path $shots "$n.png"
    if (!(Test-Path $p)) { $script:checks += New-KitCheck $false "$n.png is missing"; continue }
    $img = [Drawing.Image]::FromFile($p); $w = $img.Width; $h = $img.Height; $img.Dispose()
    $script:checks += New-KitCheck ($w -ge 600) "$n.png saved: ${w}x${h} px"
    $lines += "shot $n.png ${w}x${h}"
  }
}
$lines = @(Get-KitCheckLines $script:checks) + $lines
Finish (Test-KitChecksOk $script:checks) $lines

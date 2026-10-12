<#
  Red Warden Multistream reconnect-safety test. Same isolated portable OBS as test-local.ps1.

  Four destinations in one stream:
    Shared test    -> 19351, shares OBS's encoder. Must stream throughout,
                      unaffected by the trouble on the others.
    Own 720p test  -> 19352. Its receiver is killed mid-stream and brought
                      back later. Must recover on its own.
    Dead test      -> 19353. Nothing ever listens. Persistent retry must
                      engage, and connection attempts must stay inside the
                      budget (<= 12 in any 10 minutes).
    Bad URL test   -> rtmp:///live (no host). Must fail once and never be
                      retried: retrying a broken URL or key only hammers a
                      platform.

  Run from the Multistream folder:  powershell -ExecutionPolicy Bypass -File tools\test-resilience.ps1
#>
param(
  [string]$Config = "RelWithDebInfo",
  [int]$Seconds = 85,
  [int]$KillAt = 22,
  [int]$ReviveAt = 40,
  [switch]$Snapshots   # render the dock to .testbed\snapshots every 4 s for visual review
)
$ErrorActionPreference = "Stop"
$root   = Split-Path -Parent $PSScriptRoot
$kitTools = Join-Path (Split-Path -Parent $root) "plugin\tools"                                          # published layout: <repo>\plugin
if (!(Test-Path $kitTools)) { $kitTools = Join-Path (Split-Path -Parent $root) "red-warden-stream-kit\plugin\tools" }   # development layout
. (Join-Path $kitTools "kit-testbed.ps1")
$kit    = Get-KitTestbed $Config
$bed    = $kit.Bed
$obs    = $kit.Obs
$cfg    = Join-Path $obs "config\obs-studio"
$prof   = Join-Path $cfg "basic\profiles\MultistreamTest"
$dll    = $kit.Dll
$ports  = 19350, 19351, 19352          # 19353 deliberately has no receiver
$ids    = "00000000-beac-0000-0000-000000000001", "00000000-beac-0000-0000-000000000002",
          "00000000-beac-0000-0000-000000000003", "00000000-beac-0000-0000-000000000004"

if (!(Test-Path $dll)) { throw "Build first: $dll not found" }

# 1. portable OBS (first run: copied from the Multistream testbed copy, else from C:\Program Files read-only)
Initialize-KitPortableObs $obs
# The previous harness's OBS may still be shutting down: wait for it, never kill it.
if (!(Wait-KitObsExit (Join-Path $obs "bin\64bit\obs64.exe") 30)) { throw "a testbed obs64.exe ($obs) is still running; not touching it" }

# 2. install exactly ONE plugin DLL: rws-stream-kit.dll (+pdb) and its locale. The old standalone
#    rws-multistream / rws-preflight / rws-inflight dlls and data folders are removed from the copy first.
Install-KitPlugin $kit | ForEach-Object { Write-Host $_ }

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
Profile=MultistreamTest
ProfileDir=MultistreamTest
SceneCollection=MultistreamTest
SceneCollectionFile=MultistreamTest
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
Name=MultistreamTest
[Output]
Mode=Advanced
[AdvOut]
Encoder=obs_x264
TrackIndex=1
Track1Bitrate=256
ApplyServiceSettings=true
[Video]
BaseCX=1920
BaseCY=1080
OutputCX=1920
OutputCY=1080
FPSType=0
FPSCommon=30
"@ -Encoding UTF8

# Real-world streaming settings: CBR with a 2 s keyframe interval.
Set-Content (Join-Path $prof "streamEncoder.json") '{"rate_control":"CBR","bitrate":2500,"keyint_sec":2,"preset":"veryfast"}' -Encoding UTF8

Set-Content (Join-Path $prof "service.json") ('{"type":"rtmp_custom","settings":{"server":"rtmp://127.0.0.1:' + $ports[0] + '/live","key":"k","use_auth":false}}') -Encoding UTF8

# A color source so the stream carries a real picture, not just black.
Set-Content (Join-Path $cfg "basic\scenes\MultistreamTest.json") @'
{"name":"MultistreamTest","current_scene":"Scene","current_program_scene":"Scene",
 "scene_order":[{"name":"Scene"}],
 "sources":[
  {"id":"color_source_v3","versioned_id":"color_source_v3","name":"Ember","uuid":"b0000000-0000-0000-0000-000000000002","settings":{"color":4282038770,"width":1920,"height":1080}},
  {"id":"scene","versioned_id":"scene","name":"Scene","uuid":"b0000000-0000-0000-0000-000000000001","settings":{"id_counter":1,"items":[{"name":"Ember","source_uuid":"b0000000-0000-0000-0000-000000000002","id":1,"visible":true}]}}
 ]}
'@ -Encoding UTF8

Set-Content (Join-Path $prof "rws-multistream.json") (@"
{"version":1,"destinations":[
 {"id":"$($ids[0])","name":"Shared test","platform":"youtube","server":"rtmp://127.0.0.1:$($ports[1])/live","enabled":true,"encoder":{"shared":true}},
 {"id":"$($ids[1])","name":"Own 720p test","platform":"twitch","server":"rtmp://127.0.0.1:$($ports[2])/live","enabled":true,
  "encoder":{"shared":false,"encoder_id":"obs_x264","video_bitrate":1500,"width":1280,"height":720}},
 {"id":"$($ids[2])","name":"Dead test","platform":"kick","server":"rtmp://127.0.0.1:19353/live","enabled":true,
  "encoder":{"shared":false,"encoder_id":"obs_x264","video_bitrate":800,"width":854,"height":480}},
 {"id":"$($ids[3])","name":"Bad URL test","platform":"tiktok","server":"rtmp:///live","enabled":true,
  "encoder":{"shared":false,"encoder_id":"obs_x264","video_bitrate":800,"width":854,"height":480}},
 {"id":"00000000-beac-0000-0000-000000000005","name":"Facebook page","platform":"facebook","server":"rtmps://rtmp-api.facebook.com:443/rtmp/","enabled":false,
  "encoder":{"shared":true}}
]}
"@) -Encoding UTF8

# 3. stream keys, written exactly the way the plugin writes them (UTF-8 blob)
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
foreach ($id in $ids) { Set-Key $id "k" }

# 4. receivers
$outDir = Join-Path $bed "received"
New-Item -ItemType Directory -Force $outDir | Out-Null
Get-ChildItem $outDir | Remove-Item -Force
function Start-Sink($p, $name) {
  Start-Process ffmpeg -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $outDir "$name.log") -ArgumentList @(
    "-hide_banner", "-loglevel", "warning", "-y", "-listen", "1",
    "-i", "rtmp://127.0.0.1:$p/live/k", "-c", "copy", ('"' + (Join-Path $outDir "$name.mkv") + '"'))
}
$sinks = @{}
foreach ($p in $ports) { $sinks["port$p"] = Start-Sink $p "port$p" }
Start-Sleep -Seconds 2

# obs-websocket v5 client: Hello -> Identify -> requests
function Invoke-ObsWs([string[]]$Requests, [int]$Port = 4466) {
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
  $i = 0
  foreach ($rq in $Requests) { $i++; & $send ('{"op":6,"d":{"requestType":"' + $rq + '","requestId":"' + $i + '"}}'); & $recv }
  $ws.Dispose()
}

# A small, known "upload capacity" so the estimate and both warning levels show.
$pcfg = Join-Path $cfg "plugin_config\rws-multistream"
New-Item -ItemType Directory -Force $pcfg | Out-Null
Set-Content (Join-Path $pcfg "settings.json") '{"upload_mbps":7.0,"upload_source":"manual","upload_when":"2026-10-01T07:00:00"}' -Encoding UTF8

if ($Snapshots) {
  $env:RWS_MULTISTREAM_SNAPSHOT = Join-Path $bed "snapshots"
  if (Test-Path $env:RWS_MULTISTREAM_SNAPSHOT) { Remove-Item -Recurse -Force $env:RWS_MULTISTREAM_SNAPSHOT }
} else { Remove-Item Env:RWS_MULTISTREAM_SNAPSHOT -ErrorAction SilentlyContinue }

# 5. run OBS
$proc = Start-KitObs $obs "MultistreamTest" "MultistreamTest"   # hidden, no focus steal
$t0 = Get-Date
function Wait-Until($sec) { $left = $sec - ((Get-Date) - $t0).TotalSeconds; if ($left -gt 0) { Start-Sleep -Milliseconds ([int]($left * 1000)) } }
# Start offline for a while (the "before going live" view), then stream.
Wait-Until 11
try { Invoke-ObsWs @("StartStream") | Out-Null } catch { Write-Warning "websocket: $_" }
Wait-Until $KillAt
Write-Host "T+$KillAt s: killing the Own 720p receiver (simulated platform drop)"
$sinks["port19352"].Kill()
Wait-Until $ReviveAt
Write-Host "T+$ReviveAt s: receiver back"
$sinks["revived"] = Start-Sink 19352 "port19352-revived"
Wait-Until $Seconds
Write-Host "Pressing Stop Streaming via obs-websocket..."
try { Invoke-ObsWs @("StopStream") | Out-Null } catch { Write-Warning "websocket: $_" }
Start-Sleep -Seconds $(if ($Snapshots) { 7 } else { 5 })  # the after-stream snapshot is taken 4 s after stopping
[void](Close-KitObs $proc)   # hidden OBS: WM_CLOSE to its window (CloseMainWindow needs a visible one)
Start-Sleep -Seconds 2
$sinks.Values | ForEach-Object { if (!$_.HasExited) { $_.Kill() } }

foreach ($id in $ids) { [void][RWS.Cred]::CredDeleteW("RedWardenStudios/Multistream/$id", 1, 0) }

# 6. verdict
$log = Get-ChildItem (Join-Path $cfg "logs") | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$text = Get-Content $log.FullName -Raw
Write-Host "`n--- the plugin timeline ---"
Select-String -Path $log.FullName -Pattern "\[rws-multistream\]" | ForEach-Object { $_.Line }
$fail = $false
Write-Host "`n--- Kit load checks (one DLL, one module, one vendor, one dock) ---"
if (!(Write-KitChecks (Test-KitLoadLog $text $cfg))) { $fail = $true }
function FrameCount($f) { if (!(Test-Path $f)) { return 0 }; [int](ffprobe -v error -count_packets -select_streams v -show_entries stream=nb_read_packets -of csv=p=0 $f) }
function Attempts($port) { ([regex]::Matches($text, "Connecting to RTMP URL rtmp://127\.0\.0\.1:$port/")).Count }

Write-Host "`n--- checks ---"
$main = FrameCount (Join-Path $outDir "port19350.mkv")
$shared = FrameCount (Join-Path $outDir "port19351.mkv")
Write-Host "Shared test: $shared frames vs main $main"
if ($main - $shared -gt 120) { Write-Host "  FAIL: Shared test was disrupted by trouble on other destinations"; $fail = $true }

$revived = FrameCount (Join-Path $outDir "port19352-revived.mkv")
$recovered = $text -match [regex]::Escape("[Own 720p test] reconnected") -or $text -match "\[Own 720p test\] live[\s\S]*\[Own 720p test\] live"
Write-Host "Own 720p test: recovered=$recovered, $revived frames after the receiver came back"
if (!$recovered -or $revived -lt 30) { Write-Host "  FAIL: Own 720p test did not recover by itself"; $fail = $true }

$dead = Attempts 19353
$deadRetry = $text -match [regex]::Escape("[Dead test] next connection attempt")
Write-Host "Dead test: $dead connection attempts in $Seconds s, persistent retry engaged=$deadRetry"
if ($dead -gt 12) { Write-Host "  FAIL: more connection attempts than the budget allows"; $fail = $true }
if (!$deadRetry) { Write-Host "  FAIL: persistent retry never engaged"; $fail = $true }

$badRetry = $text -match [regex]::Escape("[Bad URL test] next connection attempt")
$badStarts = ([regex]::Matches($text, [regex]::Escape("[Bad URL test] starting"))).Count
Write-Host "Bad URL test: started $badStarts time(s), retried=$badRetry"
if ($badRetry -or $badStarts -gt 1) { Write-Host "  FAIL: a broken URL was retried"; $fail = $true }

# The end-of-stream report must tell the same story.
$rep = Get-ChildItem (Join-Path $cfg "plugin_config\rws-multistream\reports") -Filter "stream-*.txt" -ErrorAction SilentlyContinue |
  Sort-Object LastWriteTime -Descending | Select-Object -First 1
if (!$rep) { Write-Host "Stream report: FAIL, no report file written"; $fail = $true }
else {
  $r = Get-Content $rep.FullName -Raw
  Write-Host "`n--- stream report ---"; Write-Host $r
  function Block($name) { if ($r -match "(?ms)^$([regex]::Escape($name))\r?\n(.*?)(\r?\n\r?\n|\z)") { $Matches[1] } else { "" } }
  $okShared = (Block "Shared test") -match "No drops"
  $okOwn = (Block "Own 720p test") -match "Dropped (once|twice|\d+ times).*came back (once|twice|\d+ times)"
  $okDead = (Block "Dead test") -match "Never went live"
  $okBad = (Block "Bad URL test") -match "Never went live: Invalid server URL"
  $okFb = -not ($r -match "Facebook page")
  Write-Host "Stream report: shared clean=$okShared, own drop+recovery=$okOwn, dead never live=$okDead, bad URL reason=$okBad, off destination left out=$okFb"
  if (!($okShared -and $okOwn -and $okDead -and $okBad -and $okFb)) { Write-Host "  FAIL: the stream report is wrong"; $fail = $true }
}

# Pre-existing settings: plugin_config\rws-multistream\settings.json was seeded above (upload 7 Mbps, manual).
# The Kit build must read it from the same folder and leave it alone. Read: the first (offline) dock snapshot says
# "Needs about N Mbps of your 7 Mbps upload. That's more than your connection can reliably carry" in the error
# colour (without the setting it would be the grey "Test my upload speed" hint), so its note band (below the header,
# above the destinations, scaled to the snapshot) must hold error-red pixels.
Write-Host "`n--- pre-existing settings (plugin_config\rws-multistream\settings.json) ---"
$sj = Join-Path $pcfg "settings.json"
$sOk = (Test-Path $sj) -and ((Get-Content $sj -Raw | ConvertFrom-Json).upload_mbps -eq 7)
Write-Host "seeded settings.json still intact: $sOk"
if (!$sOk) { Write-Host "  FAIL: the seeded settings.json was lost or changed"; $fail = $true }
$snap1 = Join-Path $bed "snapshots\dock-1-w380.png"
if ($Snapshots -and (Test-Path $snap1)) {
  Add-Type -AssemblyName System.Drawing
  $bmp = New-Object Drawing.Bitmap $snap1
  $red = 0
  # The hidden OBS can open on any monitor, so the snapshot's scale varies (380 logical px wide = 380 px at 100 %,
  # ~420 at 110 %, ~490 at 130 %) and the note wraps lower. Scale the band, and skip the left 70 logical px where
  # the red platform badges sit.
  $f = $bmp.Width / 380.0
  $y0 = [int](60 * $f); $y1 = [int](230 * $f); $x0 = [int](70 * $f)
  for ($y = $y0; $y -le $y1 -and $y -lt $bmp.Height; $y++) { for ($x = $x0; $x -lt $bmp.Width; $x++) {
    $px = $bmp.GetPixel($x, $y); if ($px.R -gt 200 -and $px.G -lt 130 -and $px.B -lt 130) { $red++ } } }
  $bmp.Dispose()
  Write-Host "dock snapshot note band: $red error-red pixels (7 Mbps upload setting read: needs >= 200)"
  if ($red -lt 200) { Write-Host "  FAIL: the seeded 7 Mbps upload setting did not reach the dock"; $fail = $true }
} elseif ($Snapshots) { Write-Host "  FAIL: no dock-1-w380.png snapshot to check the seeded setting"; $fail = $true }
Write-Host ("`nOBS exit code: " + $proc.ExitCode)
if ($proc.ExitCode -ne 0) { Write-Host "OBS did not shut down cleanly"; $fail = $true }
if ($fail) { Write-Host "RESULT: FAIL"; exit 1 } else { Write-Host "RESULT: PASS" }

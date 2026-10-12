<#
  Red Warden Multistream local end-to-end test.

  Builds nothing and touches none of your real OBS settings. It:
    1. makes a portable copy of OBS in .testbed\obs (first run only),
    2. installs the freshly built rws-stream-kit.dll (the ONE Kit DLL; old standalone dlls removed) into that copy,
    3. starts three local RTMP receivers (ffmpeg) on 127.0.0.1,
    4. launches the portable OBS with a throwaway profile whose main stream
       goes to receiver 0, plus two the plugin destinations:
         - "Shared test"  -> receiver 1, shares OBS's encoder
         - "Own 720p test" -> receiver 2, separate x264 encoder at 720p
    5. streams for ~20 s, closes OBS cleanly, and checks each receiver got
       real video + audio (ffprobe) and that the OBS log shows the plugin going live.

  Run from the Multistream folder:  powershell -ExecutionPolicy Bypass -File tools\test-local.ps1
#>
param(
  [string]$Config = "RelWithDebInfo",
  [int]$Seconds = 20
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
$ports  = 19350, 19351, 19352
$ids    = "00000000-beac-0000-0000-000000000001", "00000000-beac-0000-0000-000000000002"

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
 {"id":"$($ids[0])","name":"Shared test","platform":"custom","server":"rtmp://127.0.0.1:$($ports[1])/live","enabled":true,"encoder":{"shared":true}},
 {"id":"$($ids[1])","name":"Own 720p test","platform":"custom","server":"rtmp://127.0.0.1:$($ports[2])/live","enabled":true,
  "encoder":{"shared":false,"encoder_id":"obs_x264","video_bitrate":1500,"width":1280,"height":720}}
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
$sinks = @(foreach ($p in $ports) {
  Start-Process ffmpeg -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $outDir "port$p.log") -ArgumentList @(
    "-hide_banner", "-loglevel", "warning", "-y", "-listen", "1",
    "-i", "rtmp://127.0.0.1:$p/live/k", "-c", "copy", ('"' + (Join-Path $outDir "port$p.mkv") + '"'))
})
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

# One obs-websocket request with optional requestData (JSON). Returns the raw response.
function Invoke-ObsRequest([string]$Type, [string]$DataJson = "{}", [int]$Port = 4466) {
  $ws = New-Object System.Net.WebSockets.ClientWebSocket
  $ws.Options.AddSubProtocol("obswebsocket.json")
  $ct = [Threading.CancellationToken]::None
  $ws.ConnectAsync([Uri]"ws://127.0.0.1:$Port", $ct).Wait()
  $buf = New-Object byte[] 262144
  $recv = { $r = $ws.ReceiveAsync((New-Object ArraySegment[byte] -ArgumentList (,$buf)), $ct).Result; [Text.Encoding]::UTF8.GetString($buf, 0, $r.Count) }
  $send = { param($t) $b = [Text.Encoding]::UTF8.GetBytes($t); $ws.SendAsync((New-Object ArraySegment[byte] -ArgumentList (,$b)), "Text", $true, $ct).Wait() }
  [void](& $recv)
  & $send '{"op":1,"d":{"rpcVersion":1,"eventSubscriptions":0}}'
  [void](& $recv)
  & $send ('{"op":6,"d":{"requestType":"' + $Type + '","requestId":"x","requestData":' + $DataJson + '}}')
  $out = & $recv
  $ws.Dispose()
  return $out
}

# 5. run OBS
$proc = Start-KitObs $obs "MultistreamTest" "MultistreamTest" @("--startstreaming")   # hidden, no focus steal
Start-Sleep -Seconds $Seconds

# Remote control, the way Streamer.bot / Touch Portal / a Stream Deck plugin
# would drive it: list destinations, then switch one off by name.
$vendor = '{"vendorName":"RedWardenMultistream","requestType":"GetDestinations","requestData":{}}'
$list = Invoke-ObsRequest "CallVendorRequest" $vendor
$vendor = '{"vendorName":"RedWardenMultistream","requestType":"SetDestination","requestData":{"destination":"own 720p test","enabled":false}}'
$set = Invoke-ObsRequest "CallVendorRequest" $vendor
Start-Sleep -Seconds 2
# ...and the per-destination hotkey (what a Stream Deck Hotkey key ends up
# pressing) turns it back on.
# Each local receiver takes one connection, so give the re-enabled
# destination a fresh one (a real platform would simply accept it).
$sinks += Start-Process ffmpeg -PassThru -WindowStyle Hidden -RedirectStandardError (Join-Path $outDir "again.log") -ArgumentList @(
  "-hide_banner", "-loglevel", "warning", "-y", "-listen", "1",
  "-i", "rtmp://127.0.0.1:$($ports[2])/live/k", "-c", "copy", ('"' + (Join-Path $outDir "port$($ports[2])-again.mkv") + '"'))
Start-Sleep -Milliseconds 800
$hotkeys = Invoke-ObsRequest "GetHotkeyList"
$hkName = "rws_multistream.toggle.$($ids[1])"
$press = Invoke-ObsRequest "TriggerHotkeyByName" ('{"hotkeyName":"' + $hkName + '"}')
Start-Sleep -Seconds 5
Write-Host "Pressing Stop Streaming via obs-websocket..."
try { Invoke-ObsWs @("StopStream") | Out-Null } catch { Write-Warning "websocket: $_" }
Start-Sleep -Seconds 5
[void](Close-KitObs $proc)   # hidden OBS: WM_CLOSE to its window (CloseMainWindow needs a visible one)
Start-Sleep -Seconds 2
$sinks | ForEach-Object { if (!$_.HasExited) { $_.Kill() } }

foreach ($id in $ids) { [void][RWS.Cred]::CredDeleteW("RedWardenStudios/Multistream/$id", 1, 0) }

# 6. verdict
$log = Get-ChildItem (Join-Path $cfg "logs") | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Write-Host "`n--- OBS log (the plugin lines) ---"
Select-String -Path $log.FullName -Pattern "rws-multistream|rws-stream-kit|Stream Kit|\[Shared test\]|\[Own 720p test\]|crash|error" | ForEach-Object { $_.Line }
Write-Host "`n--- receivers ---"
$fail = $false
foreach ($p in $ports) {
  $f = Join-Path $outDir "port$p.mkv"
  if (!(Test-Path $f) -or (Get-Item $f).Length -lt 10000) {
    Write-Host "port $p : NOTHING RECEIVED"; $fail = $true
    Get-Content (Join-Path $outDir "port$p.log") -ErrorAction SilentlyContinue | Select-Object -Last 6 | ForEach-Object { Write-Host "    $_" }
    continue
  }
  $probe = ffprobe -v error -show_entries "stream=codec_type,codec_name,width,height:format=duration" -of compact $f 2>&1
  Write-Host "port $p : $([math]::Round((Get-Item $f).Length/1KB)) KB"
  $probe | ForEach-Object { Write-Host "    $_" }
}
# Every the plugin receiver must hold (nearly) as many frames as the main stream.
# This catches an output that stays "connected" but stops sending.
function FrameCount($f) { [int](ffprobe -v error -count_packets -select_streams v -show_entries stream=nb_read_packets -of csv=p=0 $f) }
$mainFrames = FrameCount (Join-Path $outDir "port$($ports[0]).mkv")
# A shared destination deliberately joins only after OBS's own stream is
# producing, then waits for the next keyframe, so it may trail the main
# stream by a few seconds of frames. A stall shows up as far more than that.
# The separate-encoder destination is switched off remotely ~3 s before the
# end (the control test above), so it may trail by about that much.
$allowedShort = @{ $ports[1] = 30 * 4; $ports[2] = 30 * 5 }   # frames at 30 fps
foreach ($p in $ports[1..2]) {
  $f = Join-Path $outDir "port$p.mkv"
  if (!(Test-Path $f)) { continue }
  $n = FrameCount $f
  $again = Join-Path $outDir "port$p-again.mkv"
  if (Test-Path $again) { $n += FrameCount $again }   # after the hotkey turned it back on
  $short = $mainFrames - $n
  Write-Host "port $p : $n frames vs main $mainFrames (short by $short, allowed $($allowedShort[$p]))"
  if ($short -gt $allowedShort[$p]) { Write-Host "port $p : STALLED - far fewer frames than the main stream"; $fail = $true }
}

$text = Get-Content $log.FullName -Raw
Write-Host "`n--- Kit load checks (one DLL, one module, one vendor, one dock) ---"
if (!(Write-KitChecks (Test-KitLoadLog $text $cfg))) { $fail = $true }
# "Match OBS" audio must resolve to the profile's 256 kbps, not a lower default.
$m = [regex]::Match($text, "\[Own 720p test\] audio: (\S+), (\d+) kbps, track (\d+)")
if ($m.Success) {
  Write-Host "Own 720p test audio: $($m.Groups[1].Value) at $($m.Groups[2].Value) kbps, track $($m.Groups[3].Value)"
  if ([int]$m.Groups[2].Value -ne 256) { Write-Host "audio did not match OBS's 256 kbps"; $fail = $true }
} else { Write-Host "no audio line for Own 720p test"; $fail = $true }
Write-Host "`n--- remote control (obs-websocket vendor requests) ---"
$listObj = ($list | ConvertFrom-Json).d.responseData.responseData
$setObj = ($set | ConvertFrom-Json).d.responseData.responseData
$states = ($listObj.destinations | ForEach-Object { "$($_.name)=$($_.state)" }) -join ", "
Write-Host "GetDestinations: $states"
Write-Host "SetDestination off: ok=$($setObj.ok) enabled=$($setObj.enabled) state=$($setObj.state)"
if (@($listObj.destinations | Where-Object { $_.state -eq "live" }).Count -ne 2) { Write-Host "GetDestinations did not report both live"; $fail = $true }
if (!$setObj.ok -or $setObj.enabled) { Write-Host "SetDestination did not switch the destination off"; $fail = $true }
$hkList = ($hotkeys | ConvertFrom-Json).d.responseData.hotkeys
$ours = @($hkList | Where-Object { $_ -like "rws_multistream.*" })
Write-Host "Hotkeys registered: $($ours -join ', ')"
foreach ($want in "rws_multistream.all_on", "rws_multistream.all_off", $hkName) {
  if ($ours -notcontains $want) { Write-Host "missing hotkey $want"; $fail = $true }
}
if (!(Write-KitChecks (Test-KitHotkeysOnce $hkList @("rws_multistream.all_on", "rws_multistream.all_off", "rws_multistream.toggle.$($ids[0])", $hkName)))) { $fail = $true }
$pressOk = ($press | ConvertFrom-Json).d.requestStatus.result
$liveCount = ([regex]::Matches($text, [regex]::Escape("[Own 720p test] live"))).Count
Write-Host "Toggle hotkey pressed: request ok=$pressOk; Own 720p test went live $liveCount time(s)"
if (!$pressOk -or $liveCount -lt 2) { Write-Host "the toggle hotkey did not turn the destination back on"; $fail = $true }
$offAt = [regex]::Match($text, "(\d\d:\d\d:\d\d\.\d+): \[rws-multistream\] \[Own 720p test\] stopped").Groups[1].Value
$mainStop = [regex]::Match($text, "(\d\d:\d\d:\d\d\.\d+): ==== Streaming Stop").Groups[1].Value
Write-Host "Own 720p test stopped at $offAt; OBS stream stopped at $mainStop"
if (!$offAt -or ($offAt -ge $mainStop)) { Write-Host "remote switch-off did not stop the destination before Stop Streaming"; $fail = $true }

foreach ($n in "Shared test", "Own 720p test") {
  if ($text -notmatch [regex]::Escape("[$n] live")) { Write-Host "[$n] never went live"; $fail = $true }
  if ($text -notmatch [regex]::Escape("[$n] stopped")) { Write-Host "[$n] did not stop with OBS"; $fail = $true }
}
Write-Host ("`nOBS exit code: " + $proc.ExitCode)
if ($proc.ExitCode -ne 0) { Write-Host "OBS did not shut down cleanly"; $fail = $true }
if ($fail) { Write-Host "RESULT: FAIL"; exit 1 } else { Write-Host "RESULT: PASS" }

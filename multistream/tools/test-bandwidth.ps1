<#
  Red Warden Multistream: Twitch bandwidth test. Same isolated portable OBS as test-local.ps1.

  One Twitch destination (sharing OBS's encoder, switched off) points at a
  local receiver. OBS never starts streaming. The plugin runs the bandwidth
  test on it and the harness checks that:
    - the stream key went out with ?bandwidthtest=true (what keeps Twitch off air)
    - the receiver got a real stream for the length of the test, then it stopped
    - the verdict says the connection is steady
    - OBS's own stream was never started

  Run from the Multistream folder:  powershell -ExecutionPolicy Bypass -File tools\test-bandwidth.ps1
#>
param([string]$Config = "RelWithDebInfo", [int]$Seconds = 34, [switch]$Snapshots)
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

$id = "00000000-beac-0000-0000-0000000000b1"
Set-Content (Join-Path $prof "rws-multistream.json") (@"
{"version":2,"destinations":[
 {"id":"$id","name":"Twitch test","platform":"twitch","server":"rtmp://127.0.0.1:19355/app","enabled":false,"encoder":{"shared":true}}
]}
"@) -Encoding UTF8

Add-Type -Namespace RWS -Name CredB -MemberDefinition @'
[StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
public struct CREDENTIAL { public int Flags; public int Type; public string TargetName; public string Comment;
  public long LastWritten; public int CredentialBlobSize; public System.IntPtr CredentialBlob; public int Persist;
  public int AttributeCount; public System.IntPtr Attributes; public string TargetAlias; public string UserName; }
[DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool CredWriteW(ref CREDENTIAL c, int flags);
[DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool CredDeleteW(string target, int type, int flags);
'@ -ErrorAction SilentlyContinue
$bytes = [Text.Encoding]::UTF8.GetBytes("k")
$ptr = [Runtime.InteropServices.Marshal]::AllocHGlobal($bytes.Length)
[Runtime.InteropServices.Marshal]::Copy($bytes, 0, $ptr, $bytes.Length)
$c = New-Object RWS.CredB+CREDENTIAL
$c.Type = 1; $c.TargetName = "RedWardenStudios/Multistream/$id"; $c.UserName = "stream-key"
$c.CredentialBlobSize = $bytes.Length; $c.CredentialBlob = $ptr; $c.Persist = 2
if (![RWS.CredB]::CredWriteW([ref]$c, 0)) { throw "CredWrite failed" }
[Runtime.InteropServices.Marshal]::FreeHGlobal($ptr)

# The receiver: a local stand-in for Twitch's ingest. It only accepts the
# stream name the plugin must send in bandwidth-test mode.
$outDir = Join-Path $bed "received"
New-Item -ItemType Directory -Force $outDir | Out-Null
Get-ChildItem $outDir | Remove-Item -Force
$sinkLog = Join-Path $outDir "bwtest.log"
$sinkFile = Join-Path $outDir "bwtest.mkv"
$sink = Start-Process ffmpeg -PassThru -WindowStyle Hidden -RedirectStandardError $sinkLog -ArgumentList @(
  "-hide_banner", "-loglevel", "info", "-y", "-listen", "1",
  "-i", '"rtmp://127.0.0.1:19355/app/k?bandwidthtest=true"', "-c", "copy", ('"' + $sinkFile + '"'))
Start-Sleep -Seconds 2

$snap = Join-Path $bed "snapshots"
if ($Snapshots) { if (Test-Path $snap) { Remove-Item -Recurse -Force $snap }; $env:RWS_MULTISTREAM_SNAPSHOT = $snap }
$env:RWS_MULTISTREAM_TEST_BWTEST = "Twitch test"
$proc = Start-KitObs $obs "MultistreamTest" "MultistreamTest"   # hidden, no focus steal
[void]$proc.Handle
Start-Sleep -Seconds $Seconds
[void](Close-KitObs $proc)   # hidden OBS: WM_CLOSE to its window (CloseMainWindow needs a visible one)
Remove-Item Env:RWS_MULTISTREAM_TEST_BWTEST, Env:RWS_MULTISTREAM_SNAPSHOT -ErrorAction SilentlyContinue
Start-Sleep -Seconds 1
if (!$sink.HasExited) { $sink.Kill() }
[void][RWS.CredB]::CredDeleteW("RedWardenStudios/Multistream/$id", 1, 0)

$fail = $false
function Check($ok, $msg) { if ($ok) { Write-Host "ok:   $msg" } else { Write-Host "FAIL: $msg"; $script:fail = $true } }
$log = Get-ChildItem (Join-Path $cfg "logs") | Sort-Object LastWriteTime -Descending | Select-Object -First 1
$text = Get-Content $log.FullName -Raw
if (!(Write-KitChecks (Test-KitLoadLog $text $cfg))) { $fail = $true }
Select-String -Path $log.FullName -Pattern "\[rws-multistream\]" | ForEach-Object { $_.Line }
Write-Host "--- receiver ---"; Get-Content $sinkLog | Select-Object -Last 6
$frames = 0
if (Test-Path $sinkFile) { $frames = [int](ffprobe -v error -count_packets -select_streams v -show_entries stream=nb_read_packets -of csv=p=0 $sinkFile) }
Check ($text -match "Twitch bandwidth test started") "bandwidth test started"
# ffmpeg names a mismatched stream ("Unexpected stream k, expecting k?bandwidthtest=true").
Check ((Test-Path $sinkLog) -and -not (Select-String -Path $sinkLog -Pattern "Unexpected stream" -Quiet)) "stream key went out as k?bandwidthtest=true"
Check ($frames -ge 30 * 15) "receiver got the test stream ($frames frames at 30 fps)"
Check ($frames -le 30 * 30) "the test stopped by itself"
Check ($text -match "bandwidth test verdict: Twitch received a steady") "verdict: steady"
Check (-not ($text -match "==== Streaming Start")) "OBS's own stream never started"
Check ($proc.ExitCode -eq 0) "OBS shut down cleanly (exit code $($proc.ExitCode))"
if ($fail) { Write-Host "RESULT: FAIL"; exit 1 } else { Write-Host "RESULT: PASS" }

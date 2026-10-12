<#
  Red Warden Multistream import test. Same isolated portable OBS as test-local.ps1.

  The test profile starts with no Red Warden destinations, but with settings
  files from obs-multi-rtmp and Aitum Multistream next to it. Checks that:
    - the empty dock offers the import (snapshot: .testbed\snapshots)
    - the import dialog lists them (snapshot: import.png)
    - importing creates switched-off destinations with the right platform,
      encoder and audio settings, keys in Credential Manager, and skips the
      SRT target and the duplicate saved in both plugins.

  Run from the Multistream folder:  powershell -ExecutionPolicy Bypass -File tools\test-import.ps1
#>
param([string]$Config = "RelWithDebInfo", [int]$Seconds = 14)
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

# Settings left behind by the other plugins (formats as they write them).
Set-Content (Join-Path $prof "obs-multi-rtmp.json") @'
{"targets":[
 {"id":"t1","name":"YouTube main","protocol":"RTMP","service-param":{"server":"rtmps://a.rtmps.youtube.com:443/live2","key":"yt-test-key"}},
 {"id":"t2","name":"Kick","protocol":"RTMP","service-param":{"server":"rtmps://fa723fc1b171.global-contribute.live-video.net/app","key":"sk_test_kick"},
  "video-config":"v1","audio-config":"a1"},
 {"id":"t3","name":"SRT relay","protocol":"SRT","service-param":{"server":"srt://10.0.0.5:9000","key":""}}
],
"video_configs":[{"id":"v1","encoder":"no_such_encoder","param":{"bitrate":4500},"resolution":"1280x720"}],
"audio_configs":[{"id":"a1","encoder":"ffmpeg_aac","param":{"bitrate":320},"mixerId":0}]}
'@ -Encoding UTF8
$aitumDir = Join-Path $cfg "plugin_config\aitum-multistream"
New-Item -ItemType Directory -Force $aitumDir | Out-Null
Set-Content (Join-Path $aitumDir "config.json") @'
{"profiles":[
 {"name":"Some other profile","outputs":[{"name":"Not this one","stream_server":"rtmp://live.twitch.tv/app","stream_key":"x"}]},
 {"name":"MultistreamTest","outputs":[
  {"name":"TikTok","stream_server":"rtmp://push-rtmp-l11-va01.tiktokcdn.com/game/","stream_key":"tt-test-key",
   "advanced":true,"video_encoder":"obs_x264","video_encoder_settings":{"bitrate":2500},"audio_encoder_settings":{"bitrate":192}},
  {"name":"YouTube (same as obs-multi-rtmp)","stream_server":"rtmps://a.rtmps.youtube.com:443/live2","stream_key":"yt-test-key"}
 ]}
]}
'@ -Encoding UTF8

Add-Type -Namespace RWS -Name CredR -MemberDefinition @'
[DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool CredReadW(string target, int type, int flags, out System.IntPtr cred);
[DllImport("advapi32.dll")] public static extern void CredFree(System.IntPtr cred);
[DllImport("advapi32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
public static extern bool CredDeleteW(string target, int type, int flags);
'@ -ErrorAction SilentlyContinue
function Get-KeyExists($id) {
  $p = [IntPtr]::Zero
  if ([RWS.CredR]::CredReadW("RedWardenStudios/Multistream/$id", 1, 0, [ref]$p)) { [RWS.CredR]::CredFree($p); return $true }
  return $false
}

$snap = Join-Path $bed "snapshots"
if (Test-Path $snap) { Remove-Item -Recurse -Force $snap }
$env:RWS_MULTISTREAM_SNAPSHOT = $snap
$env:RWS_MULTISTREAM_TEST_IMPORT = "1"

# Credential Manager targets are global to the Windows user: snapshot the RedWardenStudios/ targets BEFORE the run and delete,
# afterwards, exactly the ones that appeared during it (never an entry that was already there).
$credBefore = Get-KitCredTargets
$proc = Start-KitObs $obs "MultistreamTest" "MultistreamTest"   # hidden, no focus steal
[void]$proc.Handle  # keeps ExitCode readable after exit
Start-Sleep -Seconds $Seconds
[void](Close-KitObs $proc)   # hidden OBS: WM_CLOSE to its window (CloseMainWindow needs a visible one)
Remove-Item Env:RWS_MULTISTREAM_TEST_IMPORT, Env:RWS_MULTISTREAM_SNAPSHOT -ErrorAction SilentlyContinue

# verdict
$fail = $false
function Check($ok, $msg) { if ($ok) { Write-Host "ok:   $msg" } else { Write-Host "FAIL: $msg"; $script:fail = $true } }
$log = Get-ChildItem (Join-Path $cfg "logs") | Sort-Object LastWriteTime -Descending | Select-Object -First 1
Select-String -Path $log.FullName -Pattern "\[rws-multistream\]" | ForEach-Object { $_.Line }
$text = Get-KitLogText $cfg
if (!(Write-KitChecks (Test-KitLoadLog $text $cfg))) { $fail = $true }
Check (Test-Path (Join-Path $snap "dock-1-w380.png")) "empty dock snapshot taken (shows the import offer)"
Check (Test-Path (Join-Path $snap "import.png")) "import dialog snapshot taken"
$cfgFile = Join-Path $prof "rws-multistream.json"
Check (Test-Path $cfgFile) "destinations saved"
$dests = @()
if (Test-Path $cfgFile) { $dests = @((Get-Content $cfgFile -Raw | ConvertFrom-Json).destinations) }
Write-Host ("Imported: " + (($dests | ForEach-Object { "$($_.name) [$($_.platform)]" }) -join ", "))
Check ($dests.Count -eq 3) "3 destinations imported (SRT skipped, YouTube duplicate skipped)"
Check (@($dests | Where-Object { $_.enabled }).Count -eq 0) "every imported destination starts switched off"
$yt = $dests | Where-Object { $_.name -eq "YouTube main" }
$kick = $dests | Where-Object { $_.name -eq "Kick" }
$tt = $dests | Where-Object { $_.name -eq "TikTok" }
Check ($yt -and $yt.platform -eq "youtube" -and $yt.encoder.shared) "YouTube: shares OBS's encoder"
Check ($kick -and $kick.platform -eq "kick" -and -not $kick.encoder.shared -and $kick.encoder.video_bitrate -eq 4500 -and $kick.encoder.width -eq 1280) "Kick: own encoder, 4500 kbps, 1280x720"
Check ($kick -and [string]::IsNullOrEmpty($kick.encoder.encoder_id)) "Kick: unknown encoder falls back to the default"
Check ($kick -and $kick.encoder.audio_bitrate -eq 320 -and $kick.encoder.audio_track -eq 0) "Kick: audio 320 kbps on track 1"
Check ($tt -and $tt.platform -eq "tiktok" -and $tt.encoder.encoder_id -eq "obs_x264" -and $tt.encoder.audio_bitrate -eq 192) "TikTok (Aitum): own x264 encoder, 192 kbps audio"
Check (-not ($dests | Where-Object { $_.name -eq "Not this one" })) "Aitum outputs from other profiles left alone"
foreach ($d in $dests) {
  Check (Get-KeyExists $d.id) "$($d.name): key stored in Credential Manager"
}
$created = @(Remove-KitNewCredTargets $credBefore)
Write-Host ("Credential targets created by this run and deleted: " + ($created -join ", "))
Check ($created.Count -eq $dests.Count -and @($dests | Where-Object { $created -notcontains ("RedWardenStudios/Multistream/" + $_.id) }).Count -eq 0) "exactly the $($dests.Count) imported destinations created Credential Manager targets, and only those were deleted"
Check ($proc.ExitCode -eq 0) "OBS shut down cleanly (exit code $($proc.ExitCode))"
if ($fail) { Write-Host "RESULT: FAIL"; exit 1 } else { Write-Host "RESULT: PASS" }

# Red Warden Stream Kit: shared portable-OBS helpers for the testbed harnesses (dot-sourced). ASCII only.
#
#   . (Join-Path <plugin>\tools "kit-testbed.ps1")
#   $kit = Get-KitTestbed "RelWithDebInfo"
#   Initialize-KitPortableObs $kit.Obs
#   Install-KitPlugin $kit            # exactly ONE Kit DLL + locale; old DLLs and data folders removed
#   $proc = Start-KitObs $kit.Obs "MyProfile" "MyCollection" @("--startstreaming")
#   ...
#   Close-KitObs $proc                # hidden OBS has no MainWindowHandle: posts WM_CLOSE, falls back to its PID only
#   $text = Get-KitLogText $kit.Cfg
#   Write-KitChecks (Test-KitLoadLog $text $kit.Cfg)
#
# Nothing here ever touches C:\Program Files\obs-studio (read-only copy source) or %APPDATA%\obs-studio, and
# nothing kills a process by image name.

$script:KitPluginRoot = Split-Path -Parent $PSScriptRoot      # ...\plugin
# Layout. Published repository: multistream\ preflight\ inflight\ sit next to plugin\. Development tree: sibling
# red-warden-<tool>\ folders one level above the Kit folder.
$script:KitRepoRoot = Split-Path -Parent $script:KitPluginRoot
$script:KitPublicLayout = Test-Path (Join-Path $script:KitRepoRoot "multistream\src")
$script:KitProjectsRoot = if ($script:KitPublicLayout) { $script:KitRepoRoot } else { Split-Path -Parent $script:KitRepoRoot }
# The source folder of a tool ("multistream", "preflight" or "inflight") in either layout.
function Get-KitToolDir([string]$Name) {
  if ($script:KitPublicLayout) { return (Join-Path $script:KitRepoRoot $Name) }
  return (Join-Path $script:KitProjectsRoot "red-warden-$Name")
}

# Paths of the Kit testbed. The portable OBS lives in <plugin>\.testbed\obs (gitignored).
function Get-KitTestbed([string]$Config = "RelWithDebInfo") {
  $root = $script:KitPluginRoot
  $bed = Join-Path $root ".testbed"
  $obs = Join-Path $bed "obs"
  New-Item -ItemType Directory -Force $bed | Out-Null
  return @{
    Root = $root; Bed = $bed; Obs = $obs
    Cfg = Join-Path $obs "config\obs-studio"
    Dll = Join-Path $root "build_x64\$Config\rws-stream-kit.dll"
    Pdb = Join-Path $root "build_x64\$Config\rws-stream-kit.pdb"
    Locale = Join-Path $root "data\locale\en-US.ini"
    Dumps = Join-Path $bed "crashdumps"   # crash dumps of the test OBS (Windows LocalDumps for obs64.exe, ProcDump); inside the git-ignored .testbed
  }
}

# First use: copy a portable OBS into $Obs (from the folder named by $env:KIT_OBS_SOURCE if set, else the Multistream
# testbed copy if present, else a read-only robocopy of C:\Program Files\obs-studio). Returns nothing; throws on failure.
function Initialize-KitPortableObs([string]$Obs) {
  if (Test-Path (Join-Path $Obs "bin\64bit\obs64.exe")) { return }
  $src = $env:KIT_OBS_SOURCE
  if (!$src -or !(Test-Path (Join-Path $src "bin\64bit\obs64.exe"))) { $src = Join-Path (Get-KitToolDir "multistream") ".testbed\obs" }
  if (!(Test-Path (Join-Path $src "bin\64bit\obs64.exe"))) { $src = "C:\Program Files\obs-studio" }
  Write-Host "Copying OBS into the Kit testbed (one time) from $src ..."
  robocopy $src $Obs /E /MT:16 /NFL /NDL /NJH /NJS /NP | Out-Null
  if ($LASTEXITCODE -ge 8) { throw "robocopy of portable OBS failed, exit $LASTEXITCODE" }
  New-Item -ItemType File -Force (Join-Path $Obs "portable_mode.txt") | Out-Null
}

# Waits (never kills) for any process running THIS testbed's obs64.exe to exit. Returns $true when none is left.
function Wait-KitObsExit([string]$ObsExe, [int]$Seconds = 30) {
  $full = [IO.Path]::GetFullPath($ObsExe)
  for ($i = 0; $i -le $Seconds; $i++) {
    $left = @(Get-Process -Name obs64 -ErrorAction SilentlyContinue | Where-Object { $_.Path -and ([IO.Path]::GetFullPath($_.Path) -eq $full) })
    if ($left.Count -eq 0) { return $true }
    if ($i -lt $Seconds) { Start-Sleep -Seconds 1 }
  }
  return $false
}

# Clears the throwaway OBS config. Residual handles (CEF helpers) may linger after the OBS exit: retry up to 30 s.
function Clear-KitConfig([string]$Cfg) {
  for ($try = 0; $try -lt 30 -and (Test-Path $Cfg); $try++) {
    Remove-Item -Recurse -Force $Cfg -ErrorAction SilentlyContinue
    if (Test-Path $Cfg) { Start-Sleep -Seconds 1 }
  }
  if (Test-Path $Cfg) { throw "could not clear the throwaway config (still locked): $Cfg" }
}

# Installs exactly ONE plugin DLL (rws-stream-kit.dll, plus its pdb) and its locale into the portable OBS.
# First REMOVES the three old standalone plugins (dll/pdb/exp/lib and data folders), so a stale copy can never
# be loaded next to the Kit. Returns a list of what it did (strings).
function Install-KitPlugin($Kit) {
  if (!(Test-Path $Kit.Dll)) { throw "Build first: $($Kit.Dll) not found" }
  $log = @()
  Restore-KitObsWebsocket $Kit.Obs   # a previous -NoWebsocket run that died must never leave obs-websocket disabled
  $pbin = Join-Path $Kit.Obs "obs-plugins\64bit"
  $pdat = Join-Path $Kit.Obs "data\obs-plugins"
  New-Item -ItemType Directory -Force $pbin | Out-Null
  foreach ($old in "rws-multistream", "rws-preflight", "rws-inflight") {
    $files = @(Get-ChildItem $pbin -Filter "$old.*" -ErrorAction SilentlyContinue)
    foreach ($f in $files) { Remove-Item $f.FullName -Force; $log += "removed old plugin file $($f.Name)" }
    $d = Join-Path $pdat $old
    if (Test-Path $d) { Remove-Item $d -Recurse -Force; $log += "removed old data folder $old" }
  }
  $kitData = Join-Path $pdat "rws-stream-kit"
  if (Test-Path $kitData) { Remove-Item $kitData -Recurse -Force }
  $loc = Join-Path $kitData "locale"
  New-Item -ItemType Directory -Force $loc | Out-Null
  Copy-Item $Kit.Dll $pbin -Force
  if (Test-Path $Kit.Pdb) { Copy-Item $Kit.Pdb $pbin -Force }
  Copy-Item $Kit.Locale $loc -Force
  $dlls = @(Get-ChildItem $pbin -Filter "rws-*.dll" | ForEach-Object { $_.Name })
  $log += "installed rws-stream-kit.dll; rws-*.dll now in obs-plugins\64bit: $($dlls -join ', ')"
  return $log
}

# Starts the portable OBS HIDDEN (no window, no focus steal) and returns the Process. Never a bare Start-Process.
function Start-KitObs([string]$Obs, [string]$ProfileName, [string]$Collection, [string[]]$Extra = @()) {
  $exeDir = Join-Path $Obs "bin\64bit"
  $args2 = @("--portable", "--multi", "--profile", $ProfileName, "--collection", $Collection) + $Extra +
           @("--disable-updater", "--disable-missing-files-check")
  $p = Start-Process (Join-Path $exeDir "obs64.exe") -WindowStyle Hidden -WorkingDirectory $exeDir -PassThru -ArgumentList $args2
  [void]$p.Handle   # keeps ExitCode readable after exit
  Start-KitWindowWatch $p
  return $p
}

Add-Type -Namespace RWS -Name KitWin -MemberDefinition @"
public delegate bool EnumProc(System.IntPtr h, System.IntPtr l);
[DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc p, System.IntPtr l);
[DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(System.IntPtr h, out uint pid);
[DllImport("user32.dll")] public static extern bool PostMessage(System.IntPtr h, uint m, System.IntPtr w, System.IntPtr l);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(System.IntPtr h, System.Text.StringBuilder s, int n);
[DllImport("user32.dll")] public static extern bool IsWindowVisible(System.IntPtr h);
[DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassName(System.IntPtr h, System.Text.StringBuilder s, int n);
"@ -ErrorAction SilentlyContinue

# Posts WM_CLOSE to the (hidden) main window of $Proc (title "OBS ..."): what File > Exit / the X button does.
# A hidden window has no MainWindowHandle, so Process.CloseMainWindow() cannot be used. Returns the number of
# windows it posted to and remembers it in $script:KitLastPosted (read by Test-KitClosePath).
$script:KitLastPosted = -1
function Send-KitWmClose($Proc) {
  $wins = New-Object System.Collections.ArrayList
  $cb = [RWS.KitWin+EnumProc]{ param($h, $l)
    $wp = [uint32]0; [void][RWS.KitWin]::GetWindowThreadProcessId($h, [ref]$wp)
    if ($wp -eq $Proc.Id) {
      $sb = New-Object System.Text.StringBuilder 256
      [void][RWS.KitWin]::GetWindowText($h, $sb, 256)
      if ($sb.ToString() -like 'OBS *') { [void]$wins.Add($h) }
    }
    return $true }
  [void][RWS.KitWin]::EnumWindows($cb, [IntPtr]::Zero)
  foreach ($h in $wins) { [void][RWS.KitWin]::PostMessage($h, 0x10, [IntPtr]::Zero, [IntPtr]::Zero) }
  $script:KitLastPosted = $wins.Count
  return $wins.Count
}

# Closes a (hidden) OBS via Send-KitWmClose. Returns $true only if WM_CLOSE reached at least one OBS window AND OBS then
# exited within $Seconds. If no window got the message it returns $false (a process that exits by itself, or that has to be
# stopped, does not count as a close through the window). If OBS is still there after $Seconds it is stopped by PID (this
# process only) with a warning.
function Close-KitObs($Proc, [int]$Seconds = 20) {
  $n = Send-KitWmClose $Proc
  if ($Proc.WaitForExit($Seconds * 1000)) { return ($n -gt 0) }
  Write-Warning "OBS did not exit cleanly ($n window(s) closed); stopping pid $($Proc.Id)"
  $Proc.Kill()
  return $false
}
# Close-path assertion (separate from the clean-exit assertion): WM_CLOSE was posted to at least one OBS window.
function Test-KitClosePath {
  New-KitCheck ($script:KitLastPosted -gt 0) "close path: WM_CLOSE was posted to $($script:KitLastPosted) OBS window(s) (must be at least 1)"
}

# ---- visible-window monitor -----------------------------------------------------------------------------------------
# A hidden OBS must never put a window on the screen. Start-KitObs starts a background sampler (every 250 ms, from process
# start until the process is gone or Stop-KitWindowWatch) that records every visible top-level window of the captured PID.
# Stop-KitWindowWatch (call it AFTER OBS has exited, so shutdown is covered) returns the distinct "title [class]" strings.
$script:KitWatch = $null
function Start-KitWindowWatch($Proc) {
  $state = [hashtable]::Synchronized(@{ Stop = $false; Seen = [hashtable]::Synchronized(@{}); Samples = 0; Pid = $Proc.Id })
  $rs = [runspacefactory]::CreateRunspace()
  $rs.Open()
  $rs.SessionStateProxy.SetVariable('state', $state)
  $ps = [powershell]::Create()
  $ps.Runspace = $rs
  [void]$ps.AddScript({
    while (-not $state.Stop) {
      $alive = $true
      try { [void][Diagnostics.Process]::GetProcessById($state.Pid) } catch { $alive = $false }
      if (-not $alive) { break }
      $cb = [RWS.KitWin+EnumProc]{ param($h, $l)
        $wp = [uint32]0; [void][RWS.KitWin]::GetWindowThreadProcessId($h, [ref]$wp)
        if ($wp -eq $state.Pid -and [RWS.KitWin]::IsWindowVisible($h)) {
          $sb = New-Object System.Text.StringBuilder 256
          [void][RWS.KitWin]::GetWindowText($h, $sb, 256)
          $cl = New-Object System.Text.StringBuilder 256
          [void][RWS.KitWin]::GetClassName($h, $cl, 256)
          $key = "'" + $sb.ToString() + "' [" + $cl.ToString() + "]"
          if (-not $state.Seen.ContainsKey($key)) { $state.Seen[$key] = [DateTime]::Now.ToString("HH:mm:ss.fff") }
        }
        return $true }
      [void][RWS.KitWin]::EnumWindows($cb, [IntPtr]::Zero)
      $state.Samples = $state.Samples + 1
      Start-Sleep -Milliseconds 250
    }
  })
  $h = $ps.BeginInvoke()
  $script:KitWatch = @{ Ps = $ps; Rs = $rs; State = $state; Handle = $h }
}
function Stop-KitWindowWatch {
  $w = $script:KitWatch
  if (!$w) { return @() }
  $w.State.Stop = $true
  try { [void]$w.Ps.EndInvoke($w.Handle) } catch { }
  $seen = @($w.State.Seen.Keys | ForEach-Object { $_ + " first seen " + $w.State.Seen[$_] })   # time = same local clock as the OBS log
  $script:KitWatchSamples = $w.State.Samples
  try { $w.Ps.Dispose(); $w.Rs.Dispose() } catch { }
  $script:KitWatch = $null
  return $seen
}
function Test-KitHidden($Seen, [string]$AllowPattern = "") {
  $bad = @($Seen | Where-Object { -not $AllowPattern -or $_ -notlike $AllowPattern })
  New-KitCheck ($bad.Count -eq 0) "hidden launch: no visible top-level window of the OBS pid in $($script:KitWatchSamples) samples (every 250 ms, start through exit)$(if ($AllowPattern) { '; only allowed: ' + $AllowPattern }) (seen: $($Seen -join ', '))"
}

# Newest OBS log of the portable copy, read with sharing (OBS may still hold it open).
function Get-KitLogFile([string]$Cfg) {
  Get-ChildItem (Join-Path $Cfg "logs") -ErrorAction SilentlyContinue | Sort-Object LastWriteTime -Descending | Select-Object -First 1
}
function Get-KitLogText([string]$Cfg) {
  $f = Get-KitLogFile $Cfg
  if (!$f) { return "" }
  $fs = [IO.File]::Open($f.FullName, 'Open', 'Read', 'ReadWrite')
  try { $sr = New-Object IO.StreamReader($fs); return $sr.ReadToEnd() } finally { $fs.Close() }
}

function New-KitCheck([bool]$Ok, [string]$Msg) { [pscustomobject]@{ Ok = $Ok; Msg = $Msg } }

# Prints "ok:   msg" / "FAIL: msg" for each check; returns $true when every check passed.
function Write-KitChecks($Checks) {
  $all = $true
  foreach ($c in @($Checks)) {
    if ($c.Ok) { Write-Host "ok:   $($c.Msg)" } else { Write-Host "FAIL: $($c.Msg)"; $all = $false }
  }
  return $all
}

# DockState is the base64 Qt QMainWindow::saveState blob in OBS's ini. Each dock is a record that starts with its objectName
# written by QDataStream as a QString: a 4-byte BIG-endian BYTE length followed by the UTF-16BE characters. This parses
# for exactly that record (length prefix must equal 2 x characters) instead of scanning raw bytes for the text, and
# returns how many such records each id has: $null when OBS saved no DockState, else id -> count (0 = absent).
function Get-KitSavedDockCounts([string]$Cfg, [string[]]$Ids) {
  foreach ($ini in "user.ini", "global.ini") {
    $f = Join-Path $Cfg $ini
    if (!(Test-Path $f)) { continue }
    $m = [regex]::Match([IO.File]::ReadAllText($f), '(?m)^DockState=(.+?)\s*$')
    if (!$m.Success) { continue }
    $b64 = $m.Groups[1].Value
    if ($b64.StartsWith("@ByteArray(")) { $b64 = $b64.Substring(11).TrimEnd(')') }
    try { $bytes = [Convert]::FromBase64String($b64) } catch { continue }
    $counts = @{}
    foreach ($id in $Ids) {
      $needle = [Text.Encoding]::BigEndianUnicode.GetBytes($id)
      $n = 0
      for ($i = 4; $i -le $bytes.Length - $needle.Length; $i++) {
        if ($bytes[$i] -ne $needle[0]) { continue }
        $hit = $true
        for ($j = 1; $j -lt $needle.Length; $j++) { if ($bytes[$i + $j] -ne $needle[$j]) { $hit = $false; break } }
        if (!$hit) { continue }
        $len = ([int]$bytes[$i - 4] * 16777216) + ([int]$bytes[$i - 3] * 65536) + ([int]$bytes[$i - 2] * 256) + [int]$bytes[$i - 1]
        if ($len -eq $needle.Length) { $n++ }
      }
      $counts[$id] = $n
    }
    return $counts
  }
  return $null
}
# Back-compat: the dock ids from $Ids that have a record in the saved state.
function Get-KitSavedDockIds([string]$Cfg, [string[]]$Ids) {
  $c = Get-KitSavedDockCounts $Cfg $Ids
  if ($null -eq $c) { return @() }
  return @($Ids | Where-Object { $c[$_] -ge 1 })
}

# Module / vendor / dock assertions on the OBS log of ONE run with ONLY the Kit DLL installed. Every log pattern is anchored
# to a whole log line ("HH:MM:SS.mmm: [prefix] full message"), not a substring.
#   - the Kit module loaded exactly once ("[rws-stream-kit] Red Warden Stream Kit loaded (version X)"), one rws-stream-kit.dll
#     in OBS's module list, no old rws-multistream/rws-preflight/rws-inflight dll listed, no "could not register the dock"
#   - every tool in -Tools loaded exactly once with its own prefix and the version $ExpectVersion, its panel created once
#   - ONE dock for the whole Kit: "[rws-stream-kit] dock registered (id rws-stream-kit)" once; saved DockState (parsed,
#     see Get-KitSavedDockCounts): the id rws-stream-kit has exactly ONE record after the clean exit and the three OLD dock ids
#     (rws-multistream, rws-preflight-dock, rws-inflight-dock) have NONE
#   - Multistream: one obs-websocket vendor "RedWardenMultistream" and no other Kit vendor registration; with -NoVendor
#     (obs-websocket removed from the portable copy): NO vendor line and the "obs-websocket not available" line instead
function Test-KitLoadLog([string]$Text, [string]$Cfg, [string[]]$Tools = @("multistream"), [string[]]$DockIds = @(), [string]$ExpectVersion = "1.1.0", [switch]$NoVendor) {
  $r = @()
  $ts = '(?m)^\d\d:\d\d:\d\d\.\d+: '
  $eol = '\r?$'
  function Count($rx) { ([regex]::Matches($Text, $rx)).Count }
  $ver = [regex]::Escape($ExpectVersion)
  $kitLoaded = Count ($ts + '\[rws-stream-kit\] Red Warden Stream Kit loaded \(version [0-9.]+\)' + $eol)
  $r += New-KitCheck ($kitLoaded -eq 1) "Kit module loaded exactly once (line '[rws-stream-kit] Red Warden Stream Kit loaded (version ...)' x$kitLoaded)"
  $kitV = Count ($ts + '\[rws-stream-kit\] Red Warden Stream Kit loaded \(version ' + $ver + '\)' + $eol)
  $r += New-KitCheck ($kitV -eq 1) "Kit load line reports version $ExpectVersion exactly (x$kitV)"
  $kitListed = Count ($ts + '\s+rws-stream-kit\.dll\s*$')
  $r += New-KitCheck ($kitListed -eq 1) "OBS module list shows rws-stream-kit.dll exactly once (x$kitListed)"
  $oldListed = Count ($ts + '\s+rws-(multistream|preflight|inflight)\.dll\s*$')
  $r += New-KitCheck ($oldListed -eq 0) "no old standalone rws-multistream/preflight/inflight dll loaded (x$oldListed)"
  $fails = Count '(?i)failed to load[^\r\n]*rws|rws[^\r\n]*failed to load|could not load[^\r\n]*rws'
  $r += New-KitCheck ($fails -eq 0) "no module load failure for an rws plugin (x$fails)"
  $dockErr = Count ($ts + '\[rws-(stream-kit|multistream|preflight|inflight)\] could not register the dock' + $eol)
  $r += New-KitCheck ($dockErr -eq 0) "no '[rws-<kit or tool>] could not register the dock' line (x$dockErr)"
  $kitDock = Count ($ts + '\[rws-stream-kit\] dock registered \(id rws-stream-kit\)' + $eol)
  $r += New-KitCheck ($kitDock -eq 1) "the ONE Kit dock registered: line '[rws-stream-kit] dock registered (id rws-stream-kit)' once (x$kitDock)"
  $ids = @("rws-stream-kit")
  if ($Tools -contains "multistream") {
    $n = Count ($ts + '\[rws-multistream\] Red Warden Multistream loaded \(version ' + $ver + '\)' + $eol)
    $r += New-KitCheck ($n -eq 1) "Multistream loaded exactly once, line '[rws-multistream] Red Warden Multistream loaded (version $ExpectVersion)' (x$n)"
    $vendor = Count ($ts + '\[rws-multistream\] obs-websocket vendor "RedWardenMultistream" registered' + $eol)
    $anyVendor = Count ($ts + '\[rws-[a-z]+\] obs-websocket vendor "[^"]*" registered' + $eol)
    if ($NoVendor) {
      $nw = Count ($ts + '\[rws-multistream\] obs-websocket not available; remote control disabled' + $eol)
      $r += New-KitCheck ($vendor -eq 0 -and $anyVendor -eq 0) "obs-websocket removed: no vendor registered (x$anyVendor)"
      $r += New-KitCheck ($nw -eq 1) "obs-websocket removed: '[rws-multistream] obs-websocket not available; remote control disabled' once (x$nw)"
    } else {
      $r += New-KitCheck ($vendor -eq 1) "exactly one obs-websocket vendor RedWardenMultistream registered (x$vendor)"
      $r += New-KitCheck ($anyVendor -eq 1) "exactly one Kit vendor registration in total (x$anyVendor)"
    }
  }
  if ($Tools -contains "preflight") {
    $n = Count ($ts + '\[rws-preflight\] loaded version ' + $ver + $eol)
    $r += New-KitCheck ($n -eq 1) "Pre-Flight loaded exactly once, line '[rws-preflight] loaded version $ExpectVersion' (x$n)"
    $d = Count ($ts + '\[rws-preflight\] panel created \(shown in the Stream Kit dock\)' + $eol)
    $r += New-KitCheck ($d -eq 1) "Pre-Flight line '[rws-preflight] panel created (shown in the Stream Kit dock)' once (x$d)"
  }
  if ($Tools -contains "inflight") {
    $n = Count ($ts + '\[rws-inflight\] loaded \(version ' + $ver + '\)' + $eol)
    $r += New-KitCheck ($n -eq 1) "In-Flight loaded exactly once, line '[rws-inflight] loaded (version $ExpectVersion)' (x$n)"
    $d = Count ($ts + '\[rws-inflight\] panel created \(shown in the Stream Kit dock\)' + $eol)
    $r += New-KitCheck ($d -eq 1) "In-Flight line '[rws-inflight] panel created (shown in the Stream Kit dock)' once (x$d)"
  }
  if ($Cfg) {
    if ($DockIds.Count -gt 0) { $ids = $DockIds }
    $old = @("rws-multistream", "rws-preflight-dock", "rws-inflight-dock")   # the earlier per-tool docks: never registered any more
    $c = Get-KitSavedDockCounts $Cfg ($ids + $old)
    if ($null -eq $c) {
      $r += New-KitCheck $false "OBS saved no DockState at exit (cannot check dock ids)"
    } else {
      foreach ($id in $ids) { $r += New-KitCheck ($c[$id] -eq 1) "saved DockState has exactly one record for dock id $id after exit (x$($c[$id]))" }
      foreach ($id in $old) { $r += New-KitCheck ($c[$id] -eq 0) "saved DockState has NO record for the old per-tool dock id $id (x$($c[$id]))" }
    }
  }
  return $r
}

# The update notice. The Kit has ONE scheduler (the Kit dock, plugin\src\kit\kit-dock.cpp; the tools carry or
# carried the checker class but never start it). The scheduler itself logs NOTHING; the only line any of it writes is
# "Stream Kit update available: ..." when the manifest lists a newer version. In the testbed (an OBS under a
# .testbed folder) the Kit dock does not even schedule the check (no network in the harness), so the log cannot show
# the scheduler run. What can be asserted: at most one notice line, none from Pre-Flight/In-Flight's prefixes
# (the old Pre-Flight scheduling/notice path), and statically exactly one UpdateChecker::start call site in all
# three source trees (Test-KitUpdateSource).
function Test-KitUpdateNotice([string]$Text) {
  $r = @()
  $n = ([regex]::Matches($Text, 'Stream Kit update available')).Count
  $r += New-KitCheck ($n -le 1) "at most one 'Stream Kit update available' notice (x$n)"
  $other = ([regex]::Matches($Text, '(?im)^[^\r\n]*\[rws-(preflight|inflight)\][^\r\n]*update[^\r\n]*$')).Count
  $r += New-KitCheck ($other -eq 0) "Pre-Flight / In-Flight log no update scheduling or notice line (x$other)"
  return $r
}
function Test-KitUpdateSource {
  $r = @()
  $hits = @()
  foreach ($t in "multistream\src", "preflight\src", "inflight\src", "plugin\src\kit", "plugin\src") {
    $dir = if ($t -like "plugin*") { Join-Path $script:KitPluginRoot $t.Substring(7) } else { Join-Path (Get-KitToolDir $t.Split("\")[0]) "src" }
    foreach ($f in Get-ChildItem $dir -Filter *.cpp -ErrorAction SilentlyContinue) {
      foreach ($m in (Select-String -Path $f.FullName -Pattern '\bupdates_?\.start\(' | Where-Object { $_.Line -notmatch '^\s*//' })) { $hits += "$($t)\$($f.Name):$($m.LineNumber)" }
    }
  }
  $r += New-KitCheck ($hits.Count -eq 1 -and $hits[0] -like '*kit-dock.cpp*') "static: exactly one update scheduler call site (UpdateChecker start), in the Kit dock (found: $($hits -join '; '))"
  return $r
}

# ---- obs-websocket absent (the -nows steps) ------------------------------------------------------------------------
# Renames obs-websocket.dll in the PORTABLE copy to obs-websocket.dll.kitoff so OBS does not load it (Multistream then logs
# "obs-websocket not available" and registers no vendor; Pre-Flight/In-Flight fall back to "not found"). Always pair with
# Restore-KitObsWebsocket in a finally block; Install-KitPlugin restores too, so a crashed run cannot leave it disabled.
function Disable-KitObsWebsocket([string]$Obs) {
  $d = Join-Path $Obs "obs-plugins\64bit\obs-websocket.dll"
  if (!(Test-Path $d)) { throw "obs-websocket.dll not found in the portable copy: $d" }
  Rename-Item $d "obs-websocket.dll.kitoff"
}
function Restore-KitObsWebsocket([string]$Obs) {
  $d = Join-Path $Obs "obs-plugins\64bit\obs-websocket.dll"
  $off = $d + ".kitoff"
  if ((Test-Path $off) -and !(Test-Path $d)) { Rename-Item $off "obs-websocket.dll" }
}

# ---- Credential Manager: only what THIS run created -----------------------------------------------------------------
# Credential Manager targets are global to the Windows user (portable OBS or not). Snapshot the RedWardenStudios/ targets
# before the run; afterwards delete exactly the targets that were not there before.
function Get-KitCredTargets {
  $t = @()
  foreach ($l in (& cmdkey /list 2>$null)) { $m = [regex]::Match("$l", 'target=(RedWardenStudios/\S+)'); if ($m.Success) { $t += $m.Groups[1].Value } }
  return @($t | Sort-Object -Unique)
}
function Remove-KitNewCredTargets([string[]]$Before) {
  $new = @(Get-KitCredTargets | Where-Object { $Before -notcontains $_ })
  foreach ($t in $new) { [void](& cmdkey "/delete:$t" 2>$null) }
  return $new
}

# ---- seeded settings were LOADED (not just left on disk) --------------------------------------------------------------
# Observable effects of the seeds in the log: Pre-Flight with setup_done + a custom item reports "state Setup=Hidden" (default
# without settings: Amber) and "state Custom=Red" (an unticked custom item; default Hidden); In-Flight with levels_mode lufs +
# levels_true_peak logs "levels: mode lufs" and "levels: true peak on" at start. The shared update preference has no
# observable effect in the testbed (no scheduler runs there; deferred with the update-path test).
function Test-KitSeedLoaded([string]$Text, [string[]]$Which) {
  $r = @()
  if ($Which -contains "preflight") {
    $a = ([regex]::Matches($Text, '(?m)^\S+ \[rws-preflight\] state Setup=Hidden\r?$|^\d\d:\d\d:\d\d\.\d+: \[rws-preflight\] state Setup=Hidden')).Count
    $c = ([regex]::Matches($Text, '\[rws-preflight\] state Custom=Red')).Count
    $r += New-KitCheck ($a -ge 1 -and $c -ge 1) "seeded Pre-Flight settings were loaded: 'state Setup=Hidden' (x$a, default Amber) and 'state Custom=Red' for the seeded custom item (x$c, default Hidden)"
  }
  if ($Which -contains "inflight") {
    $m = ([regex]::Matches($Text, '\[rws-inflight\] levels: mode lufs')).Count
    $t = ([regex]::Matches($Text, '\[rws-inflight\] levels: true peak on')).Count
    $r += New-KitCheck ($m -ge 1 -and $t -ge 1) "seeded In-Flight settings were loaded: 'levels: mode lufs' (x$m) and 'levels: true peak on' (x$t) (defaults are dB mode, true peak off)"
  }
  return $r
}

# Check lists -> text lines / one verdict, for the harnesses that build a result as a list of lines.
function Get-KitCheckLines($Checks) { @($Checks) | ForEach-Object { if ($_.Ok) { "ok:   $($_.Msg)" } else { "FAIL: $($_.Msg)" } } }
function Test-KitChecksOk($Checks) { @(@($Checks) | Where-Object { -not $_.Ok }).Count -eq 0 }

# Visible top-level windows of $Proc (hidden OBS must have none: nothing may pop up on a screen).
function Get-KitVisibleWindows($Proc) {
  $vis = New-Object System.Collections.ArrayList
  $cb = [RWS.KitWin+EnumProc]{ param($h, $l)
    $wp = [uint32]0; [void][RWS.KitWin]::GetWindowThreadProcessId($h, [ref]$wp)
    if ($wp -eq $Proc.Id -and [RWS.KitWin]::IsWindowVisible($h)) {
      $sb = New-Object System.Text.StringBuilder 256
      [void][RWS.KitWin]::GetWindowText($h, $sb, 256)
      $cl = New-Object System.Text.StringBuilder 256
      [void][RWS.KitWin]::GetClassName($h, $cl, 256)
      [void]$vis.Add("'" + $sb.ToString() + "' [" + $cl.ToString() + "]")
    }
    return $true }
  [void][RWS.KitWin]::EnumWindows($cb, [IntPtr]::Zero)
  return @($vis)
}

# ---- pre-existing settings (they must survive the merge) ----------------------------------------------------
# Seeds the three settings files with recognisable values, as an upgrading user would have them:
#   plugin_config\rws-stream-kit\settings.json   {"check_updates":false}   (the shared update preference: off)
#   plugin_config\rws-preflight\settings.json    (legacy per-plugin check_updates=TRUE: if the Kit re-derived the shared
#                                                 value instead of reading it, the migration would flip it on)
#   plugin_config\rws-inflight\settings.json     (an unknown top-level key and a holding scene with an unknown key)
# -Which picks the files ("kit", "preflight", "inflight"). Written without a BOM.
$script:KitSeed = @{
  kit = '{"check_updates":false}'
  preflight = '{"setup_done":true,"use_main_stream":true,"use_multistream":false,"mic_uuid":"kit-seed-mic","start_scene_uuid":"kit-seed-scene","record_intent":false,"check_updates":true,"custom_items":[{"text":"Seed item"}]}'
  inflight = '{"setup_done":true,"mic_uuid":"kit-seed-mic2","desktop_uuid":"kit-seed-desk","check_updates":true,"levels_mode":"lufs","levels_true_peak":true,"levels_lufs_ref":-18.5,"kit_marker":"survives","holding_scenes":[{"scene_uuid":"kit-seed-scene","limit_seconds":300,"label":"Kit Seed Scene","future_key":"keep-me"}]}'
}
$script:KitSeedDirs = @{ kit = "rws-stream-kit"; preflight = "rws-preflight"; inflight = "rws-inflight" }
function Write-KitSeedSettings([string]$Cfg, [string[]]$Which = @("kit", "preflight", "inflight")) {
  foreach ($w in $Which) {
    $d = Join-Path $Cfg ("plugin_config\" + $script:KitSeedDirs[$w])
    New-Item -ItemType Directory -Force $d | Out-Null
    [IO.File]::WriteAllText((Join-Path $d "settings.json"), $script:KitSeed[$w], (New-Object Text.UTF8Encoding($false)))
  }
}
function Read-KitSeedJson([string]$Cfg, [string]$Which) {
  $f = Join-Path $Cfg ("plugin_config\" + $script:KitSeedDirs[$Which] + "\settings.json")
  if (!(Test-Path $f)) { return $null }
  try { return (Get-Content $f -Raw | ConvertFrom-Json) } catch { return $null }
}
# After a clean exit: every seeded file is still there, valid, and still holds its recognisable values. The shared
# update preference still reads OFF although the legacy Pre-Flight value says ON (so it was read, not re-migrated).
function Test-KitSeedSettings([string]$Cfg, [string[]]$Which = @("kit", "preflight", "inflight")) {
  $r = @()
  if ($Which -contains "kit") {
    $k = Read-KitSeedJson $Cfg "kit"
    $r += New-KitCheck ($k -ne $null -and $k.check_updates -eq $false) "shared plugin_config\rws-stream-kit\settings.json intact after exit, update preference still off (check_updates=$(if ($k) { $k.check_updates } else { 'FILE MISSING/INVALID' }))"
  }
  if ($Which -contains "preflight") {
    $p = Read-KitSeedJson $Cfg "preflight"
    $ok = $p -ne $null -and $p.setup_done -eq $true -and $p.mic_uuid -eq "kit-seed-mic" -and $p.start_scene_uuid -eq "kit-seed-scene" -and @($p.custom_items).Count -eq 1 -and $p.custom_items[0].text -eq "Seed item"
    $r += New-KitCheck $ok "plugin_config\rws-preflight\settings.json intact after exit (seeded mic, scene and custom item still there)"
    if ($Which -contains "kit") {
      $r += New-KitCheck ($p -ne $null -and $p.check_updates -eq $true) "legacy Pre-Flight check_updates=true left alone while the shared preference stays off (shared file read, not re-derived)"
    }
  }
  if ($Which -contains "inflight") {
    $i = Read-KitSeedJson $Cfg "inflight"
    $ok = $i -ne $null -and $i.setup_done -eq $true -and $i.mic_uuid -eq "kit-seed-mic2" -and $i.desktop_uuid -eq "kit-seed-desk" -and $i.levels_lufs_ref -eq -18.5 -and @($i.holding_scenes).Count -eq 1 -and $i.holding_scenes[0].label -eq "Kit Seed Scene" -and $i.holding_scenes[0].future_key -eq "keep-me"
    $r += New-KitCheck $ok "plugin_config\rws-inflight\settings.json intact after exit (seeded mic, desktop, LUFS reference, holding scene and its unknown key)"
    $r += New-KitCheck ($i -ne $null -and $i.kit_marker -eq "survives") "In-Flight kept an unknown top-level key (kit_marker) through the run"
  }
  return $r
}

# Each name in $Names must appear exactly once in the websocket GetHotkeyList result, and no rws_multistream.* name
# may appear twice. $HotkeyList is the hotkeys array from GetHotkeyList.
function Test-KitHotkeysOnce($HotkeyList, [string[]]$Names) {
  $r = @()
  $all = @($HotkeyList)
  foreach ($n in $Names) {
    $c = @($all | Where-Object { $_ -eq $n }).Count
    $r += New-KitCheck ($c -eq 1) "hotkey $n registered exactly once (x$c)"
  }
  $dups = @($all | Where-Object { $_ -like "rws_multistream.*" } | Group-Object | Where-Object { $_.Count -gt 1 })
  $r += New-KitCheck ($dups.Count -eq 0) "no rws_multistream.* hotkey registered twice ($(($dups | ForEach-Object { $_.Name }) -join ', '))"
  return $r
}

param([string[]]$Steps = @("configure", "build", "unit"), [int]$CostMinutes = 30, [int]$CostWarmupSeconds = 60)
# Red Warden Stream Kit (plugin\): configures, builds and runs the unit tests of all three tools plus the
# Kit's own tests. Results land in .testbed\run-*.txt and .testbed\status-*.txt. It takes
# several minutes, so run it in the background and hidden, never a bare Start-Process:
#   Start-Process powershell -WindowStyle Hidden -ArgumentList '-ExecutionPolicy','Bypass','-File','tools\run-all.ps1','-Steps','configure,build,unit' -WorkingDirectory <this plugin\ dir>
# Default steps (no -Steps): configure, build, unit.
# Harness steps (each launches the portable OBS in .testbed\obs HIDDEN with only rws-stream-kit.dll installed;
# the scripts live in the tools' own tools\ folders and share tools\kit-testbed.ps1):
#   ms-local, ms-resilience, ms-import, ms-bandwidth   (multistream\tools)
#   pf-load, pf-state, pf-integration, pf-ui           (preflight\tools: test-load/state/integration/ui)
#   if-load, if-audio, if-ui                           (inflight\tools: test-load/audio/ui)
#   pf-state-nows, if-load-nows                        (the same pf-state / if-load with obs-websocket removed from the portable OBS copy, restored after)
#   exit-stress                                         (tools\test-exit-stress.ps1: 20 OBS start/close cycles, exit code 0 each)
#   net-light                                           (tools\test-net-light.ps1: one stream, three outputs, one of them congested through a
#                                                        throttling relay: the Network light names it, the real "Turn off <destination>" button stops it,
#                                                        the light is green again, switching it back on mid-stream is judged again; offscreen shot
#                                                        .testbed\shots\kit-dock-net-red.png)
#   kit-dock                                            (tools\test-kit-dock.ps1: the one dock - first run docks it, second start does not, tools off,
#                                                        forced live; offscreen screenshots to .testbed\shots\kit-dock-*.png)
#   showcase                                            (OPTIONAL, in no release list and not read by check-test-results.ps1: tools\shots-showcase.ps1 builds
#                                                        a tidy realistic setup - Mic, Desktop Audio, Starting Soon/Live/Be Right Back/Ending, YouTube/Twitch/Kick -
#                                                        and writes the 8 clean 2x screenshots for the manual and the website to .testbed\shots\showcase\; it needs
#                                                        ffmpeg on PATH and takes about 6 minutes; run it as -Steps showcase)
#   cost                                                (OPTIONAL, in no release list and not read by check-test-results.ps1: tools\test-cost.ps1 measures what the
#                                                        Kit costs in CPU while streaming. Process-level A/B of obs64's CPU time (A Kit not installed, B Kit with all
#                                                        tools on and Levels closed, C the same with Levels open in LUFS mode, A and B once more, then D with two
#                                                        Multistream destinations) plus the Kit's own in-process timing counters (RWS_KIT_TEST_COST=1, "kit cost:" log
#                                                        lines). RESULT: PASS when the in-process total in the C case is under 0.5 % of one core. It takes about
#                                                        (6 x (CostMinutes + 3)) minutes: -CostMinutes (default 30) and -CostWarmupSeconds (default 60) of this script
#                                                        are handed to it; run it as -Steps cost (a smoke test: -Steps cost -CostMinutes 2). Needs ffmpeg on PATH.
#                                                        Results: .testbed\run-cost.txt; progress while it runs: .testbed\cost-progress.txt)
# The tool folders are found in both layouts (see kit-testbed.ps1, Get-KitToolDir): red-warden-<tool>\ next to the
# Kit folder, or <tool>\ next to plugin\ in the published repository.
# Per-step status .testbed\status-<step>.txt (written when the step ends; all status-*.txt and run-*.txt are deleted at the start of
# every run): key=value lines step, exit (the step's exit code), result (PASS only if the output has a RESULT: PASS line and no
# RESULT: FAIL), dll_sha256_start and dll_sha256 (SHA-256 of build_x64\RelWithDebInfo\rws-stream-kit.dll before and after the step),
# finished. status-unit.txt instead holds one "test=<name> exit=<n>" line per unit test. tools\check-test-results.ps1 (the release
# gate) reads these, not run-done.txt.
# Result files .testbed\run-<step>.txt; a harness step passes on exit 0 AND
# "RESULT: PASS" in its file, else its name is listed in run-done.txt ("FAILED: ms-local,..."). An unknown step
# name fails the run as "unknown:<name>". Run them after "build" (they use build_x64\RelWithDebInfo\rws-stream-kit.dll).
# run-done.txt: first line "OK" only if every step exited 0, else "FAILED at <step> exit=<n>"
# (configure/build stop the run) or "FAILED: <steps>" (unit). All result files are ASCII.
# A -Steps subset runs only those steps, so its "OK" means OK for that subset only (the other steps have no status file).
# The build step also fails when the linker reports a duplicate symbol (LNK2005, LNK4006, LNK4088 ...):
# the one-DLL merge must not define a symbol twice.
$root = Split-Path -Parent $PSScriptRoot
Set-Location $root
$bed = Join-Path $root ".testbed"
New-Item -ItemType Directory -Force $bed | Out-Null
Remove-Item (Join-Path $bed "run-*.txt"), (Join-Path $bed "status-*.txt") -ErrorAction SilentlyContinue
$doneFile = Join-Path $bed "run-done.txt"
$q = (Get-ChildItem .deps -Directory -Filter 'obs-deps-qt6-*x64' | Select-Object -First 1).FullName
$env:PATH = "$q\bin;$env:PATH"
$failed = @()

# Runs one portable-OBS harness (extra arguments in $more) against the ONE Kit DLL, hidden. Result file:
# .testbed\run-<name>.txt (ASCII). The step fails on a nonzero exit or a result file without "RESULT: PASS".
. (Join-Path $PSScriptRoot "kit-testbed.ps1")   # Get-KitToolDir: the tool folders in the published and the development layout
$msTools = Join-Path (Get-KitToolDir "multistream") "tools"
$pfTools = Join-Path (Get-KitToolDir "preflight") "tools"
$ifTools = Join-Path (Get-KitToolDir "inflight") "tools"

$dllPath = Join-Path $root "build_x64\RelWithDebInfo\rws-stream-kit.dll"
function Get-DllSha { if (Test-Path $dllPath) { return (Get-FileHash $dllPath -Algorithm SHA256).Hash.ToLower() } return "missing" }
# One status file per step: the exit code AND the result, plus the bytes (SHA-256) of the DLL the step ran against.
function Write-Status($step, $exitCode, $result, $shaStart, $extra) {
  $lines = @("step=$step", "exit=$exitCode", "result=$result") + @($extra) +
           @("dll_sha256_start=$shaStart", "dll_sha256=$(Get-DllSha)", "finished=$(Get-Date -Format 's')")
  $lines | Set-Content -Encoding ASCII (Join-Path $bed "status-$step.txt")
}
function Run-Harness($name, $script, $more) {
  $resultFile = Join-Path $bed "run-$name.txt"
  $sha0 = Get-DllSha
  if (!(Test-Path $script)) {
    "MISSING $script" | Out-File -Encoding ASCII $resultFile; $script:failed += $name
    Write-Status $name 1 "FAIL" $sha0 $null; return
  }
  powershell -ExecutionPolicy Bypass -File $script @more 2>&1 | ForEach-Object { "$_" } | Out-File -Encoding ASCII $resultFile
  $code = $LASTEXITCODE
  $txt = @(Get-Content $resultFile -ErrorAction SilentlyContinue)
  $passed = (@($txt | Where-Object { $_ -match '^\s*RESULT: PASS\s*$' }).Count -gt 0) -and (@($txt | Where-Object { $_ -match 'RESULT: FAIL' }).Count -eq 0)
  if ($code -ne 0 -or !$passed) { $script:failed += $name }
  Write-Status $name $code $(if ($passed) { "PASS" } else { "FAIL" }) $sha0 $null
}

foreach ($s in ($Steps -join ',' -split ',')) {
  switch ($s) {
    "configure" {
      cmake --preset windows-x64 2>&1 | Out-File -Encoding ASCII (Join-Path $bed "run-configure.txt")
      $code = $LASTEXITCODE
      if ($code -ne 0) { "FAILED at configure exit=$code" | Set-Content -Encoding ASCII $doneFile; exit 1 }
    }
    "build" {
      $buildLog = Join-Path $bed "run-build.txt"
      cmake --build build_x64 --config RelWithDebInfo 2>&1 | Out-File -Encoding ASCII $buildLog
      $code = $LASTEXITCODE
      if ($code -ne 0) { "FAILED at build exit=$code" | Set-Content -Encoding ASCII $doneFile; exit 1 }
      $dups = Select-String -Path $buildLog -Pattern 'LNK2005|LNK4006|LNK4088|LNK2019|LNK2001|multiply defined|already defined'
      if ($dups) {
        $dups | ForEach-Object { "$_" } | Set-Content -Encoding ASCII (Join-Path $bed "run-build-duplicates.txt")
        "FAILED at build (duplicate or unresolved symbols, see run-build-duplicates.txt)" | Set-Content -Encoding ASCII $doneFile
        exit 1
      }
    }
    "unit" {
      $out = Join-Path $bed "run-unit.txt"
      $unitSha0 = Get-DllSha
      $unitRows = @(); $unitBad = 0
      $tests = @(
        "rws-multistream-tests", "rws-multistream-importers-test", "rws-multistream-session-test",
        "rws-multistream-limits-test", "rws-multistream-update-test", "rws-multistream-serverurl-test",
        "rws-preflight-update-test", "rws-preflight-eval-test", "rws-preflight-disk-test", "rws-preflight-rungen-test",
        "rws-inflight-update-test", "rws-inflight-settings-test", "rws-inflight-settings-check-test", "rws-inflight-eval-test",
        "rws-inflight-probe-test", "rws-inflight-level-test", "rws-inflight-levels-test", "rws-inflight-loudness-test",
        "rws-inflight-ring-test", "rws-inflight-ui-test",
        "rws-kit-paths-test", "rws-kit-locale-test", "rws-kit-stage-test", "rws-kit-settings-test"
      )
      foreach ($t in $tests) {
        $exe = ".\build_x64\RelWithDebInfo\$t.exe"
        "== $t" | Add-Content -Encoding ASCII $out
        if (Test-Path $exe) {
          $o = & $exe 2>&1
          $code = $LASTEXITCODE
          $o | ForEach-Object { "$_" } | Add-Content -Encoding ASCII $out
        } else {
          "MISSING $exe" | Add-Content -Encoding ASCII $out
          $code = -1
        }
        "exit=$code" | Add-Content -Encoding ASCII $out
        $unitRows += "test=$t exit=$code"
        if ($code -ne 0) { $failed += "unit:$t"; $unitBad++ }
      }
      Write-Status "unit" $(if ($unitBad -eq 0) { 0 } else { 1 }) $(if ($unitBad -eq 0) { "PASS" } else { "FAIL" }) $unitSha0 $unitRows
    }
    "ms-local" { Run-Harness "ms-local" (Join-Path $msTools "test-local.ps1") @() }
    "ms-resilience" { Run-Harness "ms-resilience" (Join-Path $msTools "test-resilience.ps1") @("-Snapshots") }
    "ms-import" { Run-Harness "ms-import" (Join-Path $msTools "test-import.ps1") @() }
    "ms-bandwidth" { Run-Harness "ms-bandwidth" (Join-Path $msTools "test-bandwidth.ps1") @("-Snapshots") }
    "pf-load" { Run-Harness "pf-load" (Join-Path $pfTools "test-load.ps1") @() }
    "pf-state" { Run-Harness "pf-state" (Join-Path $pfTools "test-state.ps1") @() }
    "pf-integration" { Run-Harness "pf-integration" (Join-Path $pfTools "test-integration.ps1") @() }
    "pf-ui" { Run-Harness "pf-ui" (Join-Path $pfTools "test-ui.ps1") @() }
    "if-load" { Run-Harness "if-load" (Join-Path $ifTools "test-load.ps1") @() }
    "if-audio" { Run-Harness "if-audio" (Join-Path $ifTools "test-audio.ps1") @() }
    "pf-state-nows" { Run-Harness "pf-state-nows" (Join-Path $pfTools "test-state.ps1") @("-NoWebsocket") }
    "if-load-nows" { Run-Harness "if-load-nows" (Join-Path $ifTools "test-load.ps1") @("-NoWebsocket") }
    "if-ui" { Run-Harness "if-ui" (Join-Path $ifTools "test-ui.ps1") @() }
    "exit-stress" { Run-Harness "exit-stress" (Join-Path $PSScriptRoot "test-exit-stress.ps1") @() }
    "kit-dock" { Run-Harness "kit-dock" (Join-Path $PSScriptRoot "test-kit-dock.ps1") @() }
    "net-light" { Run-Harness "net-light" (Join-Path $PSScriptRoot "test-net-light.ps1") @() }
    "showcase" { Run-Harness "showcase" (Join-Path $PSScriptRoot "shots-showcase.ps1") @() }
    "cost" { Run-Harness "cost" (Join-Path $PSScriptRoot "test-cost.ps1") @("-Minutes", "$CostMinutes", "-WarmupSeconds", "$CostWarmupSeconds") }
    default { if ($s.Trim() -ne "") { $failed += "unknown:$($s.Trim())" } }
  }
}
$head = if ($failed.Count -eq 0) { "OK" } else { "FAILED: " + ($failed -join ",") }
@($head, "done $(Get-Date -Format HH:mm:ss)") | Set-Content -Encoding ASCII $doneFile
if ($failed.Count -ne 0) { exit 1 }

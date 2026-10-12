<#
  Release test gate for the Red Warden Stream Kit: succeeds only if every unit-test executable AND every
  release harness passed against the exact DLL being shipped.

    powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-test-results.ps1 `
      -PluginDir <plugin\ folder> -Dll <path to rws-stream-kit.dll>

  Reads <plugin>\.testbed\status-<step>.txt (and status-unit.txt), the per-step status plugin\tools\run-all.ps1
  writes when each step ends: the step's exit code, its result, and the SHA-256 of the DLL it ran against.
  Required for every step: exit 0, result PASS, and a DLL SHA-256 (before and after the step) equal to the SHA-256
  of the DLL given here, so results of other bytes are refused even if their timestamps look right. The status and
  output files must also be newer than the DLL. The harness output run-<step>.txt must still hold RESULT: PASS.
  run-done.txt is never read: a partial test run writes a clean-looking summary there.
  (run-all.ps1 deletes every run-*.txt and status-*.txt when it starts, so a subset run leaves the other steps missing.)
  The lists below are also checked against plugin\tools\run-all.ps1: a unit test or harness step that run-all can
  run but this gate does not list (or the reverse) is a failure, so a new test cannot be left out silently.
  Prints "[OK] ..." and exits 0, or prints "[FAIL] <first problem>" and exits 1.
#>
param(
  [Parameter(Mandatory = $true)][string]$PluginDir,
  [Parameter(Mandatory = $true)][string]$Dll
)
$ErrorActionPreference = "Stop"

# --- What must have passed. Add a new test here AND in tools\test-gate.ps1. ----------------------------
# Every unit-test executable run-all.ps1 writes into run-unit.txt as "== <name>" ... "exit=<n>".
$units = @(
  "rws-multistream-tests", "rws-multistream-importers-test", "rws-multistream-session-test",
  "rws-multistream-limits-test", "rws-multistream-update-test", "rws-multistream-serverurl-test",
  "rws-preflight-update-test", "rws-preflight-eval-test", "rws-preflight-disk-test", "rws-preflight-rungen-test",
  "rws-inflight-update-test", "rws-inflight-settings-test", "rws-inflight-settings-check-test", "rws-inflight-eval-test",
  "rws-inflight-probe-test", "rws-inflight-level-test", "rws-inflight-levels-test", "rws-inflight-loudness-test",
  "rws-inflight-ring-test", "rws-inflight-ui-test",
  "rws-kit-paths-test", "rws-kit-locale-test", "rws-kit-stage-test", "rws-kit-settings-test"
)
# Every harness step whose .testbed\run-<step>.txt must exist, be fresh and contain "RESULT: PASS".
$harnesses = @(
  "kit-dock",
  "pf-load", "pf-state", "pf-state-nows", "pf-integration", "pf-ui",
  "if-load", "if-load-nows", "if-audio", "if-ui",
  "ms-local", "ms-resilience", "ms-import", "ms-bandwidth",
  "net-light", "exit-stress"
)
# Optional run-all steps that never gate a release (screenshots, cost measurement). Named here so that any OTHER
# step run-all gains still fails the list check below.
$optional = @("showcase", "cost")

function Fail($msg) { Write-Host "[FAIL] $msg" -ForegroundColor Red; exit 1 }

# Some test exes write UTF-16, so appended ASCII lines can carry stray NULs: strip them before matching.
function Read-Lines($path) { @(Get-Content $path) | ForEach-Object { "$_" -replace '\x00', '' } }

if (!(Test-Path -LiteralPath $Dll)) { Fail "DLL not found: $Dll" }
$dllItem = Get-Item -LiteralPath $Dll
$dllSha = (Get-FileHash -LiteralPath $Dll -Algorithm SHA256).Hash.ToLower()
$bed = Join-Path $PluginDir ".testbed"

# --- The lists against run-all.ps1 itself ------------------------------------------------------------
$runAll = Join-Path $PluginDir "tools\run-all.ps1"
if (!(Test-Path -LiteralPath $runAll)) { Fail "run-all.ps1 not found ($runAll): cannot check the gate's lists against it" }
$ra = Get-Content -LiteralPath $runAll -Raw
$raSteps = @([regex]::Matches($ra, '(?m)^\s+"([a-z0-9-]+)"\s*\{') | ForEach-Object { $_.Groups[1].Value } | Where-Object { $_ -notin "configure", "build", "unit" })
$mm = [regex]::Match($ra, '(?s)\$tests = @\((.*?)\n\s*\)')
$raUnits = @([regex]::Matches($mm.Groups[1].Value, '"(rws-[a-z0-9-]+)"') | ForEach-Object { $_.Groups[1].Value })
if ($raUnits.Count -eq 0 -or $raSteps.Count -eq 0) { Fail "could not read the unit tests or harness steps from run-all.ps1" }
$dif = @($raSteps | Where-Object { $_ -notin $harnesses -and $_ -notin $optional }) + @($harnesses | Where-Object { $_ -notin $raSteps })
if ($dif.Count -gt 0) { Fail "the gate's harness list and run-all.ps1 differ: $($dif -join ', '). Update tools\check-test-results.ps1 and tools\test-gate.ps1." }
$dif = @($raUnits | Where-Object { $_ -notin $units }) + @($units | Where-Object { $_ -notin $raUnits })
if ($dif.Count -gt 0) { Fail "the gate's unit-test list and run-all.ps1 differ: $($dif -join ', '). Update tools\check-test-results.ps1 and tools\test-gate.ps1." }

# key=value status files. Returns $null when the file is missing.
function Read-Status($step) {
  $f = Join-Path $bed "status-$step.txt"
  if (!(Test-Path -LiteralPath $f)) { return $null }
  $h = @{ File = $f; Item = (Get-Item -LiteralPath $f); Tests = @{} }
  foreach ($l in Read-Lines $f) {
    if ($l -match '^test=(\S+) exit=(-?\d+)\s*$') { $h.Tests[$Matches[1]] = [int]$Matches[2] }
    elseif ($l -match '^([a-z0-9_]+)=(.*)$' -and !$h.ContainsKey($Matches[1])) { $h[$Matches[1]] = $Matches[2].Trim() }
  }
  return $h
}
# The common checks for one status: the step ran, is fresh, passed (exit 0, result PASS) against exactly this DLL.
function Test-Status($step, $st, [bool]$perTest) {
  if (!$st) { Fail "$step did not run (no status-$step.txt in $bed). Run plugin\tools\run-all.ps1 with every step." }
  if ($st.step -ne $step) { Fail "$($st.File) is for step '$($st.step)', not '$step'" }
  if ($st.Item.LastWriteTime -le $dllItem.LastWriteTime) { Fail "$step status is not newer than $($dllItem.Name) ($($st.File))" }
  if ($st.dll_sha256 -ne $dllSha -or $st.dll_sha256_start -ne $dllSha) {
    Fail "$step ran against a different DLL than $($dllItem.Name) (status SHA-256 start $($st.dll_sha256_start), end $($st.dll_sha256); this DLL $dllSha). Re-run the tests."
  }
  if (!$perTest) {
    if ($st.exit -ne "0") { Fail "$step exited $($st.exit) ($($st.File))" }
    if ($st.result -cne "PASS") { Fail "$step result is '$($st.result)', not PASS ($($st.File))" }
  }
}

# --- Unit tests: each named test must appear with exit=0, in the status and in run-unit.txt ------------
$ust = Read-Status "unit"
Test-Status "unit" $ust $false
foreach ($t in $units) {
  if (!$ust.Tests.ContainsKey($t)) { Fail "unit test $t is not in $($ust.File)" }
  if ($ust.Tests[$t] -ne 0) { Fail "unit test $t exited $($ust.Tests[$t]) ($($ust.File))" }
}
$unitFile = Join-Path $bed "run-unit.txt"
if (!(Test-Path -LiteralPath $unitFile)) { Fail "no run-unit.txt ($unitFile). Run plugin\tools\run-all.ps1 first." }
$unitItem = Get-Item -LiteralPath $unitFile
# Freshness is strict: a result file with the same timestamp as the DLL proves nothing about it.
if ($unitItem.LastWriteTime -le $dllItem.LastWriteTime) { Fail "run-unit.txt is not newer than $($dllItem.Name). Re-run the unit tests." }
# The test executables sit next to the DLL. Results older than an exe describe a previous build of it.
$dllDir = Split-Path -Parent $dllItem.FullName
foreach ($t in $units) {
  $exe = Join-Path $dllDir "$t.exe"
  if (!(Test-Path -LiteralPath $exe)) { Fail "$t.exe not found in $dllDir" }
  if ($unitItem.LastWriteTime -le (Get-Item -LiteralPath $exe).LastWriteTime) { Fail "$t.exe is newer than run-unit.txt: re-run the unit tests" }
}
# Every "== name" entry must be closed by exactly one exit line, and appear once. A name that appears
# twice could hide a failing run behind a later passing one.
$exits = @{}; $seen = @{}; $dangling = @(); $cur = $null
foreach ($l in Read-Lines $unitFile) {
  if ($l -match '^== (\S+)') {
    if ($cur) { $dangling += $cur }
    $cur = $Matches[1]
    $seen[$cur] = 1 + [int]$seen[$cur]
  }
  elseif ($l -match '^exit=(-?\d+)' -and $cur) { $exits[$cur] = [int]$Matches[1]; $cur = $null }
}
if ($cur) { $dangling += $cur }
foreach ($n in ($seen.Keys | Sort-Object)) {
  if ($seen[$n] -gt 1) { Fail "$n appears $($seen[$n]) times in run-unit.txt ($unitFile)" }
}
if ($dangling.Count -gt 0) { Fail "$($dangling[0]) has no exit line ($unitFile)" }
foreach ($t in $units) {
  if (!$exits.ContainsKey($t)) { Fail "unit test $t did not run ($unitFile)" }
  if ($exits[$t] -ne 0) { Fail "unit test $t exited $($exits[$t]) ($unitFile)" }
}

# --- Harnesses: a fresh result file that says RESULT: PASS ------------------------------------------
# The status above carries the exit code and result; the output file must agree: a RESULT: PASS line, never a FAIL.
foreach ($h in $harnesses) {
  Test-Status $h (Read-Status $h) $false
  $f = Join-Path $bed "run-$h.txt"
  if (!(Test-Path -LiteralPath $f)) { Fail "$h harness did not run (no $f)" }
  if ((Get-Item -LiteralPath $f).LastWriteTime -le $dllItem.LastWriteTime) { Fail "$h harness result is not newer than $($dllItem.Name) ($f)" }
  $lines = @(Read-Lines $f)
  $ok = @($lines | Where-Object { $_ -match '^\s*RESULT: PASS\s*$' }).Count -gt 0 -and
        @($lines | Where-Object { $_ -match 'RESULT: FAIL' }).Count -eq 0
  if (!$ok) { Fail "$h harness did not pass ($f)" }
}

Write-Host "[OK] $($units.Count) unit tests and $($harnesses.Count) harnesses passed against $($dllItem.Name) (SHA-256 $dllSha)"
exit 0

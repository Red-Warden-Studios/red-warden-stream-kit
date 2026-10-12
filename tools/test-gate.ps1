<#
  Self-contained test of tools\check-test-results.ps1, the release test gate.

    powershell -NoProfile -ExecutionPolicy Bypass -File tools\test-gate.ps1

  Needs no OBS and no build. Each case builds a fake plugin folder (a dummy DLL plus .testbed result
  files with controlled content and timestamps) under a unique folder in $env:TEMP, runs the checker
  on it and compares the exit code with the expected one. The temp folder is always deleted.
  It also fails if plugin\tools\run-all.ps1 has a unit test or a harness step that this file's lists
  (and so, through the per-name cases, the checker) do not know, so a new test cannot be left out of the
  release gate silently.
  Prints "ALL PASSED" and exits 0, or "<n> FAILED" and exits 1.
#>
$ErrorActionPreference = "Stop"
$checker = Join-Path $PSScriptRoot "check-test-results.ps1"
$runAll = Join-Path (Split-Path -Parent $PSScriptRoot) "plugin\tools\run-all.ps1"
$root = Join-Path $env:TEMP ("rws-gate-test-" + [guid]::NewGuid().ToString("N"))
$failures = 0
$seq = 0

# The checker's lists, repeated here on purpose: if someone edits the checker's lists, this test
# must be edited too, so a test cannot be dropped from the gate silently.
$units = @(
  "rws-multistream-tests", "rws-multistream-importers-test", "rws-multistream-session-test",
  "rws-multistream-limits-test", "rws-multistream-update-test", "rws-multistream-serverurl-test",
  "rws-preflight-update-test", "rws-preflight-eval-test", "rws-preflight-disk-test", "rws-preflight-rungen-test",
  "rws-inflight-update-test", "rws-inflight-settings-test", "rws-inflight-settings-check-test", "rws-inflight-eval-test",
  "rws-inflight-probe-test", "rws-inflight-level-test", "rws-inflight-levels-test", "rws-inflight-loudness-test",
  "rws-inflight-ring-test", "rws-inflight-ui-test",
  "rws-kit-paths-test", "rws-kit-locale-test", "rws-kit-stage-test", "rws-kit-settings-test"
)
$harnesses = @(
  "kit-dock",
  "pf-load", "pf-state", "pf-state-nows", "pf-integration", "pf-ui",
  "if-load", "if-load-nows", "if-audio", "if-ui",
  "ms-local", "ms-resilience", "ms-import", "ms-bandwidth",
  "net-light", "exit-stress"
)
$optional = @("showcase", "cost")   # must match check-test-results.ps1

$base = (Get-Date).AddHours(-2)         # the DLL's timestamp
$fresh = $base.AddMinutes(30)           # result files newer than the DLL
$stale = $base.AddMinutes(-30)          # result files older than the DLL

function Put($path, $text, $time) {
  [IO.File]::WriteAllText($path, $text, [Text.Encoding]::ASCII)
  (Get-Item $path).LastWriteTime = $time
}
function Sha-Of($path) { (Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLower() }

# run-unit.txt text in the format run-all.ps1 writes. Some test exes write UTF-16, so the real file can
# carry NULs: $nul adds them.
function Unit-Text($skip, $badExit, $noExit, $nul) {
  $out = @()
  foreach ($t in $units) {
    if ($t -eq $skip) { continue }
    $code = if ($t -eq $badExit) { 1 } else { 0 }
    $out += "== $t$nul"
    $out += "[  PASSED  ]$nul 3 tests."
    $out += "o${nul}k${nul}"
    if ($t -ne $noExit) { $out += "exit=$code$nul" }
  }
  return (($out -join "`r`n") + "`r`n")
}

$harnessText = "installed rws-stream-kit.dll`r`nok: something`r`nRESULT: PASS`r`n"

# status-<step>.txt in the format run-all.ps1 writes.
function Status-Text($step, $exitCode, $result, $sha) {
  return (@("step=$step", "exit=$exitCode", "result=$result", "dll_sha256_start=$sha", "dll_sha256=$sha", "finished=2026-01-01T00:00:00") -join "`r`n") + "`r`n"
}
function Unit-Status-Text($sha, $skip, $badExit) {
  $rows = @($units | Where-Object { $_ -ne $skip } | ForEach-Object { "test=$_ exit=$(if ($_ -eq $badExit) { 1 } else { 0 })" })
  return ((@("step=unit", "exit=0", "result=PASS") + $rows + @("dll_sha256_start=$sha", "dll_sha256=$sha", "finished=2026-01-01T00:00:00")) -join "`r`n") + "`r`n"
}
# A stand-in run-all.ps1 with the pieces the checker reads: the switch labels and the $tests array.
function RunAll-Text($steps, $unitNames) {
  $t = "`$tests = @(`r`n" + (($unitNames | ForEach-Object { "  `"$_`"" }) -join ",`r`n") + "`r`n)`r`n"
  $t += "switch (`$s) {`r`n    `"configure`" { }`r`n    `"build`" { }`r`n    `"unit`" { }`r`n"
  foreach ($h in $steps) { $t += "    `"$h`" { Run-Harness `"$h`" x @() }`r`n" }
  return $t + "}`r`n"
}

# A plugin folder that passes everything: DLL, fresh unit file, fresh passing harness files and statuses
# recorded against the DLL's SHA-256, and a run-all.ps1 whose lists equal the gate's.
function New-Fixture {
  $script:seq++
  $dir = Join-Path $root "case$($script:seq)"
  $bed = Join-Path $dir ".testbed"
  New-Item -ItemType Directory -Force $bed, (Join-Path $dir "tools") | Out-Null
  $dll = Join-Path $dir "x.dll"
  Put $dll "dummy" $base
  $sha = Sha-Of $dll
  Put (Join-Path $bed "run-unit.txt") (Unit-Text $null $null $null "") $fresh
  Put (Join-Path $bed "status-unit.txt") (Unit-Status-Text $sha $null $null) $fresh
  foreach ($h in $harnesses) {
    Put (Join-Path $bed "run-$h.txt") $harnessText $fresh
    Put (Join-Path $bed "status-$h.txt") (Status-Text $h 0 "PASS" $sha) $fresh
  }
  Put (Join-Path $dir "tools\run-all.ps1") (RunAll-Text $harnesses $units) $fresh
  # The unit-test executables sit next to the DLL, older than the unit results.
  foreach ($t in $units) { Put (Join-Path $dir "$t.exe") "dummy" $base.AddMinutes(10) }
  return @{ Dir = $dir; Bed = $bed; Dll = $dll; Sha = $sha }
}

function Run-Case($name, $expected, $mutate, $arg) {
  $fx = New-Fixture
  if ($mutate) { & $mutate $fx $arg }
  & powershell -NoProfile -ExecutionPolicy Bypass -File $checker -PluginDir $fx.Dir -Dll $fx.Dll *> $null
  $got = $LASTEXITCODE
  if ($got -eq $expected) { Write-Host "ok:   $name" }
  else { Write-Host "FAIL: $name (expected $expected, got $got)"; $script:failures++ }
}

function Harness-Path($fx, $h) { Join-Path $fx.Bed "run-$h.txt" }
function Status-Path($fx, $h) { Join-Path $fx.Bed "status-$h.txt" }
function Unit-Path($fx) { Join-Path $fx.Bed "run-unit.txt" }

try {
  New-Item -ItemType Directory -Force $root | Out-Null

  # --- The gate's lists against the real run-all.ps1: nothing run-all can run may be unknown to the gate ---
  if (Test-Path $runAll) {
    $ra = Get-Content $runAll -Raw
    $steps = @([regex]::Matches($ra, '(?m)^\s+"([a-z0-9-]+)"\s*\{') | ForEach-Object { $_.Groups[1].Value } |
      Where-Object { $_ -notin "configure", "build", "unit" })
    $missingStep = @($steps | Where-Object { $_ -notin $harnesses -and $_ -notin $optional })
    $extraStep = @($harnesses | Where-Object { $_ -notin $steps })
    if ($missingStep.Count -eq 0 -and $extraStep.Count -eq 0) { Write-Host "ok:   test-gate harness list == run-all.ps1 harness steps ($($steps.Count))" }
    else { Write-Host "FAIL: harness steps differ. In run-all.ps1 but not here: $($missingStep -join ','). Here but not in run-all.ps1: $($extraStep -join ',')"; $failures++ }
    $m = [regex]::Match($ra, '(?s)\$tests = @\((.*?)\n\s*\)')
    $ut = @([regex]::Matches($m.Groups[1].Value, '"(rws-[a-z0-9-]+)"') | ForEach-Object { $_.Groups[1].Value })
    $missingUnit = @($ut | Where-Object { $_ -notin $units })
    $extraUnit = @($units | Where-Object { $_ -notin $ut })
    if ($ut.Count -gt 0 -and $missingUnit.Count -eq 0 -and $extraUnit.Count -eq 0) { Write-Host "ok:   test-gate unit list == run-all.ps1 unit tests ($($ut.Count))" }
    else { Write-Host "FAIL: unit tests differ (run-all found $($ut.Count)). In run-all.ps1 but not here: $($missingUnit -join ','). Here but not in run-all.ps1: $($extraUnit -join ',')"; $failures++ }
  } else {
    Write-Host "FAIL: $runAll not present"; $failures++
  }
  # The same check is part of the checker (so of packaging): the real checker against the real run-all.ps1's lists is
  # exercised by every accepted case below; here the stand-in run-all.ps1 is made to disagree with the gate.
  Run-Case "run-all.ps1 has a harness step the gate does not list: refused" 1 {
    param($fx) Put (Join-Path $fx.Dir "tools\run-all.ps1") (RunAll-Text ($harnesses + "brand-new-step") $units) $fresh }
  Run-Case "run-all.ps1 lacks a harness step the gate lists: refused" 1 {
    param($fx) Put (Join-Path $fx.Dir "tools\run-all.ps1") (RunAll-Text ($harnesses | Select-Object -Skip 1) $units) $fresh }
  Run-Case "run-all.ps1 has a unit test the gate does not list: refused" 1 {
    param($fx) Put (Join-Path $fx.Dir "tools\run-all.ps1") (RunAll-Text $harnesses ($units + "rws-brand-new-test")) $fresh }
  Run-Case "run-all.ps1 lacks a unit test the gate lists: refused" 1 {
    param($fx) Put (Join-Path $fx.Dir "tools\run-all.ps1") (RunAll-Text $harnesses ($units | Select-Object -Skip 1)) $fresh }
  Run-Case "no run-all.ps1 to compare with: refused" 1 {
    param($fx) Remove-Item (Join-Path $fx.Dir "tools\run-all.ps1") }

  # --- Accepted ---
  Run-Case "full pass is accepted" 0 $null
  Run-Case "full pass with NULs in run-unit.txt (UTF-16 test output) is accepted" 0 {
    param($fx) Put (Unit-Path $fx) (Unit-Text $null $null $null ([string][char]0)) $fresh }
  Run-Case "run-done.txt FAILED but every status passes is accepted (run-done.txt is not read)" 0 {
    param($fx) Put (Join-Path $fx.Bed "run-done.txt") "FAILED: ui`r`n" $fresh }
  Run-Case "UTF-16LE harness output with BOM ending RESULT: PASS is accepted" 0 {
    param($fx)
    $f = Harness-Path $fx "ms-import"
    "step one`r`nRESULT: PASS" | Out-File -Encoding Unicode $f
    (Get-Item $f).LastWriteTime = $fresh }

  # --- Refused: whole-run problems ---
  Run-Case "units only, no harness files, run-done.txt OK is refused" 1 {
    param($fx)
    foreach ($h in $harnesses) { Remove-Item (Harness-Path $fx $h); Remove-Item (Status-Path $fx $h) }
    Put (Join-Path $fx.Bed "run-done.txt") "OK`r`ndone 12:00:00`r`n" $fresh }
  Run-Case "DLL missing is refused" 1 { param($fx) Remove-Item $fx.Dll }
  Run-Case "no run-unit.txt is refused" 1 { param($fx) Remove-Item (Unit-Path $fx) }
  Run-Case "no status-unit.txt is refused" 1 { param($fx) Remove-Item (Status-Path $fx "unit") }
  Run-Case "status-unit.txt older than the DLL is refused" 1 {
    param($fx) (Get-Item (Status-Path $fx "unit")).LastWriteTime = $stale }
  Run-Case "run-unit.txt older than the DLL is refused" 1 {
    param($fx) (Get-Item (Unit-Path $fx)).LastWriteTime = $stale }
  Run-Case "run-unit.txt timestamp equal to the DLL's is refused" 1 {
    param($fx) (Get-Item (Unit-Path $fx)).LastWriteTime = (Get-Item $fx.Dll).LastWriteTime }
  Run-Case "duplicate unit entry (exit=1 then exit=0) is refused" 1 {
    param($fx) Put (Unit-Path $fx) ("== rws-kit-stage-test`r`nexit=1`r`n" + (Unit-Text $null $null $null "")) $fresh }
  Run-Case "named unit entry with no exit line is refused" 1 {
    param($fx) Put (Unit-Path $fx) (Unit-Text $null $null "rws-inflight-ring-test" "") $fresh }
  Run-Case "unnamed trailing unit entry with no exit line is refused" 1 {
    param($fx) Put (Unit-Path $fx) ((Unit-Text $null $null $null "") + "== some-other-test`r`n") $fresh }
  Run-Case "a unit exe newer than run-unit.txt is refused" 1 {
    param($fx) (Get-Item (Join-Path $fx.Dir "rws-kit-paths-test.exe")).LastWriteTime = $fresh.AddMinutes(5) }

  # --- Refused: the DLL's bytes ---
  Run-Case "DLL replaced by other bytes, timestamp unchanged: refused" 1 {
    param($fx) Put $fx.Dll "different bytes" $base }
  Run-Case "DLL replaced by other bytes with a newer timestamp: refused" 1 {
    param($fx) Put $fx.Dll "different bytes" $base.AddMinutes(10) }
  Run-Case "DLL rebuilt after the tests (newer than every result): refused" 1 {
    param($fx) Put $fx.Dll "dummy" $fresh.AddMinutes(10) }
  foreach ($h in (@("unit") + $harnesses)) {
    Run-Case "step $h ran against another DLL (status SHA-256 differs): refused" 1 {
      param($fx, $a) Put (Status-Path $fx $a) ((Get-Content (Status-Path $fx $a) -Raw) -replace 'dll_sha256=[0-9a-f]{64}', ('dll_sha256=' + ('0' * 64))) $fresh } $h
    Run-Case "step $h started against another DLL (start SHA-256 differs): refused" 1 {
      param($fx, $a) Put (Status-Path $fx $a) ((Get-Content (Status-Path $fx $a) -Raw) -replace 'dll_sha256_start=[0-9a-f]{64}', ('dll_sha256_start=' + ('0' * 64))) $fresh } $h
  }

  # --- Refused: every unit test, one at a time (a test dropped from the checker would be accepted here) ---
  foreach ($t in $units) {
    Run-Case "unit $t missing from run-unit.txt is refused" 1 {
      param($fx, $a) Put (Unit-Path $fx) (Unit-Text $a $null $null "") $fresh } $t
    Run-Case "unit $t missing from status-unit.txt is refused" 1 {
      param($fx, $a) Put (Status-Path $fx "unit") (Unit-Status-Text $fx.Sha $a $null) $fresh } $t
    Run-Case "unit $t exit=1 in the status is refused" 1 {
      param($fx, $a) Put (Status-Path $fx "unit") (Unit-Status-Text $fx.Sha $null $a) $fresh } $t
    Run-Case "unit $t exe missing is refused" 1 {
      param($fx, $a) Remove-Item (Join-Path $fx.Dir "$a.exe") } $t
  }

  # --- Refused: every harness, one at a time ---
  foreach ($h in $harnesses) {
    Run-Case "harness $h output missing is refused" 1 {
      param($fx, $a) Remove-Item (Harness-Path $fx $a) } $h
    Run-Case "harness $h status missing is refused" 1 {
      param($fx, $a) Remove-Item (Status-Path $fx $a) } $h
    Run-Case "harness $h status older than the DLL (stale) is refused" 1 {
      param($fx, $a) (Get-Item (Status-Path $fx $a)).LastWriteTime = $stale } $h
    Run-Case "harness $h status timestamp equal to the DLL's is refused" 1 {
      param($fx, $a) (Get-Item (Status-Path $fx $a)).LastWriteTime = (Get-Item $fx.Dll).LastWriteTime } $h
    Run-Case "harness $h prints PASS but exited non-zero (status exit=1): refused" 1 {
      param($fx, $a) Put (Status-Path $fx $a) (Status-Text $a 1 "PASS" $fx.Sha) $fresh } $h
    Run-Case "harness $h status result FAIL (exit 0): refused" 1 {
      param($fx, $a) Put (Status-Path $fx $a) (Status-Text $a 0 "FAIL" $fx.Sha) $fresh } $h
    Run-Case "harness $h status is for another step: refused" 1 {
      param($fx, $a) Put (Status-Path $fx $a) (Status-Text "other-step" 0 "PASS" $fx.Sha) $fresh } $h
    Run-Case "harness $h output with RESULT: FAIL is refused" 1 {
      param($fx, $a) Put (Harness-Path $fx $a) "step one`r`nRESULT: FAIL`r`n" $fresh } $h
  }

  # --- Refused: result-file content ---
  Run-Case "harness output with no RESULT line (empty file) is refused" 1 {
    param($fx) Put (Harness-Path $fx "pf-state") "" $fresh }
  Run-Case "harness output with both RESULT: FAIL and RESULT: PASS is refused" 1 {
    param($fx) Put (Harness-Path $fx "ms-import") "RESULT: FAIL`r`nretry`r`nRESULT: PASS`r`n" $fresh }
  Run-Case "harness output with RESULT: PASSED (not exact) is refused" 1 {
    param($fx) Put (Harness-Path $fx "ms-local") "RESULT: PASSED`r`n" $fresh }
  Run-Case "old Pre-Flight style first line PASS (no RESULT line) is refused" 1 {
    param($fx) Put (Harness-Path $fx "pf-load") "PASS`r`nmore detail`r`n" $fresh }
  Run-Case "run-all MISSING-script result (no RESULT: PASS) is refused" 1 {
    param($fx) Put (Harness-Path $fx "if-audio") "MISSING D:\x\test-audio.ps1`r`n" $fresh }
}
finally {
  if (Test-Path $root) { Remove-Item -Recurse -Force $root -ErrorAction SilentlyContinue }
}

if ($failures -eq 0) { Write-Host "ALL PASSED"; exit 0 }
Write-Host "$failures FAILED"
exit 1

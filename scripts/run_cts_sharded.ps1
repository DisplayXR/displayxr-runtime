<#
.SYNOPSIS
  Crash-isolated CTS baseline: run each automated test case in its own
  conformance_cli process so a hard crash (access violation) is attributed to
  one test instead of aborting the whole suite. Registry is applied once and
  restored in finally (same contract as run_cts.ps1).

.OUTPUTS
  A results CSV at %TEMP%\cts_<Tag>_shards.csv : name,result,exit,fails,skips
  result in { PASS, FAIL, SKIP, CRASH, TIMEOUT }.
#>
param(
  [string]$Plugin     = "sim-display",
  [string]$Graphics   = "d3d11",
  [string]$ApiVersion = "1.1",
  [string]$Spec       = "exclude:[interactive]",
  [int]   $PerTestTimeoutSec = 90,
  [string]$Tag        = "sharded_d3d11_1_1",
  # Enable the CTS's required XR_APILAYER_KHRONOS_runtime_conformance layer by
  # pointing the loader at the dir holding its manifest. "" disables it.
  [string]$ApiLayerPath = "USE_BASE",
  # Read test-case names from this file (one per line) instead of enumerating.
  # Enumeration via --list-tests is flaky because the CTS prints the runtime
  # banner over the list; a pre-captured canonical list is deterministic.
  [string]$NamesFile  = ""
)
$ErrorActionPreference = "Stop"
$wt   = (Resolve-Path "$PSScriptRoot\..").Path   # repo/worktree root (scripts/..)
$base = "$wt\build-cts\build\src\conformance\conformance_cli"
$exe  = "$base\RelWithDebInfo\conformance_cli.exe"
$devManifest = "$wt\build\Release\openxr_displayxr-dev.json"
$tmp  = $env:TEMP
$shardDir = "$tmp\cts_shards"
$csv  = "$tmp\cts_${Tag}_shards.csv"
if (Test-Path $shardDir) { Remove-Item $shardDir -Recurse -Force }
New-Item -ItemType Directory -Path $shardDir | Out-Null

# Get automated test-case names: from a canonical file if given, else slice the
# CTS --list-tests output (between the "Test names:" header and the next "***"
# banner, keeping only identifier-like tokens to drop interleaved runtime noise).
if ($NamesFile -and (Test-Path $NamesFile)) {
  $names = Get-Content $NamesFile | ForEach-Object { $_.Trim() } | Where-Object { $_ }
  Write-Output "Loaded $($names.Count) test-case names from $NamesFile"
} else {
  Push-Location $base
  try { $raw = & $exe $Spec --list-tests --verbosity quiet 2>$null } finally { Pop-Location }
  $start = ($raw | Select-String -Pattern '^Test names:').LineNumber | Select-Object -First 1
  $names = @()
  for ($i = $start; $i -lt $raw.Count; $i++) {
    if ($raw[$i] -match '^\*{3,}') { break }
    $t = $raw[$i].Trim()
    if ($t -and $t -match '^[A-Za-z_][A-Za-z0-9_./-]*$') { $names += $t }
  }
  Write-Output "Enumerated $($names.Count) automated test cases."
}
$names = $names | Select-Object -Unique

$xrKey = "HKLM:\Software\Khronos\OpenXR\1"
$dpKey = "HKLM:\Software\DisplayXR\DisplayProcessors\$Plugin"
$origRuntime = (Get-ItemProperty $xrKey -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime
$origProbe   = (Get-ItemProperty $dpKey -Name ProbeOrder   -ErrorAction SilentlyContinue).ProbeOrder
$env:DISPLAYXR_MCP = "0"; $env:VK_LOADER_LAYERS_DISABLE = "*"; $env:XRT_COMPOSITOR_START_WINDOWED = "true"
if ($ApiLayerPath -eq "USE_BASE") { $ApiLayerPath = $base }
if ($ApiLayerPath) {
  $env:XR_API_LAYER_PATH = $ApiLayerPath
  Write-Output "XR_API_LAYER_PATH = $ApiLayerPath (CTS conformance layer enabled)"
} else {
  Remove-Item Env:\XR_API_LAYER_PATH -ErrorAction SilentlyContinue
}

# Escape a test name into a Catch2 single-test spec (escape , [ ] * \).
function Esc([string]$n) { ($n -replace '([,\[\]\*\\])','\$1') }

$results = @()
try {
  Set-ItemProperty $xrKey -Name ActiveRuntime -Value $devManifest -Type String
  Set-ItemProperty $dpKey -Name ProbeOrder -Value 5 -Type DWord

  $i = 0
  foreach ($n in $names) {
    $i++
    $safe = ($n -replace '[^A-Za-z0-9._-]','_'); if ($safe.Length -gt 80) { $safe = $safe.Substring(0,80) }
    $sx = "$shardDir\$('{0:D3}' -f $i)_$safe.xml"
    $so = "$shardDir\$('{0:D3}' -f $i)_$safe.out"
    $spec = Esc $n
    $cliArgs = @($spec, "-G", $Graphics, "--apiVersion", $ApiVersion, "--reporter", "ctsxml::out=$sx")
    if ($ApiLayerPath) { $cliArgs += @("-L", "XR_APILAYER_KHRONOS_runtime_conformance") }
    $p = Start-Process -FilePath $exe -ArgumentList $cliArgs -WorkingDirectory $base -PassThru -NoNewWindow `
           -RedirectStandardOutput $so -RedirectStandardError "$so.err"
    $res = ""; $code = ""
    if (-not $p.WaitForExit($PerTestTimeoutSec * 1000)) {
      try { $p.Kill() } catch {}; $p.WaitForExit(5000) | Out-Null
      $res = "TIMEOUT"
    } else {
      try { $p.WaitForExit() | Out-Null; $code = $p.ExitCode } catch { $code = "" }
    }
    # Classify from the XML assertions, NOT the exit code: the runtime crashes
    # on instance/process teardown (Monado destruction race), so a non-zero exit
    # with failures==0 is a PASS-with-teardown-crash, not a test failure. A
    # genuine in-test crash leaves NO recorded testcases (tcs==0) or a truncated
    # XML, which we surface as CRASH.
    $fails = 0; $skips = 0; $tcs = 0; $haveXml = $false
    if (Test-Path $sx) {
      try {
        $raw = Get-Content $sx -Raw
        $idx = $raw.LastIndexOf('</testcase>')
        if ($idx -ge 0) { $raw = $raw.Substring(0,$idx+11) + "`n</testsuite></testsuites>" }
        $x = [xml]$raw
        $ts = $x.testsuites.testsuite
        if ($ts) { $fails=[int]$ts.failures; $skips=[int]$ts.skipped; $tcs=[int]$ts.tests; $haveXml=$true }
      } catch {}
    }
    if (-not $res) {
      if (-not $haveXml -or $tcs -eq 0) { $res = "CRASH" }
      elseif ($fails -gt 0)             { $res = "FAIL" }
      elseif ($skips -ge $tcs)          { $res = "SKIP" }
      else                              { $res = "PASS" }
    }
    $teardown = if ($res -in 'PASS','FAIL','SKIP' -and "$code" -ne "0" -and "$code" -ne "") { 1 } else { 0 }
    $results += [pscustomobject]@{ name=$n; result=$res; teardownCrash=$teardown; exit=$code; fails=$fails; skips=$skips; asserts=$tcs }
    Write-Output ("[{0,3}/{1}] {2,-7} {3}" -f $i,$names.Count,$res,$n)
  }
}
finally {
  if ($origRuntime) { Set-ItemProperty $xrKey -Name ActiveRuntime -Value $origRuntime -Type String }
  if ($origProbe -ne $null) { Set-ItemProperty $dpKey -Name ProbeOrder -Value ([int]$origProbe) -Type DWord }
  Write-Output "RESTORED ActiveRuntime=$((Get-ItemProperty $xrKey -Name ActiveRuntime).ActiveRuntime)"
  Write-Output "RESTORED $Plugin ProbeOrder=$((Get-ItemProperty $dpKey -Name ProbeOrder).ProbeOrder)"
}

$results | Export-Csv -Path $csv -NoTypeInformation -Encoding UTF8
$g = $results | Group-Object result | Sort-Object Name
Write-Output "`n=== SUMMARY ==="
$g | ForEach-Object { Write-Output ("  {0,-7} {1}" -f $_.Name, $_.Count) }
Write-Output "CSV: $csv"

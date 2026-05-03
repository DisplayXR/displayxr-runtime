<#
.SYNOPSIS
    Phase 1 shell-mode benchmark — captures Present-to-Present jitter and
    KeyedMutex health for the D3D11 service compositor while two
    cube_handle apps run inside the shell.

.DESCRIPTION
    Phase 1 (shell-optimization branch) added greppable breadcrumbs to
    the service log under %LOCALAPPDATA%\DisplayXR:

        [PRESENT_NS] client=<ptr|workspace> dt_ns=<int>   — env-gated by DISPLAYXR_LOG_PRESENT_NS=1
        [CLIENT_FRAME_NS] client=<ptr> dt_ns=<int>        — env-gated by DISPLAYXR_LOG_PRESENT_NS=1
        [MUTEX] client=<ptr> timeouts=<u> acquires=<u> avg_acquire_us=<u> window_s=<int>
        [ZC] client=<ptr> views=<u> zero_copy=<Y|N> reason=<str>

    `client=workspace` tags the multi_compositor_render combined-atlas
    Present (only present in workspace mode). `client=<ptr>` is the
    per-client `d3d11_service_compositor*` for service-mode clients,
    or the per-process in-process compositor for standalone runs.

    This script:
      1. Launches displayxr-shell.exe with two cube_handle_d3d11_win
         apps (or whatever app paths you pass via -App).
      2. Lets the scenario run for -Seconds (default 60).
      3. Kills the shell process tree.
      4. Tails the most recent service log, extracts the breadcrumbs, and
         emits two CSVs:
            <out>_raw.csv      — every [PRESENT_NS] sample.
            <out>_summary.csv  — p50/p95/p99 + jitter for present intervals;
                                 timeout / acquire totals from [MUTEX].
      5. Prints a one-line summary.

    The shell binary is expected at _package\bin\displayxr-shell.exe by
    default (matches scripts\build_windows.bat output).

.PARAMETER Seconds
    Run duration in seconds. Default: 60.

.PARAMETER OutDir
    Output directory for CSVs. Default: docs\roadmap\bench\.

.PARAMETER Tag
    Filename tag (e.g. "before", "after"). Default: timestamp.

.PARAMETER Shell
    Path to displayxr-shell.exe. Default: _package\bin\displayxr-shell.exe
    relative to the repo root.

.PARAMETER App
    One or more app paths to pass to displayxr-shell. Default: two
    cube_handle_d3d11_win instances.

.EXAMPLE
    # 60s baseline before Phase 1 changes (run on main):
    .\scripts\bench_shell_present.ps1 -Tag before

    # 60s after rebuild on shell/optimization:
    .\scripts\bench_shell_present.ps1 -Tag after

    # Then diff:
    .\scripts\bench_diff.ps1 -Before docs\roadmap\bench\before_summary.csv `
                              -After  docs\roadmap\bench\after_summary.csv
#>

[CmdletBinding()]
param(
    [int]    $Seconds = 60,
    [string] $OutDir  = "docs\roadmap\bench",
    [string] $Tag     = (Get-Date -Format "yyyyMMdd-HHmmss"),
    [string] $Shell   = "_package\bin\displayxr-shell.exe",
    [string[]] $App = @(
        "test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe",
        "test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe"
    )
)

$ErrorActionPreference = "Stop"

# Repo root = parent of scripts/.
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

if (-not (Test-Path $Shell)) {
    throw "displayxr-shell not found at '$Shell'. Build via 'scripts\build_windows.bat build' first."
}
foreach ($a in $App) {
    if (-not (Test-Path $a)) {
        throw "App not found at '$a'. Build test apps via 'scripts\build_windows.bat test-apps'."
    }
}

# Make output dir.
if (-not (Test-Path $OutDir)) { New-Item -ItemType Directory -Path $OutDir | Out-Null }
$rawCsv     = Join-Path $OutDir "$($Tag)_raw.csv"
$summaryCsv = Join-Path $OutDir "$($Tag)_summary.csv"

# Snapshot existing service logs so we know which ones are NEW after the run.
$logDir = Join-Path $env:LOCALAPPDATA "DisplayXR"
if (-not (Test-Path $logDir)) { New-Item -ItemType Directory -Path $logDir | Out-Null }
$preLogs = @(Get-ChildItem -Path $logDir -Filter "DisplayXR_displayxr-service*.log" -ErrorAction SilentlyContinue)
$preLatestWriteTime = if ($preLogs.Count -gt 0) { ($preLogs | Sort-Object LastWriteTime -Descending | Select-Object -First 1).LastWriteTime } else { [datetime]::MinValue }

Write-Host "[bench] launching shell for $Seconds s..."
Write-Host "[bench]   shell:  $Shell"
foreach ($a in $App) { Write-Host "[bench]   app:    $a" }

# Enable per-Present timing log for this run.
$env:DISPLAYXR_LOG_PRESENT_NS = "1"

$shellArgs = @($App)
$proc = Start-Process -FilePath $Shell -ArgumentList $shellArgs -PassThru -WindowStyle Normal

try {
    Start-Sleep -Seconds $Seconds
} finally {
    Write-Host "[bench] killing shell process tree (pid=$($proc.Id))..."
    # Kill children too — displayxr-shell spawns service + apps.
    & taskkill /T /F /PID $proc.Id 2>&1 | Out-Null
    # Best-effort cleanup of orphaned service / cube processes.
    Get-Process -ErrorAction SilentlyContinue -Name "displayxr-service","cube_handle_d3d11_win","displayxr-shell" |
        ForEach-Object { try { $_ | Stop-Process -Force -ErrorAction SilentlyContinue } catch {} }
    Remove-Item Env:\DISPLAYXR_LOG_PRESENT_NS -ErrorAction SilentlyContinue
}

# Find service log(s) modified during the run.
$runLogs = @(Get-ChildItem -Path $logDir -Filter "DisplayXR_displayxr-service*.log" -ErrorAction SilentlyContinue |
             Where-Object { $_.LastWriteTime -gt $preLatestWriteTime })

if ($runLogs.Count -eq 0) {
    # Fallback: take the single most recently modified log.
    $runLogs = @(Get-ChildItem -Path $logDir -Filter "DisplayXR_displayxr-service*.log" -ErrorAction SilentlyContinue |
                 Sort-Object LastWriteTime -Descending | Select-Object -First 1)
}
if ($runLogs.Count -eq 0) {
    throw "No service log produced under $logDir — did the service even start?"
}

Write-Host "[bench] parsing $($runLogs.Count) service log file(s)..."

# Parse breadcrumbs. Shapes after Phase 1:
#   [PRESENT_NS] client=<ptr|workspace> dt_ns=<int>  # per-swapchain present
#                                                    #   (workspace = multi_compositor_render's combined-atlas Present)
#   [CLIENT_FRAME_NS] client=<ptr> dt_ns=<int>       # per-client xrEndFrame interval
#   [MUTEX] client=<ptr> timeouts=... acquires=... avg_acquire_us=... window_s=...
#   [ZC] client=<ptr> views=... zero_copy=Y|N reason=...
$rePresent = [regex]'\[PRESENT_NS\]\s+client=(?<c>\S+)\s+dt_ns=(?<dt>\d+)'
$reFrame   = [regex]'\[CLIENT_FRAME_NS\]\s+client=(?<c>\S+)\s+dt_ns=(?<dt>\d+)'
$reMutex   = [regex]'\[MUTEX\]\s+client=(?<c>\S+)\s+timeouts=(?<to>\d+)\s+acquires=(?<aq>\d+)\s+avg_acquire_us=(?<avg>\d+)\s+window_s=(?<ws>\d+)'
$reZc      = [regex]'\[ZC\]\s+client=(?<c>\S+)\s+views=(?<v>\d+)\s+zero_copy=(?<z>[YN])\s+reason=(?<r>\S+)'

# Per-client per-source samples.
$presentByClient = @{}
$frameByClient   = @{}
$mutexEvents     = New-Object 'System.Collections.Generic.List[psobject]'
$zcEvents        = New-Object 'System.Collections.Generic.List[psobject]'

function Add-Sample {
    param([hashtable] $bag, [string] $client, [int64] $dt)
    if (-not $bag.ContainsKey($client)) { $bag[$client] = New-Object 'System.Collections.Generic.List[int64]' }
    [void]$bag[$client].Add($dt)
}

foreach ($f in $runLogs) {
    Get-Content -LiteralPath $f.FullName | ForEach-Object {
        $line = $_
        $m = $rePresent.Match($line); if ($m.Success) { Add-Sample $presentByClient $m.Groups['c'].Value ([int64]$m.Groups['dt'].Value); return }
        $m = $reFrame.Match($line);   if ($m.Success) { Add-Sample $frameByClient   $m.Groups['c'].Value ([int64]$m.Groups['dt'].Value); return }
        $m = $reMutex.Match($line);   if ($m.Success) {
            [void]$mutexEvents.Add([pscustomobject]@{
                client          = $m.Groups['c'].Value
                timeouts        = [int]$m.Groups['to'].Value
                acquires        = [int]$m.Groups['aq'].Value
                avg_acquire_us  = [int]$m.Groups['avg'].Value
                window_s        = [int]$m.Groups['ws'].Value
            }); return
        }
        $m = $reZc.Match($line);      if ($m.Success) {
            [void]$zcEvents.Add([pscustomobject]@{
                client      = $m.Groups['c'].Value
                views       = [int]$m.Groups['v'].Value
                zero_copy   = $m.Groups['z'].Value
                reason      = $m.Groups['r'].Value
            })
        }
    }
}

$presentSamples = New-Object 'System.Collections.Generic.List[int64]'
foreach ($key in $presentByClient.Keys) { $presentSamples.AddRange($presentByClient[$key]) }
$frameSamples = New-Object 'System.Collections.Generic.List[int64]'
foreach ($key in $frameByClient.Keys) { $frameSamples.AddRange($frameByClient[$key]) }

Write-Host "[bench] samples: present(total)=$($presentSamples.Count) clients_present=$($presentByClient.Count) clients_frame=$($frameByClient.Count) mutex_windows=$($mutexEvents.Count) zc_events=$($zcEvents.Count)"

# Raw CSV — every sample tagged by source (present|frame) + client.
$rawRows = New-Object 'System.Collections.Generic.List[psobject]'
foreach ($key in $presentByClient.Keys) {
    $list = $presentByClient[$key]
    for ($i = 0; $i -lt $list.Count; $i++) {
        [void]$rawRows.Add([pscustomobject]@{ source = "present"; client = $key; idx = $i; dt_ns = $list[$i] })
    }
}
foreach ($key in $frameByClient.Keys) {
    $list = $frameByClient[$key]
    for ($i = 0; $i -lt $list.Count; $i++) {
        [void]$rawRows.Add([pscustomobject]@{ source = "frame"; client = $key; idx = $i; dt_ns = $list[$i] })
    }
}
$rawRows | Export-Csv -LiteralPath $rawCsv -NoTypeInformation

function Percentile {
    param([Parameter(Mandatory=$true)][System.Collections.Generic.List[int64]] $sorted, [double] $p)
    if ($sorted.Count -eq 0) { return 0 }
    $idx = [int][math]::Round(($p / 100.0) * ($sorted.Count - 1))
    if ($idx -lt 0) { $idx = 0 }
    if ($idx -ge $sorted.Count) { $idx = $sorted.Count - 1 }
    return $sorted[$idx]
}

function Stats {
    param([Parameter(Mandatory=$true)][System.Collections.Generic.List[int64]] $samples, [string] $label)
    $sorted = New-Object 'System.Collections.Generic.List[int64]'
    $sorted.AddRange([int64[]]@($samples))
    $sorted.Sort()
    $count = $sorted.Count
    if ($count -eq 0) {
        return [pscustomobject]@{ label = $label; count = 0; p50_ns = 0; p95_ns = 0; p99_ns = 0; min_ns = 0; max_ns = 0; mean_ns = 0; jitter_ns = 0; fps_p50 = 0 }
    }
    $p50 = Percentile $sorted 50
    $p95 = Percentile $sorted 95
    $p99 = Percentile $sorted 99
    $mean = ($sorted | Measure-Object -Sum).Sum / $count
    $fps = if ($p50 -gt 0) { [math]::Round(1e9 / $p50, 1) } else { 0 }
    return [pscustomobject]@{
        label    = $label
        count    = $count
        p50_ns   = $p50
        p95_ns   = $p95
        p99_ns   = $p99
        min_ns   = $sorted[0]
        max_ns   = $sorted[$count - 1]
        mean_ns  = [int64]$mean
        jitter_ns = $p99 - $p50
        fps_p50  = $fps
    }
}

# Per-source aggregate + per-client summary rows.
$summaryRows = New-Object 'System.Collections.Generic.List[psobject]'

$aggregatePresent = New-Object 'System.Collections.Generic.List[int64]'
$aggregatePresent.AddRange([int64[]]@($presentSamples))
[void]$summaryRows.Add((Stats $aggregatePresent "present:ALL"))
foreach ($key in $presentByClient.Keys | Sort-Object) {
    [void]$summaryRows.Add((Stats $presentByClient[$key] "present:$key"))
}

$aggregateFrame = New-Object 'System.Collections.Generic.List[int64]'
$aggregateFrame.AddRange([int64[]]@($frameSamples))
[void]$summaryRows.Add((Stats $aggregateFrame "frame:ALL"))
foreach ($key in $frameByClient.Keys | Sort-Object) {
    [void]$summaryRows.Add((Stats $frameByClient[$key] "frame:$key"))
}

$totalTimeouts = ($mutexEvents | Measure-Object -Property timeouts -Sum).Sum
$totalAcquires = ($mutexEvents | Measure-Object -Property acquires -Sum).Sum
$avgAcquireUs  = if ($mutexEvents.Count -gt 0) {
    [math]::Round((($mutexEvents | Measure-Object -Property avg_acquire_us -Average).Average), 2)
} else { 0 }

# Last [ZC] decision per client (most recent transition).
$lastZc = $zcEvents | Group-Object client | ForEach-Object {
    $last = $_.Group | Select-Object -Last 1
    "$($last.client):$($last.zero_copy)/$($last.reason)"
}

# Tag/duration as the first row, then one row per (source, client) stat.
$header = [pscustomobject]@{
    label    = "META"
    count    = 0
    p50_ns   = 0
    p95_ns   = 0
    p99_ns   = 0
    min_ns   = 0
    max_ns   = 0
    mean_ns  = 0
    jitter_ns = 0
    fps_p50  = 0
    tag                  = $Tag
    duration_s           = $Seconds
    mutex_total_timeouts = $totalTimeouts
    mutex_total_acquires = $totalAcquires
    mutex_avg_acquire_us = $avgAcquireUs
    zc_per_client        = ($lastZc -join ";")
}
# Re-wrap stat rows so they share columns with the header.
$enrichedSummary = New-Object 'System.Collections.Generic.List[psobject]'
[void]$enrichedSummary.Add($header)
foreach ($r in $summaryRows) {
    [void]$enrichedSummary.Add([pscustomobject]@{
        label    = $r.label
        count    = $r.count
        p50_ns   = $r.p50_ns
        p95_ns   = $r.p95_ns
        p99_ns   = $r.p99_ns
        min_ns   = $r.min_ns
        max_ns   = $r.max_ns
        mean_ns  = $r.mean_ns
        jitter_ns = $r.jitter_ns
        fps_p50  = $r.fps_p50
        tag                  = ""
        duration_s           = ""
        mutex_total_timeouts = ""
        mutex_total_acquires = ""
        mutex_avg_acquire_us = ""
        zc_per_client        = ""
    })
}
$enrichedSummary | Export-Csv -LiteralPath $summaryCsv -NoTypeInformation

Write-Host ""
Write-Host "[bench] === summary ($Tag, ${Seconds}s) ==="
Write-Host ("[bench]   mutex timeouts      = {0} / {1} acquires (avg {2} us)" -f $totalTimeouts, $totalAcquires, $avgAcquireUs)
Write-Host ("[bench]   zc per client       = {0}" -f ($lastZc -join "; "))
Write-Host ""
Write-Host "[bench] per-source stats:"
$summaryRows | ForEach-Object {
    "[bench]   {0,-32}  n={1,5}  p50={2,7:N0}ns ({3:N1} fps)  p95={4,7:N0}  p99={5,7:N0}  jitter={6,7:N0}" -f `
        $_.label, $_.count, $_.p50_ns, $_.fps_p50, $_.p95_ns, $_.p99_ns, $_.jitter_ns | Write-Host
}
Write-Host ""
Write-Host "[bench] wrote: $rawCsv"
Write-Host "[bench] wrote: $summaryCsv"

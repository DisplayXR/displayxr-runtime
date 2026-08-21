# summarize.ps1 - fold ladder.csv into SUMMARY.md: per-arm true medians +
# the derived component costs + self-reported gates. PowerShell 5.1, ASCII.
param(
    [string]$ResultsDir = "."
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'lib_sample.ps1')

$csvPath = Join-Path $ResultsDir 'ladder.csv'
if (-not (Test-Path $csvPath)) { Write-Host 'no ladder.csv'; exit 1 }
$rows = Import-Csv $csvPath
$capsPath = Join-Path $ResultsDir 'capabilities.json'
$caps = $null
if (Test-Path $capsPath) { $caps = Get-Content $capsPath -Raw | ConvertFrom-Json }

# --- Per-arm medians ---------------------------------------------------------
$armNames = @($rows | ForEach-Object { $_.arm } | Select-Object -Unique)
$med = @{}
foreach ($a in $armNames) {
    $r = @($rows | Where-Object { $_.arm -eq $a })
    $med[$a] = [ordered]@{
        n           = $r.Count
        app_scanout = Get-TrueMedian @($r | ForEach-Object { [double]$_.app_scanout })
        app_other   = Get-TrueMedian @($r | ForEach-Object { [double]$_.app_other })
        dwm         = Get-TrueMedian @($r | ForEach-Object { [double]$_.dwm })
        total       = Get-TrueMedian @($r | ForEach-Object { [double]$_.total_gpu })
        app_cpu     = Get-TrueMedian @($r | ForEach-Object { [double]$_.app_cpu })
        presents    = Get-TrueMedian @($r | ForEach-Object { [double]$_.presents_s })
        weaves      = Get-TrueMedian @($r | ForEach-Object { [double]$_.weaves_s })
        repaints    = Get-TrueMedian @($r | ForEach-Object { [double]$_.repaints_s })
        mode        = ($r | Select-Object -Last 1).mode
        flags       = (@($r | ForEach-Object { $_.flags } | Where-Object { $_ -ne '' }) -join ' ')
    }
    # app+dwm convenience
    $med[$a].app_dwm = [math]::Round(($med[$a].app_scanout + $med[$a].app_other + $med[$a].dwm), 2)
}

function M { param([string]$a) if ($med.ContainsKey($a)) { return $med[$a] } return $null }
function AppTotal { param($m) return [math]::Round($m.app_scanout + $m.app_other, 2) }

$out = @()
$out += '# DXR Perf Ladder - SUMMARY'
$out += ''
if ($caps -ne $null) {
    $out += ('Host: **' + $caps.hostname + '**  scanout LUID `' + $caps.scanoutLuid + '`  SR Platform: ' + $caps.srPlatform)
    $out += ''
}
$out += '## Per-arm medians (GPU busy %, Running-Time deltas)'
$out += ''
$out += '| arm | n | app@scanout | app@other | dwm | app+dwm | total | app CPU | presents/s | weaves/s | repaints/s | mode | flags |'
$out += '|---|---|---|---|---|---|---|---|---|---|---|---|---|'
foreach ($a in $armNames) {
    $m = $med[$a]
    $out += ('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | {12} |' -f `
        $a, $m.n, $m.app_scanout, $m.app_other, $m.dwm, $m.app_dwm, $m.total, $m.app_cpu, `
        $m.presents, $m.weaves, $m.repaints, $m.mode, $m.flags)
}
$out += ''

# --- Derived components ------------------------------------------------------
$out += '## Derived components'
$out += ''
$d = @()
$f2 = M 'FLIP-2D'; $f3 = M 'FLIP-3D'; $f322 = M 'FLIP-3D-22'
$l3 = M 'LIVE-3D'; $s3 = M 'SHAPED-3D'; $s3m = M 'SHAPED-3D-M'
$r30 = M 'REND-30'; $r60 = M 'REND-60'; $rp = M 'REPAINT'; $s322 = M 'SHAPED-3D-22'
$ship = M 'SHIP'; $shipm = M 'SHIP-M'; $idle = M 'IDLE-P'; $idlem = M 'IDLE-M'

if ($f2 -ne $null -and $f3 -ne $null -and $f3.weaves -gt 0) {
    # points -> ms/cycle: 1% busy = 10 ms/s; per weave = 10*delta/rate
    $unit1 = [math]::Round(10.0 * ($f3.total - $f2.total) / $f3.weaves, 3)
    $d += ('- weave_unit_ms (FLIP-3D vs FLIP-2D @ {0}/s): **{1} ms**' -f $f3.weaves, $unit1)
    if ($f322 -ne $null -and ($f3.weaves - $f322.weaves) -gt 5) {
        $unit2 = [math]::Round(10.0 * ($f3.total - $f322.total) / ($f3.weaves - $f322.weaves), 3)
        $d += ('- weave_unit_ms (rate cut {0} to {1}): **{2} ms** (cross-check)' -f $f3.weaves, $f322.weaves, $unit2)
    }
}
if ($l3 -ne $null -and $f3 -ne $null) { $d += ('- dwm_live_tax (LIVE-3D dwm - FLIP-3D dwm): **{0}**' -f [math]::Round($l3.dwm - $f3.dwm, 2)) }
if ($s3 -ne $null -and $l3 -ne $null) { $d += ('- shaping_tax (SHAPED-3D - LIVE-3D, total): **{0}**' -f [math]::Round($s3.total - $l3.total, 2)) }
if ($s3m -ne $null -and $s3 -ne $null) { $d += ('- motion_tax_shaped (SHAPED-3D-M - SHAPED-3D, total): **{0}**' -f [math]::Round($s3m.total - $s3.total, 2)) }
if ($r30 -ne $null -and $s3 -ne $null) { $d += ('- render_cost_30hz (REND-30 - SHAPED-3D, app): **{0}**' -f [math]::Round((AppTotal $r30) - (AppTotal $s3), 2)) }
if ($r60 -ne $null -and $r30 -ne $null) { $d += ('- render_slope_30to60 (app): **{0}**' -f [math]::Round((AppTotal $r60) - (AppTotal $r30), 2)) }
if ($rp -ne $null -and $s322 -ne $null) {
    # Repaint only engages when the app goes QUIET (present-capped), so the
    # honest parent is the 22 Hz-present arm, same app cadence.
    $tier = 'unknown'
    if ($rp.repaints -gt 5) { $tier = 'active (see #868 tier lines in dxr-logs)' } else { $tier = 'NO REPAINTS COUNTED - arm invalid' }
    $d += ('- repaint_tax (REPAINT - SHAPED-3D-22, total): **{0}** at {1} repaints/s [{2}]' -f [math]::Round($rp.total - $s322.total, 2), $rp.repaints, $tier)
}
if ($s322 -ne $null -and $s3 -ne $null) { $d += ('- present_cap_saving (SHAPED-3D-22 - SHAPED-3D, total): **{0}**' -f [math]::Round($s322.total - $s3.total, 2)) }
if ($ship -ne $null -and $idle -ne $null) { $d += ('- our_margin (SHIP total - IDLE-P total): **{0}**' -f [math]::Round($ship.total - $idle.total, 2)) }
if ($shipm -ne $null -and $ship -ne $null) { $d += ('- ship_motion_tax (SHIP-M - SHIP, total): **{0}** (the demo-honest delta)' -f [math]::Round($shipm.total - $ship.total, 2)) }
if ($idlem -ne $null -and $idle -ne $null) { $d += ('- idle_motion_floor (IDLE-M - IDLE-P, total): **{0}** (not ours)' -f [math]::Round($idlem.total - $idle.total, 2)) }
if ($d.Count -eq 0) { $d += '- (not enough arms present for derivations)' }
$out += $d
$out += ''

# --- Gates -------------------------------------------------------------------
$out += '## Gates'
$out += ''
$g = @()
if ($caps -ne $null) {
    if ($caps.power.onAc) { $g += '- PASS: on AC power' } else { $g += '- FLAG: ON BATTERY - all numbers suspect' }
    if ($caps.elevated) { $g += '- FLAG: harness ran elevated (loader may resolve a different runtime)' } else { $g += '- PASS: non-elevated' }
}
$flagged = @($rows | Where-Object { $_.flags -ne '' })
if ($flagged.Count -eq 0) { $g += '- PASS: no per-sample flags' }
else { $g += ('- FLAG: ' + $flagged.Count + ' sample(s) carry flags (see table + ladder.csv)') }
$modeBad = @($rows | Where-Object { $_.mode -ne '' -and $_.arm -match '2D' -and $_.mode -ne '2d' })
if ($modeBad.Count -gt 0) { $g += '- FLAG: a 2D arm measured in non-2d mode' }
$out += $g
$out += ''
$out += ('Generated ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + ' by summarize.ps1 (kit results are only comparable within one kit version).')

$sumPath = Join-Path $ResultsDir 'SUMMARY.md'
$out -join "`r`n" | Out-File -Encoding utf8 $sumPath
Write-Host ('SUMMARY.md written: ' + $sumPath)

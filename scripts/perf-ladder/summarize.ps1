# summarize.ps1 - fold ladder.csv + capabilities.json into SUMMARY.md, the
# standardized "report card": a fixed-layout markdown whose sections, component
# names and units never move, so two runs (same box, other box, other app)
# diff line-by-line. PowerShell 5.1, ASCII only.
#
# Confidence policy (Arc report rec 4): every derived component carries a
# conservative spread (sum of its input arms' per-rep spreads) and a verdict -
#   OK         effect resolved (spread <= 25% of |value| and above the floor)
#   NOISY      real effect, poor magnitude (spread > 25% of |value|)
#   UNRESOLVED |value| below the resolution floor AND spread exceeds it -
#              for control quantities read as "indistinguishable from zero",
#              which for a control IS the answer, not a failure.
param(
    [string]$ResultsDir = "."
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Continue'
. (Join-Path $PSScriptRoot 'lib_sample.ps1')

$csvPath = Join-Path $ResultsDir 'ladder.csv'
if (-not (Test-Path $csvPath)) { Write-Host 'no ladder.csv'; exit 1 }
$allRows = Import-Csv $csvPath
# Medians use CLEAN samples only: a zero-GPU row from a failed sweep folds a
# 3-rep median to zero (measured). App arms additionally require a nonzero
# GPU total.
$rows = @($allRows | Where-Object {
    ($_.flags -notmatch 'GPU_COUNTER_FAIL|GPU_DELTA_FAIL|APP_NO_START') -and
    (([double]$_.total_gpu) -gt 0 -or $_.arm -like 'IDLE*')
})
$caps = $null
$capsPath = Join-Path $ResultsDir 'capabilities.json'
if (Test-Path $capsPath) { $caps = Get-Content $capsPath -Raw | ConvertFrom-Json }
$man = $null
$manPath = Join-Path $ResultsDir 'kit-manifest.json'
if (Test-Path $manPath) { $man = Get-Content $manPath -Raw | ConvertFrom-Json }

# Old result dirs predate the #1154 bucket columns; read tolerantly so a stale
# ladder.csv still summarizes instead of throwing.
function ColOf {
    param($row, [string]$name)
    if ($row.PSObject.Properties[$name] -eq $null) { return 0.0 }
    $v = $row.$name
    if ([string]::IsNullOrEmpty($v)) { return 0.0 }
    return [double]$v
}

# --- Per-arm medians + per-rep spread ---------------------------------------
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
    # SUBJECT = client + service-side buckets (composite/weave/bridge live in the
    # service on the IPC path), so app_dwm folds svc_* in when present.
    $med[$a].svc = Get-TrueMedian @($r | ForEach-Object { (ColOf $_ 'svc_scanout') + (ColOf $_ 'svc_other') })
    $med[$a].tot_scanout = Get-TrueMedian @($r | ForEach-Object { (ColOf $_ 'tot_scanout') })
    $med[$a].tot_other = Get-TrueMedian @($r | ForEach-Object { (ColOf $_ 'tot_other') })
    $med[$a].app_dwm = [math]::Round(($med[$a].app_scanout + $med[$a].app_other + $med[$a].svc + $med[$a].dwm), 2)
    $ad = @($r | ForEach-Object { [double]$_.app_scanout + [double]$_.app_other + (ColOf $_ 'svc_scanout') + (ColOf $_ 'svc_other') + [double]$_.dwm })
    $med[$a].spread = if ($ad.Count -gt 1) {
        [math]::Round((($ad | Measure-Object -Maximum).Maximum - ($ad | Measure-Object -Minimum).Minimum), 2)
    } else { 0.0 }
    $td = @($r | ForEach-Object { [double]$_.total_gpu })
    $med[$a].tspread = if ($td.Count -gt 1) {
        [math]::Round((($td | Measure-Object -Maximum).Maximum - ($td | Measure-Object -Minimum).Minimum), 2)
    } else { 0.0 }
}

function M { param([string]$a) if ($med.ContainsKey($a)) { return $med[$a] } return $null }
function AppTotal { param($m) return [math]::Round($m.app_scanout + $m.app_other, 2) }

# --- Derived-component registry ----------------------------------------------
$comps = @()
function Add-Comp {
    # $arms: the input-arm median objects - min n across them gates the verdict
    # (a single-rep run has NO spread information; zero spread must read as
    # SINGLE-REP, never as precision). $forceVerdict for arms that measured
    # nothing (e.g. repaint pair with 0 repaints/s).
    param([string]$name, $value, $spread, [string]$unit, [string]$derivation, [string]$note = '', $arms = @(), [string]$forceVerdict = '')
    if ($value -eq $null) { return }
    $v = [math]::Round([double]$value, 3)
    $s = [math]::Round([double]$spread, 3)
    $floor = if ($unit -eq 'ms') { 0.15 } else { 1.0 }
    $abs = [math]::Abs($v)
    $minN = 99
    foreach ($a in $arms) { if ($a -ne $null -and $a.n -lt $minN) { $minN = $a.n } }
    $verdict = 'OK'
    if ($abs -lt $floor -and $s -gt $abs) { $verdict = 'UNRESOLVED' }
    elseif ($abs -gt 0 -and $s -gt 0.25 * $abs) { $verdict = 'NOISY' }
    if ($minN -lt 2) { $verdict = 'SINGLE-REP' }
    if ($forceVerdict -ne '') { $verdict = $forceVerdict }
    $script:comps += New-Object PSObject -Property @{
        Name = $name; Value = $v; Spread = $s; Unit = $unit
        Verdict = $verdict; Derivation = $derivation; Note = $note
    }
}

$f2 = M 'FLIP-2D'; $f3 = M 'FLIP-3D'; $f322 = M 'FLIP-3D-22'
$l3 = M 'LIVE-3D'; $s3 = M 'SHAPED-3D'; $s3m = M 'SHAPED-3D-M'
$r30 = M 'REND-30'; $r60 = M 'REND-60'; $rp = M 'REPAINT'; $s322 = M 'SHAPED-3D-22'
$ship = M 'SHIP'; $shipm = M 'SHIP-M'; $idle = M 'IDLE-P'; $idlem = M 'IDLE-M'
$m2d = M 'MODE-2D'; $rpon = M 'REPAINT-ON'   # app-repo arm names
$noag = M 'SHIP-NOAGENT'; $p22 = M 'PRESENT-22'  # Phase B (3DLuma quadrant)

# Component deltas difference app+dwm, NOT total: background box load lands in
# 'other' and cross-arm total differences swing +/-20 points on a busy box.
# unit1 vs unit2 are DIFFERENT quantities (Arc report): unit1 = only the
# 3D-weave-vs-2D-blit increment; unit2 = the whole removed cycle (blit + weave
# + present + DP submit); unit2 >= unit1 by construction. Their difference,
# cycle_minus_weave, is the mode-independent machinery - the 85%-of-cycle
# invariant candidate and the primary cross-box comparable.
if ($f2 -ne $null -and $f3 -ne $null -and $f3.weaves -gt 0) {
    $u1 = 10.0 * ($f3.app_dwm - $f2.app_dwm) / $f3.weaves
    $u1s = 10.0 * ($f3.spread + $f2.spread) / $f3.weaves
    Add-Comp 'unit1_weave_increment' $u1 $u1s 'ms' 'FLIP-3D - FLIP-2D, app+dwm, per weave' '3D-weave vs 2D-blit increment ONLY' @($f3, $f2)
    if ($f322 -ne $null -and ($f3.weaves - $f322.weaves) -gt 5) {
        $dr = $f3.weaves - $f322.weaves
        $u2 = 10.0 * ($f3.app_dwm - $f322.app_dwm) / $dr
        $u2s = 10.0 * ($f3.spread + $f322.spread) / $dr
        Add-Comp 'unit2_full_cycle' $u2 $u2s 'ms' ('rate cut ' + $f3.weaves + ' to ' + $f322.weaves + ', app+dwm') 'whole removed cycle: blit+weave+present+DP submit' @($f3, $f322)
        Add-Comp 'cycle_minus_weave' ($u2 - $u1) ($u1s + $u2s) 'ms' 'unit2 - unit1' 'mode-independent cycle machinery - PRIMARY cross-box comparable' @($f3, $f2, $f322)
    }
}
# Phase-A app-repo variant: MODE-2D stands in for FLIP-2D against SHIP.
if ($m2d -ne $null -and $ship -ne $null -and $ship.weaves -gt 0 -and $f2 -eq $null) {
    $u1 = 10.0 * ($ship.app_dwm - $m2d.app_dwm) / $ship.weaves
    Add-Comp 'unit1_weave_increment' $u1 (10.0 * ($ship.spread + $m2d.spread) / $ship.weaves) 'ms' 'SHIP - MODE-2D, app+dwm, per weave' 'transparent-path variant: includes compose delta' @($ship, $m2d)
}
if ($l3 -ne $null -and $f3 -ne $null) { Add-Comp 'dwm_live_tax' ($l3.dwm - $f3.dwm) ($l3.spread + $f3.spread) 'pts' 'LIVE-3D dwm - FLIP-3D dwm' '' @($l3, $f3) }
if ($s3 -ne $null -and $l3 -ne $null) { Add-Comp 'shaping_tax' ($s3.app_dwm - $l3.app_dwm) ($s3.spread + $l3.spread) 'pts' 'SHAPED-3D - LIVE-3D, app+dwm' 'SIGN-UNSTABLE across runs on the reference box - noise, do not quote' @($s3, $l3) }
if ($s3m -ne $null -and $s3 -ne $null) { Add-Comp 'motion_tax_shaped' ($s3m.app_dwm - $s3.app_dwm) ($s3m.spread + $s3.spread) 'pts' 'SHAPED-3D-M - SHAPED-3D, app+dwm' 'SIGN-UNSTABLE across runs on the reference box - noise, do not quote' @($s3m, $s3) }
if ($r30 -ne $null -and $s3 -ne $null) { Add-Comp 'render_cost_30hz' ((AppTotal $r30) - (AppTotal $s3)) ($r30.spread + $s3.spread) 'pts' 'REND-30 - SHAPED-3D, app' '' @($r30, $s3) }
if ($r60 -ne $null -and $r30 -ne $null) { Add-Comp 'render_slope_30to60' ((AppTotal $r60) - (AppTotal $r30)) ($r60.spread + $r30.spread) 'pts' 'REND-60 - REND-30, app' '' @($r60, $r30) }
$rpArm = $rp; $rpBase = $s322
if ($rpArm -eq $null) {
    $rpArm = $rpon
    # Phase B reparents REPAINT-ON onto the present-capped arm (the quiet-gate
    # only opens under a cap); fall back to SHIP for legacy Phase-A results.
    if ($p22 -ne $null) { $rpBase = $p22 } else { $rpBase = $ship }
}
if ($rpArm -ne $null -and $rpBase -ne $null) {
    $rpForce = ''
    $note = 'active'
    if ($rpArm.repaints -le 5) { $note = 'NO REPAINTS - app never present-quiet; arm measures nothing'; $rpForce = 'INVALID' }
    Add-Comp 'repaint_tax' ($rpArm.app_dwm - $rpBase.app_dwm) ($rpArm.spread + $rpBase.spread) 'pts' ('repaint on - off at ' + $rpArm.repaints + ' repaints/s') $note @($rpArm, $rpBase) $rpForce
    if ($rpArm.repaints -gt 5) {
        Add-Comp 'ms_per_repaint' (10.0 * ($rpArm.app_dwm - $rpBase.app_dwm) / $rpArm.repaints) (10.0 * ($rpArm.spread + $rpBase.spread) / $rpArm.repaints) 'ms' 'repaint_tax / repaint rate' '' @($rpArm, $rpBase)
    }
}
if ($s322 -ne $null -and $s3 -ne $null) { Add-Comp 'present_cap_saving' ($s322.app_dwm - $s3.app_dwm) ($s322.spread + $s3.spread) 'pts' 'SHAPED-3D-22 - SHAPED-3D, app+dwm' 'negative = the cap saves; transparent-derived: session-bound' @($s322, $s3) }
if ($ship -ne $null -and $idle -ne $null) {
    Add-Comp 'our_margin_total' ($ship.total - $idle.total) ($ship.tspread + $idle.tspread) 'pts' 'SHIP total - IDLE-P total' 'what the box pays over idle - closest to a partner KPI' @($ship, $idle)
    Add-Comp 'our_margin_app_dwm' ($ship.app_dwm - $idle.app_dwm) ($ship.spread + $idle.spread) 'pts' 'SHIP - IDLE-P, app+dwm' '' @($ship, $idle)
}
# Phase B quadrant: the agent's cost per variant, GPU and CPU separately -
# run on BOTH variants, these two rows ARE the CPU-gap attribution.
if ($noag -ne $null -and $ship -ne $null) {
    Add-Comp 'agent_cost_gpu' ($ship.app_dwm - $noag.app_dwm) ($ship.spread + $noag.spread) 'pts' 'SHIP - SHIP-NOAGENT, app+dwm' 'the live agent GPU cost' @($ship, $noag)
    Add-Comp 'agent_cost_cpu' ($ship.app_cpu - $noag.app_cpu) 0 'cpu%' 'SHIP - SHIP-NOAGENT, app CPU' 'the live agent CPU cost (no spread propagated for CPU)' @($ship, $noag)
}
if ($p22 -ne $null -and $ship -ne $null) { Add-Comp 'present_cap_saving' ($p22.app_dwm - $ship.app_dwm) ($p22.spread + $ship.spread) 'pts' 'PRESENT-22 - SHIP, app+dwm' 'negative = the cap saves; transparent-derived: session-bound' @($p22, $ship) }
if ($shipm -ne $null -and $ship -ne $null) { Add-Comp 'ship_motion_tax' ($shipm.app_dwm - $ship.app_dwm) ($shipm.spread + $ship.spread) 'pts' 'SHIP-M - SHIP, app+dwm' 'the demo-honest delta; transparent-derived: session-bound' @($shipm, $ship) }
if ($idlem -ne $null -and $idle -ne $null) { Add-Comp 'idle_motion_floor' ($idlem.total - $idle.total) ($idlem.tspread + $idle.tspread) 'pts' 'IDLE-M - IDLE-P, total' 'control: ~0 expected (not ours)' @($idlem, $idle) }

# --- Identity block ----------------------------------------------------------
function CapStr { param($v) if ($v -eq $null -or $v -eq '') { return '?' } return [string]$v }
$runtimeTag = '?'; $pluginVer = '?'
if ($caps -ne $null -and $caps.displayxr -ne $null -and $caps.displayxr.info) {
    if ($caps.displayxr.info -match "git-tag:\s+'([^']+)'") { $runtimeTag = $Matches[1] }
    if ($caps.displayxr.info -match "version='([^']+)'") { $pluginVer = $Matches[1] }
}
# Runtime UNDER TEST: the cli reports the INSTALLED runtime, which is not what
# the arms loaded when runtimeJson pointed elsewhere (measured: a dev build in
# Program Files leaked '-128-NOTFOUND' into cards whose arms ran the build
# tree). The truth is the per-process 'loaded from:' WARN line in the
# harvested app logs - authoritative by definition.
$armRuntime = '?'
$logDir = Join-Path $ResultsDir 'dxr-logs'
if (Test-Path $logDir) {
    foreach ($lf in (Get-ChildItem $logDir -Filter '*.log' | Sort-Object LastWriteTime -Descending)) {
        $ln = (Select-String -Path $lf.FullName -Pattern "runtime .* '([^']+)' loaded from: (.+?) \(XR_RUNTIME_JSON" | Select-Object -First 1)
        if ($ln -ne $null) {
            $armRuntime = ($ln.Matches[0].Groups[1].Value + ' @ ' + $ln.Matches[0].Groups[2].Value)
            break
        }
    }
}
$panel = '?'
if ($caps -ne $null) {
    $act = @($caps.adapters | Where-Object { $_.activeMode -ne $null }) | Select-Object -First 1
    if ($act -ne $null) { $panel = $act.name + ' @ ' + $act.activeMode }
}
$worstOther = ''
$oc = @{}
foreach ($r in $rows) {
    if ($r.PSObject.Properties['top_other'] -ne $null -and $r.top_other -ne '') {
        $p = $r.top_other.Split(':')
        if ($p.Count -eq 2) {
            if (-not $oc.ContainsKey($p[0]) -or [double]$p[1] -gt $oc[$p[0]]) { $oc[$p[0]] = [double]$p[1] }
        }
    }
}
if ($oc.Count -gt 0) {
    $worstOther = (@($oc.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 2 |
        ForEach-Object { $_.Key + ':' + $_.Value })) -join ', '
}

$out = @()
$out += '# DXR Perf Ladder - REPORT CARD'
$out += ''
$out += '| | |'
$out += '|---|---|'
$out += ('| host | ' + (CapStr $(if ($caps) { $caps.hostname } else { $null })) + ' |')
$out += ('| date (UTC) | ' + (CapStr $(if ($caps) { $caps.probedAtUtc } else { $null })) + ' |')
$out += ('| kit / results | ' + (CapStr $(if ($man) { $man.kit + ' ' + $man.version } else { $null })) + ' / ' + (Split-Path $ResultsDir -Leaf) + ' |')
$out += ('| runtime (arms loaded) | ' + $armRuntime + ' |')
$out += ('| runtime (installed, via cli) | ' + $runtimeTag + ' |')
$out += ('| display plug-in | ' + $pluginVer + ' |')
$out += ('| scanout | ' + $panel + '  LUID ' + (CapStr $(if ($caps) { $caps.scanoutLuid } else { $null })) + ' |')
$out += ('| SR Platform | ' + (CapStr $(if ($caps) { $caps.srPlatform } else { $null })) + ' |')
$out += ('| power / elevation | ' + $(if ($caps -and $caps.power.onAc) { 'AC' } else { 'BATTERY' }) + ' / ' + $(if ($caps -and $caps.elevated) { 'elevated' } else { 'non-elevated' }) + ' |')
$out += ('| background (worst top_other) | ' + $(if ($worstOther -ne '') { $worstOther } else { 'none > 1 pt' }) + ' |')
$out += ''

$out += '## Derived components (value +/- WITHIN-RUN spread; app+dwm deltas unless stated)'
$out += ''
$out += 'Spread here is rep-to-rep PRECISION inside this one run. Reference-box measurement (Arc, 9 runs / 3 sessions): across-session reproducibility is ~2x wider for FLIP/opaque-derived components and ~5-6x wider for transparent-derived ones - single-run bars on transparent components are not reproducibility. FLIP-derived components travel across sessions/boxes; transparent-derived ones are session-bound and need a per-session anchor.'
$out += ''
$out += '| component | value | spread | unit | verdict | derivation | note |'
$out += '|---|---:|---:|---|---|---|---|'
foreach ($c in $comps) {
    $vTxt = if ($c.Verdict -eq 'OK') { '**' + $c.Value + '**' } else { [string]$c.Value }
    $out += ('| {0} | {1} | +/-{2} | {3} | {4} | {5} | {6} |' -f $c.Name, $vTxt, $c.Spread, $c.Unit, $c.Verdict, $c.Derivation, $c.Note)
}
if ($comps.Count -eq 0) { $out += '| (not enough arms for derivations) | | | | | | |' }
$out += ''
$out += 'Only **bold** (OK) values are quotable. NOISY = real effect, poor magnitude; UNRESOLVED = below the resolution floor at this n (for a control arm that IS the answer); SINGLE-REP = one rep, no spread information (smoke); INVALID = the arm did not measure its mechanism.'
$out += ''

$out += '## Per-arm medians (GPU busy %, Running-Time deltas; spread = max-min across kept reps, app+dwm)'
$out += ''
$out += 'app+dwm is the SUBJECT (client + svc + dwm). **tot@scanout / tot@other are adapter totals across ALL processes** and are first-class, not noise: the same physical cross-adapter transfer is charged to the SERVICE when a bridge issues it explicitly, but to dwm/System when it happens implicitly inside Present - so subject-only accounting flatters the implicit arm and biases against the split (#1154).'
$out += ''
$out += '| arm | n | app@scanout | app@other | svc | dwm | app+dwm | spread | tot@scanout | tot@other | total | app CPU | presents/s | weaves/s | repaints/s | mode | flags |'
$out += '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|'
foreach ($a in $armNames) {
    $m = $med[$a]
    $out += ('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | {12} | {13} | {14} | {15} | {16} |' -f `
        $a, $m.n, $m.app_scanout, $m.app_other, $m.svc, $m.dwm, $m.app_dwm, $m.spread, `
        $m.tot_scanout, $m.tot_other, $m.total, $m.app_cpu, `
        $m.presents, $m.weaves, $m.repaints, $m.mode, $m.flags)
}
$out += ''

# --- Gates -------------------------------------------------------------------
$out += '## Gates'
$out += ''
$g = @()
if ($caps -ne $null) {
    if ($caps.power.onAc) { $g += '- PASS: on AC power' } else { $g += '- FLAG: ON BATTERY - all numbers suspect' }
    if ($caps.elevated) { $g += '- FLAG: harness ran elevated (loader may resolve a different runtime)' } else { $g += '- PASS: non-elevated' }
}
$dropped = $allRows.Count - $rows.Count
if ($dropped -gt 0) { $g += ('- FLAG: ' + $dropped + ' sample(s) DROPPED from medians (failed GPU sweep / zero row)') }
$flagged = @($rows | Where-Object { $_.flags -ne '' })
if ($flagged.Count -eq 0 -and $dropped -eq 0) { $g += '- PASS: no per-sample flags' }
elseif ($flagged.Count -gt 0) { $g += ('- FLAG: ' + $flagged.Count + ' kept sample(s) carry flags (see table + ladder.csv)') }
$modeBad = @($rows | Where-Object { $_.mode -ne '' -and $_.arm -match '2D' -and $_.mode -ne '2d' })
if ($modeBad.Count -gt 0) { $g += '- FLAG: a 2D arm measured in non-2d mode' }
$out += $g
$out += ''
$out += ('Generated ' + (Get-Date -Format 'yyyy-MM-dd HH:mm:ss') + ' by summarize.ps1. Compare report cards by DIFFING them: section order, component names and units are stable. Compare derived components across boxes; never raw columns (dwm is bistable across sessions).')

$sumPath = Join-Path $ResultsDir 'SUMMARY.md'
[IO.File]::WriteAllText($sumPath, (($out -join "`r`n") + "`r`n"), (New-Object Text.UTF8Encoding($false)))
Write-Host ('SUMMARY.md written: ' + $sumPath)

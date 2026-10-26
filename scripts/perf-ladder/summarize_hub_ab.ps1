# summarize_hub_ab.ps1 - runs.csv (run_hub_ab.ps1) -> summary.md.
# Only runs whose path was VERIFIED (status OK) are counted; REFUSED runs are
# listed with their placement line so the reason is visible, never averaged.
# Verdict per app: interleaved pairs (native rep k vs hub rep k, both OK) on
# R p50 - the weave->scanout residual of app frames.
# PowerShell 5.1, ASCII only.
param(
    [Parameter(Mandatory = $true)][string]$ResultsDir,
    [double]$NoiseMs = 0.5,        # absolute floor of the noise band
    [double]$NoiseFrac = 0.10      # relative band: 10% of the native median
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib_sample.ps1')

$csv = Join-Path $ResultsDir 'runs.csv'
$rows = @(Import-Csv $csv)
$envj = $null
$ej = Join-Path $ResultsDir 'environment.json'
if (Test-Path $ej) { $envj = Get-Content $ej -Raw | ConvertFrom-Json }

function Med { param($rs, [string]$c)
    $v = @($rs | ForEach-Object { $_.$c } | Where-Object { $_ -ne $null -and $_ -ne '' } | ForEach-Object { [double]$_ })
    if ($v.Count -eq 0) { return '-' }
    return [math]::Round((Get-TrueMedian $v), 2)
}
function Mode1 { param($rs, [string]$c)
    $g = @($rs | Where-Object { $_.$c -ne '' } | Group-Object $c | Sort-Object Count -Descending)
    if ($g.Count -eq 0) { return '-' }
    return $g[0].Name
}

$md = New-Object System.Collections.ArrayList
function W { param([string]$s) [void]$md.Add($s) }

W '# Hub A/B - native vs hub, in-process compositors (#2317 P1d)'
W ''
if ($envj -ne $null) {
    W ('- host `' + $envj.host + '`, started ' + $envj.started + ', topology **' + $envj.topology + '** ' + (($envj.topologyEnv | ConvertTo-Json -Compress) -replace '\|', '/'))
    W ('- window ' + $envj.windowSec + ' s, warmup ' + $envj.warmupSec + ' s, reps ' + $envj.reps + ' (interleaved A/B per rep), cooldown ' + $envj.cooldownSec + ' s')
    W ('- AC power: ' + $envj.onAc + '; harness elevated: ' + $envj.harnessElevated + ' (apps launched de-elevated via explorer.exe)')
    $ad = @($envj.adapters | ForEach-Object { $_.name + ' (' + $_.activeMode + ')' }) -join '; '
    W ('- adapters: ' + $ad + '; scanout LUID ' + $envj.scanoutLuid)
    W ('- runtime: ' + $(if ($envj.runtimeJson) { 'XR_RUNTIME_JSON=' + $envj.runtimeJson } else { 'installed (registry ActiveRuntime)' }) + '; PresentMon: ' + $envj.presentMon)
    W ('- tracking: ' + $envj.tracking)
}
$rts = @($rows | Where-Object { $_.runtime -ne '' } | ForEach-Object { $_.runtime + ' / dp ' + $_.dp } | Select-Object -Unique)
W ('- runtime loaded by the runs (from each app log): ' + $(if ($rts.Count -gt 0) { '`' + ($rts -join '`, `') + '`' } else { '-' }) + $(if ($rts.Count -gt 1) { ' **MIXED - runs off the pinned tag are REFUSED**' } else { '' }))
W ''
W 'R = weave->scanout residual of APP frames (ms, from `DXR_WEAVE_LATENCY_CSV`); D = scanout - predictedDisplayTime (ms); GPU = Running Time busy % of the app pid per adapter (scanout / render-other), dwm = all dwm on all adapters. Medians across verified reps.'
W ''
W '| app | arm | path | n ok/all | R site | R p50 | R p95 | R p99 | R n | D p50 | D p95 | present/s | repaint/s | weave/s | GPU app scanout % | GPU app other % | GPU dwm % | app CPU % | PM mode | PM ms between p50/p95 | PM ms until displayed p50 | tracked |'
W '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|'
$groups = $rows | Group-Object { $_.app + '|' + $_.arm }
foreach ($g in $groups) {
    $all = @($g.Group); $ok = @($all | Where-Object { $_.status -eq 'OK' })
    $a = $all[0]
    $pth = $(if ($ok.Count -gt 0) { $ok[0].path }
             elseif ((Mode1 $all 'path') -ne $a.expect_path) { 'REFUSED (' + (Mode1 $all 'path') + ' != ' + $a.expect_path + ')' }
             else { 'REFUSED (' + ((Mode1 $all 'flags') -replace '\|', '/') + ')' })
    W ('| {0} | {1} | {2} | {3}/{4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | {12} | {13} | {14} | {15} | {16} | {17} | {18} | {19} | {20}/{21} | {22} | {23} |' -f `
        $a.app, $a.arm, $pth, $ok.Count, $all.Count, (Mode1 $ok 'r_site'), (Med $ok 'r_p50'), (Med $ok 'r_p95'), (Med $ok 'r_p99'), (Med $ok 'r_n'),
        (Med $ok 'd_p50'), (Med $ok 'd_p95'), (Med $ok 'present_s'), (Med $ok 'repaint_s'), (Med $ok 'weave_s'),
        (Med $ok 'gpu_app_scanout'), (Med $ok 'gpu_app_render'), (Med $ok 'gpu_dwm'), (Med $ok 'app_cpu'),
        (Mode1 $ok 'pm_mode'), (Med $ok 'pm_between_p50'), (Med $ok 'pm_between_p95'), (Med $ok 'pm_until_displayed_p50'), (Mode1 $all 'tracked'))
}
W ''
W '## Verdict (interleaved pairs, R p50)'
W ''
$combos = @()
foreach ($app in @($rows | ForEach-Object { $_.app } | Select-Object -Unique)) {
    $others = @($rows | Where-Object { $_.app -eq $app -and $_.arm -ne 'native' } | ForEach-Object { $_.arm } | Select-Object -Unique)
    if ($others.Count -eq 0) { $combos += ,@($app, '') } else { foreach ($o in $others) { $combos += ,@($app, $o) } }
}
foreach ($cb in $combos) {
    $app = $cb[0]; $hubArm = $cb[1]
    $lbl = $(if ($hubArm -ne '') { $app + ' ' + $hubArm + ' vs native' } else { $app })
    $nat = @($rows | Where-Object { $_.app -eq $app -and $_.arm -eq 'native' -and $_.status -eq 'OK' -and $_.r_p50 -ne '' })
    $hub = @($rows | Where-Object { $_.app -eq $app -and $_.arm -eq $hubArm -and $_.status -eq 'OK' -and $_.r_p50 -ne '' })
    if ($hub.Count -eq 0 -and @($rows | Where-Object { $_.app -eq $app -and $_.arm -eq $hubArm }).Count -eq 0) {
        W ('- **' + $lbl + '**: no hub arm (native only).'); continue
    }
    $deltas = @()
    $pres = @()
    $gpus = @()
    $p2d = @()
    foreach ($n in $nat) {
        $h = $hub | Where-Object { $_.rep -eq $n.rep } | Select-Object -First 1
        if ($h -ne $null) {
            $deltas += ([double]$h.r_p50 - [double]$n.r_p50)
            $pres += ([double]$h.present_s - [double]$n.present_s)
            if ($h.gpu_app_total -ne '' -and $n.gpu_app_total -ne '') { $gpus += ([double]$h.gpu_app_total - [double]$n.gpu_app_total) }
            if ($h.pm_until_displayed_p50 -ne '' -and $n.pm_until_displayed_p50 -ne '') { $p2d += ([double]$h.pm_until_displayed_p50 - [double]$n.pm_until_displayed_p50) }
        }
    }
    if ($deltas.Count -eq 0) {
        # Fallback when one site writes no R (vknative needs VK_KHR_present_wait):
        # PresentMon present->display. A DIFFERENT metric, labelled as such.
        $natP = @($rows | Where-Object { $_.app -eq $app -and $_.arm -eq 'native' -and $_.status -eq 'OK' -and $_.pm_until_displayed_p50 -ne '' })
        $hubP = @($rows | Where-Object { $_.app -eq $app -and $_.arm -eq $hubArm -and $_.status -eq 'OK' -and $_.pm_until_displayed_p50 -ne '' })
        $pd = @()
        foreach ($n in $natP) { $h = $hubP | Where-Object { $_.rep -eq $n.rep } | Select-Object -First 1
            if ($h -ne $null) { $pd += ([double]$h.pm_until_displayed_p50 - [double]$n.pm_until_displayed_p50) } }
        if ($pd.Count -eq 0) { W ('- **' + $lbl + '**: no verified native/hub pair - no verdict (see refused runs).'); continue }
        $nm = Get-TrueMedian @($natP | ForEach-Object { [double]$_.pm_until_displayed_p50 })
        $bd = [math]::Max($NoiseMs, $NoiseFrac * $nm); $pm = Get-TrueMedian $pd
        $v = 'within noise'
        if ($pm -le -$bd -and @($pd | Where-Object { $_ -lt 0 }).Count -eq $pd.Count) { $v = 'hub BETTER' }
        elseif ($pm -ge $bd -and @($pd | Where-Object { $_ -gt 0 }).Count -eq $pd.Count) { $v = 'hub WORSE' }
        W ('- **{0}**: {1} on PresentMon MsUntilDisplayed p50 (present->display; R unavailable on one arm) - hub-native median {2:+0.00;-0.00} ms over {3} pair(s), band +/-{4:0.00} ms{5}' -f `
            $lbl, $v, $pm, $pd.Count, $bd, $(if ($pd.Count -lt 3) { ' (n<3: indicative only)' } else { '' }))
        continue
    }
    $natMed = Get-TrueMedian @($nat | ForEach-Object { [double]$_.r_p50 })
    $band = [math]::Max($NoiseMs, $NoiseFrac * $natMed)
    $dm = Get-TrueMedian $deltas
    $neg = @($deltas | Where-Object { $_ -lt 0 }).Count; $pos = @($deltas | Where-Object { $_ -gt 0 }).Count
    $v = 'within noise'
    if ($dm -le -$band -and $neg -eq $deltas.Count) { $v = 'hub BETTER' }
    elseif ($dm -ge $band -and $pos -eq $deltas.Count) { $v = 'hub WORSE' }
    $note = ''
    if ($deltas.Count -lt 3) { $note = ' (n=' + $deltas.Count + ' pair(s): indicative only)' }
    W ('- **{0}**: {1} - R p50 hub-native median {2:+0.00;-0.00} ms over {3} pair(s) [{4}], band +/-{5:0.00} ms; present/s delta median {6:+0.0;-0.0}; app GPU busy delta median {7} points; present->display delta median {9} ms{8}' -f `
        $lbl, $v, $dm, $deltas.Count, (($deltas | ForEach-Object { '{0:+0.00;-0.00}' -f $_ }) -join ', '), $band, (Get-TrueMedian $pres),
        $(if ($gpus.Count -gt 0) { '{0:+0.0;-0.0}' -f (Get-TrueMedian $gpus) } else { '-' }), $note,
        $(if ($p2d.Count -gt 0) { '{0:+0.00;-0.00}' -f (Get-TrueMedian $p2d) } else { '-' }))
}
$hasThr = ($rows.Count -gt 0 -and $rows[0].PSObject.Properties['thr_cpu'] -ne $null -and
           @($rows | Where-Object { $_.thr_cpu -ne '' }).Count -gt 0)
if ($hasThr) {
    W ''
    W '## Per-thread CPU (-ThreadSample, #2371)'
    W ''
    W 'Percent of ONE core over the measurement window, from `thread_sampler.ps1`. A thread is named by the module it STARTED in (Win32 start address; every CRT-created thread starts in `ucrtbase`), `[main]` = the app main thread, where the in-process compositor and the hub weave + present. user/kernel: a loop around a syscall is kernel-heavy; a busy-wait in a driver is user-heavy.'
    W ''
    W '| app | arm | n ok | app CPU % | threads % | user % | kernel % |'
    W '|---|---|---|---|---|---|---|'
    foreach ($g in $groups) {
        $ok = @($g.Group | Where-Object { $_.status -eq 'OK' -and $_.thr_cpu -ne '' })
        $a = @($g.Group)[0]
        W ('| {0} | {1} | {2} | {3} | {4} | {5} | {6} |' -f $a.app, $a.arm, $ok.Count, (Med $ok 'app_cpu'), (Med $ok 'thr_cpu'), (Med $ok 'thr_user'), (Med $ok 'thr_kernel'))
    }
    W ''
    W '| rep | app | arm | pid | status | app CPU % | threads % (user/kernel) | top 3 threads: module+offset tid: % (user/kernel) |'
    W '|---|---|---|---|---|---|---|---|'
    foreach ($r in $rows) {
        if ($r.thr_cpu -eq '') { continue }
        W ('| {0} | {1} | {2} | {3} | {4} | {5} | {6} ({7}/{8}) | {9} |' -f $r.rep, $r.app, $r.arm, $r.pid, $r.status, $r.app_cpu,
            $r.thr_cpu, $r.thr_user, $r.thr_kernel, (($r.thr_top3 -replace '\|', '/') -replace ' / ', '<br>'))
    }
}
W ''
W '## Placement evidence (verbatim, first run of each arm)'
W ''
foreach ($g in $groups) {
    $r0 = @($g.Group)[0]
    $rj = Join-Path (Join-Path $ResultsDir $r0.run_dir) 'run.json'
    if (-not (Test-Path $rj)) { continue }
    $j = [IO.File]::ReadAllText($rj, [Text.Encoding]::UTF8) | ConvertFrom-Json
    W ('- `' + $r0.app + '/' + $r0.arm + '` (' + $j.status + ', path ' + $j.path + '):')
    W ('  - `' + $(if ($j.placement) { $j.placement } else { '<no weave placement line>' }) + '`')
    if ($j.hubPlacement) { W ('  - `' + $j.hubPlacement + '`') }
    foreach ($l in @($j.activeLines)) { if ($l) { W ('  - `' + $l + '`') } }
}
W ''
W '## All runs'
W ''
W '| rep | app | arm | status | path | witness site | present/s | R p50 | R p95 | R n | D n | GPU app % | fg | flags |'
W '|---|---|---|---|---|---|---|---|---|---|---|---|---|---|'
foreach ($r in $rows) {
    W ('| {0} | {1} | {2} | {3} | {4} | {5} | {6} | {7} | {8} | {9} | {10} | {11} | {12} | {13} |' -f $r.rep, $r.app, $r.arm, $r.status, $r.path, $r.witness_site,
        $r.present_s, $r.r_p50, $r.r_p95, $r.r_n, $r.d_n, $r.gpu_app_total, $r.foreground, ($r.flags -replace '\|', '/'))
}
$out = Join-Path $ResultsDir 'summary.md'
[IO.File]::WriteAllText($out, ($md -join "`n") + "`n", (New-Object Text.UTF8Encoding($false)))
Write-Host ('summary: ' + $out)

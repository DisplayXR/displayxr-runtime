# run_hub_ab.ps1 - native-vs-hub A/B for the in-process compositors (#2317 P1d).
#
# For each cube_handle_<api>_win app, runs every arm (an arm is a NAME plus an
# ENV MAP, data in hub-ab.json) INTERLEAVED: rep 1 = A,B ; rep 2 = A,B ; ...
# Each run is one fresh app launch, measured over one window after warmup.
#
# Per run it collects:
#   - weave->scanout residual R (p50/p95/p99, n) and prediction error D from
#     DXR_WEAVE_LATENCY_CSV=<run dir>\wl  (-> wl.<site>.csv, site = d3d11 |
#     d3d12 | vknative), restricted to the sample window by QPC;
#   - [WITNESS] present/repaint/weave rates + mode (DXR_FRAME_WITNESS);
#   - the `weave placement:` / `hub placement:` lines VERBATIM, plus the split /
#     reroute ACTIVE lines, classified into a path (native | fill-arm |
#     own-split | hub). A run whose path does not match the arm's expectPath is
#     REFUSED: it is recorded, but never counted in the summary;
#   - GPU Running Time per adapter for the app pid (+ dwm, + adapter totals),
#     the lib_sample.ps1 / gpusample.ps1 delta method;
#   - PresentMon present mode + MsBetweenPresents / MsUntilDisplayed when a
#     PresentMon exe is on PATH or passed with -PresentMon (optional).
#
# Hygiene: the app is launched NON-elevated through explorer.exe -> .bat (env is
# process-level in the .bat, so the runtime DLL's getenv sees it), centred, and
# forced to the foreground; a run that is not foreground at window start/end is
# flagged. Before every launch the harness waits (up to -WaitForPanelMin) while
# any OTHER XR client is running - it never kills a process it did not start,
# and never touches displayxr-service.exe.
#
# PowerShell 5.1, ASCII only. Run from any prompt (elevated is fine: the app is
# de-elevated by the explorer launch; PresentMon needs admin or Performance Log
# Users).
param(
    [string]$ConfigPath = (Join-Path $PSScriptRoot 'hub-ab.json'),
    [string]$OutRoot = (Join-Path $PSScriptRoot 'results'),
    [string]$AppDir = '',                 # overrides config appDir
    [string[]]$Apps = @(),                # subset, e.g. -Apps d3d12,vk
    [string[]]$Arms = @(),                # subset, e.g. -Arms native,hub (default: all non-optional)
    [string]$Topology = 'default',        # key of config.topologies
    [string]$RuntimeJson = '',            # sets XR_RUNTIME_JSON (omitted = installed runtime)
    [string]$RuntimePath = '',            # prepended to PATH (a from-source runtime's _package\bin)
    [int]$Windows = 0,                    # window seconds (0 = config)
    [int]$Reps = 0,                       # reps (0 = config)
    [int]$Warmup = -1,
    [int]$Cooldown = -1,
    [string]$PresentMon = '',             # PresentMon exe; '' = look on PATH; 'none' = skip
    [int]$WaitForPanelMin = 20,
    [string]$Label = '',
    [string]$ExpectRuntime = ''          # runtime git tag every run must load; '' = pinned to the first run's
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'lib_sample.ps1')

foreach ($n in @('Apps', 'Arms')) {
    $v = Get-Variable $n -ValueOnly
    if ($v.Count -eq 1 -and $v[0] -match ',') { Set-Variable $n ($v[0].Split(',')) }
}

$cfg = Get-Content $ConfigPath -Raw | ConvertFrom-Json
$windowSec = if ($Windows -gt 0) { $Windows } else { [int]$cfg.windowSeconds }
$reps      = if ($Reps -gt 0) { $Reps } else { [int]$cfg.reps }
$warmupSec = if ($Warmup -ge 0) { $Warmup } else { [int]$cfg.warmupSeconds }
$coolSec   = if ($Cooldown -ge 0) { $Cooldown } else { [int]$cfg.cooldownSeconds }
if ($cfg.topologies.PSObject.Properties[$Topology] -eq $null) { throw ("unknown topology '" + $Topology + "'") }
$topoEnv = $cfg.topologies.$Topology

if ($AppDir -eq '') {
    $AppDir = $cfg.appDir
    if (-not [IO.Path]::IsPathRooted($AppDir)) { $AppDir = Join-Path (Split-Path (Resolve-Path $ConfigPath) -Parent) $AppDir }
}
if (-not (Test-Path $AppDir)) { throw ('app dir not found: ' + $AppDir + ' (pass -AppDir)') }
$AppDir = (Resolve-Path $AppDir).Path

# PresentMon
$pmExe = $null
if ($PresentMon -ne 'none') {
    if ($PresentMon -ne '') { if (Test-Path $PresentMon) { $pmExe = (Resolve-Path $PresentMon).Path } }
    else {
        foreach ($c in @('PresentMon', 'PresentMon64', 'PresentMon-x64')) {
            $g = Get-Command $c -ErrorAction SilentlyContinue
            if ($g -ne $null) { $pmExe = $g.Source; break }
        }
    }
}

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$tag = 'hub-ab-' + $stamp
if ($Label -ne '') { $tag = $tag + '-' + $Label }
$outDir = Join-Path $OutRoot $tag
New-Item -ItemType Directory -Force $outDir | Out-Null
$runLog = Join-Path $outDir 'run.log'
$csvPath = Join-Path $outDir 'runs.csv'

function Log { param([string]$m)
    $line = ('[{0}] {1}' -f (Get-Date -Format 'HH:mm:ss'), $m)
    Write-Host $line
    Add-Content -Path $runLog -Value $line
}

# --- Win32: focus + QPC -------------------------------------------------------
Ensure-Win32
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class HubAbWin32 {
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int n);
    [DllImport("user32.dll")] public static extern bool BringWindowToTop(IntPtr h);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool AttachThreadInput(uint a, uint b, bool f);
    [DllImport("kernel32.dll")] public static extern uint GetCurrentThreadId();
    [DllImport("user32.dll")] public static extern void keybd_event(byte vk, byte scan, uint flags, UIntPtr extra);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    public static uint ForegroundPid() {
        uint p = 0; GetWindowThreadProcessId(GetForegroundWindow(), out p); return p;
    }
    // AttachThreadInput to the current foreground thread lifts the
    // foreground lock; a lone ALT tap is the fallback when that is refused.
    public static bool Focus(IntPtr h, bool altTap) {
        uint dummy;
        uint fgT = GetWindowThreadProcessId(GetForegroundWindow(), out dummy);
        uint me = GetCurrentThreadId();
        if (altTap) { keybd_event(0x12, 0, 0, UIntPtr.Zero); keybd_event(0x12, 0, 2, UIntPtr.Zero); }
        bool att = (fgT != 0 && fgT != me) ? AttachThreadInput(me, fgT, true) : false;
        ShowWindow(h, 9); BringWindowToTop(h); SetForegroundWindow(h);
        if (att) AttachThreadInput(me, fgT, false);
        return GetForegroundWindow() == h;
    }
}
'@
function Get-Qpc { return [Diagnostics.Stopwatch]::GetTimestamp() }

function Set-AppForeground {
    param([IntPtr]$hwnd, [int]$appPid)
    if ([HubAbWin32]::ForegroundPid() -eq $appPid) { return $true }
    if ([HubAbWin32]::Focus($hwnd, $false)) { return $true }
    Start-Sleep -Milliseconds 200
    [HubAbWin32]::Focus($hwnd, $true) | Out-Null
    Start-Sleep -Milliseconds 200
    return ([HubAbWin32]::ForegroundPid() -eq $appPid)
}

function Read-Shared {
    # The app keeps its log open for writing; read with FileShare.ReadWrite, UTF-8
    # (placement lines carry an em dash).
    param([string]$path)
    if ([string]::IsNullOrEmpty($path) -or -not (Test-Path $path)) { return @() }
    $fs = New-Object IO.FileStream($path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $sr = New-Object IO.StreamReader($fs, [Text.Encoding]::UTF8)
        $txt = $sr.ReadToEnd()
    } finally { $fs.Close() }
    return @($txt -split "`r?`n")
}

# --- Foreign XR clients: wait, never kill -------------------------------------
function Get-ForeignXrClients {
    param([int[]]$mine)
    $out = @()
    $rows = @()
    try { $rows = @(tasklist /m DisplayXRClient.dll /fo csv /nh 2>$null) } catch { }
    foreach ($r in $rows) {
        if ($r -match '^"([^"]+)","(\d+)"') {
            $nm = $Matches[1]; $id = [int]$Matches[2]
            if ($nm -match '^displayxr-') { continue }   # service + its satellites (tracking/conversion host), cli
            if ($mine -contains $id) { continue }
            $out += ($nm + ':' + $id)
        }
    }
    foreach ($p in @(Get-Process -ErrorAction SilentlyContinue | Where-Object { $_.ProcessName -match '^(cube_|windowspace_|cts_|weave_rpc|workspace_minimal)' })) {
        if ($mine -contains $p.Id) { continue }
        $k = $p.ProcessName + '.exe:' + $p.Id
        if (-not ($out -contains $k)) { $out += $k }
    }
    return $out
}

function Wait-PanelFree {
    # Our own previous instance must be GONE first (Stop-Process returns before
    # the process has exited), then nobody else may hold the panel.
    $t = (Get-Date).AddSeconds(20)
    while ((Get-Date) -lt $t) {
        $alive = @($script:startedPids | Where-Object { (Get-Process -Id $_ -ErrorAction SilentlyContinue) -ne $null })
        if ($alive.Count -eq 0) { break }
        Start-Sleep -Milliseconds 500
    }
    $deadline = (Get-Date).AddMinutes($WaitForPanelMin)
    while ($true) {
        $f = @(Get-ForeignXrClients -mine $script:startedPids)
        if ($f.Count -eq 0) { return $true }
        if ((Get-Date) -gt $deadline) { Log ('ABORT: panel still busy after ' + $WaitForPanelMin + ' min: ' + ($f -join ', ')); return $false }
        Log ('panel busy (' + ($f -join ', ') + ') - waiting 60 s; not touching processes this harness did not start')
        Start-Sleep -Seconds 60
    }
}

# --- Log parsing ----------------------------------------------------------------
function Get-PathEvidence {
    # Classify the engaged path from the app's runtime log. The LAST placement
    # line is the truth (a retire emits `weave placement: CHANGED`).
    param([string[]]$lines)
    $ev = [ordered]@{
        placement = ''; hubPlacement = ''; active = @(); path = 'unknown'; tracked = 'unknown'
        lateWeave = ''; loadedFrom = ''; changed = $false; runtime = ''; dp = ''
    }
    foreach ($ln in $lines) {
        if ($ln -match '(weave placement: .*)$') {
            $ev.placement = $Matches[1]
            if ($ln -match 'weave placement: CHANGED') { $ev.changed = $true }
        }
        if ($ln -match '(hub placement: .*)$') { $ev.hubPlacement = $Matches[1] }
        if ($ln -match 'output-device split ACTIVE|reroute ACTIVE|ENGAGE via the d3d11 fill arm') {
            $i = $ln.IndexOf('] [', 26); if ($i -lt 0) { $i = 0 }
            $ev.active += $ln.Substring($i).TrimStart(']', ' ')
        }
        if ($ln -match 'Eye tracking state changed: isTracking=(\d)') { $ev.tracked = $Matches[1] }
        if ($ln -match 'loaded from: (.*)$') { $ev.loadedFrom = $Matches[1] }
        if ($ev.runtime -eq '' -and $ln -match "DisplayXR runtime \S+ '([^']+)' loaded from") { $ev.runtime = $Matches[1] }
        if ($ev.dp -eq '' -and $ln -match "plugin loader: active plug-in: id=(\S+) .*version='([^']*)'") { $ev.dp = $Matches[1] + '@' + $Matches[2] }
        if ($ev.lateWeave -eq '' -and $ln -match '(Late-weave: .*)$') { $ev.lateWeave = $Matches[1] }
    }
    $act = ($ev.active -join ' | ')
    if ($ev.hubPlacement -match 'hub=1') { $ev.path = 'hub' }
    elseif ($ev.changed) { $ev.path = 'retired' }
    elseif ($act -match 'heavy-d3d12 reroute ACTIVE|VK output-device split ACTIVE') { $ev.path = 'fill-arm' }
    elseif ($act -match 'd3d12_split_stage_a|D3D11 output-device split ACTIVE|#918 output-device split ACTIVE') { $ev.path = 'own-split' }
    elseif ($ev.placement -match 'split=0') { $ev.path = 'native' }
    elseif ($ev.placement -eq '') { $ev.path = 'native?' }   # no placement line at all (GL on older builds)
    return $ev
}

# --- Latency CSV -------------------------------------------------------------------
function Get-Pct {
    param([double[]]$sorted, [double]$p)
    $n = $sorted.Count
    if ($n -eq 0) { return '' }
    $i = [int][math]::Ceiling($p / 100.0 * $n) - 1
    if ($i -lt 0) { $i = 0 }; if ($i -ge $n) { $i = $n - 1 }
    return [math]::Round($sorted[$i], 2)
}

function Measure-LatencyCsv {
    # R = sync_qpc(first S at/after the F row with PresentCount >= frame's) -
    # qpc_weave (the dxr-perf-study weave_latency.py join), APP weaves only
    # (repaint=0), weave inside [q0, q1]. D = scanout - predictedDisplayTime,
    # rows whose predicted time falls inside the window.
    param([string]$path, [long]$q0, [long]$q1)
    $r = [ordered]@{ site = ''; frames = 0; repaints = 0; rN = 0; r50 = ''; r95 = ''; r99 = ''; dN = 0; d50 = ''; d95 = ''; d99 = '' }
    $r.site = ([IO.Path]::GetFileNameWithoutExtension($path) -split '\.')[-1]
    $freq = 0.0
    $F = New-Object System.Collections.ArrayList
    $S = New-Object System.Collections.ArrayList
    $D = New-Object System.Collections.ArrayList
    $pos = 0
    foreach ($ln in (Read-Shared $path)) {
        $pos++
        if ($ln.Length -lt 3) { continue }
        $p = $ln.Split(',')
        switch ($p[0]) {
            'H' { $freq = [double]$p[1] }
            'F' { if ($p.Count -ge 5) { $rp = 0; if ($p.Count -ge 6) { $rp = [int]$p[5] }
                    [void]$F.Add(@([double]$p[2], [double]$p[4], $pos, $rp)) } }
            'S' { if ($p.Count -ge 6) { [void]$S.Add(@([double]$p[1], [double]$p[4], $pos)) } }
            'D' { if ($p.Count -ge 5) { [void]$D.Add(@([double]$p[3], [double]$p[4])) } }
        }
    }
    if ($freq -le 0) { return $r }
    $ms = 1000.0 / $freq
    $res = New-Object System.Collections.ArrayList
    $si = 0
    foreach ($f in $F) {
        $inWin = ($f[0] -ge $q0 -and $f[0] -le $q1)
        $j = $si
        while ($j -lt $S.Count -and ($S[$j][2] -lt $f[2] -or $S[$j][0] -lt $f[1])) { $j++ }
        if ($inWin) {
            $r.frames++
            if ($f[3] -ne 0) { $r.repaints++ }
            elseif ($j -lt $S.Count -and $S[$j][1] -gt $f[0]) { [void]$res.Add(($S[$j][1] - $f[0]) * $ms) }
        }
        $si = [math]::Max($si, $j - 8)
    }
    $sr = [double[]]@($res | Sort-Object)
    $r.rN = $sr.Count; $r.r50 = Get-Pct $sr 50; $r.r95 = Get-Pct $sr 95; $r.r99 = Get-Pct $sr 99
    $ns0 = $q0 * 1e9 / $freq; $ns1 = $q1 * 1e9 / $freq + 2e8
    $dv = New-Object System.Collections.ArrayList
    foreach ($d in $D) { if ($d[0] -ge $ns0 -and $d[0] -le $ns1) { [void]$dv.Add($d[1] / 1e6) } }
    $sd = [double[]]@($dv | Sort-Object)
    $r.dN = $sd.Count; $r.d50 = Get-Pct $sd 50; $r.d95 = Get-Pct $sd 95; $r.d99 = Get-Pct $sd 99
    return $r
}

# --- PresentMon --------------------------------------------------------------------
function Measure-PresentMon {
    param([string]$path)
    $m = [ordered]@{ n = 0; mode = ''; between50 = ''; between95 = ''; display50 = ''; untilDisp50 = ''; droppedPct = '' }
    if (-not (Test-Path $path)) { return $m }
    $rows = @(Import-Csv $path)
    if ($rows.Count -eq 0) { return $m }
    $cols = @($rows[0].PSObject.Properties | ForEach-Object { $_.Name })
    function Col { param([string]$want) foreach ($c in $cols) { if ($c -ieq $want) { return $c } }; return $null }
    $cMode = Col 'PresentMode'; $cBet = Col 'MsBetweenPresents'; $cDisp = Col 'MsBetweenDisplayChange'
    $cUntil = Col 'MsUntilDisplayed'; $cDrop = Col 'Dropped'
    # The app's swapchain only: the busiest SwapChainAddress (a cube has one).
    $cSc = Col 'SwapChainAddress'
    if ($cSc -ne $null) {
        $top = $rows | Group-Object $cSc | Sort-Object Count -Descending | Select-Object -First 1
        $rows = @($top.Group)
    }
    $m.n = $rows.Count
    if ($cMode -ne $null) { $m.mode = (($rows | Group-Object $cMode | Sort-Object Count -Descending | Select-Object -First 1).Name) }
    function Vals { param($c) if ($c -eq $null) { return [double[]]@() }
        return [double[]]@($rows | ForEach-Object { $v = $_.$c; if ($v -ne $null -and $v -ne '' -and $v -ne 'NA') { [double]$v } } | Sort-Object) }
    $b = Vals $cBet; $m.between50 = Get-Pct $b 50; $m.between95 = Get-Pct $b 95
    $d = Vals $cDisp; $m.display50 = Get-Pct $d 50
    $u = Vals $cUntil; $m.untilDisp50 = Get-Pct $u 50
    if ($cDrop -ne $null) {
        $dr = @($rows | Where-Object { $_.$cDrop -eq '1' }).Count
        $m.droppedPct = [math]::Round(100.0 * $dr / $rows.Count, 1)
    }
    return $m
}

# --- Block 0: environment -------------------------------------------------------
& (Join-Path $PSScriptRoot 'probe_caps.ps1') -OutDir $outDir | Out-Null
$caps = Get-Content (Join-Path $outDir 'capabilities.json') -Raw | ConvertFrom-Json
$scanoutLuid = ''
if ($caps.scanoutLuid) { $scanoutLuid = $caps.scanoutLuid }
$cliExe = $null
foreach ($p in @((Join-Path $env:ProgramFiles 'DisplayXR\Runtime\displayxr-cli.exe'), (Join-Path $env:ProgramFiles 'DisplayXR\Runtime\bin\displayxr-cli.exe'))) {
    if (Test-Path $p) { $cliExe = $p; break }
}
if ($cliExe -ne $null) {
    try { & $cliExe info 2>&1 | Out-File -Encoding utf8 (Join-Path $outDir 'displayxr-cli-info.txt') } catch { }
    try { & $cliExe status --json 2>$null | Out-File -Encoding utf8 (Join-Path $outDir 'displayxr-cli-status.json') } catch { }
}
$envInfo = [ordered]@{
    harness = 'run_hub_ab.ps1'; kitVersion = $cfg.kitVersion; started = (Get-Date).ToString('s')
    host = $env:COMPUTERNAME; topology = $Topology; topologyEnv = $topoEnv
    runtimeJson = $RuntimeJson; runtimePath = $RuntimePath; appDir = $AppDir
    windowSec = $windowSec; warmupSec = $warmupSec; reps = $reps; cooldownSec = $coolSec
    presentMon = $(if ($pmExe) { $pmExe } else { 'not found - skipped' })
    harnessElevated = $caps.elevated; onAc = $caps.power.onAc; scanoutLuid = $scanoutLuid
    adapters = $caps.adapters
    tracking = 'not controlled: untracked is the autonomous default; a tracked run needs a human in front of the panel. Per-run tracked= is the last isTracking event in the app log.'
}
[IO.File]::WriteAllText((Join-Path $outDir 'environment.json'), ($envInfo | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding($false)))
Log ('hub A/B: topology=' + $Topology + ' window=' + $windowSec + 's reps=' + $reps + ' onAC=' + $caps.power.onAc + ' scanout=' + $scanoutLuid)
if (-not $caps.power.onAc) { Log 'FLAG: on battery - results invalid; plug in AC.' }
if ($pmExe) { Log ('PresentMon: ' + $pmExe) } else { Log 'PresentMon: not found (pass -PresentMon <exe>) - present-mode columns skipped' }

# --- Plan --------------------------------------------------------------------------
$plan = @()
foreach ($app in $cfg.apps) {
    if ($Apps.Count -gt 0 -and -not ($Apps -contains $app.name)) { continue }
    $exe = Join-Path $AppDir $app.exe
    if (-not (Test-Path $exe)) { Log ('SKIP app ' + $app.name + ': ' + $exe + ' not found'); continue }
    foreach ($arm in $app.arms) {
        $opt = ($arm.PSObject.Properties['optional'] -ne $null -and $arm.optional)
        if ($arm.PSObject.Properties['topologies'] -ne $null -and -not (@($arm.topologies) -contains $Topology)) { continue }
        if ($Arms.Count -gt 0) { if (-not ($Arms -contains $arm.name)) { continue } }
        elseif ($opt) { continue }
        $plan += New-Object PSObject -Property @{ app = $app; arm = $arm; exe = $exe }
    }
}
if ($plan.Count -eq 0) { throw 'nothing to run (check -Apps / -Arms / -AppDir)' }
Log ('plan per rep: ' + (($plan | ForEach-Object { $_.app.name + '/' + $_.arm.name }) -join ', '))

$hdr = 'rep,app,arm,topology,pid,status,path,expect_path,witness_site,present_s,repaint_s,weave_s,mode,r_site,r_frames,r_repaints,r_n,r_p50,r_p95,r_p99,d_n,d_p50,d_p95,d_p99,gpu_app_scanout,gpu_app_render,gpu_app_total,gpu_dwm,gpu_tot_scanout,gpu_tot_other,app_cpu,pm_n,pm_mode,pm_between_p50,pm_between_p95,pm_display_p50,pm_until_displayed_p50,pm_dropped_pct,runtime,dp,tracked,foreground,window_s,run_dir,flags'
$hdr | Out-File -Encoding ascii $csvPath

function Add-FailRow {
    param([int]$rep, $app, $arm, [int]$appPid, [string]$why, [string]$runName)
    $n = ($hdr -split ',').Count
    $v = @($rep, $app.name, $arm.name, $Topology, $appPid, $why, '', [string]$arm.expectPath)
    while ($v.Count -lt $n - 2) { $v += '' }
    $v += $runName; $v += $why
    Add-Content -Path $csvPath -Value ($v -join ',') -Encoding Ascii
}

function Invoke-Run {
    param($item, [int]$rep, [int]$attempt = 1)
    $app = $item.app; $arm = $item.arm
    $flags = @()
    $runName = ('r{0}-{1}-{2}' -f $rep, $app.name, $arm.name)
    if ($attempt -gt 1) { $runName = $runName + '-try' + $attempt }
    $runDir = Join-Path $outDir $runName
    New-Item -ItemType Directory -Force $runDir | Out-Null
    $exeBase = [IO.Path]::GetFileNameWithoutExtension($item.exe)

    if (-not (Wait-PanelFree)) { return 'ABORT' }

    # Env: base -> topology -> arm (later wins). Process-level, in the .bat.
    $envMap = [ordered]@{}
    foreach ($kv in $cfg.baseEnv.PSObject.Properties) { $envMap[$kv.Name] = [string]$kv.Value }
    foreach ($kv in $topoEnv.PSObject.Properties)     { $envMap[$kv.Name] = [string]$kv.Value }
    foreach ($kv in $arm.env.PSObject.Properties)     { $envMap[$kv.Name] = [string]$kv.Value }
    $envMap['DXR_WEAVE_LATENCY_CSV'] = (Join-Path $runDir 'wl')
    if ($RuntimeJson -ne '') { $envMap['XR_RUNTIME_JSON'] = $RuntimeJson }
    $lines = @('@echo off')
    foreach ($k in $envMap.Keys) { $lines += ('set "{0}={1}"' -f $k, $envMap[$k]) }
    if ($RuntimePath -ne '') { $lines += ('set "PATH={0};%PATH%"' -f $RuntimePath) }
    $lines += ('cd /d "{0}"' -f $AppDir)
    # A launch explorer.exe delivers AFTER this run gave up (seen: ~2 min late while another
    # process held the panel) must not start: the next run would adopt it, with THIS arm's env.
    $cancel = Join-Path $runDir 'launch.cancel'
    $lines += ('if exist "{0}" exit /b 0' -f $cancel)
    $lines += ('"{0}" > "{1}" 2>&1' -f $item.exe, (Join-Path $runDir 'app-stdout.txt'))
    $bat = Join-Path $runDir 'launch.bat'
    Set-Content -Path $bat -Value ($lines -join "`r`n") -Encoding Ascii

    $before = @(Get-Process -Name $exeBase -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $t0 = Get-Date
    Start-Process explorer.exe -ArgumentList ('"' + $bat + '"') | Out-Null
    $proc = $null
    while (((Get-Date) - $t0).TotalSeconds -lt 30) {
        $proc = Get-Process -Name $exeBase -ErrorAction SilentlyContinue |
                Where-Object { -not ($before -contains $_.Id) -and $_.StartTime -gt $t0.AddSeconds(-2) } | Select-Object -First 1
        if ($proc -ne $null) { break }
        Start-Sleep -Milliseconds 300
    }
    if ($proc -eq $null) {
        # Cancel a late delivery of this launch (the .bat checks the file before starting the app).
        New-Item -ItemType File -Path $cancel -Force | Out-Null
        Log '  APP_NO_START (launch.cancel written: a late explorer delivery will not start the app)'
        Add-FailRow $rep $app $arm 0 'APP_NO_START' $runName; return 'APP_NO_START'
    }
    $appPid = $proc.Id
    $script:startedPids += $appPid
    $logPath = $null
    $result = 'OK'
    $foreground = 'no'
    try {
        $hwnd = Get-MainWindow -procId $appPid -timeoutSec 20
        if ($hwnd -eq [IntPtr]::Zero) { $flags += 'NO_HWND' }

        # Settle: first [WITNESS] line = the session is presenting.
        $witReq = -not ($arm.PSObject.Properties['witnessRequired'] -ne $null -and -not $arm.witnessRequired)
        $deadline = (Get-Date).AddSeconds([int]$cfg.startupTimeoutSec)
        $settled = $false
        while ((Get-Date) -lt $deadline) {
            if ((Get-Process -Id $appPid -ErrorAction SilentlyContinue) -eq $null) { break }
            if ($logPath -eq $null) { $logPath = Get-LatestDxrLog $exeBase $appPid }
            if ($witReq) {
                if (@(Get-WitnessLines $logPath).Count -gt 0) { $settled = $true; break }
                Start-Sleep -Seconds 1
            } else {
                if (((Get-Date) - $t0).TotalSeconds -ge [int]$cfg.noWitnessSettleSec) { $settled = $true; break }
                Start-Sleep -Seconds 1
            }
        }
        if ((Get-Process -Id $appPid -ErrorAction SilentlyContinue) -eq $null) {
            Log '  APP_EXITED'
            $lp = Get-LatestDxrLog $exeBase $appPid
            if ($lp -ne $null) { Copy-Item $lp (Join-Path $runDir 'app.log') -ErrorAction SilentlyContinue }
            Add-FailRow $rep $app $arm $appPid 'APP_EXITED' $runName
            return 'APP_EXITED'
        }
        if (-not $settled) { $flags += 'NO_WITNESS_AT_STARTUP' }

        if ($hwnd -ne [IntPtr]::Zero) {
            $rc = New-Object HubAbWin32+RECT
            [HubAbWin32]::GetWindowRect($hwnd, [ref]$rc) | Out-Null
            Add-Type -AssemblyName System.Windows.Forms
            $scr = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
            $w = $rc.R - $rc.L; $h = $rc.B - $rc.T
            Move-WindowTo -hwnd $hwnd -x ([int](($scr.Width - $w) / 2)) -y ([int](($scr.Height - $h) / 2))
            if (-not (Set-AppForeground -hwnd $hwnd -appPid $appPid)) { $flags += 'NOT_FOREGROUND_START' }
        }
        Set-CursorParked
        Start-Sleep -Seconds $warmupSec
        if ([HubAbWin32]::ForegroundPid() -ne $appPid) {
            if ($hwnd -eq [IntPtr]::Zero -or -not (Set-AppForeground -hwnd $hwnd -appPid $appPid)) { $flags += 'NOT_FOREGROUND_WINDOW' }
        }

        $witBefore = @(Get-WitnessLines $logPath).Count
        $pm = $null
        $pmCsv = Join-Path $runDir 'presentmon.csv'
        if ($pmExe) {
            $pmArgs = @('--process_id', $appPid, '--output_file', ('"' + $pmCsv + '"'), '--timed', $windowSec,
                        '--terminate_after_timed', '--v1_metrics', '--no_console_stats',
                        '--session_name', 'dxr_hub_ab', '--stop_existing_session')
            $pm = Start-Process -FilePath $pmExe -ArgumentList $pmArgs -WindowStyle Hidden -PassThru `
                  -RedirectStandardOutput (Join-Path $runDir 'presentmon-stdout.txt') -RedirectStandardError (Join-Path $runDir 'presentmon-stderr.txt')
        }
        $cpu0 = Get-CpuSnapshot -procIds @($appPid)
        $g0 = Get-GpuSnapshot
        $q0 = Get-Qpc
        # Focus is part of the subject: losing the foreground mid-window moved R
        # p50 from 16.6 to 66-133 ms in validation. Poll it; re-take it if lost;
        # name the thief; a run that lost focus is REFUSED.
        $fgLostPolls = 0; $thief = ''; $contended = @(); $poll = 0
        $tEnd = (Get-Date).AddSeconds($windowSec)
        while ((Get-Date) -lt $tEnd) {
            Start-Sleep -Milliseconds 500
            # A peer that launches an XR app mid-window shares the panel/GPU:
            # check every ~2 s, never act on it, refuse the run.
            if (($poll++ % 4) -eq 3) {
                foreach ($c in @(Get-ForeignXrClients -mine $script:startedPids)) { if (-not ($contended -contains $c)) { $contended += $c } }
            }
            $fp = [HubAbWin32]::ForegroundPid()
            if ($fp -ne $appPid) {
                $fgLostPolls++
                if ($thief -eq '') { $tp = Get-Process -Id $fp -ErrorAction SilentlyContinue; $thief = $(if ($tp) { $tp.ProcessName } else { 'pid' + $fp }) }
                if ($hwnd -ne [IntPtr]::Zero) { Set-AppForeground -hwnd $hwnd -appPid $appPid | Out-Null }
            }
        }
        $q1 = Get-Qpc
        if ($fgLostPolls -gt 0) { $flags += ('FOCUS_LOST_' + $fgLostPolls + 'x500ms_to_' + $thief) }
        if ($contended.Count -gt 0) { $flags += ('CONTENDED_' + ($contended -join '+')) }
        $g1 = Get-GpuSnapshot
        $cpu1 = Get-CpuSnapshot -procIds @($appPid)
        if ($fgLostPolls -eq 0 -and -not ($flags -match 'NOT_FOREGROUND')) { $foreground = 'yes' }
        if ($pm -ne $null) { $pm.WaitForExit(15000) | Out-Null; if (-not $pm.HasExited) { $flags += 'PM_TIMEOUT'; Stop-Process -Id $pm.Id -Force -ErrorAction SilentlyContinue } }
        $windowS = [math]::Round(($q1 - $q0) / [Diagnostics.Stopwatch]::Frequency, 2)

        $allWit = @(Get-WitnessLines $logPath)
        $wit = @()
        if ($allWit.Count -gt $witBefore) { $wit = @($allWit[$witBefore..($allWit.Count - 1)]) }
        if ($wit.Count -eq 0 -and $witReq) { $flags += 'NO_WITNESS' }
    } finally {
        # Close ONLY the process this harness started.
        Stop-Process -Id $appPid -Force -ErrorAction SilentlyContinue
        Start-Sleep -Seconds 2
    }
    if ($logPath -eq $null) { $logPath = Get-LatestDxrLog $exeBase $appPid }
    if ($logPath -ne $null) { Copy-Item $logPath (Join-Path $runDir 'app.log') -ErrorAction SilentlyContinue }
    $logLines = @(Read-Shared (Join-Path $runDir 'app.log'))
    $ev = Get-PathEvidence $logLines

    # Verify the arm by its own log line, never by its env.
    $expect = [string]$arm.expectPath
    $placeReq = -not ($arm.PSObject.Properties['placementRequired'] -ne $null -and -not $arm.placementRequired)
    $status = 'OK'
    $pathOk = ($ev.path -eq $expect) -or ($expect -eq 'native' -and $ev.path -eq 'native?' -and -not $placeReq)
    if (-not $pathOk) { $status = 'REFUSED' }
    if ($placeReq -and $ev.placement -eq '') { $status = 'REFUSED'; $flags += 'NO_PLACEMENT_LINE' }
    if (-not $placeReq -and $ev.placement -eq '') { $flags += 'NO_PLACEMENT_LINE' }
    if ($expect -eq 'hub' -and $ev.hubPlacement -eq '') { $flags += 'NO_HUB_PLACEMENT_LINE' }
    if ($arm.PSObject.Properties['expectLine'] -ne $null) {
        $hit = @($logLines | Where-Object { $_ -match $arm.expectLine }).Count
        if ($hit -eq 0) { $status = 'REFUSED'; $pathOk = $false; $flags += 'EXPECT_LINE_MISSING' }
    }
    # Every run must load the SAME runtime + DP: a peer pushing a build or a
    # plug-in mid-campaign silently changes the subject (measured: one arm of a
    # validation pass loaded a different git tag than its sibling).
    if ($script:pinRuntime -eq '' -and $ev.runtime -ne '') { $script:pinRuntime = $ev.runtime; $script:pinDp = $ev.dp; Log ('  runtime pinned: ' + $ev.runtime + ' dp=' + $ev.dp) }
    if ($ev.runtime -ne $script:pinRuntime) { $status = 'REFUSED'; $flags += ('RUNTIME_CHANGED_' + $ev.runtime) }
    if ($script:pinDp -ne '' -and $ev.dp -ne $script:pinDp) { $status = 'REFUSED'; $flags += ('DP_CHANGED_' + $ev.dp) }

    $witSite = ''; $wp = ''; $wr = ''; $ww = ''; $wm = ''
    if ($wit.Count -gt 0) {
        $witSite = (@($wit | ForEach-Object { $_.Site } | Select-Object -Unique) -join '+')
        $wp = Get-TrueMedian @($wit | ForEach-Object { $_.PresentPerS })
        $wr = Get-TrueMedian @($wit | ForEach-Object { $_.RepaintPerS })
        $ww = Get-TrueMedian @($wit | ForEach-Object { $_.WeavePerS })
        $wm = ($wit | Select-Object -Last 1).Mode
        # The witness SITE is a second, independent check of the path.
        $wantSite = $(if ($expect -eq 'fill-arm' -or $expect -eq 'hub') { 'd3d11' } else { [string]$app.nativeSite })
        if ($status -eq 'OK' -and $witSite -ne $wantSite) { $status = 'REFUSED'; $flags += ('WITNESS_SITE_' + $witSite + '_want_' + $wantSite) }
        if ($wm -ne '3d') { $flags += ('MODE_' + $wm) }
    }
    if ($flags -contains 'NO_WITNESS') { $status = 'REFUSED' }
    if ($foreground -ne 'yes') { $status = 'REFUSED' }
    if ($flags -match '^CONTENDED_') { $status = 'REFUSED' }

    # Latency CSVs: prefer the witness site's file; vk native writes 'vknative'.
    $lat = $null
    $wl = @(Get-ChildItem (Join-Path $runDir 'wl.*.csv') -ErrorAction SilentlyContinue)
    if ($wl.Count -gt 0) {
        $want = $witSite; if ($want -eq 'vk') { $want = 'vknative' }
        $pick = $wl | Where-Object { $_.Name -eq ('wl.' + $want + '.csv') } | Select-Object -First 1
        if ($pick -eq $null) { $pick = $wl | Sort-Object Length -Descending | Select-Object -First 1 }
        $lat = Measure-LatencyCsv -path $pick.FullName -q0 $q0 -q1 $q1
        if ($wl.Count -gt 1) { $flags += ('WL_SITES_' + (($wl | ForEach-Object { ($_.BaseName -split '\.')[-1] }) -join '+')) }
        if ($lat.dN -eq 0) { $flags += ('NO_D_ROWS_' + $lat.site) }
    } else {
        $flags += 'NO_LATENCY_CSV'
        $lat = [ordered]@{ site = ''; frames = ''; repaints = ''; rN = ''; r50 = ''; r95 = ''; r99 = ''; dN = ''; d50 = ''; d95 = ''; d99 = '' }
    }

    # GPU per adapter for the app pid + dwm + adapter totals.
    $gs = ''; $gr = ''; $gt = ''; $gd = ''; $gts = ''; $gto = ''; $cpu = ''
    $perLuid = @{}
    if ($g0.ok -and $g1.ok) {
        $delta = Get-GpuDelta $g0 $g1
        if ($delta.ok) {
            $col = Resolve-GpuColumns -deltaRows $delta.rows -appPids @($appPid) -svcPids @() -scanoutLuid $scanoutLuid
            $gs = $col.app_scanout; $gr = $col.app_other; $gt = [math]::Round($col.app_scanout + $col.app_other, 2)
            $gd = $col.dwm; $gts = $col.tot_scanout; $gto = $col.tot_other
            foreach ($row in $delta.rows) {
                if ($row.ProcId -ne $appPid) { continue }
                if (-not $perLuid.ContainsKey($row.Luid)) { $perLuid[$row.Luid] = 0.0 }
                $perLuid[$row.Luid] = [math]::Round($perLuid[$row.Luid] + $row.BusyPct, 2)
            }
        } else { $flags += 'GPU_DELTA_FAIL' }
    } else { $flags += 'GPU_COUNTER_FAIL' }
    $cpu = Get-CpuDeltaPct $cpu0 $cpu1

    $pmr = $null
    if ($pmExe) { $pmr = Measure-PresentMon (Join-Path $runDir 'presentmon.csv'); if ($pmr.n -eq 0) { $flags += 'PM_NO_ROWS' } }
    else { $pmr = [ordered]@{ n = ''; mode = ''; between50 = ''; between95 = ''; display50 = ''; untilDisp50 = ''; droppedPct = '' } }

    $run = [ordered]@{
        rep = $rep; app = $app.name; arm = $arm.name; topology = $Topology; pid = $appPid; status = $status
        path = $ev.path; expectPath = $expect; placement = $ev.placement; hubPlacement = $ev.hubPlacement
        activeLines = $ev.active; lateWeave = $ev.lateWeave; loadedFrom = $ev.loadedFrom; tracked = $ev.tracked
        env = $envMap; gpuPerLuid = $perLuid; latency = $lat; presentMon = $pmr; flags = $flags
    }
    [IO.File]::WriteAllText((Join-Path $runDir 'run.json'), ($run | ConvertTo-Json -Depth 6), (New-Object Text.UTF8Encoding($false)))

    $vals = @($rep, $app.name, $arm.name, $Topology, $appPid, $status, $ev.path, $expect, $witSite, $wp, $wr, $ww, $wm,
              $lat.site, $lat.frames, $lat.repaints, $lat.rN, $lat.r50, $lat.r95, $lat.r99, $lat.dN, $lat.d50, $lat.d95, $lat.d99,
              $gs, $gr, $gt, $gd, $gts, $gto, $cpu,
              $pmr.n, $pmr.mode, $pmr.between50, $pmr.between95, $pmr.display50, $pmr.untilDisp50, $pmr.droppedPct, $ev.runtime, $ev.dp,
              $ev.tracked, $foreground, $windowS, $runName, ($flags -join ';'))
    Add-Content -Path $csvPath -Value (($vals | ForEach-Object { ([string]$_) -replace ',', ';' }) -join ',') -Encoding Ascii
    Log ('  {0} path={1} (want {2}) site={3} present/s={4} R p50/p95={5}/{6} n={7} D n={8} gpu app={9}% fg={10} {11}' -f `
        $status, $ev.path, $expect, $witSite, $wp, $lat.r50, $lat.r95, $lat.rN, $lat.dN, $gt, $foreground, ($flags -join ';'))
    if ($status -eq 'REFUSED') { Log ('    placement: ' + $ev.placement); if ($ev.active.Count -gt 0) { Log ('    active: ' + ($ev.active -join ' | ')) } }
    if ($status -eq 'REFUSED' -and ($foreground -ne 'yes' -or ($flags -match '^CONTENDED_')) -and $pathOk) { return 'REFUSED_FOCUS' }
    return $status
}

$script:startedPids = @()
$script:pinRuntime = $ExpectRuntime
$script:pinDp = ''
$aborted = $false
for ($rep = 1; $rep -le $reps; $rep++) {
    foreach ($item in $plan) {
        Log ('rep ' + $rep + ' ' + $item.app.name + '/' + $item.arm.name)
        $s = 'ERROR'
        try { $s = Invoke-Run -item $item -rep $rep } catch { Log ('  ERROR: ' + $_.Exception.Message + ' @ ' + $_.InvocationInfo.ScriptLineNumber) }
        # Focus theft / a peer XR app is environmental, not the arm: retry so the
        # interleaved pair survives. A path mismatch is deterministic - no retry.
        $maxR = 1; if ($cfg.PSObject.Properties['maxRetries'] -ne $null) { $maxR = [int]$cfg.maxRetries }
        $att = 1
        while (($s -eq 'REFUSED_FOCUS' -or $s -eq 'APP_EXITED' -or $s -eq 'ERROR') -and $att -le $maxR) {
            $att++
            Log ('  environmental refusal (' + $s + ') - retry ' + ($att - 1) + '/' + $maxR)
            Start-Sleep -Seconds $coolSec
            $s = 'ERROR'
            try { $s = Invoke-Run -item $item -rep $rep -attempt $att } catch { Log ('  ERROR: ' + $_.Exception.Message) }
        }
        if ($s -eq 'ABORT') { $aborted = $true; break }
        Start-Sleep -Seconds $coolSec
    }
    if ($aborted) { break }
}

& (Join-Path $PSScriptRoot 'summarize_hub_ab.ps1') -ResultsDir $outDir
Log ('DONE: ' + $outDir)

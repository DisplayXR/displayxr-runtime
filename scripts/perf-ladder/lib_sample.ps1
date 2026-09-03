# lib_sample.ps1 - shared sampling/utility functions for the DXR perf ladder.
# Dot-source from run_ladder.ps1 / probe_caps.ps1. PowerShell 5.1, ASCII only.
#
# Sampling law (learned the hard way, see runtime memory/issue #1113):
#   - GPU cost = \GPU Engine(*)\Running Time DELTAS (100 ns units, monotonic),
#     never Utilization Percentage (instantaneous gauge + ~2.5 s per wildcard
#     query = 7 usable samples in 15 s and a 2x spread run-to-run).
#   - Attribution: instance names are pid_<pid>_luid_<hi>_<lo>_phys_..._eng_...
#     Sum per (pid, luid). The scanout LUID (from the probe) splits app cost
#     into scanout-adapter vs other-adapter columns on hybrid boxes.

Set-StrictMode -Version 2

function Get-GpuSnapshot {
    # One counter sweep -> array of {pid; luid; engtype; raw100ns} + timestamp.
    # RawValue is the monotonic 100 ns busy accumulator; CookedValue for this
    # counter type is NOT (measured: idle desktop summed to 5000+ "percent").
    # PDH quirks, all measured: with hundreds of churned pid_* instances a
    # sweep routinely contains ONE invalid sample, and -ErrorAction Stop then
    # throws away the whole sweep ("data ... is not valid"). Accept partial
    # sweeps and keep only Status-0 samples; retry only a truly empty sweep.
    $c = $null
    foreach ($try in 1..3) {
        $c = Get-Counter '\GPU Engine(*)\Running Time' -ErrorAction SilentlyContinue
        if ($c -ne $null -and $c.CounterSamples.Count -gt 0) { break }
        $c = $null
        Start-Sleep -Seconds 1
    }
    if ($c -eq $null) { return @{ ok = $false; time = (Get-Date); rows = @() } }
    $t = Get-Date   # stamp after the sweep (a wildcard sweep takes ~2 s)
    $rows = @()
    foreach ($s in ($c.CounterSamples | Where-Object { $_.Status -eq 0 })) {
        # InstanceName e.g. pid_1234_luid_0x00000000_0x0000C0DE_phys_0_eng_0_engtype_3D
        # Key on the FULL name: a (pid, luid) has several engines of the same
        # engtype (eng_0..eng_N) and collapsing them cross-diffs the wrong
        # engine's accumulator (measured: +7800% on the System pid).
        if ($s.InstanceName -match '^pid_(\d+)_luid_0x([0-9A-Fa-f]+)_0x([0-9A-Fa-f]+)') {
            $rows += New-Object PSObject -Property @{
                Key    = $s.InstanceName
                ProcId = [int]$Matches[1]
                Luid   = ('0x{0}_0x{1}' -f $Matches[2], $Matches[3])
                Raw    = [double]$s.RawValue
            }
        }
    }
    return @{ ok = $true; time = $t; rows = $rows }
}

function Get-GpuDelta {
    # Diff two snapshots -> busy-percent per (pid, luid), plus totals.
    param($snap0, $snap1)
    $elapsed100ns = ($snap1.time - $snap0.time).TotalSeconds * 1e7
    if ($elapsed100ns -le 0) { return @{ ok = $false } }
    $base = @{}
    foreach ($r in $snap0.rows) { $base[$r.Key] = $r.Raw }
    $agg = @{}   # "pid|luid" -> busy 100ns
    foreach ($r in $snap1.rows) {
        # Only instances present in BOTH snapshots: an instance that appears
        # mid-window would dump its whole since-boot accumulator into the
        # delta (measured: +4600% on an idle desktop).
        if (-not $base.ContainsKey($r.Key)) { continue }
        $d = $r.Raw - $base[$r.Key]
        if ($d -lt 0) { $d = 0 }              # counter re-created mid-window
        if ($d -gt $elapsed100ns * 1.05) { continue }  # one engine cannot exceed wall time - bogus sample
        $pk = $r.ProcId.ToString() + '|' + $r.Luid
        if (-not $agg.ContainsKey($pk)) { $agg[$pk] = 0.0 }
        $agg[$pk] = $agg[$pk] + $d
    }
    $out = @()
    foreach ($pk in $agg.Keys) {
        $parts = $pk.Split('|')
        $out += New-Object PSObject -Property @{
            ProcId  = [int]$parts[0]
            Luid    = $parts[1]
            BusyPct = [math]::Round(100.0 * $agg[$pk] / $elapsed100ns, 3)
        }
    }
    return @{ ok = $true; rows = $out; elapsedSec = ($snap1.time - $snap0.time).TotalSeconds }
}

function Resolve-GpuColumns {
    # Delta rows -> per-BUCKET x per-ADAPTER columns for one arm sample.
    #
    # Why buckets and not just "the app": the same physical work is charged to
    # different processes depending on the arm (#918/#1154). With an explicit
    # cross-adapter bridge the transfer is a copy the SERVICE issues, so it
    # lands on the service pid. With the implicit path it happens inside
    # Present, executed by the display UMD on behalf of the compositing stack,
    # and is charged to dwm/System - NOT to the app or the service. Accounting
    # only the subject therefore flatters the implicit arm by hiding a real
    # per-frame cost (~6.4 ms/frame of scanout-adapter time, measured in #918)
    # in a bucket a naive harness calls noise. So: every bucket is reported,
    # `other` is a REPORTED QUANTITY and never a dropped residual, and adapter
    # TOTALS across all processes are first-class.
    #
    # appPids: the client under test. svcPids: other processes that are part of
    # the subject (e.g. displayxr-service, which hosts composite + weave +
    # bridge). scanoutLuid: '' = single-adapter box.
    param($deltaRows, [int[]]$appPids, [int[]]$svcPids, [string]$scanoutLuid)
    $dwmPids = @(Get-Process -Name dwm -ErrorAction SilentlyContinue | ForEach-Object { $_.Id })
    $sysPids = @(0, 4)   # Idle + System; driver/kernel contexts land here
    $buckets = @('app', 'svc', 'dwm', 'sys', 'oth')
    $col = @{}
    foreach ($b in $buckets) { $col[$b + '_scanout'] = 0.0; $col[$b + '_other'] = 0.0 }
    $col['tot_scanout'] = 0.0; $col['tot_other'] = 0.0; $col['top_other'] = ''
    $otherByPid = @{}
    foreach ($r in $deltaRows) {
        $b = 'oth'
        if ($appPids -contains $r.ProcId) { $b = 'app' }
        elseif ($svcPids -contains $r.ProcId) { $b = 'svc' }
        elseif ($dwmPids -contains $r.ProcId) { $b = 'dwm' }
        elseif ($sysPids -contains $r.ProcId) { $b = 'sys' }
        $onScanout = ($scanoutLuid -eq '' -or $r.Luid -eq $scanoutLuid)
        $suffix = if ($onScanout) { '_scanout' } else { '_other' }
        $col[$b + $suffix] = $col[$b + $suffix] + $r.BusyPct
        $col['tot' + $suffix] = $col['tot' + $suffix] + $r.BusyPct
        if ($b -eq 'oth') {
            if (-not $otherByPid.ContainsKey($r.ProcId)) { $otherByPid[$r.ProcId] = 0.0 }
            $otherByPid[$r.ProcId] = $otherByPid[$r.ProcId] + $r.BusyPct
        }
    }
    # Name the noise: top 'other' consumer, so a polluted arm says WHO polluted
    # it (this box idled at 34% 'other' with no attribution - never again).
    $topPid = $otherByPid.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 1
    if ($topPid -ne $null -and $topPid.Value -gt 1.0) {
        $p = Get-Process -Id $topPid.Key -ErrorAction SilentlyContinue
        $pname = if ($p -ne $null) { $p.ProcessName } else { 'pid' + $topPid.Key }
        $col['top_other'] = ('{0}:{1}' -f $pname, [math]::Round($topPid.Value, 1))
    }
    foreach ($k in @($col.Keys)) { if ($k -ne 'top_other') { $col[$k] = [math]::Round($col[$k], 2) } }
    # Back-compat aliases for the pre-#1154 column names.
    $col['app_scanout_compat'] = $col['app_scanout']
    $col['app_other_compat'] = $col['app_other']
    $col['dwm'] = [math]::Round($col['dwm_scanout'] + $col['dwm_other'], 2)
    $col['other'] = [math]::Round($col['oth_scanout'] + $col['oth_other'] + $col['sys_scanout'] + $col['sys_other'], 2)
    $col['total'] = [math]::Round($col['tot_scanout'] + $col['tot_other'], 2)
    return $col
}

function Get-CpuSnapshot {
    param([int[]]$procIds)
    $m = @{}
    foreach ($procId in $procIds) {
        $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
        if ($p -ne $null) { $m[$procId] = $p.TotalProcessorTime.TotalSeconds }
    }
    return @{ time = (Get-Date); map = $m }
}

function Get-CpuDeltaPct {
    # Percent of ONE core (matches the render-ladder report's app CPU column).
    param($cpu0, $cpu1)
    $el = ($cpu1.time - $cpu0.time).TotalSeconds
    if ($el -le 0) { return 0.0 }
    $sum = 0.0
    foreach ($k in $cpu1.map.Keys) {
        $prev = 0.0
        if ($cpu0.map.ContainsKey($k)) { $prev = $cpu0.map[$k] }
        $sum = $sum + ($cpu1.map[$k] - $prev)
    }
    return [math]::Round(100.0 * $sum / $el, 1)
}

function Start-CursorOrbit {
    # Deterministic cursor load: circle of radiusPx at hz around centerX/Y,
    # run as a separate hidden powershell so the sampler is never blocked.
    param([int]$centerX, [int]$centerY, [int]$radiusPx, [double]$hz, [int]$durationSec)
    $code = @"
Add-Type -AssemblyName System.Windows.Forms
`$end = (Get-Date).AddSeconds($durationSec)
`$w = 2.0 * [math]::PI * $hz
while ((Get-Date) -lt `$end) {
    `$t = ((Get-Date).Ticks % 864000000000) / 1e7
    `$x = $centerX + [int]($radiusPx * [math]::Cos(`$w * `$t))
    `$y = $centerY + [int]($radiusPx * [math]::Sin(`$w * `$t))
    [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point(`$x, `$y)
    Start-Sleep -Milliseconds 16
}
"@
    $enc = [Convert]::ToBase64String([Text.Encoding]::Unicode.GetBytes($code))
    return Start-Process powershell -ArgumentList @('-NoProfile', '-WindowStyle', 'Hidden', '-EncodedCommand', $enc) -PassThru
}

function Set-CursorParked {
    Add-Type -AssemblyName System.Windows.Forms
    # Park at top-right corner, away from window and taskbar hotspots.
    $b = [System.Windows.Forms.Screen]::PrimaryScreen.Bounds
    [System.Windows.Forms.Cursor]::Position = New-Object System.Drawing.Point(($b.Right - 4), ($b.Top + 4))
}

$script:Win32Loaded = $false
function Ensure-Win32 {
    if ($script:Win32Loaded) { return }
    Add-Type @'
using System;
using System.Runtime.InteropServices;
public class LadderWin32 {
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr a, int x, int y, int cx, int cy, uint f);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint m, IntPtr w, IntPtr l);
    [DllImport("user32.dll")] public static extern IntPtr GetForegroundWindow();
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc cb, IntPtr l);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] static extern bool IsWindowVisible(IntPtr h);
    delegate bool EnumProc(IntPtr h, IntPtr l);
    static IntPtr found;
    static uint wantPid;
    static bool OnWindow(IntPtr h, IntPtr l) {
        uint pid; GetWindowThreadProcessId(h, out pid);
        if (pid == wantPid && IsWindowVisible(h)) { found = h; return false; }
        return true;
    }
    public static IntPtr FirstVisibleWindowOfPid(int pid) {
        found = IntPtr.Zero; wantPid = (uint)pid;
        EnumWindows(OnWindow, IntPtr.Zero);
        return found;
    }
}
'@
    $script:Win32Loaded = $true
}

function Get-MainWindow {
    param([int]$procId, [int]$timeoutSec)
    Ensure-Win32
    $end = (Get-Date).AddSeconds($timeoutSec)
    while ((Get-Date) -lt $end) {
        $p = Get-Process -Id $procId -ErrorAction SilentlyContinue
        if ($p -eq $null) { return [IntPtr]::Zero }
        $p.Refresh()
        if ($p.MainWindowHandle -ne [IntPtr]::Zero) { return $p.MainWindowHandle }
        # MainWindowHandle misses the borderless WS_POPUP some launches
        # (measured NO_HWND on single-instance arms) - enumerate directly.
        $h = [LadderWin32]::FirstVisibleWindowOfPid($procId)
        if ($h -ne [IntPtr]::Zero) { return $h }
        Start-Sleep -Milliseconds 500
    }
    return [IntPtr]::Zero
}

function Move-WindowTo {
    # MOVE only, never resize (SWP_NOSIZE) - size comes from the app knob.
    param([IntPtr]$hwnd, [int]$x, [int]$y)
    Ensure-Win32
    [LadderWin32]::SetWindowPos($hwnd, [IntPtr]::Zero, $x, $y, 0, 0, 0x0001 -bor 0x0004) | Out-Null  # NOSIZE|NOZORDER
}

function Send-KeyToWindow {
    # PostMessage WM_KEYDOWN/WM_KEYUP (Win32 windows take PostMessage input).
    param([IntPtr]$hwnd, [int]$vk)
    Ensure-Win32
    [LadderWin32]::PostMessage($hwnd, 0x0100, [IntPtr]$vk, [IntPtr]::Zero) | Out-Null
    Start-Sleep -Milliseconds 60
    [LadderWin32]::PostMessage($hwnd, 0x0101, [IntPtr]$vk, [IntPtr]::Zero) | Out-Null
}

function Get-LatestDxrLog {
    # Runtime log for the exe - keyed by PID when known (the filename embeds
    # it: DisplayXR_<exe>.exe.<pid>_<ts>.log). Newest-by-mtime is only a
    # fallback: with several instances alive it picks the wrong one (measured
    # in the first full run - a leaked instance shadowed every later arm).
    param([string]$exeBase, [int]$procId = 0)
    $dir = Join-Path $env:LOCALAPPDATA 'DisplayXR'
    if (-not (Test-Path $dir)) { return $null }
    $pat = 'DisplayXR_' + $exeBase + '*.log'
    if ($procId -gt 0) { $pat = 'DisplayXR_' + $exeBase + '.exe.' + $procId + '_*.log' }
    $f = Get-ChildItem $dir -Filter $pat -ErrorAction SilentlyContinue |
         Sort-Object LastWriteTime -Descending | Select-Object -First 1
    if ($f -eq $null) { return $null }
    return $f.FullName
}

function Get-WitnessLines {
    param([string]$logPath)
    if ([string]::IsNullOrEmpty($logPath) -or -not (Test-Path $logPath)) { return @() }
    $out = @()
    foreach ($ln in (Get-Content $logPath -ErrorAction SilentlyContinue)) {
        # #1339: the witness now names the columns present/s (app frames),
        # repaint/s and weave/s (everything reaching the panel). The legacy
        # old line is emitted verbatim for one release with the new keys APPENDED
        # after mode=; parse the new keys from the tail (non-anchored).
        if ($ln -match '\[WITNESS\] site=(\S+) window=([\d.]+)s .*?mode=(\S+) present/s=([\d.]+) repaint/s=([\d.]+) weave/s=([\d.]+)') {
            $out += New-Object PSObject -Property @{
                Site = $Matches[1]; WindowS = [double]$Matches[2]
                PresentPerS = [double]$Matches[4]; RepaintPerS = [double]$Matches[5]
                WeavePerS = [double]$Matches[6]; Mode = $Matches[3]
            }
        }
    }
    return $out
}

function Get-TrueMedian {
    param([double[]]$vals)
    $s = @($vals | Sort-Object)
    $n = $s.Count
    if ($n -eq 0) { return 0.0 }
    if ($n % 2 -eq 1) { return $s[[int][math]::Floor($n / 2)] }
    return ($s[$n / 2 - 1] + $s[$n / 2]) / 2.0   # even n: mean of middle two (the n=3->max bug, never again)
}

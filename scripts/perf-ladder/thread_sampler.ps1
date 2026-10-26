# thread_sampler.ps1 - per-THREAD CPU of one or more processes, attributed to
# the module each thread STARTED in (ProcessThread.StartAddress, the Win32
# start address), split into user and kernel time (#2371).
#
# Read-only: opens no window, touches no service, registry or installer, and
# never stops a process. PowerShell 5.1+, ASCII only.
#
# Why: the #2371 single-GPU run measured ~50 % app-process CPU on the hub's
# DIRECT ingress where native sat near 10 %. A process total cannot say WHICH
# thread burns the core:
#   - the app's own thread (start module = the exe or the CRT): the hub's
#     weave + present runs there (Present / frame-statistics pollers, a
#     driver call made from inside them);
#   - a thread that started inside a GPU driver's user-mode module: a driver
#     worker spinning;
#   - the runtime's repaint thread, a vendor weaver thread, a watchdog.
# The user/kernel split is the second discriminator: a loop around a syscall
# is kernel-heavy, a busy-wait on a mapped fence value is user-heavy.
#
# Output in -OutDir:
#   threads.csv        one row per thread per sample (t_ms = Unix epoch ms)
#   modules_<pid>.csv  module map (name, base, size), refreshed every 5 s
#
# Modes:
#   run_hub_ab.ps1 -ThreadSample starts this per run with -ProcessId <app pid>;
#   it stops by itself when every target has exited (or after -Seconds).
#   Standalone check against anything already running (read-only):
#     .\thread_sampler.ps1 -ProcessName displayxr-service -Seconds 10 -Report
param(
    [Parameter(Mandatory = $true, ParameterSetName = 'ById')][int[]]$ProcessId,
    [Parameter(Mandatory = $true, ParameterSetName = 'ByName')][string]$ProcessName,
    [string]$OutDir = '',
    [int]$Seconds = 0,          # 0 = until every target has exited
    [int]$IntervalMs = 1000,
    [switch]$Report             # print the top threads over the whole sampling period
)
$ErrorActionPreference = 'Continue'
if ($OutDir -eq '') { $OutDir = Join-Path ([IO.Path]::GetTempPath()) ('thread_sampler-' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
$csv = Join-Path $OutDir 'threads.csv'
if (-not (Test-Path $csv)) {
    Set-Content -Path $csv -Value 't_ms,pid,proc,tid,start_addr,user_ms,kernel_ms,state,wait_reason' -Encoding Ascii
}

$byId = ($PSCmdlet.ParameterSetName -eq 'ById')
function Get-Targets {
    if ($byId) {
        return @($ProcessId | ForEach-Object { Get-Process -Id $_ -ErrorAction SilentlyContinue } | Where-Object { $_ -ne $null })
    }
    return @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
}
function Now-Ms { return [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds() }

# ProcessThread.StartAddress is the KERNEL start address (RtlUserThreadStart
# for every user thread), useless for attribution. The Win32 start address -
# the function the thread was created on - comes from NtQueryInformationThread
# (ThreadQuerySetWin32StartAddress = 9). Cached per thread id.
Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class DxrThreadStart {
    [DllImport("kernel32.dll", SetLastError = true)] static extern IntPtr OpenThread(uint access, bool inherit, uint tid);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
    [DllImport("ntdll.dll")] static extern int NtQueryInformationThread(IntPtr h, int cls, out IntPtr info, int len, IntPtr retLen);
    public static long Win32StartAddress(int tid) {
        IntPtr h = OpenThread(0x0040 /* THREAD_QUERY_INFORMATION: class 9 needs it */, false, (uint)tid);
        if (h == IntPtr.Zero) return 0;
        try {
            IntPtr a;
            if (NtQueryInformationThread(h, 9, out a, IntPtr.Size, IntPtr.Zero) != 0) return 0;
            return a.ToInt64();
        } finally { CloseHandle(h); }
    }
}
'@
$startOf = @{}

$modAt = @{}
$tStart = Now-Ms
$sw = [Diagnostics.Stopwatch]::StartNew()
$seenAny = $false
while ($true) {
    $targets = Get-Targets
    if ($targets.Count -eq 0 -and ($seenAny -or ((Now-Ms) - $tStart) -gt 30000)) { break }
    if ($Seconds -gt 0 -and ((Now-Ms) - $tStart) -ge ($Seconds * 1000)) { break }
    $rows = New-Object System.Collections.Generic.List[string]
    $tms = Now-Ms
    foreach ($p in $targets) {
        $seenAny = $true
        $pidv = $p.Id
        $age = 0
        try { $age = ([DateTime]::Now - $p.StartTime).TotalSeconds } catch { $age = 999 }
        $last = 0
        if ($modAt.ContainsKey($pidv)) { $last = $modAt[$pidv] }
        if ($age -gt 3 -and ($tms - $last) -gt 5000) {
            try {
                $m = @($p.Modules | ForEach-Object { '{0},{1},{2}' -f $_.ModuleName, ([int64]$_.BaseAddress), ([int64]$_.ModuleMemorySize) })
                Set-Content -Path (Join-Path $OutDir ('modules_' + $pidv + '.csv')) -Value (@('module,base,size') + $m) -Encoding Ascii
                $modAt[$pidv] = $tms
            } catch { }
        }
        foreach ($t in @($p.Threads)) {
            try {
                $st = [string]$t.ThreadState
                $wr = ''
                if ($st -eq 'Wait') { $wr = [string]$t.WaitReason }
                $key = [string]$pidv + ':' + $t.Id
                if (-not $startOf.ContainsKey($key)) { $startOf[$key] = [DxrThreadStart]::Win32StartAddress($t.Id) }
                $rows.Add(('{0},{1},{2},{3},{4},{5},{6},{7},{8}' -f $tms, $pidv, $p.ProcessName, $t.Id, $startOf[$key],
                        ([int64]$t.UserProcessorTime.TotalMilliseconds), ([int64]$t.PrivilegedProcessorTime.TotalMilliseconds), $st, $wr))
            } catch { }
        }
    }
    if ($rows.Count -gt 0) { Add-Content -Path $csv -Value $rows -Encoding Ascii }
    Start-Sleep -Milliseconds ([int]($IntervalMs - ($sw.ElapsedMilliseconds % $IntervalMs)))
}

if ($Report) {
    . (Join-Path $PSScriptRoot 'lib_sample.ps1')
    $pids = @(Import-Csv $csv | ForEach-Object { [int]$_.pid } | Select-Object -Unique)
    foreach ($pv in $pids) {
        $a = Measure-ThreadCpu -Dir $OutDir -ProcId $pv -Top 5
        Write-Host ('pid {0}: {1} % of one core over {2} s (user {3} / kernel {4}), {5} threads' -f $pv, $a.total, $a.windowS, $a.user, $a.kernel, $a.threads)
        foreach ($l in $a.top) { Write-Host ('  ' + $l) }
    }
}
Write-Host ('thread samples: ' + $OutDir)

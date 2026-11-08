<#
.SYNOPSIS
  Measure an OpenXR app's frame rate and GPU 3D / COPY engine load across
  environment "arms", and report the compositor route each arm landed on.

.DESCRIPTION
  Written for the single-adapter ingress class of bug (#2518 D3D12, #2563 VK):
  a session that hands every frame through the comp_xbridge STAGED copy on ONE GPU
  holds 60 fps on a fast card and loses ~20 fps on a weaker one at 8K. Frame rate
  alone did not catch it on the fast card — the GPU COPY engine did (43 % staged
  vs 6 % DIRECT on an RTX 3080). So this measures both, per arm, plus the log
  lines that name the route:

    hub placement: ... ingress=direct|xbridge ... ingress_reason=...
    SAME-ADAPTER BRIDGE (the one-time warning when a bridge copies on one GPU)
    [RENDER] ... xb_same_adapter=... ingress=...

  Each arm launches the app fresh with its own environment (set in this process
  so the child inherits it, then cleared), lets it warm up, samples for
  -Seconds, and kills it. The first arm is preceded by one throwaway launch: the
  first session after a service (re)start can read low while the weaver warms up.

  Read-only: no install, no registry. Runs whatever runtime the loader resolves
  (set XR_RUNTIME_JSON in the arm to test a dev build). The log's `loaded from:`
  line is printed so a stale runtime cannot pass unnoticed.

.EXAMPLE
  # VK app (Media Player Pro), today's default vs forced bridge vs split off
  .\measure_hub_ingress.ps1 -Exe "C:\Program Files\DisplayXR\Apps\MediaPlayerPro\mediaplayer_pro_handle_vk_win.exe" `
      -AppArgs '"D:\clips\trailer_2x1.mp4"' `
      -Arms @{ default = @{}; xbridge = @{ DXR_HUB_INGRESS = 'xbridge' }; splitoff = @{ DXR_SPLIT_SAME_ADAPTER = '0' } } `
      -Common @{ MEDIAPLAYER_FULLSCREEN = '1' }

.EXAMPLE
  # D3D11 test app: hub (default) vs the tier's own legs
  .\measure_hub_ingress.ps1 -Exe test_apps\build\bin\cube_handle_d3d11_win.exe `
      -Arms @{ hub = @{}; ownlegs = @{ DXR_HUB = '0' } }
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Exe,
    [string]$AppArgs = '',
    # name -> hashtable of env vars for that arm. Ordered by name.
    [hashtable]$Arms = @{ default = @{} },
    # env vars applied to every arm (e.g. full screen).
    [hashtable]$Common = @{},
    [int]$WarmupSeconds = 8,
    [int]$Seconds = 12,
    # Keys sent to the app window after launch (WScript.Shell SendKeys syntax),
    # e.g. '{F11}' for a test app's full screen or '{TAB}' for Pro's panels.
    [string]$SendKeys = '',
    [switch]$NoThrowaway
)

$ErrorActionPreference = 'Stop'
$exeName = [IO.Path]::GetFileNameWithoutExtension($Exe)
$logDir = Join-Path $env:LOCALAPPDATA 'DisplayXR'

function Set-ArmEnv([hashtable]$vars) {
    foreach ($k in $vars.Keys) { Set-Item -Path "Env:$k" -Value ([string]$vars[$k]) }
}
function Clear-ArmEnv([hashtable]$vars) {
    foreach ($k in $vars.Keys) { Remove-Item -Path "Env:$k" -ErrorAction SilentlyContinue }
}

# Per-PID GPU engine utilisation (the "pid_<n>_..._engtype_<kind>" instances).
function Get-EnginePct([int]$procId, [string]$kind) {
    $s = (Get-Counter "\GPU Engine(pid_$($procId)_*engtype_$kind)\Utilization Percentage" -ErrorAction SilentlyContinue).CounterSamples
    if (-not $s) { return 0 }
    return [math]::Round(($s | Measure-Object CookedValue -Sum).Sum, 1)
}

# An elevated process's children are elevated, and the Khronos loader then
# IGNORES XR_RUNTIME_JSON and loads the registry's ActiveRuntime (CLAUDE.md's
# elevated-terminal caveat) — so a dev-build arm silently measures the
# installed runtime. When elevated, launch each arm through explorer.exe and a
# generated .bat, which runs at the user's normal (Medium) integrity; the arm's
# variables are set in the .bat because that child does not inherit ours.
$elevated = (New-Object Security.Principal.WindowsPrincipal ([Security.Principal.WindowsIdentity]::GetCurrent())).IsInRole(
    [Security.Principal.WindowsBuiltInRole]::Administrator)
if ($elevated) { Write-Host "elevated shell: launching arms through explorer.exe (Medium integrity)" }

function Start-App([hashtable]$vars) {
    if (-not $elevated) {
        Set-ArmEnv $Common; Set-ArmEnv $vars
        try {
            if ($AppArgs) { return Start-Process $Exe -ArgumentList $AppArgs -PassThru }
            return Start-Process $Exe -PassThru
        } finally { Clear-ArmEnv $vars; Clear-ArmEnv $Common }
    }
    $bat = Join-Path $env:TEMP "measure_hub_ingress_$PID.bat"
    $lines = @('@echo off')
    foreach ($h in @($Common, $vars)) { foreach ($k in $h.Keys) { $lines += "set `"$k=$($h[$k])`"" } }
    $lines += "start `"`" `"$Exe`" $AppArgs"
    Set-Content -Path $bat -Value $lines -Encoding ascii
    $before = @(Get-Process $exeName -ErrorAction SilentlyContinue | ForEach-Object Id)
    & explorer.exe $bat
    for ($i = 0; $i -lt 40; $i++) {
        Start-Sleep -Milliseconds 250
        $p = Get-Process $exeName -ErrorAction SilentlyContinue | Where-Object { $before -notcontains $_.Id } | Select-Object -First 1
        if ($p) { return $p }
    }
    throw "the app did not start through explorer.exe ($bat)"
}

Add-Type @"
using System; using System.Runtime.InteropServices;
public static class MhiWin { [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h); }
"@ -ErrorAction SilentlyContinue

function Invoke-Arm([string]$name, [hashtable]$vars, [bool]$measure) {
    # Only ever stop what this script started (below). A dev box often has other
    # sessions running the same app; killing them by name breaks their tests.
    Start-Sleep 2
    $t0 = Get-Date
    $p = Start-App $vars
    if ($SendKeys) {
        Start-Sleep 5
        $p.Refresh()
        [MhiWin]::SetForegroundWindow($p.MainWindowHandle) | Out-Null
        Start-Sleep -Milliseconds 500
        (New-Object -ComObject WScript.Shell).SendKeys($SendKeys)
    }
    Start-Sleep $WarmupSeconds
    $fps = @(); $g3d = @(); $gcp = @()
    if ($measure) {
        $end = (Get-Date).AddSeconds($Seconds)
        while ((Get-Date) -lt $end -and -not $p.HasExited) {
            $p.Refresh()
            if ($p.MainWindowTitle -match '(\d+)\s*fps') { $fps += [int]$Matches[1] }
            $g3d += Get-EnginePct $p.Id '3D'
            $gcp += Get-EnginePct $p.Id 'Copy'
            Start-Sleep -Milliseconds 1500
        }
    }
    $alive = -not $p.HasExited
    Stop-Process -Id $p.Id -Force -ErrorAction SilentlyContinue
    Start-Sleep 2
    if (-not $measure) { return }

    $log = Get-ChildItem $logDir -Filter "DisplayXR_$exeName*.log" -ErrorAction SilentlyContinue |
        Where-Object LastWriteTime -gt $t0 | Sort-Object LastWriteTime | Select-Object -Last 1
    $lines = @()
    if ($log) {
        $lines = Select-String $log.FullName -Pattern 'loaded from:|hub placement:|SAME-ADAPTER BRIDGE|\[RENDER\] split|ERROR' |
            ForEach-Object { $_.Line -replace '^\[[^\]]+\] \[[A-Z ]+\] ', '' } |
            Select-Object -First 1 -Last 6
    }
    $med = { param($a) if ($a.Count) { ($a | Sort-Object)[[int]($a.Count / 2)] } else { '-' } }
    [pscustomobject]@{
        Arm      = $name
        Env      = (($vars.GetEnumerator() | ForEach-Object { "$($_.Key)=$($_.Value)" }) -join ' ')
        FpsMed   = & $med $fps
        FpsMin   = if ($fps.Count) { ($fps | Measure-Object -Minimum).Minimum } else { '-' }
        Gpu3D    = & $med $g3d
        GpuCopy  = & $med $gcp
        Alive    = $alive
        Log      = if ($log) { $log.Name } else { '(no log)' }
        Lines    = $lines
    }
}

# Refuse to run alongside another instance of the same app: it would share the
# panel and skew the GPU counters, and stopping it is not ours to do.
$others = @(Get-Process $exeName -ErrorAction SilentlyContinue)
if ($others.Count -gt 0) {
    throw "$exeName is already running (PID $(($others | ForEach-Object Id) -join ', ')) - close it, or ask whoever owns it, then re-run."
}

if (-not $NoThrowaway) {
    $first = ($Arms.Keys | Sort-Object)[0]
    Write-Host "throwaway launch ($first)..."
    Invoke-Arm $first $Arms[$first] $false | Out-Null
}
$results = foreach ($k in ($Arms.Keys | Sort-Object)) {
    Write-Host "arm $k ..."
    Invoke-Arm $k $Arms[$k] $true
}

$results | Format-Table Arm, Env, FpsMed, FpsMin, Gpu3D, GpuCopy, Alive -AutoSize | Out-String -Width 200 | Write-Output
foreach ($r in $results) {
    Write-Output "--- $($r.Arm)  ($($r.Log))"
    $r.Lines | ForEach-Object { Write-Output "    $($_.Substring(0, [Math]::Min(220, $_.Length)))" }
}

# probe_caps.ps1 - Block 0 of the DXR perf ladder: capability probe.
# Writes capabilities.json next to the results. Read-only, non-elevated.
# PowerShell 5.1, ASCII only.
param(
    [string]$OutDir = "."
)
Set-StrictMode -Version 2
$ErrorActionPreference = 'Continue'

$caps = [ordered]@{
    kit          = 'DXRPerfLadder'
    probedAtUtc  = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ssZ')
    hostname     = $env:COMPUTERNAME
    os           = (Get-CimInstance Win32_OperatingSystem).Caption
    osBuild      = (Get-CimInstance Win32_OperatingSystem).BuildNumber
}

# --- Adapters + active modes (which adapter owns the panel mode) -------------
$adapters = @()
foreach ($vc in (Get-CimInstance Win32_VideoController)) {
    $adapters += [ordered]@{
        name          = $vc.Name
        driverVersion = $vc.DriverVersion
        activeMode    = if ($vc.CurrentHorizontalResolution) {
                            ('{0}x{1}@{2}' -f $vc.CurrentHorizontalResolution, $vc.CurrentVerticalResolution, $vc.CurrentRefreshRate)
                        } else { $null }
    }
}
$caps.adapters = $adapters

# --- Scanout LUID via DXGI enumeration + QueryDisplayConfig source adapter ---
# The naive DXGI output walk lies on Optimus (render-only dGPU also enumerates
# the panel); QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS).sourceInfo.adapterId is
# the truth (same lesson as runtime PR #1078).
Add-Type @'
using System;
using System.Runtime.InteropServices;
public class QdcProbe {
    [StructLayout(LayoutKind.Sequential)] public struct LUID { public uint Low; public int High; }
    [DllImport("user32.dll")] public static extern int GetDisplayConfigBufferSizes(uint flags, out uint numPath, out uint numMode);
    [DllImport("user32.dll")] public static extern int QueryDisplayConfig(uint flags, ref uint numPath, IntPtr paths, ref uint numMode, IntPtr modes, IntPtr topo);
    public const uint QDC_ONLY_ACTIVE_PATHS = 2;
    public const int PATH_SIZE = 72;   // sizeof(DISPLAYCONFIG_PATH_INFO)
    public const int MODE_SIZE = 64;   // sizeof(DISPLAYCONFIG_MODE_INFO)
    // sourceInfo.adapterId is the first field of DISPLAYCONFIG_PATH_INFO
    public static string FirstSourceLuid() {
        uint np, nm;
        if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, out np, out nm) != 0 || np == 0) return "";
        IntPtr pp = Marshal.AllocHGlobal((int)(np * PATH_SIZE));
        IntPtr pm = Marshal.AllocHGlobal((int)(nm * MODE_SIZE));
        try {
            if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, ref np, pp, ref nm, pm, IntPtr.Zero) != 0) return "";
            LUID l = (LUID)Marshal.PtrToStructure(pp, typeof(LUID));
            return string.Format("0x{0:X8}_0x{1:X8}", l.High, l.Low);
        } finally { Marshal.FreeHGlobal(pp); Marshal.FreeHGlobal(pm); }
    }
}
'@ -ErrorAction SilentlyContinue
try { $caps.scanoutLuid = [QdcProbe]::FirstSourceLuid() } catch { $caps.scanoutLuid = '' }

# Map the scanout LUID to a GPU-Engine-counter LUID string (pid_..._luid_HI_LO):
# counter instances use 0x%08X_0x%08X of (High, Low) - same format we emit above.

# --- Vulkan: present_wait / queue families (vulkaninfo if present) -----------
$caps.vulkan = [ordered]@{ vulkaninfoFound = $false; presentId = $null; presentWait = $null; queueFamilies = @() }
$vi = Get-Command vulkaninfo -ErrorAction SilentlyContinue
if ($vi -ne $null) {
    $caps.vulkan.vulkaninfoFound = $true
    $tmp = Join-Path $env:TEMP 'ladder_vulkaninfo.txt'
    & $vi.Source | Out-File -Encoding utf8 $tmp 2>$null
    if (Test-Path $tmp) {
        $txt = Get-Content $tmp -Raw
        $caps.vulkan.presentId   = ($txt -match 'VK_KHR_present_id')
        $caps.vulkan.presentWait = ($txt -match 'VK_KHR_present_wait')
        $qf = @()
        foreach ($m in [regex]::Matches($txt, 'queueCount\s*=\s*(\d+)')) { $qf += [int]$m.Groups[1].Value }
        $caps.vulkan.queueFamilies = $qf
    }
}
# Fallback: the runtime itself logs presentId/presentWait and #868 queue facts;
# summarize.ps1 harvests those witness lines from the arm logs.

# --- DisplayXR runtime / plugin / SR Platform --------------------------------
$caps.displayxr = [ordered]@{ cliFound = $false; info = $null; activeRuntime = $null }
$cli = $null
$cliPaths = @(
    (Join-Path ${env:ProgramFiles} 'DisplayXR\Runtime\displayxr-cli.exe'),
    (Join-Path ${env:ProgramFiles} 'DisplayXR\Runtime\bin\displayxr-cli.exe')
)
foreach ($p in $cliPaths) { if (Test-Path $p) { $cli = $p; break } }
if ($cli -ne $null) {
    $caps.displayxr.cliFound = $true
    try { $caps.displayxr.info = (& $cli info 2>&1 | Out-String) } catch { }
}
try {
    $caps.displayxr.activeRuntime = (Get-ItemProperty 'HKLM:\Software\Khronos\OpenXR\1' -ErrorAction SilentlyContinue).ActiveRuntime
} catch { }

$caps.srPlatform = $null
$srDir = Join-Path ${env:ProgramFiles} 'LeiaSR\Platform'
if (Test-Path $srDir) {
    $dll = Get-ChildItem $srDir -Recurse -Filter 'SR*.dll' -ErrorAction SilentlyContinue | Select-Object -First 1
    if ($dll -ne $null) { $caps.srPlatform = $dll.VersionInfo.ProductVersion }
}

# --- Power state (battery invalidates perf wholesale) ------------------------
$bat = Get-CimInstance Win32_Battery -ErrorAction SilentlyContinue
$caps.power = [ordered]@{
    hasBattery = ($bat -ne $null)
    onAc       = if ($bat -eq $null) { $true } else { ($bat.BatteryStatus -eq 2) }
    plan       = ((powercfg /getactivescheme) -join ' ')
}

# --- Elevation (informational; the ladder should run non-elevated) -----------
$id = [Security.Principal.WindowsIdentity]::GetCurrent()
$caps.elevated = (New-Object Security.Principal.WindowsPrincipal($id)).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)

$outPath = Join-Path $OutDir 'capabilities.json'
$caps | ConvertTo-Json -Depth 6 | Out-File -Encoding utf8 $outPath
Write-Host ("capabilities.json written: " + $outPath)

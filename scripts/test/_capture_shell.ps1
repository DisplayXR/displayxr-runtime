# Capture the DisplayXR shell compositor window to a PNG.
#
# The shell window is created on a background thread by displayxr-service.exe,
# so Get-Process ... MainWindowHandle returns 0. Instead we enumerate ALL
# top-level windows, filter to ones owned by displayxr-service, and pick the
# one whose title contains "DisplayXR" (falling back to the largest visible
# window owned by the service if no match).
#
# Uses PrintWindow API with PW_RENDERFULLCONTENT (2) so DWM-backed windows
# render correctly.
#
# Arg: absolute Windows path for the output PNG.
#
# Exit codes:
#   0  success
#   1  no shell window found
#   2  capture failed

param(
    [Parameter(Mandatory=$true)]
    [string]$OutPath
)

Add-Type -ReferencedAssemblies System.Drawing @'
using System;
using System.Collections.Generic;
using System.Drawing;
using System.Runtime.InteropServices;
using System.Text;

public class WCap {
    public delegate bool EnumWindowsProc(IntPtr h, IntPtr l);

    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumWindowsProc cb, IntPtr l);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll", CharSet=CharSet.Auto, SetLastError=true)]
    public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool PrintWindow(IntPtr h, IntPtr dc, uint f);

    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L, T, R, B; }

    public static List<IntPtr> Find(uint targetPid) {
        var result = new List<IntPtr>();
        EnumWindows((h, l) => {
            if (!IsWindowVisible(h)) return true;
            uint pid;
            GetWindowThreadProcessId(h, out pid);
            if (pid != targetPid) return true;
            result.Add(h);
            return true;
        }, IntPtr.Zero);
        return result;
    }

    public static string Title(IntPtr h) {
        var sb = new StringBuilder(512);
        GetWindowTextW(h, sb, sb.Capacity);
        return sb.ToString();
    }

    public static bool Cap(IntPtr h, string path) {
        RECT r; GetWindowRect(h, out r);
        int w = r.R - r.L, ht = r.B - r.T;
        if (w <= 0 || ht <= 0) return false;
        var b = new Bitmap(w, ht);
        var g = Graphics.FromImage(b);
        IntPtr dc = g.GetHdc();
        PrintWindow(h, dc, 2);
        g.ReleaseHdc(dc);
        b.Save(path);
        g.Dispose(); b.Dispose();
        return true;
    }
}
'@

$proc = Get-Process displayxr-service -ErrorAction SilentlyContinue | Select-Object -First 1
if ($null -eq $proc) {
    Write-Host "no displayxr-service process"
    exit 1
}

$targetPid = [uint32]$proc.Id
$hwnds = [WCap]::Find($targetPid)
if ($hwnds.Count -eq 0) {
    Write-Host "displayxr-service PID $targetPid has no visible windows"
    exit 1
}

# Prefer a window whose title contains "DisplayXR".
$chosen = [IntPtr]::Zero
$chosenTitle = ""
foreach ($h in $hwnds) {
    $t = [WCap]::Title($h)
    if ($t -match "DisplayXR") {
        $chosen = $h
        $chosenTitle = $t
        break
    }
}

# Fallback: largest visible window owned by the process.
if ($chosen -eq [IntPtr]::Zero) {
    $bestArea = 0
    foreach ($h in $hwnds) {
        $r = New-Object WCap+RECT
        if ([WCap]::GetWindowRect($h, [ref]$r)) {
            $area = ($r.R - $r.L) * ($r.B - $r.T)
            if ($area -gt $bestArea) {
                $bestArea = $area
                $chosen = $h
                $chosenTitle = [WCap]::Title($h)
            }
        }
    }
}

if ($chosen -eq [IntPtr]::Zero) {
    Write-Host "no candidate window"
    exit 1
}

Write-Host "target: [$chosenTitle] hwnd=$chosen"

if ([WCap]::Cap($chosen, $OutPath)) {
    Write-Host "captured: $OutPath"
    exit 0
} else {
    Write-Host "capture failed"
    exit 2
}

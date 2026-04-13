# shell_screenshot.ps1 — Toggle launcher (Ctrl+L) and capture the compositor
# combined atlas via F12 hotkey (D3D11 back buffer read-back).
#
# Usage:  powershell -ExecutionPolicy Bypass -File scripts\shell_screenshot.ps1
#         powershell -ExecutionPolicy Bypass -File scripts\shell_screenshot.ps1 -NoToggle
#
# Requires displayxr-shell.exe and displayxr-service.exe to be running.
# Screenshot saves to %TEMP%\shell_screenshot.png (full 3840x2160 SBS atlas).

param([switch]$NoToggle)

Add-Type @'
using System;
using System.Runtime.InteropServices;
public class ShellScreenshot {
    [DllImport("user32.dll", CharSet=CharSet.Ansi)]
    public static extern IntPtr FindWindowExA(IntPtr parent, IntPtr after, string cls, string title);
    [DllImport("user32.dll")] public static extern bool PostMessage(IntPtr h, uint msg, IntPtr wp, IntPtr lp);
    [DllImport("user32.dll")] public static extern bool SetForegroundWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern uint SendInput(uint n, INPUT[] i, int sz);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EP cb, IntPtr lp);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint p);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    public delegate bool EP(IntPtr h, IntPtr lp);
    [StructLayout(LayoutKind.Sequential)] public struct INPUT { public uint type; public KI ki; }
    [StructLayout(LayoutKind.Sequential)] public struct KI {
        public ushort wVk; public ushort wScan; public uint dwFlags;
        public uint time; public IntPtr dwExtraInfo; public uint p1; public uint p2;
    }
    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int L,T,R,B; }

    public static IntPtr FindShellMsgHwnd() {
        // Message-only window: class "Static", title "DisplayXR Shell Msg"
        return FindWindowExA(new IntPtr(-3), IntPtr.Zero, "Static", "DisplayXR Shell Msg");
    }

    public static IntPtr FindCompHwnd() {
        var svc = System.Diagnostics.Process.GetProcessesByName("displayxr-service");
        if (svc.Length == 0) return IntPtr.Zero;
        uint pid = (uint)svc[0].Id;
        IntPtr result = IntPtr.Zero;
        EnumWindows((h, lp) => {
            uint p; GetWindowThreadProcessId(h, out p);
            if (p == pid) { RECT r; GetWindowRect(h, out r); if((r.R-r.L)>100){result=h;return false;} }
            return true;
        }, IntPtr.Zero);
        return result;
    }

    public static void ToggleLauncher() {
        var h = FindShellMsgHwnd();
        if (h == IntPtr.Zero) { Console.WriteLine("Shell msg window not found"); return; }
        PostMessage(h, 0x0312, new IntPtr(2), IntPtr.Zero);  // WM_HOTKEY, HOTKEY_LAUNCH=2
    }

    public static void SendF12() {
        var h = FindCompHwnd();
        if (h == IntPtr.Zero) { Console.WriteLine("Compositor window not found"); return; }
        SetForegroundWindow(h);
        System.Threading.Thread.Sleep(500);
        var inp = new INPUT[2];
        inp[0].type = 1; inp[0].ki.wVk = 0x7B;
        inp[1].type = 1; inp[1].ki.wVk = 0x7B; inp[1].ki.dwFlags = 2;
        SendInput(2, inp, System.Runtime.InteropServices.Marshal.SizeOf(typeof(INPUT)));
    }
}
'@

if (-not $NoToggle) {
    [ShellScreenshot]::ToggleLauncher()
    Write-Host "Launcher toggled — waiting 3s..."
    Start-Sleep -Seconds 3
}

[ShellScreenshot]::SendF12()
Write-Host "F12 sent — waiting 2s..."
Start-Sleep -Seconds 2

$path = "$env:TEMP\shell_screenshot.png"
if (Test-Path $path) {
    $fi = Get-Item $path
    Write-Host "Screenshot saved: $path ($($fi.Length) bytes)"
} else {
    Write-Host "ERROR: Screenshot not found at $path"
}

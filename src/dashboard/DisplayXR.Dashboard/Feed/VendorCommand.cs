// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Diagnostics;
using System.Runtime.InteropServices;

namespace DisplayXR.Dashboard.Feed;

/// <summary>
/// "Open in vendor dashboard" (ADR-051 D7): the plug-in hands over a command
/// line, with <c>{serial}</c> / <c>{monitor_id}</c> placeholders, and the
/// dashboard launches it without parsing it. The runtime knows no vendor
/// executable name.
/// </summary>
public static class VendorCommand
{
    /// <summary>Fill the two documented placeholders; nothing else in the string is touched.</summary>
    public static string Expand(string template, string serial, string monitorId) =>
        template.Replace("{serial}", serial, StringComparison.Ordinal)
                .Replace("{monitor_id}", monitorId, StringComparison.Ordinal);

    /// <summary>
    /// The executable token and the verbatim rest, for the ShellExecute
    /// fallback only: a leading quoted token is honoured, otherwise the split
    /// is at the first space. The rest is never parsed further.
    /// </summary>
    public static (string File, string Arguments) SplitFirst(string commandLine)
    {
        string s = commandLine.TrimStart();
        if (s.StartsWith('"'))
        {
            int close = s.IndexOf('"', 1);
            if (close > 0) return (s[1..close], s[(close + 1)..].TrimStart());
            return (s.Trim('"'), "");
        }
        int sp = s.IndexOf(' ');
        return sp < 0 ? (s, "") : (s[..sp], s[(sp + 1)..]);
    }

    /// <summary>
    /// Launch: CreateProcess with the whole command line (Windows resolves the
    /// first token itself); if that cannot find the program, ShellExecute
    /// semantics, which also consult App Paths. Returns null on success, else
    /// the reason, for the inline note.
    /// </summary>
    public static string? Launch(string commandLine)
    {
        DashboardLog.Info($"open in vendor dashboard: {commandLine}");
        if (OperatingSystem.IsWindows())
        {
            var si = new STARTUPINFOW { cb = Marshal.SizeOf<STARTUPINFOW>() };
            // CreateProcessW may write into the buffer: hand it a private, writable copy.
            var buffer = new char[commandLine.Length + 1];
            commandLine.CopyTo(0, buffer, 0, commandLine.Length);
            if (CreateProcessW(null, buffer, IntPtr.Zero, IntPtr.Zero, false, 0, IntPtr.Zero, null, ref si, out var pi))
            {
                CloseHandle(pi.hProcess);
                CloseHandle(pi.hThread);
                return null;
            }
            int err = Marshal.GetLastWin32Error();
            if (err != 2 && err != 3) return $"Could not start the vendor dashboard (Windows error {err}).";
        }
        try
        {
            var (file, args) = SplitFirst(commandLine);
            Process.Start(new ProcessStartInfo(file, args) { UseShellExecute = true })?.Dispose();
            return null;
        }
        catch (Exception ex)
        {
            return $"Could not start the vendor dashboard: {ex.Message}";
        }
    }

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    private struct STARTUPINFOW
    {
        public int cb;
        public string? lpReserved, lpDesktop, lpTitle;
        public int dwX, dwY, dwXSize, dwYSize, dwXCountChars, dwYCountChars, dwFillAttribute, dwFlags;
        public short wShowWindow, cbReserved2;
        public IntPtr lpReserved2, hStdInput, hStdOutput, hStdError;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct PROCESS_INFORMATION
    {
        public IntPtr hProcess, hThread;
        public int dwProcessId, dwThreadId;
    }

    [DllImport("kernel32.dll", SetLastError = true, CharSet = CharSet.Unicode)]
    private static extern bool CreateProcessW(string? app, [In, Out] char[] commandLine, IntPtr procAttr, IntPtr threadAttr,
        bool inherit, uint flags, IntPtr env, string? cwd, ref STARTUPINFOW si, out PROCESS_INFORMATION pi);

    [DllImport("kernel32.dll", SetLastError = true)]
    private static extern bool CloseHandle(IntPtr h);
}

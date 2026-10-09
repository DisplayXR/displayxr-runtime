// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.IO;
using System.Threading;

namespace DisplayXR.Dashboard;

/// <summary>
/// <c>%LOCALAPPDATA%\DisplayXR\dashboard.log</c>: lifecycle lines and every
/// exception the app catches or is handed by an unhandled-exception hook.
/// Rolled to <c>dashboard.log.1</c> past 2 MB. Never throws.
/// </summary>
public static class DashboardLog
{
    private static readonly object Gate = new();
    private static int _errors;
    private const long MaxBytes = 2 * 1024 * 1024;

    public static string Path { get; } = System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DisplayXR", "dashboard.log");

    /// <summary>Exceptions logged this session (shown on Developer).</summary>
    public static int ErrorCount => Volatile.Read(ref _errors);

    /// <summary>Set by the tests: no file writes.</summary>
    public static bool Disabled { get; set; }

    public static void Info(string message) => Write("INFO", message);
    public static void Warn(string message) => Write("WARN", message);

    public static void Error(string where, Exception ex)
    {
        Interlocked.Increment(ref _errors);
        Write("ERROR", $"{where}: {ex}");
    }

    private static void Write(string level, string message)
    {
        if (Disabled) return;
        try
        {
            lock (Gate)
            {
                Directory.CreateDirectory(System.IO.Path.GetDirectoryName(Path)!);
                var fi = new FileInfo(Path);
                if (fi.Exists && fi.Length > MaxBytes)
                {
                    string old = Path + ".1";
                    if (File.Exists(old)) File.Delete(old);
                    File.Move(Path, old);
                }
                File.AppendAllText(Path,
                    $"{DateTime.Now:yyyy-MM-dd HH:mm:ss.fff} [{Environment.ProcessId}] {level} {message}{Environment.NewLine}");
            }
        }
        catch
        {
            // A log that cannot be written must not take the dashboard down.
        }
    }
}

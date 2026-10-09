// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Threading.Tasks;
using Avalonia;

namespace DisplayXR.Dashboard;

internal static class Program
{
    /// <summary><c>--page displays</c> opens on that page (home, displays, windows, performance, developer).</summary>
    public static string StartPage { get; private set; } = "home";

    /// <summary><c>--fixture file.ndjson</c>: replay snapshots from a file (development aid, see README).</summary>
    public static string? Fixture { get; private set; }

    [STAThread]
    public static int Main(string[] args)
    {
        for (int i = 0; i < args.Length - 1; i++)
        {
            if (args[i] == "--page") StartPage = args[i + 1].ToLowerInvariant();
            if (args[i] == "--fixture") Fixture = args[i + 1];
        }

        AppDomain.CurrentDomain.UnhandledException += (_, e) =>
        {
            if (e.ExceptionObject is Exception ex) DashboardLog.Error("unhandled (AppDomain)", ex);
        };
        TaskScheduler.UnobservedTaskException += (_, e) =>
        {
            DashboardLog.Error("unobserved task", e.Exception);
            e.SetObserved();
        };

        // One dashboard per logon session: each instance holds a DIAG slot on the
        // service (quota 4), so a second launch raises the first instead.
        using var single = new System.Threading.Mutex(true, @"Local\DisplayXR.Dashboard", out bool first);
        if (!first)
        {
            RaiseExisting();
            return 0;
        }

        DashboardLog.Info($"DisplayXR Dashboard {typeof(Program).Assembly.GetName().Version} starting (elevated={Ui.Elevation.IsElevated})");
        try
        {
            return BuildAvaloniaApp().StartWithClassicDesktopLifetime(args);
        }
        catch (Exception ex)
        {
            DashboardLog.Error("fatal", ex);
            return 1;
        }
        finally
        {
            DashboardLog.Info("DisplayXR Dashboard exiting");
        }
    }

    private static void RaiseExisting()
    {
        try
        {
            int self = Environment.ProcessId;
            foreach (var p in System.Diagnostics.Process.GetProcessesByName("displayxr-dashboard"))
            {
                using (p)
                {
                    if (p.Id == self || p.MainWindowHandle == IntPtr.Zero) continue;
                    if (OperatingSystem.IsWindows())
                    {
                        ShowWindow(p.MainWindowHandle, 9 /* SW_RESTORE */);
                        SetForegroundWindow(p.MainWindowHandle);
                    }
                    return;
                }
            }
        }
        catch (Exception ex) { DashboardLog.Warn($"raise existing dashboard: {ex.Message}"); }
    }

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool ShowWindow(IntPtr hwnd, int cmd);

    [System.Runtime.InteropServices.DllImport("user32.dll")]
    private static extern bool SetForegroundWindow(IntPtr hwnd);

    public static AppBuilder BuildAvaloniaApp() =>
        AppBuilder.Configure<App>()
            .UsePlatformDetect()
            .WithInterFont()
            .LogToTrace();
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Threading.Tasks;
using Avalonia;

namespace DisplayXR.Dashboard;

internal static class Program
{
    /// <summary>
    /// <c>--page displays</c> opens on that page: home, displays, components, settings, troubleshooting, and every
    /// older id where its content now is (catalog: components; performance: settings; windows: troubleshooting,
    /// Running 3D apps; developer: troubleshooting, Advanced). Model/PageRoutes.cs.
    /// </summary>
    public static string StartPage { get; private set; } = "home";

    /// <summary><c>--fixture file.ndjson</c>: replay snapshots from a file (development aid, see README).</summary>
    public static string? Fixture { get; private set; }

#if DEBUG
    // Debug builds only (never in a published dashboard): open Components on one catalog item, run a verb on
    // it, and save the window as a PNG after a delay, then exit. For laying the tab out from fixtures.
    public static string? CatalogItem { get; private set; }
    public static string? CatalogDo { get; private set; }
    public static string? Screenshot { get; private set; }
    public static double ScreenshotDelay { get; private set; } = 3;
#endif

    [STAThread]
    public static int Main(string[] args)
    {
        // --version: print and exit before anything touches a display server,
        // the log or the single-instance mutex (the packaging smoke runs it in
        // a container with no X11 / Wayland).
        if (Array.Exists(args, a => a == "--version"))
        {
            if (OperatingSystem.IsWindows()) AttachConsole(-1 /* ATTACH_PARENT_PROCESS */);
            Console.Out.WriteLine($"displayxr-dashboard {VersionText}");
            Console.Out.Flush();
            return 0;
        }

        for (int i = 0; i < args.Length - 1; i++)
        {
            if (args[i] == "--page") StartPage = args[i + 1].ToLowerInvariant();
            if (args[i] == "--fixture") Fixture = args[i + 1];
#if DEBUG
            if (args[i] == "--catalog-item") CatalogItem = args[i + 1];
            if (args[i] == "--catalog-do") CatalogDo = args[i + 1];
            if (args[i] == "--screenshot") Screenshot = args[i + 1];
            if (args[i] == "--screenshot-delay" && double.TryParse(args[i + 1], System.Globalization.NumberStyles.Float,
                    System.Globalization.CultureInfo.InvariantCulture, out double d)) ScreenshotDelay = d;
#endif
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

    /// <summary>The informational version (CI stamps the runtime's <c>git describe</c>), else the assembly version.</summary>
    public static string VersionText
    {
        get
        {
            var asm = typeof(Program).Assembly;
            var info = (System.Reflection.AssemblyInformationalVersionAttribute?)Attribute.GetCustomAttribute(
                asm, typeof(System.Reflection.AssemblyInformationalVersionAttribute));
            return info?.InformationalVersion ?? asm.GetName().Version?.ToString() ?? "unknown";
        }
    }

    [System.Runtime.InteropServices.DllImport("kernel32.dll")]
    private static extern bool AttachConsole(int processId);

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

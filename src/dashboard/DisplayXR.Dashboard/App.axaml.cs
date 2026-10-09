// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.ApplicationLifetimes;
using Avalonia.Markup.Xaml;
using Avalonia.Threading;

namespace DisplayXR.Dashboard;

public partial class App : Application
{
    public override void Initialize() => AvaloniaXamlLoader.Load(this);

    public override void OnFrameworkInitializationCompleted()
    {
        // Anything that escapes a UI handler is logged and swallowed: the
        // dashboard is a diagnostics surface and must not die on one bad read.
        Dispatcher.UIThread.UnhandledException += (_, e) =>
        {
            DashboardLog.Error("UI thread", e.Exception);
            e.Handled = true;
        };

        if (ApplicationLifetime is IClassicDesktopStyleApplicationLifetime desktop)
        {
            desktop.ShutdownMode = ShutdownMode.OnMainWindowClose;
            var window = new MainWindow(Program.StartPage);
            desktop.MainWindow = window;
            desktop.Exit += (_, _) => window.ShutdownFeed();
        }
        base.OnFrameworkInitializationCompleted();
    }
}

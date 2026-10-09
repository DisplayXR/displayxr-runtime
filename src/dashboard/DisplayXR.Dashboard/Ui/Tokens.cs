// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Security.Principal;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using Avalonia.Media.Immutable;
using Avalonia.Threading;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;

namespace DisplayXR.Dashboard.Ui;

/// <summary>Code-side access to Styles/Theme.axaml. Cached: the theme never changes at run time.</summary>
public static class Tokens
{
    private static readonly Dictionary<string, object> Cache = new();

    private static T Find<T>(string key, T fallback)
    {
        if (Cache.TryGetValue(key, out var cached) && cached is T c) return c;
        if (Application.Current is { } app && app.TryFindResource(key, out var value) && value is T v)
        {
            Cache[key] = v;
            return v;
        }
        return fallback;
    }

    public static IBrush Brush(string key) => Find<IBrush>(key, Brushes.Magenta);
    public static Color Color(string key) => Find(key, Colors.Magenta);
    public static CornerRadius Radius(string key) => Find(key, new CornerRadius(8));

    public static IBrush Fg => Brush("FgBrush");
    public static IBrush FgDim => Brush("FgDimBrush");
    public static IBrush Soft => Brush("SoftBrush");
    public static IBrush Muted => Brush("MutedBrush");
    public static IBrush Secondary => Brush("SecondaryBrush");
    public static IBrush Line => Brush("LineBrush");
    public static IBrush AccentText => Brush("AccentTextBrush");
    public static IBrush Accent => Brush("AccentBrush");
    public static IBrush Panel => Brush("PanelBrush");
    public static IBrush PanelRaised => Brush("PanelRaisedBrush");
    public static IBrush Field => Brush("FieldBrush");
    public static FontFamily Mono => Find<FontFamily>("MonoFont", FontFamily.Default);

    public static IBrush Alpha(string colorKey, double alpha)
    {
        var c = Color(colorKey);
        return new ImmutableSolidColorBrush(Avalonia.Media.Color.FromArgb((byte)Math.Round(alpha * 255), c.R, c.G, c.B));
    }

    public static string ColorKey(Level level) => level switch
    {
        Level.Ok => "OkColor",
        Level.Info => "InfoColor",
        Level.Warn => "WarnColor",
        Level.Critical => "CriticalColor",
        _ => "SecondaryColor",
    };

    public static IBrush For(Level level) => level switch
    {
        Level.Ok => Brush("OkBrush"),
        Level.Info => Brush("InfoBrush"),
        Level.Warn => Brush("WarnBrush"),
        Level.Critical => Brush("CriticalBrush"),
        _ => Brush("SecondaryBrush"),
    };
}

public static class Elevation
{
    public static bool IsElevated
    {
        get
        {
            if (!OperatingSystem.IsWindows()) return false;
            try
            {
                using var id = WindowsIdentity.GetCurrent();
                return new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator);
            }
            catch { return false; }
        }
    }
}

/// <summary>The feed's scheduler in the app: the UI thread and DispatcherTimer.</summary>
public sealed class DispatcherScheduler : IFeedScheduler
{
    public DateTime Now => DateTime.UtcNow;

    public void Post(Action action) => Dispatcher.UIThread.Post(() =>
    {
        try { action(); }
        catch (Exception ex) { DashboardLog.Error("feed", ex); }
    });

    public IDisposable Schedule(TimeSpan delay, Action action) =>
        DispatcherTimer.RunOnce(() =>
        {
            try { action(); }
            catch (Exception ex) { DashboardLog.Error("feed timer", ex); }
        }, delay);
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;
using DisplayXR.Dashboard.Ui;

namespace DisplayXR.Dashboard.Pages;

/// <summary>
/// Developer: the designed-but-unbuilt Phase-2 settings list and the
/// dashboard's own diagnostics (log path, CLI path, feed state). The display
/// processor is chosen per screen on Displays; a machine-wide PreferredPlugin
/// (one plug-in for every monitor, wrong on a mixed-vendor box) only shows here
/// as a notice with a way to clear it.
/// </summary>
public sealed class DeveloperPage : Page
{
    public DeveloperPage() : base("developer", "Developer") { }

    public override bool HoldsFeed => false;

    public override string Caption => "The planned developer settings and this dashboard's own diagnostics.";

    public override void OnShown() => _ = Ctx.LoadDpAsync(maxAge: TimeSpan.FromMinutes(1));

    protected override string Key()
    {
        var sb = new StringBuilder();
        sb.Append(Ctx.ActionRunning).Append('|').Append(Ctx.LastActionArea).Append(Ctx.LastAction).Append('|')
          .Append(Ctx.DpError).Append('|').Append(Ctx.Feed.Mode).Append('|').Append(Ctx.Feed.ChildStarts).Append('|')
          .Append(DashboardLog.ErrorCount).Append('|').Append(Ctx.Feed.Snapshot?.Source);
        sb.Append(Ctx.Dp?.Preferred);
        return sb.ToString();
    }

    protected override Control Build()
    {
        var page = U.VStack(18);
        if (OverrideNotice() is { } notice) page.Children.Add(notice);
        var grid = new U.CardGrid(2) { MinColumnWidth = 440 };
        grid.Add(PhaseTwoCard());
        grid.Add(DiagnosticsCard());
        page.Children.Add(grid);
        return page;
    }

    /// <summary>
    /// Only when a machine-wide PreferredPlugin is set: an amber notice and the
    /// way out. The per-screen choice lives on Displays (one place to change it).
    /// </summary>
    private Control? OverrideNotice()
    {
        bool failed = Ctx.LastActionArea == "dp" && Ctx.LastActionFailed && Ctx.LastAction is not null;
        if (StatusText.MachineOverrideNotice(Ctx.Dp) is not { } n)
        {
            // Cleared (or never set): nothing about it, unless a clear just failed.
            return failed ? U.Notice(Level.Critical, "The machine-wide override did not change", Ctx.LastAction!) : null;
        }
        var clear = U.Button("Clear machine-wide override", () => _ = Ctx.RunActionsAsync("dp", "dp reset"), "warn", "sm");
        clear.IsEnabled = !Ctx.ActionRunning;
        var displays = U.Button("Open Displays", () => Ctx.Navigate("displays"), "outline", "sm");
        var box = U.VStack(8, U.Notice(Level.Warn, n.Title, n.Text, clear, displays));
        if (failed) box.Children.Add(U.Wrapped(Ctx.LastAction!, "soft"));
        return box;
    }

    private static Control PhaseTwoCard()
    {
        var body = U.VStack(12, U.CardTitle("Developer settings", "Designed, not built yet (Phase 2)."),
            U.Wrapped("A gated list of the Tier 1 and Tier 2 levers — name, current value, provenance (env / user / machine / default), compiled default and a per-row reset — plus one “Reset all”. Collapsed and off by default, and it does not persist across launches. Tier 4 levers never appear.", "dim"),
            U.Wrapped("Design: docs/roadmap/control-panel-performance-settings.md §3 (Developer settings) and §5 (anti-stale). Until it ships, the levers are set with  displayxr-cli perf set <name> <value>  and read on Performance.", "desc"));
        return U.Card(body);
    }

    private Control DiagnosticsCard()
    {
        var kv = new U.KvList(130);
        kv.Add("Dashboard", $"{typeof(DeveloperPage).Assembly.GetName().Version?.ToString(3)}{StatusText.Sep}.NET {Environment.Version}");
        kv.Add("Log", DashboardLog.Path, mono: true);
        kv.Add("Errors logged", DashboardLog.ErrorCount == 0 ? "none this session" : $"{DashboardLog.ErrorCount} this session",
               brush: DashboardLog.ErrorCount == 0 ? null : Tokens.For(Level.Warn));
        kv.Add("CLI", Ctx.CliInfo.CliExists ? Ctx.CliInfo.CliPath : $"{Ctx.CliInfo.CliPath} (missing)", mono: true,
               brush: Ctx.CliInfo.CliExists ? null : Tokens.For(Level.Critical));
        string feed = Ctx.Feed.Mode switch
        {
            FeedMode.Watching => "one status --watch child",
            FeedMode.Polling => "polling status --json every 30 s",
            _ => "idle (no status page on screen)",
        };
        kv.Add("Feed", $"{feed}{StatusText.Sep}{Ctx.Feed.ChildStarts} start{(Ctx.Feed.ChildStarts == 1 ? "" : "s")}");
        kv.Add("Elevated", Elevation.IsElevated ? "yes (the service will not answer: headless only)" : "no",
               brush: Elevation.IsElevated ? Tokens.For(Level.Warn) : null);
        var open = U.Button("Open log folder", () =>
        {
            string dir = Path.GetDirectoryName(DashboardLog.Path)!;
            Directory.CreateDirectory(dir);
            string opener = OperatingSystem.IsWindows() ? "explorer.exe" : "xdg-open";
            try { Process.Start(new ProcessStartInfo(opener, $"\"{dir}\"") { UseShellExecute = false })?.Dispose(); }
            catch (Exception ex) { DashboardLog.Warn($"open log folder ({opener}): {ex.Message}"); }
        }, "outline", "sm");
        return U.Card(U.VStack(14, U.CardTitle("This dashboard", null, open), kv));
    }
}

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
/// Developer: the PreferredPlugin switch (Tier 1 of the old panel), the
/// designed-but-unbuilt Phase-2 settings list, and the dashboard's own
/// diagnostics (log path, CLI path, feed state).
/// </summary>
public sealed class DeveloperPage : Page
{
    public DeveloperPage() : base("developer", "Developer") { }

    public override bool HoldsFeed => false;

    public override string Caption => "Display-processor override, the planned developer settings, and this dashboard's own diagnostics.";

    public override void OnShown() => _ = Ctx.LoadDpAsync();

    protected override string Key()
    {
        var sb = new StringBuilder();
        sb.Append(Ctx.ActionRunning).Append('|').Append(Ctx.LastActionArea).Append(Ctx.LastAction).Append('|')
          .Append(Ctx.DpError).Append('|').Append(Ctx.Feed.Mode).Append('|').Append(Ctx.Feed.ChildStarts).Append('|')
          .Append(DashboardLog.ErrorCount).Append('|').Append(Ctx.Feed.Snapshot?.Source);
        if (Ctx.Dp is { } dp)
        {
            sb.Append(dp.Preferred);
            foreach (var r in dp.Plugins) sb.Append(r).Append(';');
        }
        return sb.ToString();
    }

    protected override Control Build()
    {
        var page = U.VStack(18);
        page.Children.Add(OverrideCard());
        var grid = new U.CardGrid(2) { MinColumnWidth = 440 };
        grid.Add(PhaseTwoCard());
        grid.Add(DiagnosticsCard());
        page.Children.Add(grid);
        return page;
    }

    private Control OverrideCard()
    {
        var reset = U.Button("Reset to default discovery", () => _ = Ctx.RunActionsAsync("dp", "dp reset"), "outline", "sm");
        reset.IsEnabled = !Ctx.ActionRunning && Ctx.Dp?.Preferred is not null;
        var body = U.VStack(14, U.CardTitle("Display-processor override (PreferredPlugin)",
            "Forces one display-processor plug-in for every app, machine-wide, until it is reset. Unset (the normal state), the runtime picks by probe order and per-monitor claim.", reset));

        var dp = Ctx.Dp;
        if (dp is null)
        {
            body.Children.Add(U.Wrapped(Ctx.DpError ?? "Reading the registered plug-ins…", Ctx.DpError is null ? "empty" : "soft"));
            return U.Card(body);
        }
        if (dp.Preferred is { } pref)
            body.Children.Add(U.Notice(Level.Warn, $"Override active: '{pref}'", "It persists across reboots until it is reset."));
        else
            body.Children.Add(U.Wrapped("PreferredPlugin is unset: automatic selection.", "dim"));

        var list = U.VStack(0);
        for (int i = 0; i < dp.Plugins.Count; i++)
        {
            var r = dp.Plugins[i];
            if (i > 0) list.Children.Add(U.Rule());
            var name = U.Text(r.Name.Length > 0 ? r.Name : r.Id, "value");
            name.FontWeight = Avalonia.Media.FontWeight.SemiBold;
            var head = U.Flow(8, 4, name);
            if (r.Active) U.AddFlow(head, U.Chip("active", Level.Ok));
            if (r.Preferred) U.AddFlow(head, U.Chip("preferred", Level.Warn));
            var meta = U.Text($"{r.Id}{StatusText.Sep}{r.Version}{StatusText.Sep}ProbeOrder {r.ProbeOrder}", "meta");
            var col = U.VStack(2, head, meta);
            string id = r.Id;
            var use = U.Button(r.Preferred ? "In use" : "Use", () => _ = Ctx.RunActionsAsync("dp", $"dp use {id}"), "outline", "sm");
            use.IsEnabled = !Ctx.ActionRunning && !r.Preferred;
            var g = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto"), Margin = new Thickness(0, 10) };
            g.Children.Add(col);
            Grid.SetColumn(use, 1);
            g.Children.Add(use);
            list.Children.Add(g);
        }
        body.Children.Add(list);
        body.Children.Add(U.Wrapped("A switch takes effect on the next process: restart the service or relaunch the app. Writing the override needs administrator rights.", "desc"));
        if (Ctx.LastActionArea == "dp" && Ctx.LastAction is { } a)
            body.Children.Add(U.Notice(Ctx.LastActionFailed ? Level.Critical : Level.Info, Ctx.LastActionFailed ? "The override did not change" : "Done", a));
        return U.Card(body);
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
            Process.Start(new ProcessStartInfo("explorer.exe", $"\"{dir}\"") { UseShellExecute = false })?.Dispose();
        }, "outline", "sm");
        return U.Card(U.VStack(14, U.CardTitle("This dashboard", null, open), kv));
    }
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Threading;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;
using DisplayXR.Dashboard.Ui;

namespace DisplayXR.Dashboard.Pages;

/// <summary>
/// Developer: the read-only "Developer settings" list of every Tier 1 / Tier 2
/// runtime lever (perf-settings Phase 2A, <c>perf list --all --json</c>) and
/// the dashboard's own diagnostics (log path, CLI path, feed state). The display
/// processor is chosen per screen on Displays; a machine-wide PreferredPlugin
/// (one plug-in for every monitor, wrong on a mixed-vendor box) only shows here
/// as a notice with a way to clear it.
/// </summary>
public sealed class DeveloperPage : Page
{
    public DeveloperPage() : base("developer", "Developer") { }

    // Developer settings: collapsed on every launch (design §3: it does not
    // persist), read only while expanded. The filters live outside Key() and are
    // applied in place, so typing in the search box never rebuilds the page.
    private bool _devOpen;
    private string _search = "";
    private int _tier;
    private ProcFilter _proc = ProcFilter.All;
    private bool _searchFocused;
    private readonly List<(DevLever Lever, Control Row)> _rows = new();
    private TextBlock? _shown;

    public override bool HoldsFeed => false;

    public override string Caption => "Every Tier 1 and Tier 2 runtime lever (read-only) and this dashboard's own diagnostics.";

    public override void OnShown()
    {
        _ = Ctx.LoadDpAsync(maxAge: TimeSpan.FromMinutes(1));
        if (_devOpen) _ = Ctx.LoadPerfAllAsync(force: false);
    }

    protected override string Key()
    {
        var sb = new StringBuilder();
        sb.Append(Ctx.ActionRunning).Append('|').Append(Ctx.LastActionArea).Append(Ctx.LastAction).Append('|')
          .Append(Ctx.DpError).Append('|').Append(Ctx.Feed.Mode).Append('|').Append(Ctx.Feed.ChildStarts).Append('|')
          .Append(DashboardLog.ErrorCount).Append('|').Append(Ctx.Feed.Snapshot?.Source);
        sb.Append(Ctx.Dp?.Preferred).Append('|').Append(_devOpen);
        if (_devOpen)
        {
            sb.Append('|').Append(Ctx.PerfAllLoading).Append('|').Append(Ctx.PerfAllError).Append('|');
            if (Ctx.PerfAll is { } all)
            {
                sb.Append(all.UserFile).Append(all.UserWritten).Append(all.ResolvedFor);
                foreach (var l in all.Levers) sb.Append(l).Append(';');
            }
        }
        return sb.ToString();
    }

    protected override Control Build()
    {
        var page = U.VStack(18);
        if (OverrideNotice() is { } notice) page.Children.Add(notice);
        var grid = new U.CardGrid(2) { MinColumnWidth = 440 };
        grid.Add(DevSettingsCard(), span: 2);
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

    private void ToggleDevSettings()
    {
        _devOpen = !_devOpen;
        Ctx.PerfAllWanted = _devOpen;
        if (_devOpen) _ = Ctx.LoadPerfAllAsync(force: true); // fresh on every expand; never polled
        Update();
    }

    /// <summary>
    /// Developer settings (design §3, Phase 2A): name, value, provenance,
    /// compiled default, tier, process. Managed levers (the settings file can
    /// hold them) get a Reset; every other lever is environment-only and says
    /// which variable to set instead: writing it to the file would do nothing.
    /// </summary>
    private Control DevSettingsCard()
    {
        _rows.Clear();
        _shown = null;
        var list = Ctx.PerfAll;
        string caption = _devOpen && list is not null && list.IsCatalog
            ? StatusText.DevSettingsCount(list)
            : StatusText.DevSettingsCollapsed;
        var toggle = U.Button(_devOpen ? "Hide" : "Show", ToggleDevSettings, "outline", "sm");
        var body = U.VStack(12, U.CardTitle(StatusText.DevSettingsTitle, caption, toggle));
        if (!_devOpen) return U.Card(body);

        body.Children.Add(U.Wrapped(StatusText.DevSettingsIntro, "dim"));
        body.Children.Add(U.Wrapped(StatusText.DevSettingsEnvOnlyNote, "desc"));
        if (list is null)
        {
            body.Children.Add(Ctx.PerfAllError is { } err
                ? U.Notice(Level.Critical, "Could not read the lever list", err)
                : U.Wrapped("Reading the levers (displayxr-cli perf list --all)…", "empty"));
            return U.Card(body);
        }
        if (!list.IsCatalog)
        {
            body.Children.Add(U.Notice(Level.Warn, "This displayxr-cli predates the developer list",
                "It ignored --all and listed only the settable levers. Update the runtime to see every Tier 1 and Tier 2 lever."));
        }
        if (Ctx.PerfAllError is { } stale)
            body.Children.Add(U.Notice(Level.Warn, "The last read failed; showing the previous one", stale));
        if (Ctx.LastActionArea == "devperf" && Ctx.LastAction is { } a)
            body.Children.Add(U.Notice(Ctx.LastActionFailed ? Level.Critical : Level.Info,
                Ctx.LastActionFailed ? "The reset did not apply" : "Reset", a));

        body.Children.Add(FilterBar());

        var rows = U.VStack(6);
        foreach (var l in list.Levers)
        {
            var row = LeverRow(l);
            _rows.Add((l, row));
            rows.Children.Add(row);
        }
        body.Children.Add(rows);

        var shown = U.Text("", "meta");
        shown.VerticalAlignment = VerticalAlignment.Center;
        _shown = shown;
        var resetAll = U.Button("Reset all", () => _ = Ctx.RunActionsAsync("devperf", "perf reset"), "warn", "sm");
        resetAll.IsEnabled = !Ctx.ActionRunning;
        body.Children.Add(U.Flow(14, 6, shown, resetAll));
        body.Children.Add(U.Wrapped(StatusText.DevResetAllNote, "desc"));
        if (Ctx.PerfAllLoading) body.Children.Add(U.Text("Refreshing…", "meta"));
        ApplyFilter();
        return U.Card(body);
    }

    private Control FilterBar()
    {
        var search = new TextBox
        {
            Text = _search,
            Watermark = "Search name or description",
            MinWidth = 260,
            VerticalAlignment = VerticalAlignment.Center,
        };
        Avalonia.Automation.AutomationProperties.SetName(search, "Search developer settings");
        search.TextChanged += (_, _) =>
        {
            _search = search.Text ?? "";
            ApplyFilter();
        };
        search.GotFocus += (_, _) => _searchFocused = true;
        search.LostFocus += (_, _) => _searchFocused = false;
        if (_searchFocused)
        {
            // A rebuild (a read landed) replaced the box under the cursor: keep typing in the new one.
            search.AttachedToVisualTree += (_, _) => Dispatcher.UIThread.Post(() =>
            {
                search.Focus();
                search.CaretIndex = search.Text?.Length ?? 0;
            });
        }

        string tg = "devtier" + Guid.NewGuid().ToString("N");
        string pg = "devproc" + Guid.NewGuid().ToString("N");
        var tierLabel = U.Text("Tier", "label");
        var procLabel = U.Text("Process", "label");
        tierLabel.VerticalAlignment = VerticalAlignment.Center;
        procLabel.VerticalAlignment = VerticalAlignment.Center;
        return U.Flow(12, 6, search, tierLabel,
            FilterOption(tg, "All", _tier == 0, () => _tier = 0),
            FilterOption(tg, "1", _tier == 1, () => _tier = 1),
            FilterOption(tg, "2", _tier == 2, () => _tier = 2),
            procLabel,
            FilterOption(pg, "All", _proc == ProcFilter.All, () => _proc = ProcFilter.All),
            FilterOption(pg, "App", _proc == ProcFilter.App, () => _proc = ProcFilter.App),
            FilterOption(pg, "Service", _proc == ProcFilter.Service, () => _proc = ProcFilter.Service));
    }

    private RadioButton FilterOption(string group, string label, bool isChecked, Action pick)
    {
        var rb = new RadioButton { GroupName = group, IsChecked = isChecked, Content = label, VerticalAlignment = VerticalAlignment.Center };
        rb.Classes.Add("option");
        rb.IsCheckedChanged += (_, _) =>
        {
            if (rb.IsChecked != true) return;
            pick();
            ApplyFilter();
        };
        return rb;
    }

    private void ApplyFilter()
    {
        int shown = 0;
        foreach (var (lever, row) in _rows)
        {
            bool visible = lever.Matches(_search, _tier, _proc);
            row.IsVisible = visible;
            if (visible) shown++;
        }
        if (_shown is not null) _shown.Text = StatusText.DevShownText(shown, _rows.Count);
    }

    private Control LeverRow(DevLever l)
    {
        var name = U.Text(l.Name, "mono");
        var value = U.Text(l.Value ?? "default", "mono");
        value.Foreground = l.NonDefault ? Tokens.For(Level.Warn) : Tokens.Muted;
        name.VerticalAlignment = VerticalAlignment.Center;
        value.VerticalAlignment = VerticalAlignment.Center;
        var head = U.Flow(10, 4, name, value, U.Chip(l.Source, StatusText.DevSourceLevel(l.Source)),
            U.Chip(StatusText.DevTierLabel(l.Tier)), U.Chip(l.Proc.Length > 0 ? l.Proc : StatusText.Dash));
        var col = U.VStack(4, head);
        if (l.Description.Length > 0) col.Children.Add(U.Wrapped(l.Description, "desc"));

        var dflt = U.Text($"Compiled default: {(l.Default.Length > 0 ? l.Default : StatusText.Dash)}", "meta");
        dflt.VerticalAlignment = VerticalAlignment.Center;
        var foot = U.Flow(12, 2, dflt);
        if (l.Managed)
        {
            var reset = U.Button("Reset", () => _ = Ctx.RunActionsAsync("devperf", $"perf reset {l.Name}"), "outline", "sm");
            reset.IsEnabled = !Ctx.ActionRunning && l.Resettable;
            Avalonia.Automation.AutomationProperties.SetName(reset, $"Reset {l.Name}");
            U.AddFlow(foot, U.Chip("settable", Level.Info), 12, 2);
            U.AddFlow(foot, reset, 12, 2);
            if (l.NonDefault && StatusText.DevResetBlocked(l) is { } why)
            {
                var w = U.Text(why, "meta");
                w.VerticalAlignment = VerticalAlignment.Center;
                U.AddFlow(foot, w, 12, 2);
            }
        }
        else
        {
            var hint = U.Wrapped(StatusText.DevEnvOnlyHint(l), "meta");
            hint.VerticalAlignment = VerticalAlignment.Center;
            U.AddFlow(foot, hint, 12, 2);
        }
        col.Children.Add(foot);

        var b = new Border { Child = col, Padding = new Thickness(12, 8) };
        b.Classes.Add("field");
        if (l.NonDefault)
        {
            b.BorderBrush = Tokens.Alpha(Tokens.ColorKey(Level.Warn), 0.55);
            b.Background = Tokens.Alpha(Tokens.ColorKey(Level.Warn), 0.06);
        }
        return b;
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

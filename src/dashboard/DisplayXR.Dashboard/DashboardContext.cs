// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Linq;
using System.Threading.Tasks;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;

namespace DisplayXR.Dashboard;

/// <summary>
/// What the pages share: the status feed, and the one-shot CLI reads and
/// actions (info, perf, dp, selftest) with their last results. Lives on the
/// UI thread; <see cref="Changed"/> fires there.
/// </summary>
public sealed class DashboardContext
{
    public static readonly TimeSpan InfoTimeout = TimeSpan.FromSeconds(60);
    public static readonly TimeSpan ActionTimeout = TimeSpan.FromSeconds(30);
    public static readonly TimeSpan SelftestTimeout = TimeSpan.FromSeconds(90);

    public DashboardContext(StatusFeed feed, CliProcessSource cli, IProcessSource? runner = null)
    {
        Feed = feed;
        CliInfo = cli;
        Cli = runner ?? cli;
    }

    public StatusFeed Feed { get; }
    /// <summary>Where the one-shot verbs run (the CLI, or the fixture source in --fixture mode).</summary>
    public IProcessSource Cli { get; }
    /// <summary>The resolved CLI, for the Developer page.</summary>
    public CliProcessSource CliInfo { get; }

    public InfoResult? Info { get; private set; }
    public bool InfoLoading { get; private set; }
    public string? InfoError { get; private set; }
    public DateTime? InfoWhen { get; private set; }

    public DpList? Dp { get; private set; }
    public string? DpError { get; private set; }

    public PerfState? Perf { get; private set; }
    public string? PerfError { get; private set; }
    public DateTime? PerfWhen { get; private set; }

    public SelftestResult? Selftest { get; private set; }
    public bool SelftestRunning { get; private set; }
    public string? SelftestError { get; private set; }

    /// <summary>The last action's one-line result (perf / dp / runtime), shown under the controls that ran it.</summary>
    public string? LastAction { get; private set; }
    public bool LastActionFailed { get; private set; }
    public string? LastActionArea { get; private set; }
    public bool ActionRunning { get; private set; }

    /// <summary>Bumped on every change, so pages can key their rebuilds on it.</summary>
    public int Version { get; private set; }

    public event Action? Changed;

    public Action<string> Navigate { get; set; } = _ => { };
    public Func<string, Task> CopyText { get; set; } = _ => Task.CompletedTask;
    public Action<string> Toast { get; set; } = _ => { };

    private void Bump()
    {
        Version++;
        try { Changed?.Invoke(); }
        catch (Exception ex) { DashboardLog.Error("context listener", ex); }
    }

    /// <summary><c>info --json</c>: once per Home show, and on Refresh (never polled: it loads every plug-in).</summary>
    public async Task LoadInfoAsync(bool force)
    {
        if (InfoLoading || (Info is not null && !force)) return;
        InfoLoading = true;
        Bump();
        try
        {
            var r = await Cli.RunAsync("info --json", InfoTimeout);
            InfoResult? info = null;
            string? err = null;
            if (r.Started && !r.TimedOut && InfoResult.TryParse(r.Json, out info, out err))
            {
                Info = info;
                InfoError = null;
                InfoWhen = DateTime.Now;
                if (info!.Perf.Levers.Count > 0 && Perf is null) { Perf = info.Perf; PerfWhen = DateTime.Now; }
            }
            else
            {
                InfoError = r.Started && !r.TimedOut ? $"Could not read 'displayxr-cli info --json': {err}" : r.Summary;
            }
        }
        catch (Exception ex)
        {
            InfoError = ex.Message;
            DashboardLog.Error("info", ex);
        }
        finally
        {
            InfoLoading = false;
            Bump();
        }
    }

    public bool DpLoading { get; private set; }
    private DateTime? _dpWhen;
    private bool _dpReloadPending;

    /// <summary>
    /// <c>dp list --json</c>. Since the per-screen overrides it loads every
    /// plug-in (a headless instance, ~10 s), so it is never polled: read when a
    /// page that needs it is shown and the last read is older than
    /// <paramref name="maxAge"/> (rapid page switching reuses it), on Refresh
    /// and after a change (<paramref name="force"/>), one read at a time.
    /// </summary>
    public async Task LoadDpAsync(bool force = false, TimeSpan? maxAge = null)
    {
        if (DpLoading)
        {
            // A change landed while an older read is in flight: read once more after it.
            if (force) _dpReloadPending = true;
            return;
        }
        if (!force && Dp is not null && _dpWhen is { } t && DateTime.Now - t < (maxAge ?? TimeSpan.FromSeconds(30))) return;
        DpLoading = true;
        Bump();
        try
        {
            var r = await Cli.RunAsync("dp list --json", InfoTimeout);
            if (r.Ok && DpList.TryParse(r.Json, out var list)) { Dp = list; DpError = null; _dpWhen = DateTime.Now; }
            else DpError = r.Ok ? "Could not read 'displayxr-cli dp list --json'." : r.Summary;
        }
        catch (Exception ex) { DpError = ex.Message; DashboardLog.Error("dp list", ex); }
        finally { DpLoading = false; }
        Bump();
        if (_dpReloadPending)
        {
            _dpReloadPending = false;
            await LoadDpAsync(force: true);
        }
    }

    public async Task LoadPerfAsync(bool force)
    {
        if (!force && PerfWhen is { } t && DateTime.Now - t < TimeSpan.FromSeconds(5)) return;
        try
        {
            var r = await Cli.RunAsync("perf list --json", ActionTimeout);
            if (r.Ok && PerfStateParser.TryParse(r.Json, out var p)) { Perf = p; PerfError = null; PerfWhen = DateTime.Now; }
            else PerfError = r.Ok ? "Could not read 'displayxr-cli perf list --json'." : r.Summary;
        }
        catch (Exception ex) { PerfError = ex.Message; DashboardLog.Error("perf list", ex); }
        Bump();
    }

    public async Task RunSelftestAsync()
    {
        if (SelftestRunning) return;
        SelftestRunning = true;
        SelftestError = null;
        Bump();
        try
        {
            var r = await Cli.RunAsync("selftest --json", SelftestTimeout);
            if (r.Started && !r.TimedOut && SelftestResult.TryParse(r.Json, DateTime.Now, out var st)) Selftest = st;
            else
            {
                SelftestError = r.Summary;
                DashboardLog.Warn($"selftest: exit {r.ExitCode}, timed out {r.TimedOut}, {r.Stdout.Length} bytes stdout; stderr tail: {Tail(r.Stderr)}");
            }
        }
        catch (Exception ex) { SelftestError = ex.Message; DashboardLog.Error("selftest", ex); }
        finally
        {
            SelftestRunning = false;
            Bump();
        }
    }

    /// <summary>
    /// Run CLI writes in order (one writer, the CLI; the panel never writes a
    /// setting itself), stop at the first failure, then re-read the resolved
    /// state: what the page shows is what the chain resolved, never what the
    /// click assumed (control_panel_main.c perf_action).
    /// </summary>
    public async Task RunActionsAsync(string area, params string[] commands)
    {
        if (ActionRunning) return;
        ActionRunning = true;
        LastActionArea = area;
        Bump();
        try
        {
            CliResult? last = null;
            foreach (var cmd in commands)
            {
                DashboardLog.Info($"action: displayxr-cli {cmd}");
                last = await Cli.RunAsync(cmd, ActionTimeout);
                if (!last.Ok) break;
            }
            if (last is not null)
            {
                LastActionFailed = !last.Ok;
                string summary = last.Summary;
                if (!last.Ok && area == "dp" && !Ui.Elevation.IsElevated)
                    summary += " — the display-processor override is a machine-wide (HKLM) setting: run the dashboard as administrator to change it.";
                LastAction = summary;
            }
        }
        catch (Exception ex)
        {
            LastActionFailed = true;
            LastAction = ex.Message;
            DashboardLog.Error("action", ex);
        }
        finally
        {
            ActionRunning = false;
            Bump();
        }
        if (area == "perf") await LoadPerfAsync(force: true);
        if (area is "dp" or "screen-dp") await LoadDpAsync(force: true);
        if (area == "runtime") await LoadInfoAsync(force: true);
    }

    private static string Tail(string s) => s.Length <= 400 ? s : s[^400..];

    public bool HasMultipleAdapters => Info?.Gpu is { Probed: true } g && g.Adapters.Count > 1;

    public string? PreferredPlugin => Dp?.Preferred;

    public bool PerfNonDefault => Perf?.AnyNonDefault ?? false;

    public string ElevationNote => Ui.Elevation.IsElevated
        ? "The dashboard is running elevated. The service answers status reads only from non-elevated diagnostics clients, so the feed falls back to headless snapshots. Start it from the Start menu (not as administrator) for live rows."
        : "";

    public int AppClientCount => Feed.Snapshot?.Clients.Count(c => !c.IsDiag) ?? 0;
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
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

    public DashboardContext(StatusFeed feed, CliProcessSource cli, IProcessSource? runner = null, ICatalogEngine? engine = null)
    {
        Feed = feed;
        CliInfo = cli;
        Cli = runner ?? cli;
        HotkeySuspend = new HotkeySuspend(Cli);
        Catalog = new CatalogService(engine ?? new DxrGetEngine(cli.CliPath), Cli);
    }

    // ── Catalog (ADR-064 step 8) ────────────────────────────────────────────

    /// <summary>The install engine and the licence verbs, behind one class (Feed/CatalogService.cs).</summary>
    public CatalogService Catalog { get; }

    /// <summary>The last read of the catalog: the list, or why there is none.</summary>
    public CatalogRead? CatalogRead { get; private set; }
    public bool CatalogLoading { get; private set; }
    private DateTime? _catalogWhen;
    private bool _catalogReloadPending;

    /// <summary>The install / update / uninstall in flight (one at a time), and how the last one ended.</summary>
    public CatalogOperation? CatalogOp { get; private set; }
    public (string ItemId, CatalogOutcome Outcome)? CatalogResult { get; private set; }

    /// <summary>The key activation in flight, and how the last one ended. Neither ever holds the key.</summary>
    public string? ActivatingItem { get; private set; }
    public (string ItemId, ActivationOutcome Outcome)? ActivationResult { get; private set; }

    /// <summary>An item (or a Components row) another page asked the Catalog to open; the Catalog page consumes it.</summary>
    public CatalogFocus? CatalogFocus { get; set; }

    /// <summary>Open the Catalog on the item behind a Components row.</summary>
    public void OpenInCatalog(ComponentClass cls, string componentId, string name)
    {
        CatalogFocus = new CatalogFocus(null, cls, componentId, name);
        Navigate("catalog");
    }

    /// <summary>
    /// <c>list --json</c> through the engine: on Catalog show when older than 5 min, on Refresh and
    /// after an operation (<paramref name="force"/>). It reaches the network (bounded by the engine),
    /// so it is never polled, and never runs on the UI thread.
    /// </summary>
    public async Task LoadCatalogAsync(bool force = false)
    {
        if (CatalogLoading) { if (force) _catalogReloadPending = true; return; }
        if (!force && CatalogRead is not null && _catalogWhen is { } t && DateTime.Now - t < TimeSpan.FromMinutes(5)) return;
        CatalogLoading = true;
        Bump();
        try { CatalogRead = await Task.Run(() => Catalog.ListAsync()); }
        catch (Exception ex)
        {
            CatalogRead = Model.CatalogRead.From(-1, "", false, false, ex.Message);
            DashboardLog.Error("catalog list", ex);
        }
        finally
        {
            CatalogLoading = false;
            _catalogWhen = DateTime.Now;
        }
        DashboardLog.Info($"catalog: {CatalogRead.Status}, {CatalogRead.List?.Items.Count ?? 0} item(s)");
        Bump();
        if (_catalogReloadPending)
        {
            _catalogReloadPending = false;
            await LoadCatalogAsync(force: true);
        }
    }

    /// <summary>
    /// Install, update or uninstall one item: bounded, cancellable (the wait, see
    /// <see cref="ICatalogEngine.RunElevatedAsync"/>), and always ending in an outcome the page
    /// shows. Then the catalog, and the item's class rows, are re-read.
    /// </summary>
    public async Task RunCatalogAsync(CatalogVerb verb, CatalogItem item)
    {
        if (CatalogBusy) return;
        var op = new CatalogOperation(item.Id, item.Name, verb, DateTime.Now);
        CatalogOp = op;
        CatalogResult = null;
        Bump();
        CatalogOutcome outcome;
        try
        {
            DashboardLog.Info($"catalog: {verb} {item.Id}");
            void Phase(string p) => Avalonia.Threading.Dispatcher.UIThread.Post(() =>
            {
                if (!ReferenceEquals(CatalogOp, op)) return;
                op.Phase = p;
                Bump();
            });
            outcome = await Task.Run(() => Catalog.RunAsync(verb, item, Phase, op.StopWaiting.Token));
        }
        catch (Exception ex)
        {
            outcome = CatalogExit.StartFailed(ex.Message);
            DashboardLog.Error("catalog operation", ex);
        }
        DashboardLog.Info($"catalog: {verb} {item.Id} -> {(outcome.Ok ? "ok" : "not done")} (exit {outcome.ExitCode?.ToString() ?? "-"}): {outcome.Title}");
        CatalogOp = null;
        CatalogResult = (item.Id, outcome);
        op.StopWaiting.Dispose();
        if (outcome.StillRunning is { IsCompleted: false } running)
        {
            // The wait ended, the engine did not: nothing else may start until it has (it takes no lock).
            var background = new CatalogBackground(item.Id, item.Name, verb);
            CatalogBackground = background;
            Bump();
            _ = WatchBackgroundAsync(background, running, item);
            return;
        }
        Bump();
        await LoadCatalogAsync(force: true);
        if (outcome.Changed && item.PolicyClass is { } cls) await LoadPolicyAsync(cls, force: true);
    }

    /// <summary>
    /// The engine run whose wait ended (Stop waiting, or the timeout) while the engine was still at
    /// work. While it is set, Install / Update / Uninstall stay disabled. Cleared when the process is
    /// known to have exited, or by Refresh (the list then shows where things stand).
    /// </summary>
    public CatalogBackground? CatalogBackground { get; private set; }

    /// <summary>An install, update or uninstall is in flight or still finishing: no other may start.</summary>
    public bool CatalogBusy => CatalogOp is not null || CatalogBackground is not null;

    private async Task WatchBackgroundAsync(CatalogBackground background, Task<int> running, CatalogItem item)
    {
        CatalogOutcome outcome;
        try { outcome = CatalogExit.Describe(background.Verb, await running, background.Name); }
        catch (Exception ex)
        {
            outcome = CatalogExit.StartFailed(ex.Message);
            DashboardLog.Warn($"catalog background: {ex.GetType().Name}");
        }
        if (_shutdown.IsCancellationRequested) return;
        DashboardLog.Info($"catalog: {background.Verb} {background.ItemId} ended in the background (exit {outcome.ExitCode?.ToString() ?? "-"})");
        if (ReferenceEquals(CatalogBackground, background)) CatalogBackground = null;
        CatalogResult = (background.ItemId, outcome);
        Bump();
        await LoadCatalogAsync(force: true);
        if (outcome.Changed && item.PolicyClass is { } cls) await LoadPolicyAsync(cls, force: true);
    }

    /// <summary>Refresh: the user asked to see where things stand; the list they get is the answer.</summary>
    public void ForgetCatalogBackground()
    {
        if (CatalogBackground is null) return;
        CatalogBackground = null;
        Bump();
    }

    private readonly System.Threading.CancellationTokenSource _shutdown = new();

    /// <summary>The window is closing: end the Dashboard's own waits and polls (never an engine run).</summary>
    public void Shutdown()
    {
        try { _shutdown.Cancel(); }
        catch (ObjectDisposedException) { }
    }

    /// <summary>Stop waiting for the operation in flight. The engine, once elevated, finishes on its own.</summary>
    public void StopWaitingForCatalog()
    {
        try { CatalogOp?.StopWaiting.Cancel(); }
        catch (ObjectDisposedException) { }
    }

    public void DismissCatalogResult()
    {
        CatalogResult = null;
        ActivationResult = null;
        Bump();
    }

    /// <summary>Deliver a pasted key to an installed component, then re-read its licence. The key is not kept.</summary>
    public async Task ActivateAsync(CatalogItem item, ComponentClass cls, string key)
    {
        if (ActivatingItem is not null) return;
        ActivatingItem = item.Id;
        ActivationResult = null;
        Bump();
        ActivationOutcome outcome;
        try { outcome = await Task.Run(() => Catalog.ActivateAsync(cls, item.ComponentId, item.Name, key)); }
        catch (Exception ex)
        {
            outcome = new ActivationOutcome(false, "The key could not be delivered", "Try again.");
            DashboardLog.Warn($"activate: {ex.GetType().Name}");
        }
        DashboardLog.Info($"catalog: activate {item.Id} -> {(outcome.Accepted ? "accepted" : "not accepted")}");
        ActivatingItem = null;
        ActivationResult = (item.Id, outcome);
        Bump();
        await LoadPolicyAsync(cls, force: true);
    }

    /// <summary>How the last press of Buy went (what was opened, or why nothing was), and the Buy still being finished.</summary>
    public (string ItemId, BuyPlan Plan)? BuyNote { get; private set; }
    public BuyPending? BuyPending { get; private set; }
    public string? BuyStarting { get; private set; }
    private readonly HashSet<string> _noBuy = new();

    /// <summary><c>activate-begin</c> said this item has no acceptable purchase page: no Buy button for it.</summary>
    public bool NoBuy(string itemId) => _noBuy.Contains(itemId);

    /// <summary>How often the licence is re-read while a Buy is pending (each read loads the class's components).</summary>
    public TimeSpan BuyPollInterval { get; set; } = TimeSpan.FromSeconds(15);

    private static string LicenceKey(PolicyRow? r) => r is null ? "" : $"{r.Licence}|{r.LicenceSource}|{r.LicenceExpires}";

    /// <summary>
    /// Buy (activation-deep-link.md §3): start the activation through the CLI, open what it says to
    /// open in the default browser, and, while the purchase may still be coming back through
    /// <c>displayxr://activate</c>, keep the item's licence fresh: on window focus and on a bounded
    /// poll that ends with the pending activation (15 minutes) or as soon as the licence changes.
    /// </summary>
    public async Task BuyAsync(CatalogItem item, ComponentClass cls, Uri? purchase, bool? deepLink)
    {
        if (BuyStarting is not null) return;
        BuyStarting = item.Id;
        BuyNote = null;
        Bump();
        BuyPlan plan;
        try { plan = await Task.Run(() => Catalog.BeginBuyAsync(cls, item.ComponentId, item.Name, purchase, deepLink)); }
        catch (Exception ex)
        {
            plan = BuyPlan.Fallback(item.Name, purchase);
            DashboardLog.Warn($"buy: {ex.GetType().Name}");
        }
        BuyStarting = null;
        if (plan.Kind == BuyKind.NoPurchasePage) _noBuy.Add(item.Id);
        if (plan.Opens && !OpenUrl(plan.Url!.AbsoluteUri))
            plan = plan with { Kind = BuyKind.NotNow, Title = "The browser didn't open", Text = "Try again. You can still paste a key." };
        BuyNote = (item.Id, plan);
        if (plan.Opens)
        {
            _policies.TryGetValue(cls, out var pl);
            // A purchase page without a return link still gets the focus refresh, for 15 minutes.
            var until = plan.Expires ?? DateTimeOffset.UtcNow.AddMinutes(15);
            var pending = new BuyPending(item.Id, item.Name, cls, item.ComponentId, plan, LicenceKey(pl?.Row(item.ComponentId)), until);
            BuyPending = pending;
            if (plan.Kind == BuyKind.OpenWithReturn) _ = PollBuyAsync(pending);
        }
        Bump();
    }

    /// <summary>Opens an https URL in the browser; replaceable in tests.</summary>
    public Func<string, bool> OpenUrl { get; set; } = UrlLauncher.Open;

    private async Task PollBuyAsync(BuyPending pending)
    {
        try
        {
            while (ReferenceEquals(BuyPending, pending) && DateTimeOffset.UtcNow < pending.Until)
            {
                await Task.Delay(BuyPollInterval, _shutdown.Token);
                if (!ReferenceEquals(BuyPending, pending)) return;
                await RefreshPendingBuyAsync();
            }
        }
        catch (OperationCanceledException) { return; } // the window is closing
        catch (Exception ex) { DashboardLog.Warn($"buy poll: {ex.GetType().Name}"); }
        if (ReferenceEquals(BuyPending, pending))
        {
            BuyPending = null; // the pending activation has ended: stop looking
            Bump();
        }
    }

    /// <summary>
    /// Re-read the licence of the item a Buy is pending for (the window came back to the front, or the
    /// poll ticked). When it changed, the Buy is over; when it is now licensed, say so.
    /// </summary>
    public async Task RefreshPendingBuyAsync()
    {
        if (BuyPending is not { } pending) return;
        if (DateTimeOffset.UtcNow >= pending.Until)
        {
            BuyPending = null;
            Bump();
            return;
        }
        await LoadPolicyAsync(pending.Class, force: true);
        if (!ReferenceEquals(BuyPending, pending)) return;
        _policies.TryGetValue(pending.Class, out var pl);
        var row = pl?.Row(pending.ComponentId);
        if (row is null || LicenceKey(row) == pending.LicenceWas) return;
        BuyPending = null;
        BuyNote = null;
        if (row.Licence == "licensed")
            ActivationResult = (pending.ItemId, new ActivationOutcome(true, $"{pending.Name} is activated", "Thank you. It is ready to use."));
        DashboardLog.Info($"catalog: licence of {pending.ItemId} is now '{row.Licence}'");
        Bump();
    }

    public void DismissBuyNote()
    {
        BuyNote = null;
        Bump();
    }

    /// <summary>Suspends the service's workspace hotkey while the capture box has focus.</summary>
    public HotkeySuspend HotkeySuspend { get; }

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

    /// <summary><c>perf list --all --json</c>: the Developer page's read-only lever list (Phase 2A).</summary>
    public DevLeverList? PerfAll { get; private set; }
    public string? PerfAllError { get; private set; }
    public bool PerfAllLoading { get; private set; }
    /// <summary>The Developer settings section is expanded: Refresh re-reads the list.</summary>
    public bool PerfAllWanted { get; set; }
    private bool _perfAllReloadPending;

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
            var r = await Cli.RunAsync(CliArgs.Words("info --json"), InfoTimeout);
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

    public WorkspaceList? Workspace { get; private set; }
    public bool WorkspaceLoading { get; private set; }
    /// <summary>The CLI has no <c>workspace list</c> verb (predates phase 8): the launch row stays hidden.</summary>
    public bool WorkspaceUnsupported { get; private set; }
    private DateTime? _wsWhen;
    private bool _wsReloadPending;

    /// <summary><c>workspace list --json</c>: on Components show when older than 30 s, forced after a change.</summary>
    public async Task LoadWorkspaceAsync(bool force = false)
    {
        if (WorkspaceLoading) { if (force) _wsReloadPending = true; return; }
        if (!force && _wsWhen is { } t && DateTime.Now - t < TimeSpan.FromSeconds(30)) return;
        WorkspaceLoading = true;
        Bump();
        try
        {
            var r = await Cli.RunAsync(CliArgs.Words("workspace list --json"), InfoTimeout);
            if (r.Started && !r.TimedOut && WorkspaceList.TryParse(r.Json, out var ws))
            {
                Workspace = ws;
                WorkspaceUnsupported = false;
            }
            else if (r.Started && !r.TimedOut)
            {
                WorkspaceUnsupported = true; // unknown verb / no JSON: an older CLI
            }
            _wsWhen = DateTime.Now;
        }
        catch (Exception ex) { DashboardLog.Error("workspace list", ex); }
        finally { WorkspaceLoading = false; }
        Bump();
        if (_wsReloadPending)
        {
            _wsReloadPending = false;
            await LoadWorkspaceAsync(force: true);
        }
    }

    // ADR-054 N3e: the per-class policy (`dp|input|conv list --json`) the Components
    // page's controls read and, after every write, re-read. One read per class at a
    // time; never polled (dp / input list load every component headlessly, ~10 s).
    private readonly Dictionary<ComponentClass, PolicyList> _policies = new();
    private readonly Dictionary<ComponentClass, string> _policyErrors = new();
    private readonly Dictionary<ComponentClass, DateTime> _policyWhen = new();
    private readonly HashSet<ComponentClass> _policyLoading = new();
    private readonly HashSet<ComponentClass> _policyReloadPending = new();
    private readonly HashSet<ComponentClass> _policyUnsupported = new();

    public IReadOnlyDictionary<ComponentClass, PolicyList> Policies => _policies;
    public bool PolicyLoading(ComponentClass c) => _policyLoading.Contains(c);
    public string? PolicyError(ComponentClass c) => _policyErrors.TryGetValue(c, out var e) ? e : null;
    /// <summary>The CLI has no <c>&lt;class&gt; list --json</c> (predates N3): the controls stay hidden.</summary>
    public bool PolicyUnsupported(ComponentClass c) => _policyUnsupported.Contains(c);

    /// <summary>
    /// <c>&lt;class&gt; list --json</c>: on Components show when older than 30 s, forced after a
    /// change. What the page shows is what the CLI resolved, never what the click assumed.
    /// </summary>
    public async Task LoadPolicyAsync(ComponentClass cls, bool force = false, TimeSpan? maxAge = null)
    {
        if (_policyLoading.Contains(cls))
        {
            if (force) _policyReloadPending.Add(cls);
            return;
        }
        if (!force && _policyWhen.TryGetValue(cls, out var t) && DateTime.Now - t < (maxAge ?? TimeSpan.FromSeconds(30))) return;
        _policyLoading.Add(cls);
        Bump();
        try
        {
            var verb = new CliArgs(cls.Verb(), "list", "--json");
            var r = await Cli.RunAsync(verb, InfoTimeout);
            if (r.Ok && PolicyList.TryParse(cls, r.Json, out var list))
            {
                _policies[cls] = list!;
                _policyErrors.Remove(cls);
                _policyUnsupported.Remove(cls);
            }
            else if (r.Started && !r.TimedOut && !r.Ok && r.Stdout.Length == 0)
            {
                _policyUnsupported.Add(cls); // unknown verb: an older CLI
            }
            else _policyErrors[cls] = r.Ok ? $"Could not read 'displayxr-cli {verb}'." : r.Summary;
            _policyWhen[cls] = DateTime.Now;
        }
        catch (Exception ex) { _policyErrors[cls] = ex.Message; DashboardLog.Error(cls.Verb() + " list", ex); }
        finally { _policyLoading.Remove(cls); }
        Bump();
        if (_policyReloadPending.Remove(cls)) await LoadPolicyAsync(cls, force: true);
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
            var r = await Cli.RunAsync(CliArgs.Words("dp list --json"), InfoTimeout);
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
            var r = await Cli.RunAsync(CliArgs.Words("perf list --json"), ActionTimeout);
            if (r.Ok && PerfStateParser.TryParse(r.Json, out var p)) { Perf = p; PerfError = null; PerfWhen = DateTime.Now; }
            else PerfError = r.Ok ? "Could not read 'displayxr-cli perf list --json'." : r.Summary;
        }
        catch (Exception ex) { PerfError = ex.Message; DashboardLog.Error("perf list", ex); }
        Bump();
    }

    /// <summary>
    /// <c>perf list --all --json</c>: read when the Developer settings section is
    /// expanded, on Refresh and after a reset (<paramref name="force"/>) — never
    /// polled. One read at a time.
    /// </summary>
    public async Task LoadPerfAllAsync(bool force)
    {
        if (PerfAllLoading)
        {
            if (force) _perfAllReloadPending = true;
            return;
        }
        if (!force && PerfAll is not null) return;
        PerfAllLoading = true;
        Bump();
        try
        {
            var r = await Cli.RunAsync(CliArgs.Words("perf list --all --json"), ActionTimeout);
            if (r.Ok && DevLeverList.TryParse(r.Json, out var list)) { PerfAll = list; PerfAllError = null; }
            else PerfAllError = r.Ok ? "Could not read 'displayxr-cli perf list --all --json'." : r.Summary;
        }
        catch (Exception ex) { PerfAllError = ex.Message; DashboardLog.Error("perf list --all", ex); }
        finally { PerfAllLoading = false; }
        Bump();
        if (_perfAllReloadPending)
        {
            _perfAllReloadPending = false;
            await LoadPerfAllAsync(force: true);
        }
    }

    public async Task RunSelftestAsync()
    {
        if (SelftestRunning) return;
        SelftestRunning = true;
        SelftestError = null;
        Bump();
        try
        {
            var r = await Cli.RunAsync(CliArgs.Words("selftest --json"), SelftestTimeout);
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
    /// Run CLI writes in order (one writer, the CLI; the dashboard never
    /// writes a setting itself), stop at the first failure, then re-read the
    /// resolved state: what the page shows is what the chain resolved, never
    /// what the click assumed.
    /// </summary>
    public async Task RunActionsAsync(string area, params CliArgs[] commands)
    {
        if (ActionRunning) return;
        ActionRunning = true;
        LastActionArea = area;
        Bump();
        try
        {
            CliResult? last = null;
            var notes = new List<string>();
            foreach (var cmd in commands)
            {
                DashboardLog.Info($"action: displayxr-cli {cmd}");
                last = await Cli.RunAsync(cmd, ActionTimeout);
                notes.AddRange(last.Notes);
                if (!last.Ok) break;
            }
            if (last is not null)
            {
                LastActionFailed = !last.Ok;
                // Verbatim: the CLI's own words (the pages say what needs admin) — its result line
                // and every note / warning of every command run, not only the first line.
                LastAction = CliResult.Compose(last.Summary, notes);
                foreach (var n in notes) DashboardLog.Info($"action: {n}");
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
        if (area is "perf" or "devperf") await LoadPerfAsync(force: true);
        if (area == "devperf" || (area == "perf" && PerfAll is not null)) await LoadPerfAllAsync(force: true);
        if (area is "dp" or "screen-dp") await LoadDpAsync(force: true);
        if (area == "workspace") await LoadWorkspaceAsync(force: true);
        if (area == "runtime") await LoadInfoAsync(force: true);
        // ADR-054 N3e: a policy write re-reads that class (and the DP list the Displays page shares).
        foreach (var c in ComponentClasses.All)
            if (area == PolicyCommands.Area(c))
            {
                await LoadPolicyAsync(c, force: true);
                if (c == ComponentClass.Display) await LoadDpAsync(force: true);
                // N6 Part 2: Enabled / Preferred change what the provider probe ranks (info's tracking block).
                if (c == ComponentClass.Tracking) await LoadInfoAsync(force: true);
            }
        // #2181: a per-app pin write re-reads the input list (pins + live holders).
        if (area == PolicyCommands.AppArea) await LoadPolicyAsync(ComponentClass.Input, force: true);
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

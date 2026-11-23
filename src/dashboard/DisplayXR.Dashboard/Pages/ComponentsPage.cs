// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Input;
using Avalonia.Media;
using Avalonia.Threading;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;
using DisplayXR.Dashboard.Ui;

namespace DisplayXR.Dashboard.Pages;

/// <summary>
/// Components: what is plugged into the runtime, grouped by ROLE (display
/// processors, workspace controller, present owners, input providers,
/// viewer-tracking providers, the conversion module, a stereo camera source,
/// diagnostics clients), never by
/// product. Clients come from the live feed; the rest from <c>info --json</c>,
/// read once per show and on Refresh.
///
/// <para>It is also where things are added (ADR-064 step 8): each section carries the catalog
/// items of its class (<c>dxr-get list --json</c>), an installed component and its catalog entry
/// being ONE row with one status, the runtime's own catalog row opens the page as "Runtime", and
/// catalog items that are not components close it as "Apps". Install / Update / Uninstall go
/// through the install engine (one Windows permission prompt, the Dashboard itself stays non-elevated). The join, the statuses and what a row may
/// offer are <see cref="ComponentRows"/>; the words are <see cref="CatalogItem"/> and friends.</para>
/// </summary>
public sealed class ComponentsPage : Page
{
    private readonly HashSet<string> _expanded = new();

    // The catalog side of the list: the filter, the rows whose detail is open, the key box.
    private RowFilter _filter;
    private readonly HashSet<string> _open = new();
    private string? _keyFor;         // the item whose key box is open
    private TextBlock? _elapsed;
    private DispatcherTimer? _tick;
    private CatalogOutcome? _resultSeen;

    public ComponentsPage() : base("components", "Components") { }

    public override string Caption => "What you have, and what you can add.";

    public override void OnShown()
    {
        _ = Ctx.LoadInfoAsync(force: false);
        _ = Ctx.LoadWorkspaceAsync();
        // ADR-054 N3e: the policy rows behind the Enabled / Preferred controls.
        foreach (var c in ComponentClasses.All) _ = Ctx.LoadPolicyAsync(c);
        _ = Ctx.LoadCatalogAsync();
        _tick ??= new DispatcherTimer(TimeSpan.FromSeconds(1), DispatcherPriority.Background, (_, _) => UpdateInPlace());
        _tick.Start();
    }

    // ADR-054 N3e: machine-wide (--machine) writes, offered only to an elevated dashboard;
    // and the row the last policy action was run for, so its CLI reply shows under it.
    private bool _machine;
    private string? _policyActionRow;

    // Hotkey capture: the controller being captured, and the last capture's note.
    private string? _capturing;
    private string? _captureNote;
    private string? _captureNoteFor;
    private int _openDropDowns;

    // A rebuild would also empty the key box under the user's fingers.
    protected override bool DeferRebuild => _capturing is not null || _openDropDowns > 0 || _keyFor is not null;

    private TextBlock? _suspendNote;

    /// <summary>End a capture: the hotkey hook is handed back on every exit path.</summary>
    private void EndCapture()
    {
        _capturing = null;
        Ctx.HotkeySuspend.End();
    }

    public override void OnHidden()
    {
        if (_capturing is not null) EndCapture();
        _tick?.Stop();
        _keyFor = null; // a half-typed key never survives leaving the page
    }

    protected override void UpdateInPlace()
    {
        // Known only after the first "on" came back: shown without a rebuild.
        if (_suspendNote is not null) _suspendNote.IsVisible = _capturing is not null && Ctx.HotkeySuspend.Unsupported;
        if (_elapsed is not null && Ctx.CatalogOp is { } op)
            _elapsed.Text = CatalogText.Elapsed(DateTime.Now - op.Started);
    }

    private IReadOnlyList<ComponentSection> Sections() =>
        Components.Build(Ctx.Feed.Snapshot, Ctx.Info?.Components, Ctx.Workspace, Ctx.Policies);

    /// <summary>The catalog the rows are joined with: the live one, else the last one read (stale), else the receipts.</summary>
    private CatalogSource Source() => CatalogSource.From(Ctx.CatalogRead, Ctx.CatalogLastGood);

    private bool CatalogLive => Ctx.CatalogRead is { Status: CatalogStatus.Ready };

    /// <summary>The runtime directory: the folder of the CLI this Dashboard runs (what its installer ships is in its "plugins" folder).</summary>
    private string? RuntimeDir => System.IO.Path.GetDirectoryName(Ctx.CliInfo.CliPath);

    /// <summary>The component's own row for an installed catalog item, when its class list has been read.</summary>
    private PolicyRow? RowOf(CatalogItem i) =>
        i.IsInstalled && i.PolicyClass is { } c && Ctx.Policies.TryGetValue(c, out var pl) ? pl.Row(i.ComponentId) : null;

    private LicenceView? LicenceOf(CatalogItem i) => RowOf(i) is { } row ? LicenceView.For(row, i.Class) : null;

    /// <summary>Asked to open one catalog item (<c>--catalog-item</c>): its row's detail, once the list is here.</summary>
    private void ConsumeFocus()
    {
        if (Ctx.CatalogFocus is not { } f || Ctx.CatalogRead is null) return;
        Ctx.CatalogFocus = null;
        var list = Source().List;
        var item = f.ItemId is not null ? list?.Find(f.ItemId)
            : f.Class is { } c && f.ComponentId is not null ? list?.FindComponent(c, f.ComponentId) : null;
        if (item is null) return;
        _filter = RowFilter.All;
        _open.Add(ComponentRows.KeyOf(item));
    }

    /// <summary>
    /// An install or update just ended well: the row it ran on is now the installed row, and it is
    /// opened. What the sections list is re-read too (the engine changed what is registered).
    /// </summary>
    private void NoteResult()
    {
        var outcome = Ctx.CatalogResult?.Outcome;
        if (ReferenceEquals(outcome, _resultSeen)) return;
        _resultSeen = outcome;
        if (outcome is not { Ok: true }) return;
        if (Ctx.CatalogResultVerb != CatalogVerb.Uninstall && Source().List?.Find(Ctx.CatalogResult!.Value.ItemId) is { } item)
            _open.Add(ComponentRows.KeyOf(item));
        // Posted: this runs inside Key(), and a load announces itself at once.
        Dispatcher.UIThread.Post(() =>
        {
            Ctx.Feed.Refresh();
            _ = Ctx.LoadWorkspaceAsync(force: true);
        });
    }

    protected override string Key()
    {
        ConsumeFocus();
        NoteResult();
        var sb = new StringBuilder();
        sb.Append(Ctx.InfoLoading).Append(Ctx.InfoError).Append(string.Join(",", _expanded)).Append('|')
          .Append(Ctx.ActionRunning).Append(_captureNoteFor).Append(_captureNote).Append('|');
        if (Ctx.LastActionArea == "workspace") sb.Append(Ctx.LastAction).Append(Ctx.LastActionFailed);
        foreach (var c in ComponentClasses.All)
        {
            sb.Append(Ctx.PolicyLoading(c)).Append(Ctx.PolicyUnsupported(c)).Append(Ctx.PolicyError(c));
            // The class-level policy facts the rows do not carry: the lock and the per-task preferences.
            if (Ctx.Policies.TryGetValue(c, out var pl))
                sb.Append(pl.PolicyLock).Append(string.Join(",", pl.PreferredByTask.Select(kv => kv.Key + "=" + kv.Value)));
            if (Ctx.LastActionArea == PolicyCommands.Area(c)) sb.Append(Ctx.LastAction).Append(Ctx.LastActionFailed).Append(_policyActionRow);
        }
        // ADR-054 N6 Part 2: the tracking card's per-screen viewer source (from the live feed).
        foreach (var t in TrackingScreens()) sb.Append(t.ScreenId).Append(t.Kind).Append(t.ProviderId).Append(t.Reason).Append(t.Policy);
        // #2181: the per-app table's inputs.
        if (Ctx.Policies.TryGetValue(ComponentClass.Input, out var ip))
        {
            foreach (var r in ip.PerAppRows()) sb.Append(r.App).Append(r.FamilyKey).Append(r.PinId).Append(r.HolderId).Append(r.Running);
            sb.Append(ip.ClientsDropped);
        }
        if (Ctx.LastActionArea == PolicyCommands.AppArea) sb.Append(Ctx.LastAction).Append(Ctx.LastActionFailed);
        sb.Append(_machine).Append('|');
        foreach (var s in Sections())
        {
            sb.Append(s.Id).Append(s.Count).Append(s.Connected).Append(s.ConnectedLine);
            foreach (var i in s.Items) sb.Append(i.Name).Append(i.Level).Append(i.State).Append(string.Join("/", i.Chips)).Append(string.Join("/", i.Lines)).Append(i.Controller?.Launch).Append(i.Policy);
            sb.Append(';');
        }
        // The catalog side: the read, what is in flight or just ended, and every item a row shows.
        var read = Ctx.CatalogRead;
        var src = Source();
        sb.Append('|').Append(Ctx.CatalogLoading).Append(read?.Status).Append(read?.Title).Append(src.Stale).Append(Ctx.CatalogLastGoodWhen)
          .Append(_filter).Append(string.Join(",", _open)).Append(_keyFor).Append('|');
        if (Ctx.CatalogOp is { } op) sb.Append(op.ItemId).Append(op.Verb).Append(op.Phase);
        sb.Append(Ctx.CatalogBackground?.ItemId);
        if (Ctx.CatalogResult is { } res) sb.Append(res.ItemId).Append(res.Outcome.Title);
        sb.Append(Ctx.ActivatingItem).Append(Ctx.BuyStarting).Append(Ctx.BuyPending?.ItemId);
        if (Ctx.BuyNote is { } bn) sb.Append(bn.ItemId).Append(bn.Plan.Kind).Append(bn.Plan.Title);
        if (Ctx.ActivationResult is { } ar) sb.Append(ar.ItemId).Append(ar.Outcome.Title);
        sb.Append('|');
        foreach (var i in src.List?.Items ?? Array.Empty<CatalogItem>())
        {
            sb.Append(i.Id).Append(i.State).Append(i.Installed).Append(i.Available).Append(i.Download).Append(i.Compatible)
              .Append(i.Detected).Append(i.Receipt);
            if (RowOf(i) is { } row)
                sb.Append(row.Licence).Append(row.LicenceSource).Append(row.LicenceExpires).Append(row.LicenceMessage)
                  .Append(row.LicencePurchaseUrl).Append(row.LicenceKeyActivation);
            sb.Append(';');
        }
        return sb.ToString();
    }

    protected override Control Build()
    {
        _openDropDowns = 0;
        _suspendNote = null;
        _elapsed = null;
        var page = U.VStack(18);
        if (Ctx.Info is null)
            page.Children.Add(U.Notice(Ctx.InfoError is null ? Level.Info : Level.Warn,
                Ctx.InfoError is null ? "Looking for your add-ons" : "Couldn't read the list of add-ons",
                Ctx.InfoError ?? "This takes about 10 seconds. Running apps appear as soon as DisplayXR reports them."));
        // A read in progress (the first one, Try again, Refresh, after an install) is said, never silent.
        if (CatalogText.Checking(Ctx.CatalogLoading) is { } checking)
        {
            var n = U.Notice(Level.Info, checking.Title, checking.Text);
            AutomationProperties.SetLiveSetting(n, AutomationLiveSetting.Polite);
            page.Children.Add(n);
        }
        if (CatalogBanner() is { } banner) page.Children.Add(banner);
        page.Children.Add(FilterBar());
        // An engine run the Dashboard stopped waiting for: say why the buttons are waiting.
        if (Ctx.CatalogBackground is { } bg) page.Children.Add(U.Notice(Level.Info, "Still working in the background", bg.Line));
        var groups = ComponentRows.Filter(ComponentRows.Join(Sections(), Source(), RuntimeDir), _filter);
        if (groups.Count == 0)
            page.Children.Add(U.Wrapped(_filter != RowFilter.Updates ? "Nothing to show."
                : CatalogLive ? "Everything is up to date." : "Updates are listed when DisplayXR can check for them.", "empty"));
        // One section per row, full width: a two-column grid stretched a short card beside a tall one.
        foreach (var g in groups) page.Children.Add(SectionCard(g));
        return page;
    }

    /// <summary>
    /// The catalog could not be read (offline, refused, no engine): said once, at the top, with what
    /// to do. Everything installed stays below and works as usual; rows from an earlier read stay
    /// too, marked as last known.
    /// </summary>
    private Control? CatalogBanner()
    {
        var read = Ctx.CatalogRead;
        if (read is null || read.Status == CatalogStatus.Ready) return null;
        var actions = new List<Control>();
        if (read.Status is CatalogStatus.NoEngineHere or CatalogStatus.EngineMissing)
            actions.Add(LinkButton("Open the Download page", CatalogText.DownloadPage, "outline", "sm"));
        else
        {
            var retry = U.Button(Ctx.CatalogLoading ? "Trying…" : "Try again", () => _ = Ctx.LoadCatalogAsync(force: true), "outline", "sm");
            retry.IsEnabled = !Ctx.CatalogLoading;
            actions.Add(retry);
        }
        var text = new StringBuilder(read.Text);
        if (Source().Stale && Ctx.CatalogLastGoodWhen is { } when) text.Append(' ').Append(CatalogSource.StaleNote(when));
        if (read.Detail is { Length: > 0 } detail) text.Append(' ').Append(detail).Append('.');
        var level = read.Status is CatalogStatus.Refused or CatalogStatus.Failed ? Level.Warn : Level.Info;
        var n = U.Notice(level, read.Title, text.ToString(), actions.ToArray());
        AutomationProperties.SetLiveSetting(n, AutomationLiveSetting.Polite);
        return n;
    }

    private Control FilterBar()
    {
        var bar = U.Flow(8, 6);
        foreach (var (label, filter) in new[] { ("All", RowFilter.All), ("Installed", RowFilter.Installed), ("Updates", RowFilter.Updates) })
        {
            bool on = _filter == filter;
            var b = U.Button(label, () => { _filter = filter; Update(force: true); }, on ? "primary" : "quiet", "sm");
            AutomationProperties.SetName(b, on ? $"{label}, selected filter" : $"Show {label}");
            U.AddFlow(bar, b, 8, 6);
        }
        return bar;
    }

    private Control SectionCard(ComponentGroup g)
    {
        var s = g.Section;
        var count = U.Badge(s.Count.ToString(System.Globalization.CultureInfo.InvariantCulture),
                            s.Connected > 0 ? Level.Ok : Level.Plain);
        var title = U.Flow(10, 4, U.Text(s.Title, "group"), count);
        var head = U.VStack(4, title, U.Wrapped(s.ConnectedLine, "dim"), U.Wrapped(s.Role, "desc"));
        var body = U.VStack(14, head);
        // ADR-054 N3e: HKLM Policy\Lock makes per-user writes of this class inert; say so once, on the class
        // (the technical line is in each row's Details and this notice's tooltip).
        if (PolicyList.ClassOfSection(s.Id) is { } sc && Ctx.Policies.TryGetValue(sc, out var spl) && spl.LockPlain is { } lockPlain)
        {
            var managed = U.Notice(Level.Info, PolicyList.ManagedTitle, lockPlain);
            ToolTip.SetTip(managed, spl.LockText);
            body.Children.Add(managed);
        }

        bool collapsed = s.Collapsed && !_expanded.Contains(s.Id);
        if (g.Rows.Count == 0)
        {
            body.Children.Add(U.Wrapped(EmptyText(s), "empty"));
        }
        else if (collapsed)
        {
            body.Children.Add(U.Button($"Show {StatusText.Plural(g.Rows.Count, "status reader", "status readers")}", () => Toggle(s.Id), "link"));
        }
        else
        {
            var list = U.VStack(0);
            for (int i = 0; i < g.Rows.Count; i++)
            {
                if (i > 0) list.Children.Add(new Border { Height = 12 });
                var row = g.Rows[i];
                // One row: the installed component (its state, its launch and policy controls), its
                // catalog entry (status, Install / Update, detail), or both. On the row itself: the
                // status, one plain state word, "Works with your display"; everything else is in Details.
                var face = row.Face;
                list.Children.Add(ItemRow(row.Installed ?? CatalogHead(row), face));
                if (row.Installed?.Controller is { Launch: not null } rc) list.Children.Add(LaunchRow(rc));
                var pr = row.Installed is { } inst ? inst.Policy : row.Catalog is { } only ? RowOf(only) : null;
                PolicyList? pl = null;
                bool writable = pr is not null && Ctx.Policies.TryGetValue(pr.Class, out pl) && pl.Writable;
                // ONE strip per row: Enabled, "Use this one", Install / Update and Details on one line, the details below it.
                var controls = writable ? PolicyControls(pr!, pl!) : null;
                if (RowStrip(row, face, writable ? PolicyNotes(pr!, pl!) : null, controls) is { } strip) list.Children.Add(strip);
            }
            body.Children.Add(list);
            if (s.Collapsed) body.Children.Add(U.Button("Hide", () => Toggle(s.Id), "link"));
        }
        // ADR-034 Am.5 (#2181): per-app pins + who holds each role per app, under Input.
        if (s.Id == ComponentClass.Input.SectionId() && Ctx.Policies.TryGetValue(ComponentClass.Input, out var ipl) && ipl.HasPerApp)
            body.Children.Add(PerAppTable(ipl));
        // ADR-054 N6 Part 2: per screen, whose eyes the views follow and the display's viewer policy.
        if (s.Id == ComponentClass.Tracking.SectionId())
            body.Children.Add(TrackingScreensTable());
        return U.Card(body);
    }

    private IReadOnlyList<TrackingScreen> TrackingScreens()
    {
        Ctx.Policies.TryGetValue(ComponentClass.Tracking, out var tl);
        return Tracking.Screens(Ctx.Feed.Snapshot, Ctx.Info?.Components.Tracking,
                                id => tl?.Row(id)?.Name is { Length: > 0 } n ? n : id);
    }

    /// <summary>
    /// ADR-054 N6 Part 2 (#2144): one line per screen — the viewer source (display / provider /
    /// nominal) and why, beside the display's viewer policy (OWN / EXTERNAL_OK / FIXED). The live
    /// feed supplies it; a headless snapshot covers the runtime's default screen only.
    /// </summary>
    private Control TrackingScreensTable()
    {
        var col = U.VStack(8, U.Text("Per display", "group"),
            U.Wrapped("Whose position the picture on each display follows. A display that tracks you itself always uses its own tracking; camera head tracking is used only where the display allows it.", "desc"));
        var rows = TrackingScreens();
        if (rows.Count == 0)
            col.Children.Add(U.Wrapped("No display reported yet.", "empty"));
        foreach (var r in rows)
        {
            var head = U.Flow(10, 6, U.Dot(r.Level), U.Text(r.Name, "value"));
            if (r.Primary) U.AddFlow(head, U.Chip("Default for 3D apps"), 10, 6);
            U.AddFlow(head, U.Chip(r.Source, r.Kind == ViewerSourceKind.Provider ? Level.Ok : Level.Plain), 10, 6);
            var (pt, pl) = r.PolicyChip;
            var policyChip = U.Chip(pt, pl);
            if (r.Policy is { Length: > 0 } rawPolicy) ToolTip.SetTip(policyChip, $"viewer policy {rawPolicy}");
            U.AddFlow(head, policyChip, 10, 6);
            if (r.CanCalibrate)
            {
                // Camera calibration lives on the display's own card on Displays.
                var calibrate = U.Button("Calibrate on Displays →", () => Ctx.Navigate("displays"), "link");
                ToolTip.SetTip(calibrate, "Calibrate camera… is on this display's card on Displays.");
                Avalonia.Automation.AutomationProperties.SetName(calibrate, $"Calibrate camera for {r.Name} on Displays");
                U.AddFlow(head, calibrate, 10, 6);
            }
            var reason = U.Wrapped(r.Reason, "soft");
            if (r.RawReason is { Length: > 0 } rawReason) ToolTip.SetTip(reason, rawReason);
            var block = U.VStack(4, head, reason);
            var box = new Border { Child = block, Margin = new Thickness(0, 6, 0, 0) };
            box.Classes.Add("field");
            col.Children.Add(box);
        }
        if (Ctx.Info?.Components.Tracking is { } sum && sum.ScanLine is { Length: > 0 } scan)
            col.Children.Add(U.Wrapped($"Last check of the installed camera head tracking: {scan}.", "meta"));
        return col;
    }

    /// <summary>
    /// ADR-034 Amendment 5 (#2181): the per-app table — each pinned or running app, per role
    /// family, its pin ("default" when none) and the provider that holds the family for it now
    /// (display name, from the running service). Prefer / Reset per app run
    /// <c>input use &lt;id&gt; --app &lt;exe&gt;</c> / <c>input reset --app &lt;exe&gt;</c> through the same
    /// action path as the other policy controls. Per user only: a per-app pin has no machine-wide
    /// tier (Policy\Lock is the machine's say, and disables the controls).
    /// </summary>
    private Control PerAppTable(PolicyList list)
    {
        const string area = PolicyCommands.AppArea;
        bool busy = Ctx.ActionRunning;
        bool locked = list.PolicyLock;
        var col = U.VStack(8, U.Text("Per app", "group"),
            U.Wrapped("Which input add-on each app uses. Choosing one for an app makes it use that add-on for everything the add-on supplies; anything else, or an add-on that is not there, uses the automatic order.", "desc"));
        if (list.PerAppLockText is { } lt) col.Children.Add(U.Notice(Level.Info, PolicyList.ManagedTitle, lt));
        if (list.ClientsDropped is > 0 and var dropped)
            col.Children.Add(U.Notice(Level.Warn, "Some apps use the automatic order",
                $"{dropped:0} app(s) went over the number DisplayXR keeps track of per app, so they use the automatic order and their choice does not apply."));
        var rows = list.PerAppRows();
        if (rows.Count == 0)
        {
            col.Children.Add(U.Wrapped(list.Holders is null
                ? "No app has a choice yet. Running apps are listed when DisplayXR runs in the background."
                : "No app has a choice yet, and no app is running through DisplayXR.", "empty"));
        }
        foreach (var app in rows.Select(r => r.App).Distinct())
        {
            var appRows = rows.Where(r => r.App == app).ToArray();
            string? pin = appRows[0].PinId;
            var head = U.Flow(10, 6, U.Text(app, "mono"), U.Chip(pin is null ? "Automatic" : $"Your choice: {appRows[0].Pin}", pin is null ? Level.Plain : Level.Info));
            if (!appRows[0].Running) U.AddFlow(head, U.Chip("not running", Level.Plain), 10, 6);

            void Run(CliArgs command) => _ = Ctx.RunActionsAsync(area, command);
            var choices = list.PinChoices();
            var pick = new ComboBox { ItemsSource = choices.Select(list.NameOf).ToArray(), SelectedIndex = pin is null ? -1 : Array.IndexOf(choices.ToArray(), pin), MinWidth = 200, IsEnabled = !busy && !locked };
            pick.Classes.Add("field");
            Avalonia.Automation.AutomationProperties.SetName(pick, $"Input add-on for {app}");
            pick.DropDownOpened += (_, _) => _openDropDowns++;
            pick.DropDownClosed += (_, _) =>
            {
                _openDropDowns = System.Math.Max(0, _openDropDowns - 1);
                Avalonia.Threading.Dispatcher.UIThread.Post(() => Update());
            };
            var prefer = U.Button("Use this one", () =>
            {
                if (pick.SelectedIndex >= 0 && pick.SelectedIndex < choices.Count) Run(PolicyCommands.PreferApp(choices[pick.SelectedIndex], app));
            }, "quiet", "sm");
            prefer.IsEnabled = !busy && !locked;
            ToolTip.SetTip(prefer, locked ? list.PerAppLockText : $"Use the chosen add-on for {app}. (displayxr-cli input use <id> --app {app})");
            Avalonia.Automation.AutomationProperties.SetName(prefer, $"Use this one: {app}");
            U.AddFlow(head, pick, 10, 6);
            U.AddFlow(head, prefer, 10, 6);
            if (pin is not null)
            {
                var reset = U.Button("Choose automatically", () => Run(PolicyCommands.ResetApp(app)), "quiet", "sm");
                reset.IsEnabled = !busy && !locked;
                ToolTip.SetTip(reset, locked ? list.PerAppLockText : $"Let DisplayXR pick for {app}. (displayxr-cli {PolicyCommands.ResetApp(app)})");
                Avalonia.Automation.AutomationProperties.SetName(reset, $"Choose automatically: {app}");
                U.AddFlow(head, reset, 10, 6);
            }
            var block = U.VStack(4, head);
            foreach (var r in appRows)
                block.Children.Add(U.Flow(10, 2, U.Text(r.Family, "dim"), U.Text("→", "dim"),
                                          U.Text(r.Holder, r.HolderId is null ? "dim" : "soft")));
            var box = new Border { Child = block, Margin = new Thickness(0, 6, 0, 0) };
            box.Classes.Add("field");
            col.Children.Add(box);
        }
        col.Children.Add(U.Wrapped("For you only. Applies at once to apps running through DisplayXR in the background; other apps the next time they start.", "meta"));
        if (Ctx.LastActionArea == area && Ctx.LastAction is { } la)
            col.Children.Add(Ctx.LastActionFailed ? U.WarningRow(new StatusWarning("POLICY", WarningLevel.Warn, la)) : U.Wrapped(la, "soft"));
        return col;
    }

    private static string EmptyText(ComponentSection s) => s.Id switch
    {
        "display_processors" => "No display driver is installed.",
        "workspace_controller" => "No spatial desktop is installed or running.",
        "present_owners" => "No app with its own window is running.",
        "input_providers" => "No input add-on reported.",
        "conversion" => "Your display driver reports no 2D-to-3D converter.",
        "conversion_providers" => "No 2D-to-3D converter is installed.",
        "tracking_providers" => "No camera head tracking is installed. A display that does not track you shows the picture for a viewer at the default position.",
        ComponentRows.AppsGroupId => "No app is installed.",
        ComponentRows.RuntimeGroupId => "DisplayXR is not installed.",
        _ => "None.",
    };

    private void Toggle(string id)
    {
        if (!_expanded.Remove(id)) _expanded.Add(id);
        Update();
    }

    /// <summary>
    /// How this workspace controller is launched (phase 8): hotkey capture,
    /// mode, Launch now. Written with `displayxr-cli workspace set|launch`, then
    /// `workspace list --json` is re-read once; the CLI makes the service reload,
    /// so a change is live.
    /// </summary>
    private Control LaunchRow(RegisteredController c)
    {
        var launch = c.Launch!;
        string id = c.Id;
        bool connected = c.Connected == true;
        bool busy = Ctx.ActionRunning;

        // (a) hotkey capture: click, press the combo, Esc cancels.
        bool capturing = _capturing == id;
        var keyText = U.Text(capturing ? "Press a key combination… (Esc cancels)"
                                       : launch.Hotkey is { Length: > 0 } hk ? hk : "No shortcut", capturing || launch.Hotkey is not { Length: > 0 } ? "dim" : "mono");
        keyText.VerticalAlignment = VerticalAlignment.Center;
        var capture = new Border
        {
            Child = keyText, Focusable = true, MinWidth = 220, Padding = new Thickness(12, 6),
            Cursor = new Avalonia.Input.Cursor(Avalonia.Input.StandardCursorType.Hand),
            BorderBrush = capturing ? Tokens.Accent : Tokens.Line, BorderThickness = new Thickness(capturing ? 2 : 1),
            Background = Tokens.Field, CornerRadius = Tokens.Radius("FieldRadius"), VerticalAlignment = VerticalAlignment.Center,
        };
        Avalonia.Automation.AutomationProperties.SetName(capture, $"Launch hotkey for {c.Name}");
        ToolTip.SetTip(capture, "Click, then press the combination: at least one of Ctrl / Shift / Alt / Win plus a key. "
                              + "The service sees it before any app, so avoid a combo your apps use (Ctrl+Space, for one).");
        capture.PointerPressed += (_, e) =>
        {
            if (busy) return;
            e.Handled = true;
            _capturing = id;
            _captureNote = null;
            Update(force: true);
        };
        if (capturing)
        {
            capture.GotFocus += (_, _) => Ctx.HotkeySuspend.Begin();
            capture.AttachedToVisualTree += (_, _) => Avalonia.Threading.Dispatcher.UIThread.Post(() => capture.Focus());
            capture.LostFocus += (_, _) =>
            {
                if (_capturing != id) return;
                EndCapture();
                Avalonia.Threading.Dispatcher.UIThread.Post(() => Update(force: true));
            };
            capture.AddHandler(InputElement.KeyDownEvent, (_, e) =>
            {
                e.Handled = true;
                if (e.Key == Avalonia.Input.Key.Escape && e.KeyModifiers == Avalonia.Input.KeyModifiers.None)
                {
                    EndCapture();
                    Update(force: true);
                    return;
                }
                if (KeyNames.IsModifier(e.Key)) return; // wait for the key itself
                var mods = KeyNames.Modifiers(e.KeyModifiers);
                string? name = KeyNames.Name(e.Key);
                string? why = name is null ? "That key cannot be used." : Hotkey.Validate(mods, name);
                EndCapture();
                _captureNoteFor = id;
                if (why is not null)
                {
                    _captureNote = why;
                    Update(force: true);
                    return;
                }
                var combo = new Hotkey(mods, name!).ToString();
                _captureNote = null;
                _ = Ctx.RunActionsAsync("workspace", new CliArgs("workspace", "set", id, "--hotkey", combo));
                Update(force: true);
            }, Avalonia.Interactivity.RoutingStrategies.Tunnel);
        }

        var clear = U.Button("Clear", () => _ = Ctx.RunActionsAsync("workspace", new CliArgs("workspace", "set", id, "--no-hotkey")), "quiet", "sm");
        clear.IsEnabled = !busy && launch.Hotkey is { Length: > 0 };

        // Back to the runtime's defaults: `workspace reset` drops the controller's
        // service.json entry, so it follows the default again (an upgraded box that
        // stored Ctrl+Space gets Ctrl+Shift+Space this way).
        var reset = U.Button("Reset", () => _ = Ctx.RunActionsAsync("workspace", new CliArgs("workspace", "reset", id)), "quiet", "sm");
        reset.IsEnabled = !busy && launch.Source == "user";
        ToolTip.SetTip(reset, $"Back to the runtime's defaults ({Hotkey.Default}, Auto on current runtimes).");

        // (b) mode.
        var mode = new ComboBox { ItemsSource = new[] { "Opens with the shortcut", "Turned off" }, SelectedIndex = launch.Disabled ? 1 : 0, MinWidth = 190, IsEnabled = !busy };
        mode.Classes.Add("field");
        Avalonia.Automation.AutomationProperties.SetName(mode, $"Launch mode for {c.Name}");
        int modeWas = mode.SelectedIndex;
        mode.DropDownOpened += (_, _) => _openDropDowns++;
        mode.DropDownClosed += (_, _) =>
        {
            _openDropDowns = System.Math.Max(0, _openDropDowns - 1);
            Avalonia.Threading.Dispatcher.UIThread.Post(() => Update());
        };
        mode.SelectionChanged += (_, _) =>
        {
            if (mode.SelectedIndex < 0 || mode.SelectedIndex == modeWas || Ctx.ActionRunning) return;
            _ = Ctx.RunActionsAsync("workspace", new CliArgs("workspace", "set", id, "--mode", mode.SelectedIndex == 1 ? "disabled" : "auto"));
        };

        // (c) launch now.
        var launchNow = U.Button("Open now", () => _ = Ctx.RunActionsAsync("workspace", new CliArgs("workspace", "launch", id)), "primary", "sm");
        launchNow.IsEnabled = !busy && !launch.Disabled && !connected;
        ToolTip.SetTip(launchNow, launch.Disabled ? "It is turned off." : connected ? "Already running." : "Open it now, the same way the shortcut does.");

        var label = U.Text("Shortcut", "label");
        label.VerticalAlignment = VerticalAlignment.Center;
        var row = U.Flow(10, 6, label, capture, clear, reset, mode, launchNow);

        // (d) when a change applies. Its state (running, where the shortcut is set) is in the row's Details.
        var note = U.Text("Changes take effect immediately.", "meta");

        var col = U.VStack(6, row, note);
        if (capturing)
        {
            _suspendNote = U.Wrapped("Press a combo other than the current one while the workspace hotkey is active.", "soft");
            _suspendNote.IsVisible = Ctx.HotkeySuspend.Unsupported;
            col.Children.Add(_suspendNote);
        }
        if (_captureNoteFor == id && _captureNote is { } cn)
            col.Children.Add(U.WarningRow(new StatusWarning("HOTKEY", WarningLevel.Warn, cn)));
        if (Ctx.LastActionArea == "workspace" && Ctx.LastAction is { } la && Ctx.LastActionFailed)
            col.Children.Add(U.Wrapped(la, "soft"));
        var box = new Border { Child = col, Margin = new Thickness(22, 10, 0, 0) };
        box.Classes.Add("field");
        return box;
    }

    /// <summary>
    /// ADR-054 N3e: Enabled and Preferred for one display processor / input provider /
    /// conversion provider. Every change is a `displayxr-cli` write (logged, visible in the
    /// log pane); the row is then re-read from `&lt;class&gt; list --json`. Per user unless the
    /// dashboard runs elevated and "Machine-wide" is ticked; a non-elevated dashboard shows the
    /// `--machine` command for an administrator instead of attempting it (in the row's Details,
    /// <see cref="PolicyNotes"/>). The controls go on the row's one strip (<see cref="RowStrip"/>), with the
    /// reply to the last change made here under them.
    /// </summary>
    private sealed record PolicyStrip(IReadOnlyList<Control> Controls, Control? Reply);

    private PolicyStrip PolicyControls(PolicyRow r, PolicyList list)
    {
        var cls = r.Class;
        string area = PolicyCommands.Area(cls);
        bool busy = Ctx.ActionRunning;
        bool elevated = Elevation.IsElevated;
        bool machine = _machine && elevated;
        var row = new List<Control>();

        void Run(CliArgs command)
        {
            _policyActionRow = r.Id;
            _ = Ctx.RunActionsAsync(area, command);
        }

        // Policy\Lock: a per-user write would be ignored, so the per-user controls are inert
        // (a machine-wide write from an elevated dashboard still applies).
        bool enabledLocked = list.LockHidesUserEnabled && !machine;
        bool prefLocked = list.LockHidesUserPreference && !machine;
        const string lockTip = "Managed by your administrator: a change made here for you would be ignored. (HKLM Policy\\Lock; an administrator changes the machine-wide value with --machine.)";

        // (a) Enabled — never on the built-in adapter, which has no Enabled value (EnabledKnown).
        if (r.EnabledKnown)
        {
            var enabled = new CheckBox { Content = "Enabled", IsChecked = r.Enabled, IsEnabled = !busy && !r.Untrusted && !enabledLocked, VerticalAlignment = VerticalAlignment.Center };
            Avalonia.Automation.AutomationProperties.SetName(enabled, $"Enabled: {r.Name}");
            ToolTip.SetTip(enabled, r.Untrusted
                ? "Blocked: publisher not verified. An administrator can approve it on this PC; the DisplayXR log says how (provenance gate, allowlist)."
                : enabledLocked ? lockTip
                : $"displayxr-cli {PolicyCommands.Toggle(cls, r.Id, r.Enabled, machine)}");
            enabled.IsCheckedChanged += (_, _) =>
            {
                if (Ctx.ActionRunning) return;
                bool on = enabled.IsChecked == true;
                if (on == r.Enabled) return;
                Run(on ? PolicyCommands.Enable(cls, r.Id, machine) : PolicyCommands.Disable(cls, r.Id, machine));
            };
            row.Add(enabled);
        }

        // (b) Preferred: global for display / input, per task for conversion.
        if (cls != ComponentClass.Conversion)
        {
            // Tracking: Policy\Lock also hides the per-user PreferredPlugin (display / input: never locked).
            if (r.Preferred)
            {
                row.Add(U.Chip("Your choice", Level.Info));
                var reset = U.Button("Choose automatically", () => Run(PolicyCommands.Reset(cls, machine)), "quiet", "sm");
                reset.IsEnabled = !busy && !prefLocked;
                ToolTip.SetTip(reset, prefLocked ? lockTip : $"Let DisplayXR pick again. (displayxr-cli {PolicyCommands.Reset(cls, machine)})");
                AutomationProperties.SetName(reset, $"Choose automatically: {r.Name}");
                row.Add(reset);
            }
            else
            {
                var prefer = U.Button("Use this one", () => Run(PolicyCommands.Prefer(cls, r.Id, machine)), "quiet", "sm");
                prefer.IsEnabled = !busy && r.Enabled && !r.Untrusted && !prefLocked;
                ToolTip.SetTip(prefer, prefLocked ? lockTip : $"Use this {cls.Noun()} first whenever it can work. (displayxr-cli {PolicyCommands.Prefer(cls, r.Id, machine)})");
                AutomationProperties.SetName(prefer, $"Use this one: {r.Name}");
                row.Add(prefer);
            }
        }
        else
        {
            foreach (var task in list.TaskNamesOf(r.Id))
            {
                if (list.IsPreferredFor(task, r.Id))
                {
                    row.Add(U.Chip($"Your choice for {task}", Level.Info));
                    var reset = U.Button($"Choose {task} automatically", () => Run(PolicyCommands.ResetTask(task, machine)), "quiet", "sm");
                    reset.IsEnabled = !busy && !prefLocked;
                    ToolTip.SetTip(reset, prefLocked ? lockTip : $"Let DisplayXR pick again. (displayxr-cli {PolicyCommands.ResetTask(task, machine)})");
                    AutomationProperties.SetName(reset, $"Choose {task} automatically: {r.Name}");
                    row.Add(reset);
                }
                else if (!r.BuiltIn) // the built-in adapter is never a provider to prefer
                {
                    var prefer = U.Button($"Use this one for {task}", () => Run(PolicyCommands.PreferTask(task, r.Id, machine)), "quiet", "sm");
                    prefer.IsEnabled = !busy && r.Enabled && !r.Untrusted && !prefLocked;
                    ToolTip.SetTip(prefer, prefLocked ? lockTip : $"Use this {cls.Noun()} first for {task}. (displayxr-cli {PolicyCommands.PreferTask(task, r.Id, machine)})");
                    AutomationProperties.SetName(prefer, $"Use this one for {task}: {r.Name}");
                    row.Add(prefer);
                }
            }
        }

        // (c) scope: machine-wide only when elevated (the dashboard runs non-elevated by design).
        if (elevated)
        {
            var mw = new CheckBox { Content = "For everyone on this PC", IsChecked = _machine, IsEnabled = !busy, VerticalAlignment = VerticalAlignment.Center };
            ToolTip.SetTip(mw, "Change the setting for everyone on this PC (the machine-wide HKLM value) instead of only for you. Offered because the Dashboard is running as administrator.");
            mw.IsCheckedChanged += (_, _) => { _machine = mw.IsChecked == true; Update(force: true); };
            row.Add(mw);
        }

        // (d) the reply to the last change made here. What a change means, and how an administrator
        // does it machine-wide, is in the row's Details (PolicyNotes).
        Control? reply = null;
        if (Ctx.LastActionArea == area && _policyActionRow == r.Id && Ctx.LastAction is { } la)
            reply = Ctx.LastActionFailed ? U.WarningRow(new StatusWarning("POLICY", WarningLevel.Warn, la)) : U.Wrapped(la, "soft");
        return new PolicyStrip(row, reply);
    }

    /// <summary>
    /// The policy lines of a row's Details: the built-in note, per user or machine-wide and when it
    /// applies, and (non-elevated) every control's machine-wide command for an administrator.
    /// </summary>
    private IReadOnlyList<(string Text, bool Command)> PolicyNotes(PolicyRow r, PolicyList list)
    {
        bool elevated = Elevation.IsElevated;
        return PolicyCommands.Notes(r, list, _machine && elevated, elevated);
    }

    /// <summary>
    /// The row's head: its name, status, at most one plain state word, "Works with your display", and one plain
    /// line under the name (publisher · version, what it drives). Everything else is in Details.
    /// </summary>
    private static Control ItemRow(ComponentItem item, RowFace face)
    {
        var dot = U.Dot(item.Level);
        dot.VerticalAlignment = VerticalAlignment.Top;
        dot.Margin = new Thickness(0, 6, 12, 0);
        var name = U.Wrapped(item.Name, "value");
        name.FontWeight = FontWeight.SemiBold;
        var head = U.Flow(8, 4, name);
        if (face.Status is { } st) U.AddFlow(head, U.Chip(st.Text, st.Level), 6, 4);
        if (face.State is { } state) U.AddFlow(head, U.Chip(state.Text, state.Level), 6, 4);
        if (face.WorksWithDisplay) U.AddFlow(head, U.Chip("Works with your display", Level.Ok), 6, 4);
        var col = U.VStack(2, head);
        if (item.Summary is { Length: > 0 } summary) col.Children.Add(U.Wrapped(summary, "soft"));
        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
        g.Children.Add(dot);
        Grid.SetColumn(col, 1);
        g.Children.Add(col);
        return g;
    }

    // ── The catalog side of a row (ADR-064 step 8) ──────────────────────────

    /// <summary>The head of a row with no installed component beside it: the catalog's name, what it does, who makes it.</summary>
    private static ComponentItem CatalogHead(ComponentRow row)
    {
        var i = row.Catalog!;
        var lines = new List<string> { CatalogText.About(i) };
        if (i.Publisher.Length > 0) lines.Add($"By {i.Publisher}");
        // The face line is what it does; the publisher is in Details.
        return new ComponentItem(i.Name, row.Status == RowStatus.Available ? Level.Plain : Level.Ok,
                                 Array.Empty<(string, Level)>(), lines, Summary: CatalogText.About(i));
    }

    /// <summary>
    /// Under a row: what the catalog can do for it (Install / Update), what is happening to it now or
    /// how the last thing ended, its Details toggle, and, for a row that cannot be removed from here,
    /// how it is removed. Details holds the row's labelled state lines (the chips the row no longer
    /// carries), what a policy change means and its machine-wide commands, and the catalog's detail.
    /// Any row with one of those has the toggle, catalog row or not.
    /// </summary>
    private Control? RowStrip(ComponentRow row, RowFace face, IReadOnlyList<(string Text, bool Command)>? notes, PolicyStrip? policy)
    {
        var col = U.VStack(8);
        var catalog = row.Catalog is { } ci && row.HasDetail ? ci : null;
        // The licence block of the catalog detail says the licence already: not twice.
        bool licenceBlock = catalog is not null && catalog.IsInstalled && LicenceOf(catalog) is not null;
        var facts = face.Details.Where(f => !(licenceBlock && f.Label == "Licence")).ToArray();
        notes ??= Array.Empty<(string, bool)>();
        bool open = _open.Contains(row.Key);
        if (catalog is { } i)
        {
            if (!i.Compatible) col.Children.Add(U.Wrapped(open ? CompatibilityLine(i) : CatalogText.OtherVersion, "meta"));
            else if (i.Unmet.FirstOrDefault(p => !p.Installable) is { } need && !i.IsInstalled)
                col.Children.Add(U.Wrapped($"Works together with {need.Name}.", "meta"));
            if (Progress(i) is { } progress) col.Children.Add(progress);
        }
        var actions = U.Flow(10, 6);
        foreach (var c in policy?.Controls ?? Array.Empty<Control>()) U.AddFlow(actions, c, 10, 6);
        if (catalog is not null && MainButton(row, catalog) is { } main) U.AddFlow(actions, main, 10, 6);
        if (catalog is not null || facts.Length > 0 || notes.Count > 0 || row.RemoveNote is not null)
        {
            // The accessible name keeps the catalog's name where the row has a catalog detail, as before.
            string name = catalog?.Name ?? row.Name;
            var details = U.Button(open ? "Hide details" : "Details", () => ToggleRow(row.Key), "quiet", "sm");
            AutomationProperties.SetName(details, $"{(open ? "Hide details" : "Details")} of {name}");
            U.AddFlow(actions, details, 10, 6);
        }
        if (actions.Children.Count > 0) col.Children.Add(actions);
        if (policy?.Reply is { } reply) col.Children.Add(reply);
        if (open)
        {
            var detail = U.VStack(14);
            if (facts.Length > 0)
            {
                var kv = new U.KvList(130);
                foreach (var (label, text) in facts) kv.Add(label, text);
                detail.Children.Add(kv);
            }
            if (notes.Count > 0)
            {
                var policyLines = U.VStack(6);
                foreach (var (text, command) in notes) policyLines.Children.Add(U.Wrapped(text, command ? "mono" : "meta"));
                detail.Children.Add(policyLines);
            }
            if (catalog is not null) detail.Children.Add(Detail(row, catalog));
            // How a row that cannot be removed from here is removed: the last line of Details.
            if (row.RemoveNote is { } note) detail.Children.Add(U.Wrapped(note, "meta"));
            if (detail.Children.Count > 0) col.Children.Add(detail);
        }
        if (col.Children.Count == 0) return null;
        var box = new Border { Child = col, Margin = new Thickness(22, 8, 0, 0) };
        box.Classes.Add("field");
        return box;
    }

    private void ToggleRow(string key)
    {
        if (!_open.Remove(key)) _open.Add(key);
        _keyFor = null;
        Update(force: true);
    }

    /// <summary>The detail adds the engine's own reason to the calm line the row shows.</summary>
    private static string CompatibilityLine(CatalogItem i) =>
        i.IncompatibleReason is { Length: > 0 } why ? $"{CatalogText.OtherVersion} ({why})" : CatalogText.OtherVersion;

    private bool Busy => Ctx.CatalogBusy;

    private Button? MainButton(ComponentRow row, CatalogItem i)
    {
        var action = row.Action;
        Button? b = action switch
        {
            CatalogAction.Install => U.Button("Install", () => _ = Ctx.RunCatalogAsync(CatalogVerb.Install, i), "primary", "sm"),
            CatalogAction.Update => U.Button("Update", () => _ = Ctx.RunCatalogAsync(CatalogVerb.Update, i), "primary", "sm"),
            CatalogAction.OpenListing => LinkButton("Get it", i.Listing!, "primary", "sm"),
            _ => i.IsRuntime && i.HasUpdate ? LinkButton("Get the update", CatalogText.DownloadPage, "primary", "sm") : null,
        };
        if (b is null) return null;
        if (action is CatalogAction.Install or CatalogAction.Update)
        {
            b.IsEnabled = !Busy;
            ToolTip.SetShowOnDisabled(b, true);
            string size = i.SizeText is { } s ? $" ({s})" : "";
            ToolTip.SetTip(b, Ctx.CatalogBackground is { } bg ? bg.Line
                : Busy ? "One thing at a time: this starts when the current one is done."
                : $"Downloads{size}, checks the publisher's signature and installs. Windows asks for permission once.");
            AutomationProperties.SetName(b, $"{(action == CatalogAction.Update ? "Update" : "Install")} {i.Name}");
        }
        else AutomationProperties.SetName(b, $"Get {i.Name}: opens its page in your browser");
        return b;
    }

    /// <summary>Uninstall takes it off the machine (the engine's receipt). Turning it off is the Enabled box above, a different thing.</summary>
    private Button UninstallButton(CatalogItem i)
    {
        var b = U.Button("Uninstall", () => _ = Ctx.RunCatalogAsync(CatalogVerb.Uninstall, i), "quiet", "sm");
        b.IsEnabled = !Busy;
        ToolTip.SetShowOnDisabled(b, true);
        ToolTip.SetTip(b, Ctx.CatalogBackground is { } bg ? bg.Line : "Removes it from this machine. Windows asks for permission once.");
        AutomationProperties.SetName(b, $"Uninstall {i.Name}");
        return b;
    }

    /// <summary>A button that opens an https page in the default browser (the URL is checked again when clicked).</summary>
    private Button LinkButton(string label, string url, params string[] classes)
    {
        var b = U.Button(label, () => { if (!UrlLauncher.Open(url)) Ctx.Toast("Could not open the browser"); }, classes);
        if (SafeUrl.TryHttps(url, out var u)) ToolTip.SetTip(b, $"Opens {u!.Host} in your browser");
        else b.IsEnabled = false;
        return b;
    }

    /// <summary>What is happening to this item now, or how the last thing ended, on its own row. Never both, never a stale spinner.</summary>
    private Control? Progress(CatalogItem i)
    {
        if (Ctx.CatalogOp is { } op && op.ItemId == i.Id)
        {
            _elapsed = U.Text(CatalogText.Elapsed(DateTime.Now - op.Started), "meta");
            _elapsed.VerticalAlignment = VerticalAlignment.Center;
            var title = U.Text(op.Title, "value");
            title.FontWeight = FontWeight.SemiBold;
            var bar = new ProgressBar { IsIndeterminate = true, Height = 4 };
            AutomationProperties.SetName(bar, op.Title);
            var stop = U.Button("Stop waiting", Ctx.StopWaitingForCatalog, "quiet", "sm");
            ToolTip.SetTip(stop, "Stops the Dashboard waiting, not the work: once it has started it finishes on its own, even if the Dashboard is closed, so nothing is left half done. Other installs wait until it is.");
            AutomationProperties.SetName(stop, $"Stop waiting for {i.Name}");
            var col = U.VStack(8, U.Flow(10, 2, title, _elapsed), bar, U.Wrapped(op.Phase, "soft"), stop);
            var box = new Border { Child = col };
            box.Classes.Add("field");
            AutomationProperties.SetLiveSetting(box, AutomationLiveSetting.Polite);
            return box;
        }
        if (Ctx.CatalogBackground is { } bg && bg.ItemId == i.Id)
        {
            var bar = new ProgressBar { IsIndeterminate = true, Height = 4 };
            AutomationProperties.SetName(bar, bg.Line);
            var box = new Border { Child = U.VStack(8, bar, U.Wrapped(bg.Line, "soft")) };
            box.Classes.Add("field");
            AutomationProperties.SetLiveSetting(box, AutomationLiveSetting.Polite);
            return box;
        }
        if (Ctx.CatalogResult is { } res && res.ItemId == i.Id)
        {
            var o = res.Outcome;
            if (o.Ok)
            {
                // A success needs no click: the row's new status says it, with one line that stays
                // until the next action. Only a failure is a message to read and dismiss.
                var done = U.Wrapped(CatalogText.Inline(o.Title, o.Text), "soft");
                AutomationProperties.SetLiveSetting(done, AutomationLiveSetting.Polite);
                return done;
            }
            var n = U.Notice(o.Level, o.Title, o.Text, U.Button("OK", Ctx.DismissCatalogResult, "quiet", "sm"));
            AutomationProperties.SetLiveSetting(n, AutomationLiveSetting.Polite);
            return n;
        }
        return null;
    }

    /// <summary>The open row: what it does, version and size, what it works together with, its licence, Uninstall.</summary>
    private Control Detail(ComponentRow row, CatalogItem i)
    {
        var col = U.VStack(14);
        // An installed row's head is the component's own state; what the item does is said here.
        if (row.Installed is not null) col.Children.Add(U.Wrapped(CatalogText.About(i), "soft"));
        if (i.DetectedHere)
            col.Children.Add(U.Wrapped(CatalogText.DetectedLine, "soft")); // never the engine's raw evidence

        var facts = new U.KvList(130);
        if (i.VersionText.Length > 0) facts.Add("Version", i.VersionText);
        if (i.SizeText is { } size) facts.Add(i.IsInstalled && !i.HasUpdate ? "Size" : "Download", size);
        if (i.Publisher.Length > 0) facts.Add("Publisher", i.Publisher);
        facts.Add("Category", CatalogClasses.Label(i.Class));
        col.Children.Add(facts);

        if (i.Prerequisites.Count > 0) col.Children.Add(Prerequisites(i));
        if (Licence(i) is { } licence) col.Children.Add(licence);
        if (row.CanUninstall) col.Children.Add(U.Flow(10, 6, UninstallButton(i)));
        if (i.IsRuntime)
            col.Children.Add(U.Wrapped("DisplayXR itself is updated from the Download page, so the Dashboard can close while it installs.", "meta"));
        return col;
    }

    private static Control Prerequisites(CatalogItem i)
    {
        var col = U.VStack(8, U.Text("Works together with", "group"));
        foreach (var p in i.Prerequisites)
        {
            var row = U.Flow(10, 4, U.Text(p.Name, "value"),
                             U.Chip(p.Met ? "Ready" : p.Installable ? "Included" : "Get it first", p.Met ? Level.Ok : Level.Info));
            var block = U.VStack(4, row, U.Wrapped(p.NextStep, "soft"));
            if (!p.Met && !p.Installable && SafeUrl.TryHttps(p.InfoUrl, out var u))
            {
                var get = U.Button($"Get {p.Name}", () => UrlLauncher.Open(u!.AbsoluteUri), "outline", "sm");
                ToolTip.SetTip(get, $"Opens {u!.Host} in your browser");
                block.Children.Add(get);
            }
            var box = new Border { Child = block };
            box.Classes.Add("field");
            col.Children.Add(box);
        }
        return col;
    }

    // ── Licence (ADR-064 §5) ────────────────────────────────────────────────

    private Control? Licence(CatalogItem i)
    {
        if (!i.IsInstalled || i.PolicyClass is not { } cls) return null;
        var row = RowOf(i);
        if (row is null)
            return Ctx.PolicyLoading(cls) ? U.VStack(6, U.Text("Licence", "group"), U.Wrapped("Asking it about its licence…", "meta")) : null;
        if (LicenceView.For(row, i.Class) is not { } v) return null;

        var col = U.VStack(8, U.Text("Licence", "group"), U.Flow(10, 4, U.Chip(v.Headline, v.Level == Level.Warn ? Level.Info : v.Level)));
        if (v.Message is not null) col.Children.Add(U.Wrapped(v.Message, "soft"));

        bool activating = Ctx.ActivatingItem == i.Id;
        bool offerBuy = v.OfferBuy && !Ctx.NoBuy(i.Id);
        if (offerBuy || v.OfferKey)
        {
            var actions = U.Flow(10, 6);
            if (offerBuy)
            {
                bool starting = Ctx.BuyStarting == i.Id;
                var buy = U.Button(starting ? "Opening…" : "Buy", () => _ = Ctx.BuyAsync(i, cls, v.PurchaseUrl, row.LicenceDeepLink), "primary", "sm");
                buy.IsEnabled = Ctx.BuyStarting is null;
                ToolTip.SetTip(buy, $"Opens {v.PurchaseUrl!.Host} in your browser. You buy from {(i.Publisher.Length > 0 ? i.Publisher : "its maker")}; DisplayXR is not part of the purchase.");
                AutomationProperties.SetName(buy, $"Buy {i.Name}: opens {v.PurchaseUrl.Host} in your browser");
                U.AddFlow(actions, buy, 10, 6);
            }
            if (v.OfferKey && _keyFor != i.Id)
            {
                var paste = U.Button("Paste a key", () => { _keyFor = i.Id; Update(force: true); }, offerBuy ? "quiet" : "primary", "sm");
                paste.IsEnabled = !activating;
                AutomationProperties.SetName(paste, $"Paste a licence key for {i.Name}");
                U.AddFlow(actions, paste, 10, 6);
            }
            if (actions.Children.Count > 0) col.Children.Add(actions);
        }
        if (Ctx.BuyNote is { } note && note.ItemId == i.Id)
        {
            var n = U.Notice(Level.Info, note.Plan.Title, note.Plan.Text, U.Button("OK", Ctx.DismissBuyNote, "quiet", "sm"));
            AutomationProperties.SetLiveSetting(n, AutomationLiveSetting.Polite);
            col.Children.Add(n);
        }
        if (v.OfferKey && _keyFor == i.Id) col.Children.Add(KeyEntry(i, cls));
        if (activating)
        {
            var bar = new ProgressBar { IsIndeterminate = true, Height = 4 };
            AutomationProperties.SetName(bar, $"Activating {i.Name}");
            col.Children.Add(U.VStack(6, bar, U.Wrapped($"Handing your key to {i.Name}…", "soft")));
        }
        if (Ctx.ActivationResult is { } ar && ar.ItemId == i.Id)
        {
            if (ar.Outcome.Accepted)
            {
                // Accepted: said in one line, nothing to dismiss (the licence chip above has changed).
                var done = U.Wrapped(CatalogText.Inline(ar.Outcome.Title, ar.Outcome.Text), "soft");
                AutomationProperties.SetLiveSetting(done, AutomationLiveSetting.Polite);
                col.Children.Add(done);
            }
            else
            {
                var n = U.Notice(Level.Info, ar.Outcome.Title, ar.Outcome.Text, U.Button("OK", Ctx.DismissCatalogResult, "quiet", "sm"));
                AutomationProperties.SetLiveSetting(n, AutomationLiveSetting.Polite);
                col.Children.Add(n);
            }
        }
        if (v.Reassurance is not null) col.Children.Add(U.Wrapped(v.Reassurance, "meta"));
        return col;
    }

    /// <summary>
    /// The masked key box. The key goes from here to <see cref="DashboardContext.ActivateAsync"/> and
    /// nowhere else: not into a field of the page, not into a tooltip, not into the log.
    /// </summary>
    private Control KeyEntry(CatalogItem i, ComponentClass cls)
    {
        var box = new TextBox
        {
            PasswordChar = '•', Watermark = "Paste your key", Width = 340, MinHeight = 32, FontSize = 13,
            Background = Tokens.Field, BorderBrush = Tokens.Line, Foreground = Tokens.Fg,
            CornerRadius = Tokens.Radius("FieldRadius"), Padding = new Thickness(10, 6), AcceptsReturn = false,
        };
        AutomationProperties.SetName(box, $"Licence key for {i.Name}");

        void Close()
        {
            box.Text = "";
            _keyFor = null;
            Update(force: true);
        }
        void Submit()
        {
            string key = box.Text ?? "";
            if (key.Trim().Length == 0) return;
            Close(); // the box is emptied before anything else happens
            _ = Ctx.ActivateAsync(i, cls, key);
        }
        var activate = U.Button("Activate", Submit, "primary", "sm");
        activate.IsEnabled = false;
        box.TextChanged += (_, _) => activate.IsEnabled = !string.IsNullOrWhiteSpace(box.Text);
        box.KeyDown += (_, e) =>
        {
            if (e.Key == Avalonia.Input.Key.Enter) { e.Handled = true; Submit(); }
            else if (e.Key == Avalonia.Input.Key.Escape) { e.Handled = true; Close(); }
        };
        box.AttachedToVisualTree += (_, _) => Dispatcher.UIThread.Post(() => box.Focus());
        var cancel = U.Button("Cancel", Close, "quiet", "sm");
        var row = U.Flow(10, 6, box, activate, cancel);
        var note = U.Wrapped($"Your key goes straight to {i.Name}. DisplayXR does not keep it or write it to any log. A licence key belongs in this box, never in a link.", "meta");
        var wrap = new Border { Child = U.VStack(8, row, note) };
        wrap.Classes.Add("field");
        return wrap;
    }
}

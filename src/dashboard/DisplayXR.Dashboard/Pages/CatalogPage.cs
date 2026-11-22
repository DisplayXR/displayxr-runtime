// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Avalonia;
using Avalonia.Automation;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using Avalonia.Media;
using Avalonia.Threading;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;
using DisplayXR.Dashboard.Ui;

namespace DisplayXR.Dashboard.Pages;

/// <summary>
/// Catalog (ADR-064 step 8): what can be added to DisplayXR, grouped by what it is for.
/// Browse, an item's detail, Install / Update / Uninstall through the install engine (one
/// Windows permission prompt, the Dashboard itself stays non-elevated), and the uniform
/// licence block for an installed item. Everything it shows is <c>dxr-get list --json</c>
/// joined with the component's own licence from <c>&lt;class&gt; list --json</c>; the words and
/// the rules are <see cref="Model.CatalogItem"/> and friends, this only lays them out.
/// </summary>
public sealed class CatalogPage : Page
{
    private string _query = "";
    private string? _class;          // null: every class
    private bool _installedOnly;
    private string? _detail;         // the item whose detail is open
    private string? _keyFor;         // the item whose key box is open
    private TextBox? _keyBox;
    private TextBox? _search;
    private Border? _results;
    private TextBlock? _elapsed;
    private DispatcherTimer? _tick;

    public CatalogPage() : base("catalog", "Catalog") { }

    public override bool HoldsFeed => false;

    public override string Caption => "Add-ons and apps for your display. One click to install.";

    // A rebuild would empty the key box under the user's fingers.
    protected override bool DeferRebuild => _keyFor is not null;

    public override void OnShown()
    {
        _ = Ctx.LoadCatalogAsync();
        _tick ??= new DispatcherTimer(TimeSpan.FromSeconds(1), DispatcherPriority.Background, (_, _) => UpdateInPlace());
        _tick.Start();
    }

    public override void OnHidden()
    {
        _tick?.Stop();
        _keyFor = null; // a half-typed key never survives leaving the page
        _keyBox = null;
    }

    protected override void UpdateInPlace()
    {
        if (_elapsed is not null && Ctx.CatalogOp is { } op)
            _elapsed.Text = CatalogText.Elapsed(DateTime.Now - op.Started);
    }

    private CatalogList? List => Ctx.CatalogRead is { Status: CatalogStatus.Ready } r ? r.List : null;

    /// <summary>The component's own row for an installed item, when its class list has been read.</summary>
    private PolicyRow? RowOf(CatalogItem i) =>
        i.IsInstalled && i.PolicyClass is { } c && Ctx.Policies.TryGetValue(c, out var pl) ? pl.Row(i.ComponentId) : null;

    private LicenceView? LicenceOf(CatalogItem i) => RowOf(i) is { } row ? LicenceView.For(row, i.Class) : null;

    /// <summary>Another page asked for an item: open it once the list is here.</summary>
    private void ConsumeFocus()
    {
        if (Ctx.CatalogFocus is not { } f || Ctx.CatalogRead is null) return;
        Ctx.CatalogFocus = null;
        var item = f.ItemId is not null ? List?.Find(f.ItemId)
            : f.Class is { } c && f.ComponentId is not null ? List?.FindComponent(c, f.ComponentId) : null;
        if (item is not null)
        {
            OpenDetail(item, rebuild: false);
            return;
        }
        // Not a catalog item (installed some other way): show its class, searched by its name.
        _detail = null;
        _class = null;
        _installedOnly = false;
        _query = f.Name ?? "";
        if (_search is not null) _search.Text = _query;
    }

    protected override string Key()
    {
        ConsumeFocus();
        var sb = new StringBuilder();
        var read = Ctx.CatalogRead;
        sb.Append(Ctx.CatalogLoading).Append(read?.Status).Append(read?.Title).Append('|')
          .Append(_class).Append(_installedOnly).Append(_detail).Append(_keyFor).Append('|');
        if (Ctx.CatalogOp is { } op) sb.Append(op.ItemId).Append(op.Verb).Append(op.Phase);
        sb.Append(Ctx.CatalogBackground?.ItemId);
        if (Ctx.CatalogResult is { } res) sb.Append(res.ItemId).Append(res.Outcome.Title);
        sb.Append(Ctx.ActivatingItem).Append(Ctx.BuyStarting).Append(Ctx.BuyPending?.ItemId);
        if (Ctx.BuyNote is { } bn) sb.Append(bn.ItemId).Append(bn.Plan.Kind).Append(bn.Plan.Title);
        if (Ctx.ActivationResult is { } ar) sb.Append(ar.ItemId).Append(ar.Outcome.Title);
        sb.Append('|');
        foreach (var i in read?.List?.Items ?? Array.Empty<CatalogItem>())
        {
            sb.Append(i.Id).Append(i.State).Append(i.Installed).Append(i.Available).Append(i.Download).Append(i.Compatible)
              .Append(i.Detected).Append(i.Receipt);
            if (RowOf(i) is { } row)
                sb.Append(row.Licence).Append(row.LicenceSource).Append(row.LicenceExpires).Append(row.LicenceMessage)
                  .Append(row.LicencePurchaseUrl).Append(row.LicenceKeyActivation);
            sb.Append(';');
        }
        foreach (var c in ComponentClasses.All) sb.Append(Ctx.PolicyLoading(c));
        return sb.ToString();
    }

    protected override Control Build()
    {
        _elapsed = null;
        _keyBox = null;
        _results = null;
        var read = Ctx.CatalogRead;
        if (read is null) return Opening();
        if (read.Status != CatalogStatus.Ready) return NotReady(read);
        var list = read.List!;
        if (_detail is not null && list.Find(_detail) is { } item) return Detail(item);
        _detail = null;
        return Browse(list);
    }

    // ── States that are not the list ────────────────────────────────────────

    private static Control Opening()
    {
        var bar = new ProgressBar { IsIndeterminate = true, Height = 4, MaxWidth = 360, HorizontalAlignment = HorizontalAlignment.Left };
        AutomationProperties.SetName(bar, "Opening the Catalog");
        return StateCard("Opening the Catalog", "Fetching the latest list of add-ons and apps…", null, bar);
    }

    private Control NotReady(CatalogRead read)
    {
        Control Download() => LinkButton("Open the Download page", CatalogText.DownloadPage, "primary");
        var retry = U.Button(Ctx.CatalogLoading ? "Trying…" : "Try again", () => _ = Ctx.LoadCatalogAsync(force: true), "outline");
        retry.IsEnabled = !Ctx.CatalogLoading;
        Control[] actions = read.Status switch
        {
            CatalogStatus.NoEngineHere => new[] { Download(), U.Button("See what is installed", () => Ctx.Navigate("components"), "outline") },
            CatalogStatus.EngineMissing => new[] { Download() },
            CatalogStatus.Offline => new Control[] { retry, U.Button("See what is installed", () => Ctx.Navigate("components"), "outline") },
            _ => new Control[] { retry },
        };
        var page = U.VStack(18, StateCard(read.Title, read.Text, read.Detail, actions));
        // Offline: what the engine installed is still here, and can still be removed (no catalog needed).
        if (read.List is { Items.Count: > 0 } have)
        {
            var grid = new U.CardGrid(2) { MinColumnWidth = 420 };
            foreach (var i in have.Items) grid.Add(ItemCard(i));
            page.Children.Add(U.VStack(10, U.Text("Installed from the Catalog", "group"), grid));
        }
        return page;
    }

    private static Control StateCard(string title, string text, string? detail, params Control[] actions)
    {
        var t = U.Wrapped(title, "group");
        t.FontSize = 19;
        var body = U.VStack(10, t, U.Wrapped(text, "soft"));
        if (actions.Length > 0)
        {
            var row = U.Flow(10, 8, actions);
            row.Margin = new Thickness(0, 8, 0, 0);
            body.Children.Add(row);
        }
        if (detail is { Length: > 0 }) body.Children.Add(U.Wrapped(detail, "meta"));
        var card = U.Card(body);
        card.MaxWidth = 720;
        card.HorizontalAlignment = HorizontalAlignment.Left;
        card.Padding = new Thickness(28, 26);
        AutomationProperties.SetLiveSetting(card, AutomationLiveSetting.Polite);
        return card;
    }

    // ── Browse ──────────────────────────────────────────────────────────────

    private Control Browse(CatalogList list)
    {
        _search ??= NewSearch();
        U.Detach(_search);
        if (_search.Text != _query) _search.Text = _query;

        var filters = U.Flow(8, 6, FilterButton("All", _class is null && !_installedOnly, () => { _class = null; _installedOnly = false; }));
        foreach (string c in list.Classes)
            U.AddFlow(filters, FilterButton(CatalogClasses.Label(c), _class == c, () => _class = _class == c ? null : c), 8, 6);
        U.AddFlow(filters, FilterButton("Installed", _installedOnly, () => _installedOnly = !_installedOnly), 8, 6);

        var bar = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
        _search.VerticalAlignment = VerticalAlignment.Top;
        bar.Children.Add(_search);
        filters.Margin = new Thickness(16, 2, 0, 0);
        Grid.SetColumn(filters, 1);
        bar.Children.Add(filters);

        _results = new Border { Child = Results(list) };
        var page = U.VStack(18, bar);
        // An engine run the Dashboard stopped waiting for: say why the buttons are waiting.
        if (Ctx.CatalogBackground is { } bg) page.Children.Add(U.Notice(Level.Info, "Still working in the background", bg.Line));
        page.Children.Add(_results);
        return page;
    }

    private TextBox NewSearch()
    {
        var box = new TextBox
        {
            Watermark = "Search the Catalog", Width = 260, MinHeight = 32, FontSize = 13,
            Background = Tokens.Field, BorderBrush = Tokens.Line, Foreground = Tokens.Fg,
            CornerRadius = Tokens.Radius("FieldRadius"), Padding = new Thickness(10, 6),
        };
        AutomationProperties.SetName(box, "Search the Catalog");
        // The query is not part of the page's key: only the results are redrawn, so the box keeps its focus.
        box.TextChanged += (_, _) =>
        {
            string q = box.Text ?? "";
            if (q == _query) return;
            _query = q;
            if (_results is not null && List is { } l) _results.Child = Results(l);
        };
        return box;
    }

    private Button FilterButton(string label, bool on, Action toggle)
    {
        var b = U.Button(label, () => { toggle(); Update(force: true); }, on ? "primary" : "quiet", "sm");
        AutomationProperties.SetName(b, on ? $"{label}, selected filter" : $"Show {label}");
        return b;
    }

    private Control Results(CatalogList list)
    {
        var groups = list.Browse(_query, _class, _installedOnly);
        if (groups.Count == 0)
        {
            string what = _query.Trim().Length > 0 ? $"Nothing matches “{_query.Trim()}” yet."
                : _installedOnly ? "Nothing from the Catalog is installed yet. Everything here is one click away."
                : list.Items.Count == 0 ? "The Catalog is being stocked. Check back soon."
                : "Nothing in this category yet.";
            return StateCard(what, "Try another word, or browse everything.", null,
                U.Button("Show everything", () =>
                {
                    _query = "";
                    _class = null;
                    _installedOnly = false;
                    if (_search is not null) _search.Text = "";
                    Update(force: true);
                }, "outline"));
        }
        var col = U.VStack(26);
        foreach (var (cls, items) in groups)
        {
            var grid = new U.CardGrid(2) { MinColumnWidth = 420 };
            foreach (var i in items) grid.Add(ItemCard(i));
            col.Children.Add(U.VStack(12, U.VStack(2, U.Text(CatalogClasses.Label(cls), "group"), U.Wrapped(CatalogClasses.Lead(cls), "desc")), grid));
        }
        return col;
    }

    /// <summary>The chips of an item: positive facts only.</summary>
    private WrapPanel Chips(CatalogItem i)
    {
        var chips = U.Flow(6, 4);
        if (i.StateChip is { } st) U.AddFlow(chips, U.Chip(st.Text, st.Level), 6, 4);
        if (i.DetectedHere) U.AddFlow(chips, U.Chip("Works with your display", Level.Ok), 6, 4);
        if (i.Recommended) U.AddFlow(chips, U.Chip("Recommended", Level.Info), 6, 4);
        if (LicenceOf(i) is { } lic) U.AddFlow(chips, U.Chip(lic.Headline, lic.Level == Level.Warn ? Level.Info : lic.Level), 6, 4);
        if (i.SizeText is { } size && (!i.IsInstalled || i.HasUpdate)) U.AddFlow(chips, U.Chip(size), 6, 4);
        return chips;
    }

    private Control ItemCard(CatalogItem i)
    {
        var name = U.Wrapped(i.Name, "value");
        name.FontWeight = FontWeight.SemiBold;
        name.FontSize = 15;
        var body = U.VStack(8, name);
        if (i.Publisher.Length > 0) body.Children.Add(U.Text($"By {i.Publisher}", "meta"));
        body.Children.Add(U.Wrapped(CatalogText.About(i), "soft"));
        var chips = Chips(i);
        if (chips.Children.Count > 0) body.Children.Add(chips);
        if (!i.Compatible) body.Children.Add(U.Wrapped(CatalogText.OtherVersion, "meta"));
        else if (i.Unmet.FirstOrDefault(p => !p.Installable) is { } need && !i.IsInstalled)
            body.Children.Add(U.Wrapped($"Works together with {need.Name}.", "meta"));
        if (Progress(i) is { } progress) body.Children.Add(progress);

        var actions = U.Flow(10, 6);
        if (MainButton(i) is { } main) U.AddFlow(actions, main, 10, 6);
        if (List is not null)
        {
            var details = U.Button("Details", () => OpenDetail(i), "quiet", "sm");
            AutomationProperties.SetName(details, $"Details of {i.Name}");
            U.AddFlow(actions, details, 10, 6);
        }
        else if (i.CanUninstall) U.AddFlow(actions, UninstallButton(i), 10, 6); // the offline view
        actions.Margin = new Thickness(0, 4, 0, 0);
        body.Children.Add(actions);
        return U.Card(body);
    }

    /// <summary>The detail view adds the engine's own reason to the calm line the card shows.</summary>
    private static string CompatibilityLine(CatalogItem i) =>
        i.IncompatibleReason is { Length: > 0 } why ? $"{CatalogText.OtherVersion} ({why})" : CatalogText.OtherVersion;

    // ── Actions ─────────────────────────────────────────────────────────────

    private bool Busy => Ctx.CatalogBusy;

    private Button? MainButton(CatalogItem i)
    {
        Button? b = i.Action switch
        {
            CatalogAction.Install => U.Button("Get it", () => _ = Ctx.RunCatalogAsync(CatalogVerb.Install, i), "primary", "sm"),
            CatalogAction.Update => U.Button("Update", () => _ = Ctx.RunCatalogAsync(CatalogVerb.Update, i), "primary", "sm"),
            CatalogAction.OpenListing => LinkButton("Get it", i.Listing!, "primary", "sm"),
            _ => i.IsRuntime && i.HasUpdate ? LinkButton("Get the update", CatalogText.DownloadPage, "primary", "sm") : null,
        };
        if (b is null) return null;
        if (i.Action is CatalogAction.Install or CatalogAction.Update)
        {
            b.IsEnabled = !Busy;
            ToolTip.SetShowOnDisabled(b, true);
            string size = i.SizeText is { } s ? $" ({s})" : "";
            ToolTip.SetTip(b, Ctx.CatalogBackground is { } bg ? bg.Line
                : Busy ? "One thing at a time: this starts when the current one is done."
                : $"Downloads{size}, checks the publisher's signature and installs. Windows asks for permission once.");
            AutomationProperties.SetName(b, $"{(i.Action == CatalogAction.Update ? "Update" : "Get")} {i.Name}");
        }
        else AutomationProperties.SetName(b, $"Get {i.Name}: opens its page in your browser");
        return b;
    }

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

    /// <summary>What is happening to this item now, or how the last thing ended. Never both, never a stale spinner.</summary>
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
            var n = U.Notice(o.Level, o.Title, o.Text, U.Button("OK", Ctx.DismissCatalogResult, "quiet", "sm"));
            AutomationProperties.SetLiveSetting(n, AutomationLiveSetting.Polite);
            return n;
        }
        return null;
    }

    // ── Detail ──────────────────────────────────────────────────────────────

    private void OpenDetail(CatalogItem i, bool rebuild = true)
    {
        _detail = i.Id;
        _keyFor = null;
        // The licence comes from the component itself: read its class when it is installed.
        // Posted: this can be reached from inside Key(), and the load announces itself at once.
        if (i.IsInstalled && i.PolicyClass is { } c) Dispatcher.UIThread.Post(() => _ = Ctx.LoadPolicyAsync(c));
        if (!rebuild) return;
        Update(force: true);
        Dispatcher.UIThread.Post(() => this.BringIntoView(new Rect(0, 0, 1, 1)));
    }

    private Control Detail(CatalogItem i)
    {
        var back = U.Button("← All of the Catalog", () => { _detail = null; _keyFor = null; Update(force: true); }, "link");
        AutomationProperties.SetName(back, "Back to the Catalog");

        var name = U.Wrapped(i.Name, "group");
        name.FontSize = 22;
        var head = U.VStack(6, name);
        string by = i.Publisher.Length > 0 ? $"By {i.Publisher} · {CatalogClasses.Label(i.Class)}" : CatalogClasses.Label(i.Class);
        head.Children.Add(U.Text(by, "meta"));
        var chips = Chips(i);
        if (chips.Children.Count > 0) head.Children.Add(chips);

        var about = U.VStack(8, U.Text("What it does", "group"), U.Wrapped(CatalogText.About(i), "soft"));
        if (i.DetectedHere)
            about.Children.Add(U.Wrapped(i.Evidence is { Length: > 0 } ev
                ? $"Works with your display: DisplayXR found it on this machine ({ev})."
                : "Works with your display: DisplayXR found it on this machine.", "soft"));
        if (!i.Compatible) about.Children.Add(U.Wrapped(CompatibilityLine(i), "meta"));

        var facts = new U.KvList(130);
        if (i.VersionText.Length > 0) facts.Add("Version", i.VersionText);
        if (i.SizeText is { } size) facts.Add(i.IsInstalled && !i.HasUpdate ? "Size" : "Download", size);
        if (i.Publisher.Length > 0) facts.Add("Publisher", i.Publisher);
        facts.Add("Category", CatalogClasses.Label(i.Class));

        var actions = U.Flow(10, 6);
        if (MainButton(i) is { } main) U.AddFlow(actions, main, 10, 6);
        if (i.CanUninstall) U.AddFlow(actions, UninstallButton(i), 10, 6);
        if (i.IsInstalled && i.PolicyClass is { } pc)
        {
            var manage = U.Button("Manage in Components", () => Ctx.Navigate("components"), "quiet", "sm");
            ToolTip.SetTip(manage, $"Turn it on or off, or prefer it, among your {CatalogClasses.Others(i.Class)}.");
            U.AddFlow(actions, manage, 10, 6);
        }

        var left = U.VStack(18, head, about);
        if (i.Prerequisites.Count > 0) left.Children.Add(Prerequisites(i));
        if (Licence(i) is { } licence) left.Children.Add(licence);
        if (Progress(i) is { } progress) left.Children.Add(progress);
        if (actions.Children.Count > 0) left.Children.Add(actions);
        if (i.IsInstalled && !i.CanUninstall && !i.IsRuntime)
            left.Children.Add(U.Wrapped("To remove it, use Windows Settings > Apps > Installed apps.", "meta"));
        if (i.IsRuntime)
            left.Children.Add(U.Wrapped("DisplayXR itself is updated from the Download page, so the Dashboard can close while it installs.", "meta"));

        var grid = new U.CardGrid(2) { MinColumnWidth = 380 };
        grid.Add(U.Card(left));
        grid.Add(U.Card(U.VStack(12, U.Text("At a glance", "group"), facts)));
        return U.VStack(14, back, grid);
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
            var n = U.Notice(ar.Outcome.Accepted ? Level.Ok : Level.Info, ar.Outcome.Title, ar.Outcome.Text,
                             U.Button("OK", Ctx.DismissCatalogResult, "quiet", "sm"));
            AutomationProperties.SetLiveSetting(n, AutomationLiveSetting.Polite);
            col.Children.Add(n);
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
        _keyBox = box;

        void Close()
        {
            box.Text = "";
            _keyFor = null;
            _keyBox = null;
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

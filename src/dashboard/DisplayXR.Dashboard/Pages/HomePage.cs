// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Linq;
using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;
using DisplayXR.Dashboard.Ui;

namespace DisplayXR.Dashboard.Pages;

/// <summary>
/// Home answers "is my 3D display working?": the verdict (<see cref="Verdicts"/>: one headline, one
/// row per display with up to three facts, each yes / no / not known yet), "Needs attention" (plain
/// sentences, or "nothing"), then "Check my setup" (the self-test) with when it last ran in this
/// session. Under them the runtime, its display drivers and the graphics cards. No ticket or
/// design-record numbers and no environment-variable names on the page.
/// </summary>
public sealed class HomePage : Page
{
    private bool _showAllChecks;

    public HomePage() : base("home", "Home") { }

    public override string Caption
    {
        get
        {
            var s = Ctx.Feed.Snapshot;
            string v = s?.Runtime.Version is { Length: > 0 } ver ? $"DisplayXR {ver}" : "DisplayXR";
            if (s is null) return v;
            // Never a zero count: a part with nothing to count is left out.
            var parts = new System.Collections.Generic.List<string> { v };
            if (s.Screens.Count > 0) parts.Add(StatusText.Plural(s.Screens.Count, "display", "displays"));
            if (s.Plugins.Count > 0) parts.Add(StatusText.Plural(s.Plugins.Count, "display driver", "display drivers"));
            return string.Join(StatusText.Sep, parts);
        }
    }

    public override void OnShown()
    {
        _ = Ctx.LoadInfoAsync(force: false);
        _ = Ctx.LoadDpAsync(maxAge: TimeSpan.FromMinutes(10));
    }

    protected override string Key()
    {
        var s = Ctx.Feed.Snapshot;
        var sb = new StringBuilder();
        sb.Append(Ctx.Version).Append('|').Append(_showAllChecks).Append('|');
        if (s is not null)
        {
            sb.Append(s.Source).Append('|').Append(s.Runtime).Append('|').Append(s.Workspace).Append('|');
            foreach (var p in s.Plugins) sb.Append(p).Append(';');
        }
        // The verdict: its words, every row's facts and every "Needs attention" sentence.
        var v = Verdict();
        sb.Append('|').Append(v.Kind).Append(v.Headline).Append(v.Subline).Append(v.Level).Append(v.SimulatedDriverId);
        foreach (var r in v.Rows) sb.Append(r.Name).Append(r.Role).Append(r.Driver).Append(r.Forced).Append(string.Join(",", r.Facts)).Append(';');
        foreach (var a in v.Attention) sb.Append(a).Append(';');
        sb.Append('|').Append(Ctx.Dp?.Preferred).Append(Ctx.ActionRunning);
        return sb.ToString();
    }

    private Verdict Verdict() =>
        Verdicts.From(Ctx.Feed.Snapshot, Ctx.Selftest, Verdicts.ActiveRuntime(Ctx.Info, Ctx.Feed.Snapshot), Ctx.Dp);

    protected override Control Build()
    {
        var s = Ctx.Feed.Snapshot;
        var page = U.VStack(18);
        var verdict = Verdict();

        // The answer to "is my 3D display working?" first: the headline, one row per display, what to fix.
        page.Children.Add(VerdictCard(verdict));
        page.Children.Add(AttentionCard(verdict));
        page.Children.Add(SelftestCard());

        // The amber badge on the Home rail entry means this notice (MainWindow.SetBadge "home").
        if (StatusText.MachineOverrideNotice(Ctx.Dp) is { } ov)
        {
            var clear = U.Button("Clear this setting", () => _ = Ctx.RunActionsAsync("dp", CliArgs.Words("dp reset")), "warn", "sm");
            clear.IsEnabled = !Ctx.ActionRunning;
            page.Children.Add(U.Notice(Level.Warn, ov.Title, ov.Text, clear));
        }

        var grid = new U.CardGrid(2) { MinColumnWidth = 440 };
        grid.Add(RuntimeCard(s));
        grid.Add(PluginsCard(s));
        grid.Add(GpuCard());
        page.Children.Add(grid);
        return page;
    }

    /// <summary>
    /// The verdict's headline and one row per display: its name, which display driver shows 3D on it
    /// (the driver's own name; its id is the tooltip) and up to three facts, each yes / no / not known
    /// yet. This replaces the "Display driver for each display" card, whose content is in the rows.
    /// </summary>
    private Control VerdictCard(Verdict v)
    {
        var dot = U.Dot(v.Level, 14);
        dot.VerticalAlignment = VerticalAlignment.Top;
        dot.Margin = new Thickness(0, 9, 14, 0);
        var headline = U.Wrapped(v.Headline, "h1");
        headline.FontSize = 24;
        if (v.Level is Level.Warn or Level.Critical) headline.Foreground = Tokens.For(v.Level);
        var head = U.VStack(4, headline);
        if (v.Subline.Length > 0) head.Children.Add(U.Wrapped(v.Subline, "dim"));
        // Only the simulated display driver: its Options (the 3D preview kind, Start in) are on Components.
        if (v.SimulatedDriverId is { } sim)
        {
            var preview = U.Button("Try a 3D preview", () =>
            {
                Ctx.CatalogFocus = new CatalogFocus(null, ComponentClass.Display, sim, null);
                Ctx.Navigate("components");
            }, "link");
            preview.HorizontalAlignment = HorizontalAlignment.Left;
            ToolTip.SetTip(preview, "Opens the simulated display's options on Components, where you can choose what the 3D preview looks like.");
            head.Children.Add(preview);
        }
        var top = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
        top.Children.Add(dot);
        Grid.SetColumn(head, 1);
        top.Children.Add(head);
        Avalonia.Automation.AutomationProperties.SetLiveSetting(top, Avalonia.Automation.AutomationLiveSetting.Polite);

        var body = U.VStack(14, top);
        foreach (var r in v.Rows) body.Children.Add(VerdictRowView(r));
        return U.Card(body);
    }

    private Control VerdictRowView(VerdictRow r)
    {
        var name = U.Wrapped(r.Name, "group");
        name.FontSize = 15;
        var title = U.Flow(10, 4, name);
        if (r.Role is { } role) U.AddFlow(title, U.Chip(role), 6, 4);
        if (!r.Is3D) U.AddFlow(title, U.Text(Verdicts.Plain2D, "meta"), 6, 4);
        var details = U.Button("Details →", () => Ctx.Navigate("displays"), "link");
        Avalonia.Automation.AutomationProperties.SetName(details, $"Details of {r.Name}");
        var head = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
        head.Children.Add(title);
        Grid.SetColumn(details, 1);
        details.VerticalAlignment = VerticalAlignment.Top;
        head.Children.Add(details);
        var col = U.VStack(6, head);
        if (r.Is3D)
        {
            var facts = U.Flow(16, 4);
            foreach (var f in r.Facts) U.AddFlow(facts, FactView(f), 16, 4);
            col.Children.Add(facts);
            if (r.Driver is { } driver)
            {
                var line = U.Text($"Display driver: {driver}", "meta");
                if (r.DriverId is { } id) ToolTip.SetTip(line, id);
                var dl = U.Flow(8, 4, line);
                if (r.Forced is { } forced) U.AddFlow(dl, U.Chip(forced, Level.Warn), 8, 4);
                col.Children.Add(dl);
            }
        }
        var box = new Border { Child = col, Padding = new Thickness(14, 10) };
        box.Classes.Add("field");
        return box;
    }

    /// <summary>One fact: a tick (yes), an amber dot (no) or a hollow ring (not known yet), then its words.</summary>
    private static Control FactView(VerdictFact f)
    {
        Control mark = f.Tick switch
        {
            Tick.Yes => new Avalonia.Controls.Shapes.Path
            {
                Data = Geometry.Parse("M 1,5 L 4,8 L 10,1"), Stroke = Tokens.For(Level.Ok), StrokeThickness = 2,
                StrokeLineCap = PenLineCap.Round, StrokeJoin = PenLineJoin.Round, Width = 11, Height = 9,
                VerticalAlignment = VerticalAlignment.Center,
            },
            Tick.No => U.Dot(Level.Warn, 8),
            _ => new Avalonia.Controls.Shapes.Ellipse
            {
                Width = 8, Height = 8, Stroke = Tokens.Muted, StrokeThickness = 1.2, VerticalAlignment = VerticalAlignment.Center,
            },
        };
        var text = U.Text(f.Text, f.Tick == Tick.NotKnownYet ? "dim" : "value");
        text.VerticalAlignment = VerticalAlignment.Center;
        var row = U.HStack(7, mark, text);
        Avalonia.Automation.AutomationProperties.SetName(row, f.Text);
        return row;
    }

    /// <summary>"Needs attention": one plain sentence per thing to fix, or "nothing", with the action the Dashboard already has for it.</summary>
    private Control AttentionCard(Verdict v)
    {
        if (v.Kind == VerdictKind.Checking) return U.Card(U.Wrapped("Needs attention: checking…", "empty"));
        if (v.Attention.Count == 0)
            return U.Card(U.Flow(8, 4, U.Text("Needs attention", "group"), U.Text("nothing", "dim")));
        var list = U.VStack(10);
        foreach (var a in v.Attention)
        {
            var dot = U.Dot(a.Level, 8);
            dot.VerticalAlignment = VerticalAlignment.Top;
            dot.Margin = new Thickness(0, 6, 10, 0);
            var text = U.Wrapped(a.Text, "soft");
            if (a.Technical is { Length: > 0 } tech) ToolTip.SetTip(text, tech);
            var col = U.VStack(6, text);
            if (AttentionButton(a) is { } b) col.Children.Add(b);
            var g = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
            g.Children.Add(dot);
            Grid.SetColumn(col, 1);
            g.Children.Add(col);
            list.Children.Add(g);
        }
        return U.Card(U.VStack(12, U.CardTitle("Needs attention"), list));
    }

    /// <summary>Only actions the Dashboard already has; none opens the system's settings.</summary>
    private Control? AttentionButton(AttentionItem a)
    {
        switch (a.Action)
        {
        case AttentionAction.OpenDisplays:
            return U.Button("Open Displays", () => Ctx.Navigate("displays"), "outline", "sm");
        case AttentionAction.OpenComponents:
            return U.Button("Open Components", () => Ctx.Navigate("components"), "outline", "sm");
        case AttentionAction.ActivateRuntime:
            var b = U.Button("Make DisplayXR the active OpenXR runtime",
                () => _ = Ctx.RunActionsAsync("runtime", CliArgs.Words("runtime activate")), "primary", "sm");
            b.IsEnabled = !Ctx.ActionRunning;
            return b;
        default:
            return null;
        }
    }

    private Control RuntimeCard(StatusSnapshot? s)
    {
        var info = Ctx.Info;
        var kv = new U.KvList(170);
        string version = s?.Runtime.Version is { Length: > 0 } v ? v : "—";
        string tag = s?.Runtime.GitTag is { Length: > 0 } t ? t : info?.GitTag ?? "";
        kv.Add("Version", tag.Length > 0 ? $"{version}  ({tag})" : version);
        int abi = s?.Runtime.PluginAbi ?? info?.PluginAbi ?? 0;
        kv.Add("Add-on interface", abi > 0 ? $"version {abi}" : "—");

        // Active OpenXR runtime: info's resolved key when read, else the snapshot's.
        string? active = info is { ActiveRuntimeQueried: true } ? (info.ActiveRuntimeSet ? info.ActiveRuntimeValue : null)
                       : s?.Runtime.ActiveOpenXrRuntime;
        bool isDxr = active is not null && active.Contains("DisplayXR", StringComparison.OrdinalIgnoreCase);
        bool known = info is not null || s is not null; // nothing read yet: no verdict, no red
        var activeBlock = U.VStack(6, U.Wrapped(known ? active ?? "<unset>" : "—", "value", "mono"));
        if (known) ((TextBlock)activeBlock.Children[0]).Foreground = isDxr ? Tokens.For(Level.Ok) : Tokens.For(Level.Critical);
        if (known && !isDxr)
            activeBlock.Children.Add(U.Button("Make DisplayXR the active OpenXR runtime",
                () => _ = Ctx.RunActionsAsync("runtime", CliArgs.Words("runtime activate")), "primary", "sm"));
        kv.Add("Active OpenXR runtime", activeBlock);

        string service = s is null ? (Ctx.Feed.IsRunning ? "connecting…" : "—")
            : s.IsService ? "running"
            : "not running";
        kv.Add("Background service", service, brush: s?.IsService == true ? Tokens.For(Level.Ok) : null);
        if (s is not null)
            kv.Add("Spatial desktop", s.Workspace.Enabled ? $"turned on{(s.Workspace.Controller is { } c ? StatusText.Sep + c : "")}" : "turned off");
        if (info?.Device is { Length: > 0 } dev) kv.Add("Device", dev);

        var body = U.VStack(16, U.CardTitle("Runtime"), kv);
        if (Ctx.LastActionArea == "runtime" && Ctx.LastAction is { } a)
            body.Children.Add(U.Wrapped(a, Ctx.LastActionFailed ? "soft" : "desc"));
        return U.Card(body);
    }

    private Control PluginsCard(StatusSnapshot? s)
    {
        var body = U.VStack(14, U.CardTitle("Display drivers",
            "Each installed display driver and whether it is ready. A note under one is the display maker's own words."));
        if (s is null || s.Plugins.Count == 0)
        {
            body.Children.Add(U.Wrapped(s is null ? "Looking for display drivers…" : "No display driver is installed.", "empty"));
            return U.Card(body);
        }
        var list = U.VStack(0);
        for (int i = 0; i < s.Plugins.Count; i++)
        {
            var p = s.Plugins[i];
            if (i > 0) list.Children.Add(new Border { Height = 12 });
            var dot = U.Dot(StatusText.PluginLevel(p));
            dot.VerticalAlignment = VerticalAlignment.Top;
            dot.Margin = new Thickness(0, 6, 12, 0);
            var name = U.Text(p.Name.Length > 0 ? p.Name : p.Id, "group");
            name.FontSize = 14;
            // The technical words (load verdict, platform state, id, ProbeOrder) are the name's tooltip.
            ToolTip.SetTip(name, $"{p.Load}{StatusText.Sep}{p.PlatformState}{StatusText.Sep}{p.Id}{StatusText.Sep}ProbeOrder {p.ProbeOrder}");
            var head = U.Flow(8, 4, name);
            if (p.Version.Length > 0) U.AddFlow(head, U.Text("version " + p.Version, "meta"));
            var chips = U.Flow(6, 4);
            if (PlainStates.Chip(PlainStates.DisplayProcessor(p.Load, null)) is { } st) U.AddFlow(chips, U.Chip(st.Text, st.Level), 6, 4);
            if (StatusText.PlainPlatform(p.PlatformState) is { } plat)
                U.AddFlow(chips, U.Chip(plat, StatusText.PluginLevel(p) is Level.Ok or Level.Plain ? Level.Plain : StatusText.PluginLevel(p)), 6, 4);
            if (p.Fallback) U.AddFlow(chips, U.Chip("Used when no other driver fits"), 6, 4);
            if (StatusText.RemedyChip(p.Remedy) is { } upd) U.AddFlow(chips, U.Chip(upd.Text, upd.Level), 6, 4);
            var col = U.VStack(2, head, chips);
            if (p.Hint.Length > 0) col.Children.Add(U.Wrapped(p.Hint, "soft"));
            if (RemedyNotice(p.Remedy) is { } notice) col.Children.Add(notice);
            var g = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
            g.Children.Add(dot);
            Grid.SetColumn(col, 1);
            g.Children.Add(col);
            list.Children.Add(g);
        }
        body.Children.Add(list);
        return U.Card(body);
    }

    /// <summary>
    /// ADR-045 Amendment 2026-11-22: a display driver's platform update notice — the versions it
    /// reported and, when it gave an https page, a button that opens it. Null when there is none.
    /// </summary>
    private Control? RemedyNotice(PlatformRemedy r)
    {
        if (StatusText.RemedyLine(r) is not { } text) return null;
        var box = U.VStack(6, U.Wrapped(text, "soft"));
        if (r.Url is { } url)
        {
            var label = r.ActionLabel.Length > 0 ? r.ActionLabel : "Get the update";
            var b = U.Button(label, () => { if (!UrlLauncher.Open(url)) Ctx.Toast("Could not open the browser"); },
                             "outline", "sm");
            if (SafeUrl.TryHttps(url, out var u)) ToolTip.SetTip(b, $"Opens {u!.Host} in your browser");
            b.HorizontalAlignment = HorizontalAlignment.Left;
            box.Children.Add(b);
        }
        else if (r.ActionLabel.Length > 0)
        {
            box.Children.Add(U.Wrapped(r.ActionLabel, "soft"));
        }
        box.Margin = new Thickness(0, 4, 0, 0);
        return box;
    }

    private Control GpuCard()
    {
        var body = U.VStack(12, U.CardTitle("Graphics cards",
            "Which graphics card draws 3D apps, and which one your 3D display is plugged into."));
        var info = Ctx.Info;
        if (info is null)
        {
            body.Children.Add(U.Wrapped(Ctx.InfoLoading ? "Looking at your graphics cards (about 10 seconds)…"
                                                      : Ctx.InfoError ?? "Not read yet.", Ctx.InfoError is null ? "empty" : "soft"));
            return U.Card(body);
        }
        var g = info.Gpu;
        if (g is null || !g.Probed)
        {
            body.Children.Add(U.Wrapped($"Not checked{(g?.Note is { Length: > 0 } n ? ": " + n : " (Windows only)")}.", "empty"));
            return U.Card(body);
        }
        if (g.Adapters.Count <= 1)
        {
            body.Children.Add(U.Notice(Level.Ok, "One graphics card: it does everything.", g.Adapters.FirstOrDefault()?.Name ?? ""));
        }
        else
        {
            var list = U.VStack(10);
            foreach (var a in g.Adapters)
            {
                var head = U.Flow(8, 4, U.Text(a.Name, "value"));
                if (g.Scanout.Resolved && a.Luid == g.Scanout.Luid) U.AddFlow(head, U.Chip("Your 3D display is plugged in here", Level.Info));
                if (g.Render.Resolved && a.Luid == g.Render.Luid) U.AddFlow(head, U.Chip("Draws 3D apps"));
                if (g.Ingest.Resolved && a.Luid == g.Ingest.Luid) U.AddFlow(head, U.Chip("Used by the background service"));
                list.Children.Add(U.VStack(1, head, U.Text($"{a.VramMb} MB of graphics memory", "meta")));
            }
            body.Children.Add(list);
            body.Children.Add(U.Wrapped(g.SplitApplies
                ? "The 3D picture passes from one graphics card to the other on its way to your display."
                : "3D apps are drawn on the graphics card your display is plugged into.", "soft"));
        }
        // The technical half (adapter ids, the runtime's verdict, the configured setting and where it
        // came from) is in Troubleshooting > Graphics cards.
        body.Children.Add(U.Button("Technical details in Troubleshooting →", () => Ctx.Navigate("troubleshooting/graphics"), "link"));
        return U.Card(body);
    }

    private Control SelftestCard()
    {
        var st = Ctx.Selftest;
        var run = U.Button(Ctx.SelftestRunning ? "Checking…" : (st is null ? "Check my setup" : "Check again"),
            () => _ = Ctx.RunSelftestAsync(), "primary", "sm");
        run.IsEnabled = !Ctx.SelftestRunning;
        var right = U.HStack(12, U.Text(Verdicts.LastChecked(st, Ctx.SelftestRunning), "meta"), run);
        ((TextBlock)right.Children[0]).VerticalAlignment = VerticalAlignment.Center;
        var body = U.VStack(12, U.CardTitle("Check my setup",
            "Checks that DisplayXR finds your display driver, reads your display correctly and is the 3D system apps use. Nothing is saved: the result is for this session.", right));
        if (Ctx.SelftestError is { } err) body.Children.Add(U.Notice(Level.Critical, "The check did not finish", err));
        if (st is null)
        {
            if (Ctx.SelftestError is null)
                body.Children.Add(U.Wrapped(Ctx.SelftestRunning ? "Checking (about 10 seconds)…" : "Not checked yet in this session.", "empty"));
            return U.Card(body);
        }
        int failed = st.Checks.Count(c => !c.Ok);
        var badge = U.Badge(StatusText.SelftestVerdict(st.Checks.Count, failed), st.Passed ? Level.Ok : Level.Critical, tick: st.Passed);
        ToolTip.SetTip(badge, st.Verdict); // the CLI's own word
        body.Children.Add(U.Flow(10, 4, badge));
        var shown = _showAllChecks ? st.Checks : st.Checks.Where(c => !c.Ok).ToList();
        if (shown.Count > 0)
        {
            var list = U.VStack(7);
            foreach (var c in shown)
            {
                var dot = U.Dot(c.Ok ? Level.Ok : Level.Critical, 7);
                dot.VerticalAlignment = VerticalAlignment.Top;
                dot.Margin = new Thickness(0, 6, 10, 0);
                var text = new TextBlock { TextWrapping = TextWrapping.Wrap };
                text.Classes.Add("soft");
                text.Inlines!.Add(new Avalonia.Controls.Documents.Run(c.Name) { FontWeight = FontWeight.SemiBold, Foreground = Tokens.Fg });
                if (c.Detail.Length > 0) text.Inlines.Add(new Avalonia.Controls.Documents.Run("  " + c.Detail) { Foreground = Tokens.Muted });
                var row = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
                row.Children.Add(dot);
                Grid.SetColumn(text, 1);
                row.Children.Add(text);
                list.Children.Add(row);
            }
            body.Children.Add(list);
        }
        body.Children.Add(U.Button(_showAllChecks ? "Show failures only" : "Show every check", () =>
        {
            _showAllChecks = !_showAllChecks;
            Update();
        }, "link"));
        return U.Card(body);
    }
}

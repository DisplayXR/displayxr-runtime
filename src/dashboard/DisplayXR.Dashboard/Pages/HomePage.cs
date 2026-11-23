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
/// Home: "Check my setup" (the self-test) first, then the runtime, its display drivers and the
/// graphics cards (what the ImGui panel's Overview showed), plus the one-line multi-display
/// summary that links to Displays (design §8, the vendor dashboard's Home rule). No ticket or
/// design-record numbers and no environment-variable names on the page; the graphics card's
/// technical lines sit behind its own "Technical details" toggle.
/// </summary>
public sealed class HomePage : Page
{
    private bool _showAllChecks;
    private bool _gpuTechnical;

    public HomePage() : base("home", "Home") { }

    public override string Caption
    {
        get
        {
            var s = Ctx.Feed.Snapshot;
            string v = s?.Runtime.Version is { Length: > 0 } ver ? $"DisplayXR {ver}" : "DisplayXR";
            return s is null ? v : $"{v}{StatusText.Sep}{StatusText.Plural(s.Screens.Count, "display", "displays")}{StatusText.Sep}{StatusText.Plural(s.Plugins.Count, "display driver", "display drivers")}";
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
        sb.Append(Ctx.Version).Append('|').Append(_showAllChecks).Append(_gpuTechnical).Append('|');
        if (s is not null)
        {
            sb.Append(s.Source).Append('|').Append(s.Runtime).Append('|').Append(s.Workspace).Append('|');
            foreach (var p in s.Plugins) sb.Append(p).Append(';');
            sb.Append(StatusText.HomeLine(s));
            foreach (var d in StatusText.HomeScreenDps(s, Ctx.Dp)) sb.Append(d).Append(';');
        }
        sb.Append('|').Append(Ctx.Dp?.Preferred).Append(Ctx.ActionRunning);
        return sb.ToString();
    }

    protected override Control Build()
    {
        var s = Ctx.Feed.Snapshot;
        var page = U.VStack(18);

        // The self-test answers "is it working?": it comes first.
        page.Children.Add(SelftestCard());

        // The amber badge on the Home rail entry means this notice (MainWindow.SetBadge "home").
        if (StatusText.MachineOverrideNotice(Ctx.Dp) is { } ov)
        {
            var clear = U.Button("Clear this setting", () => _ = Ctx.RunActionsAsync("dp", CliArgs.Words("dp reset")), "warn", "sm");
            clear.IsEnabled = !Ctx.ActionRunning;
            page.Children.Add(U.Notice(Level.Warn, ov.Title, ov.Text, clear));
        }

        if (s is not null && StatusText.HomeLine(s) is { } line)
        {
            var badge = StatusText.DisplaysBadge(s);
            var level = badge?.Level ?? Level.Ok;
            page.Children.Add(U.Notice(level, line,
                badge is null ? "More than one 3D display is connected. Each one's details are on Displays."
                              : "At least one display needs attention. The details are on Displays.",
                U.Button("Open Displays", () => Ctx.Navigate("displays"), "outline", "sm")));
        }

        if (s is not null && s.Screens.Count > 0) page.Children.Add(ScreensCard(s));

        var grid = new U.CardGrid(2) { MinColumnWidth = 440 };
        grid.Add(RuntimeCard(s));
        grid.Add(PluginsCard(s));
        grid.Add(GpuCard());
        page.Children.Add(grid);
        return page;
    }

    /// <summary>
    /// Each screen's effective display processor, read-only: the selector is on
    /// Displays (one place to change it).
    /// </summary>
    private Control ScreensCard(StatusSnapshot s)
    {
        var link = U.Button("Change on Displays →", () => Ctx.Navigate("displays"), "link");
        var kv = new U.KvList(220);
        foreach (var d in StatusText.HomeScreenDps(s, Ctx.Dp))
        {
            var value = U.Flow(8, 4, U.Text(d.Plugin ?? "not a 3D display", "value"));
            if (d.Forced is { } f) U.AddFlow(value, U.Chip(f, Level.Warn));
            kv.Add(d.Name, value);
        }
        return U.Card(U.VStack(12, U.CardTitle("Display driver for each display", null, link), kv));
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
        // came from, where an app's log says what it really did): behind its own toggle.
        var toggle = U.Button(_gpuTechnical ? "Hide technical details" : "Technical details", () =>
        {
            _gpuTechnical = !_gpuTechnical;
            Update();
        }, "link");
        body.Children.Add(toggle);
        if (_gpuTechnical)
        {
            var tech = U.VStack(6);
            foreach (var a in g.Adapters) tech.Children.Add(U.Wrapped($"{a.Name}{StatusText.Sep}LUID {a.Luid}{StatusText.Sep}{a.VramMb} MB dedicated", "dev"));
            if (g.Adapters.Count > 1) tech.Children.Add(U.Wrapped($"{(g.SplitApplies ? "SPLIT" : "LOCAL")}: {g.Verdict}", "dev"));
            // The configured half: read from the CLI child's own environment (control-panel-performance-settings.md).
            bool fromEnv = g.WeaveOnScanoutSource == "env";
            tech.Children.Add(U.Wrapped(
                $"DXR_WEAVE_ON_SCANOUT = {(g.WeaveOnScanoutSet ? g.WeaveOnScanout : "<unset>")} [{(g.WeaveOnScanoutSource.Length > 0 ? g.WeaveOnScanoutSource : "?")}]" +
                $"{(fromEnv ? "  (this dashboard's environment only)" : "")}{StatusText.Sep}ingress {(g.Ingress.Length > 0 ? g.Ingress : "?")}", "dev"));
            if (g.ServiceSplit.Length > 0) tech.Children.Add(U.Wrapped(g.ServiceSplit, "desc"));
            tech.Children.Add(U.Wrapped("A running app's real placement is the 'weave placement:' line in its log (%LOCALAPPDATA%\\DisplayXR\\DisplayXR_<exe>.*.log), or its integrity row on Running 3D apps.", "desc"));
            body.Children.Add(tech);
        }
        return U.Card(body);
    }

    private Control SelftestCard()
    {
        var st = Ctx.Selftest;
        var run = U.Button(Ctx.SelftestRunning ? "Checking…" : (st is null ? "Check my setup" : "Check again"),
            () => _ = Ctx.RunSelftestAsync(), "primary", "sm");
        run.IsEnabled = !Ctx.SelftestRunning;
        var body = U.VStack(12, U.CardTitle("Check my setup",
            "Checks that DisplayXR finds your display driver, reads your display correctly and is the 3D system apps use.", run));
        if (Ctx.SelftestError is { } err) body.Children.Add(U.Notice(Level.Critical, "The check did not finish", err));
        if (st is null)
        {
            if (Ctx.SelftestError is null)
                body.Children.Add(U.Wrapped(Ctx.SelftestRunning ? "Checking (about 10 seconds)…" : "Not checked yet.", "empty"));
            return U.Card(body);
        }
        int failed = st.Checks.Count(c => !c.Ok);
        var badge = U.Badge(StatusText.SelftestVerdict(st.Checks.Count, failed), st.Passed ? Level.Ok : Level.Critical, tick: st.Passed);
        ToolTip.SetTip(badge, st.Verdict); // the CLI's own word
        var verdict = U.Flow(10, 4, badge, U.Text($"Checked at {st.When:HH:mm}", "meta"));
        body.Children.Add(verdict);
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

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Linq;
using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Media;
using DisplayXR.Dashboard.Model;
using DisplayXR.Dashboard.Ui;

namespace DisplayXR.Dashboard.Pages;

/// <summary>
/// Home: the runtime, its plug-ins, the GPU topology and the self-test (what
/// the ImGui panel's Overview showed), plus the one-line multi-screen summary
/// that links to Displays (design §8, the vendor dashboard's Home rule).
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
            string v = s?.Runtime.Version is { Length: > 0 } ver ? $"DisplayXR runtime {ver}" : "DisplayXR runtime";
            return s is null ? v : $"{v}{StatusText.Sep}{StatusText.Plural(s.Screens.Count, "monitor", "monitors")}{StatusText.Sep}{StatusText.Plural(s.Plugins.Count, "plug-in", "plug-ins")}";
        }
    }

    public override void OnShown()
    {
        _ = Ctx.LoadInfoAsync(force: false);
        _ = Ctx.LoadDpAsync();
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
            sb.Append(StatusText.HomeLine(s));
        }
        return sb.ToString();
    }

    protected override Control Build()
    {
        var s = Ctx.Feed.Snapshot;
        var page = U.VStack(18);

        if (Ctx.PreferredPlugin is { } pref)
            page.Children.Add(U.Notice(Level.Warn, $"Display-processor override active: '{pref}' is forced for every app",
                "It persists across reboots until it is reset (PreferredPlugin, machine-wide).",
                U.Button("Reset override", () => _ = Ctx.RunActionsAsync("dp", "dp reset"), "warn", "sm")));

        if (s is not null && StatusText.HomeLine(s) is { } line)
        {
            var badge = StatusText.DisplaysBadge(s);
            var level = badge?.Level ?? Level.Ok;
            page.Children.Add(U.Notice(level, line,
                badge is null ? "More than one screen is claimed; each one's state is on Displays."
                              : "At least one screen needs attention; the details are on Displays.",
                U.Button("Open Displays", () => Ctx.Navigate("displays"), "outline", "sm")));
        }

        var grid = new U.CardGrid(2) { MinColumnWidth = 440 };
        grid.Add(RuntimeCard(s));
        grid.Add(PluginsCard(s));
        grid.Add(GpuCard());
        grid.Add(SelftestCard());
        page.Children.Add(grid);
        return page;
    }

    private Control RuntimeCard(StatusSnapshot? s)
    {
        var info = Ctx.Info;
        var kv = new U.KvList(170);
        string version = s?.Runtime.Version is { Length: > 0 } v ? v : "—";
        string tag = s?.Runtime.GitTag is { Length: > 0 } t ? t : info?.GitTag ?? "";
        kv.Add("Version", tag.Length > 0 ? $"{version}  ({tag})" : version);
        int abi = s?.Runtime.PluginAbi ?? info?.PluginAbi ?? 0;
        kv.Add("Plug-in ABI", abi > 0 ? $"v{abi}" : "—");

        // Active OpenXR runtime: info's resolved key when read, else the snapshot's.
        string? active = info is { ActiveRuntimeQueried: true } ? (info.ActiveRuntimeSet ? info.ActiveRuntimeValue : null)
                       : s?.Runtime.ActiveOpenXrRuntime;
        bool isDxr = active is not null && active.Contains("DisplayXR", StringComparison.OrdinalIgnoreCase);
        var activeBlock = U.VStack(6, U.Wrapped(active ?? "<unset>", "value", "mono"));
        ((TextBlock)activeBlock.Children[0]).Foreground = isDxr ? Tokens.For(Level.Ok) : Tokens.For(Level.Critical);
        if ((info is not null || s is not null) && !isDxr)
            activeBlock.Children.Add(U.Button("Make DisplayXR the active OpenXR runtime",
                () => _ = Ctx.RunActionsAsync("runtime", "runtime activate"), "primary", "sm"));
        kv.Add("Active OpenXR runtime", activeBlock);

        string service = s is null ? (Ctx.Feed.IsRunning ? "connecting…" : "—")
            : s.IsService ? $"connected{StatusText.Sep}generation {s.Generation.Topology}/{s.Generation.Status}"
            : "not reached (headless snapshot)";
        kv.Add("Service", service, brush: s?.IsService == true ? Tokens.For(Level.Ok) : null);
        if (s is not null)
            kv.Add("Workspace", s.Workspace.Enabled ? $"enabled{(s.Workspace.Controller is { } c ? StatusText.Sep + c : "")}" : "off");
        if (info?.Device is { Length: > 0 } dev) kv.Add("Device", dev);

        var body = U.VStack(16, U.CardTitle("Runtime"), kv);
        if (Ctx.LastActionArea == "runtime" && Ctx.LastAction is { } a)
            body.Children.Add(U.Wrapped(a, Ctx.LastActionFailed ? "soft" : "desc"));
        return U.Card(body);
    }

    private Control PluginsCard(StatusSnapshot? s)
    {
        var body = U.VStack(14, U.CardTitle("Display-processor plug-ins",
            "Each registered plug-in with its platform state; a hint is the vendor's own words."));
        if (s is null || s.Plugins.Count == 0)
        {
            body.Children.Add(U.Wrapped(s is null ? "Waiting for the first status snapshot…" : "No plug-in is registered.", "empty"));
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
            var head = U.Flow(8, 4, name);
            if (p.Version.Length > 0) U.AddFlow(head, U.Text(p.Version, "meta"));
            var chips = U.Flow(6, 4,
                U.Chip(p.Load, p.Load == "ACTIVE" ? Level.Ok : Level.Plain),
                U.Chip(p.PlatformState, StatusText.PluginLevel(p) is Level.Ok or Level.Plain ? Level.Plain : StatusText.PluginLevel(p)));
            if (p.Fallback) U.AddFlow(chips, U.Chip("fallback"), 6, 4);
            U.AddFlow(chips, U.Text($"{p.Id}{StatusText.Sep}ProbeOrder {p.ProbeOrder}", "meta"), 6, 4);
            var col = U.VStack(2, head, chips);
            if (p.Hint.Length > 0) col.Children.Add(U.Wrapped(p.Hint, "soft"));
            var g = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
            g.Children.Add(dot);
            Grid.SetColumn(col, 1);
            g.Children.Add(col);
            list.Children.Add(g);
        }
        body.Children.Add(list);
        return U.Card(body);
    }

    private Control GpuCard()
    {
        var body = U.VStack(12, U.CardTitle("GPU topology",
            "Does the woven frame cross adapters to reach the panel? (#918, ADR-037)"));
        var info = Ctx.Info;
        if (info is null)
        {
            body.Children.Add(U.Wrapped(Ctx.InfoLoading ? "Reading the runtime's facts (displayxr-cli info, about 10 s)…"
                                                      : Ctx.InfoError ?? "Not read yet.", Ctx.InfoError is null ? "empty" : "soft"));
            return U.Card(body);
        }
        var g = info.Gpu;
        if (g is null || !g.Probed)
        {
            body.Children.Add(U.Wrapped($"Not probed{(g?.Note is { Length: > 0 } n ? " — " + n : " — Windows only")}.", "empty"));
            return U.Card(body);
        }
        if (g.Adapters.Count <= 1)
        {
            body.Children.Add(U.Notice(Level.Ok, "Single adapter: the weave never crosses GPUs.", g.Adapters.FirstOrDefault()?.Name ?? ""));
        }
        else
        {
            var list = U.VStack(10);
            foreach (var a in g.Adapters)
            {
                var head = U.Flow(8, 4, U.Text(a.Name, "value"));
                if (g.Scanout.Resolved && a.Luid == g.Scanout.Luid) U.AddFlow(head, U.Chip("panel scanout", Level.Info));
                if (g.Render.Resolved && a.Luid == g.Render.Luid) U.AddFlow(head, U.Chip("render (default)"));
                if (g.Ingest.Resolved && a.Luid == g.Ingest.Luid) U.AddFlow(head, U.Chip("service ingest"));
                list.Children.Add(U.VStack(1, head, U.Text($"LUID {a.Luid}{StatusText.Sep}{a.VramMb} MB dedicated", "meta")));
            }
            body.Children.Add(list);
            body.Children.Add(U.WarningRow(new StatusWarning(g.SplitApplies ? "SPLIT" : "LOCAL",
                g.SplitApplies ? WarningLevel.Warn : WarningLevel.Info, g.Verdict)));
        }
        // The configured half: read from the CLI child's own environment, so it
        // is drawn dimmed and says so (control-panel-performance-settings.md).
        bool fromEnv = g.WeaveOnScanoutSource == "env";
        body.Children.Add(U.Rule());
        body.Children.Add(U.Wrapped(
            $"DXR_WEAVE_ON_SCANOUT = {(g.WeaveOnScanoutSet ? g.WeaveOnScanout : "<unset>")} [{(g.WeaveOnScanoutSource.Length > 0 ? g.WeaveOnScanoutSource : "?")}]" +
            $"{(fromEnv ? "  (this dashboard's environment only)" : "")}{StatusText.Sep}ingress {(g.Ingress.Length > 0 ? g.Ingress : "?")}", "dev"));
        if (g.ServiceSplit.Length > 0) body.Children.Add(U.Wrapped(g.ServiceSplit, "desc"));
        body.Children.Add(U.Wrapped("A running app's real placement is the 'weave placement:' line in its log (%LOCALAPPDATA%\\DisplayXR\\DisplayXR_<exe>.*.log), or its integrity row on Windows.", "desc"));
        return U.Card(body);
    }

    private Control SelftestCard()
    {
        var st = Ctx.Selftest;
        var run = U.Button(Ctx.SelftestRunning ? "Running…" : (st is null ? "Run self-test" : "Run again"),
            () => _ = Ctx.RunSelftestAsync(), "primary", "sm");
        run.IsEnabled = !Ctx.SelftestRunning;
        var body = U.VStack(12, U.CardTitle("Self-test",
            "Plug-in discovery, ABI, display dimensions, DPI awareness and the active-runtime key, headless.", run));
        if (Ctx.SelftestError is { } err) body.Children.Add(U.Notice(Level.Critical, "The self-test did not finish", err));
        if (st is null)
        {
            if (Ctx.SelftestError is null)
                body.Children.Add(U.Wrapped(Ctx.SelftestRunning ? "Running displayxr-cli selftest (about 10 s)…" : "Not run in this session.", "empty"));
            return U.Card(body);
        }
        int failed = st.Checks.Count(c => !c.Ok);
        var verdict = U.Flow(10, 4,
            U.Badge(st.Verdict, st.Passed ? Level.Ok : Level.Critical, tick: st.Passed),
            U.Text($"{st.Checks.Count} checks{(failed > 0 ? $", {failed} failed" : "")}{StatusText.Sep}{st.When:HH:mm:ss}", "meta"));
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

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Linq;
using System.Text;
using Avalonia.Controls;
using DisplayXR.Dashboard.Model;
using DisplayXR.Dashboard.Ui;

namespace DisplayXR.Dashboard.Pages;

/// <summary>
/// Troubleshooting's "Graphics cards" part: the graphics pipeline's technical lines that sat behind
/// Home's "Technical details" toggle (adapter ids, the runtime's verdict, the configured setting and
/// where it came from, where an app's log says what it really did). Home keeps the plain card.
/// </summary>
public sealed class GraphicsDetailsPage : Page
{
    public GraphicsDetailsPage() : base("graphics", "Graphics cards") { }

    public override bool HoldsFeed => false;

    public override string Caption => "How the 3D picture travels between graphics cards, for developers and support.";

    public override void OnShown() => _ = Ctx.LoadInfoAsync(force: false);

    public override void OnRefresh() => _ = Ctx.LoadInfoAsync(force: true);

    protected override string Key()
    {
        var sb = new StringBuilder();
        sb.Append(Ctx.InfoLoading).Append('|').Append(Ctx.InfoError).Append('|').Append(Ctx.Info is null);
        if (Ctx.Info?.Gpu is { } g)
        {
            sb.Append(g.Probed).Append(g.Verdict).Append(g.SplitApplies).Append(g.WeaveOnScanout).Append(g.WeaveOnScanoutSource)
              .Append(g.Ingress).Append(g.ServiceSplit).Append(g.Note);
            foreach (var a in g.Adapters) sb.Append(a.Name).Append(a.Luid).Append(a.VramMb).Append(';');
        }
        return sb.ToString();
    }

    protected override Control Build()
    {
        var body = U.VStack(12, U.CardTitle("Graphics pipeline",
            "Each graphics card's id, whether the picture is split across two cards, and the setting that decides it."));
        var info = Ctx.Info;
        if (info is null)
        {
            body.Children.Add(U.Wrapped(Ctx.InfoLoading ? "Looking at your graphics cards (about 10 seconds)…"
                                                      : Ctx.InfoError ?? "Not read yet.", Ctx.InfoError is null ? "empty" : "soft"));
            return U.VStack(18, U.Card(body));
        }
        var g = info.Gpu;
        if (g is null || !g.Probed)
        {
            body.Children.Add(U.Wrapped($"Not checked{(g?.Note is { Length: > 0 } n ? ": " + n : " (Windows only)")}.", "empty"));
            return U.VStack(18, U.Card(body));
        }
        var tech = U.VStack(6);
        foreach (var a in g.Adapters) tech.Children.Add(U.Wrapped($"{a.Name}{StatusText.Sep}LUID {a.Luid}{StatusText.Sep}{a.VramMb} MB dedicated", "dev"));
        if (g.Adapters.Count > 1) tech.Children.Add(U.Wrapped($"{(g.SplitApplies ? "SPLIT" : "LOCAL")}: {g.Verdict}", "dev"));
        // The configured half: read from the CLI child's own environment (control-panel-performance-settings.md).
        bool fromEnv = g.WeaveOnScanoutSource == "env";
        tech.Children.Add(U.Wrapped(
            $"DXR_WEAVE_ON_SCANOUT = {(g.WeaveOnScanoutSet ? g.WeaveOnScanout : "<unset>")} [{(g.WeaveOnScanoutSource.Length > 0 ? g.WeaveOnScanoutSource : "?")}]" +
            $"{(fromEnv ? "  (this dashboard's environment only)" : "")}{StatusText.Sep}ingress {(g.Ingress.Length > 0 ? g.Ingress : "?")}", "dev"));
        if (g.ServiceSplit.Length > 0) tech.Children.Add(U.Wrapped(g.ServiceSplit, "desc"));
        tech.Children.Add(U.Wrapped("A running app's real placement is the 'weave placement:' line in its log (%LOCALAPPDATA%\\DisplayXR\\DisplayXR_<exe>.*.log), or its integrity row under Running 3D apps.", "desc"));
        body.Children.Add(tech);
        return U.VStack(18, U.Card(body));
    }
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Linq;
using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using DisplayXR.Dashboard.Model;
using DisplayXR.Dashboard.Ui;

namespace DisplayXR.Dashboard.Pages;

/// <summary>
/// Performance (#1252): the three shipped controls — Target GPU, Mode,
/// Diagnostics — over <c>displayxr-cli perf</c>, the effective state with its
/// provenance, and the amber "non-default settings" banner with reset. Every
/// write goes through the CLI and the state shown is re-read afterwards.
/// </summary>
public sealed class PerformancePage : Page
{
    public PerformancePage() : base("performance", "Performance") { }

    public override bool HoldsFeed => false;

    public override string Caption =>
        "Settings each app reads at start-up. They apply to apps started after a change; a running app keeps what it started with.";

    public override void OnShown()
    {
        _ = Ctx.LoadPerfAsync(force: false);
        _ = Ctx.LoadInfoAsync(force: false); // the adapter list for Target GPU
    }

    protected override string Key()
    {
        var sb = new StringBuilder();
        sb.Append(Ctx.ActionRunning).Append('|').Append(Ctx.LastAction).Append('|').Append(Ctx.PerfError).Append('|')
          .Append(Ctx.InfoLoading).Append('|').Append(Ctx.HasMultipleAdapters).Append('|');
        if (Ctx.Perf is { } p)
        {
            sb.Append(p.UserFile).Append(p.UserWritten);
            foreach (var l in p.Levers) sb.Append(l).Append(';');
        }
        if (Ctx.Info?.Gpu is { } g) foreach (var a in g.Adapters) sb.Append(a.Name);
        return sb.ToString();
    }

    protected override Control Build()
    {
        var page = U.VStack(18);
        var perf = Ctx.Perf;
        if (perf is null)
        {
            page.Children.Add(Ctx.PerfError is { } err
                ? U.Notice(Level.Critical, "Could not read the performance settings", err)
                : U.Card(U.Wrapped("Reading the performance settings…", "empty")));
            return page;
        }

        if (perf.AnyNonDefault)
        {
            var lines = U.VStack(2);
            foreach (var l in perf.Levers.Where(l => l.IsSet))
                lines.Children.Add(U.Wrapped($"{l.Name} = {l.Value}   [{l.Source}]", "dev"));
            var reset = U.Button("Reset all performance settings", () => _ = Ctx.RunActionsAsync("perf", "perf reset"), "warn", "sm");
            reset.IsEnabled = !Ctx.ActionRunning;
            var notice = U.Notice(Level.Warn,
                $"Non-default performance settings are in force{(perf.UserWritten.Length > 0 ? " since " + perf.UserWritten : "")}",
                "Values marked [env] come from an environment variable and cannot be reset from here.", reset);
            ((Grid)notice.Child!).Children.OfType<StackPanel>().First().Children.Insert(1, lines);
            page.Children.Add(notice);
        }

        if (Ctx.LastActionArea == "perf" && Ctx.LastAction is { } a)
            page.Children.Add(U.Notice(Ctx.LastActionFailed ? Level.Critical : Level.Info, Ctx.LastActionFailed ? "The change did not apply" : "Applied", a));

        var grid = new U.CardGrid(2) { MinColumnWidth = 420 };
        grid.Add(TargetGpuCard(perf));
        grid.Add(ModeCard(perf));
        grid.Add(DiagnosticsCard(perf));
        grid.Add(EffectiveCard(perf));
        page.Children.Add(grid);
        return page;
    }

    private RadioButton Option(string group, string label, bool isChecked, Action onPick)
    {
        var rb = new RadioButton { GroupName = group, IsChecked = isChecked, IsEnabled = !Ctx.ActionRunning };
        rb.Content = U.Wrapped(label);
        rb.Classes.Add("option");
        rb.IsCheckedChanged += (_, _) =>
        {
            if (rb.IsChecked == true && !isChecked && !Ctx.ActionRunning) onPick();
        };
        return rb;
    }

    private Control TargetGpuCard(PerfState perf)
    {
        var body = U.VStack(10, U.CardTitle("Target GPU", "Which adapter apps render on. The panel's adapter keeps the weave local to the display."));
        if (Ctx.Info is null)
        {
            body.Children.Add(U.Wrapped(Ctx.InfoLoading ? "Reading the adapters (displayxr-cli info, about 10 s)…" : Ctx.InfoError ?? "Adapters not read.", "empty"));
            return U.Card(body);
        }
        if (!Ctx.HasMultipleAdapters)
        {
            body.Children.Add(U.Wrapped("This machine has one GPU: there is nothing to choose.", "empty"));
            return U.Card(body);
        }
        string now = perf.TargetGpu;
        (string Label, string Value)[] opts =
        {
            ("Auto (recommended)", ""), ("Panel's display adapter", "scanout"),
            ("High performance (discrete)", "dgpu"), ("Power saving (integrated)", "igpu"),
        };
        string group = "gpu" + Guid.NewGuid().ToString("N");
        foreach (var (label, value) in opts)
        {
            bool active = value.Length == 0 ? now.Length == 0 : now == value;
            body.Children.Add(Option(group, label, active, () =>
            {
                if (value.Length == 0)
                    _ = Ctx.RunActionsAsync("perf", "perf reset DXR_D3D_FORCE_GPU", "perf reset DXR_VK_FORCE_GPU");
                else
                    _ = Ctx.RunActionsAsync("perf", $"perf set DXR_D3D_FORCE_GPU {value}", $"perf set DXR_VK_FORCE_GPU {value}");
            }));
        }
        if (perf.SourceOf("DXR_D3D_FORCE_GPU") == "env")
            body.Children.Add(U.WarningRow(new StatusWarning("ENV", WarningLevel.Warn, "An environment variable sets this and outranks the dashboard.")));
        return U.Card(body);
    }

    private Control ModeCard(PerfState perf)
    {
        bool compat = perf.CompatibilityMode;
        string group = "mode" + Guid.NewGuid().ToString("N");
        var body = U.VStack(10, U.CardTitle("Mode", "For “the 3D looks wrong — is it the pipeline?”"),
            Option(group, "Balanced (default)", !compat, () =>
                _ = Ctx.RunActionsAsync("perf", "perf reset DXR_WEAVE_ON_SCANOUT", "perf reset DXR_WEAVE_REPAINT")),
            Option(group, "Compatibility", compat, () =>
                _ = Ctx.RunActionsAsync("perf", "perf set DXR_WEAVE_ON_SCANOUT 0", "perf set DXR_WEAVE_REPAINT 0")),
            U.Wrapped("Compatibility turns off the cross-adapter weave split and the repaint. It costs latency; it is not a faster setting. Leave it on Balanced unless you are diagnosing. Under the workspace / shell only the split half applies: the service weaves on its own clock, so its repaint is structural rather than a setting.", "desc"));
        return U.Card(body);
    }

    private Control DiagnosticsCard(PerfState perf)
    {
        bool on = perf.DiagnosticsOn;
        string group = "diag" + Guid.NewGuid().ToString("N");
        var body = U.VStack(10, U.CardTitle("Diagnostics", "For bug reports."),
            Option(group, "Off", !on, () =>
                _ = Ctx.RunActionsAsync("perf", "perf reset DXR_FRAME_WITNESS", "perf reset DXR_FRAME_STAGE_TIMING")),
            Option(group, "On (frame witness + stage timing)", on, () =>
                _ = Ctx.RunActionsAsync("perf", "perf set DXR_FRAME_WITNESS 5", "perf set DXR_FRAME_STAGE_TIMING 1")),
            U.Wrapped("Adds frame / weave / present counters to each app's log. Changes no behaviour.", "desc"));
        return U.Card(body);
    }

    private Control EffectiveCard(PerfState perf)
    {
        var body = U.VStack(12, U.CardTitle("Effective state",
            "Resolved as a process starting now would resolve it: environment first, then the per-user file, then the machine default."));
        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto,Auto") };
        g.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        string[] heads = { "Lever", "Value", "Source" };
        for (int i = 0; i < heads.Length; i++)
        {
            var h = U.Text(heads[i], "meta");
            h.Margin = new Thickness(0, 0, i < 2 ? 18 : 0, 4);
            Grid.SetColumn(h, i);
            g.Children.Add(h);
        }
        int row = 1;
        foreach (var l in perf.Levers)
        {
            g.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
            var name = U.Wrapped(l.Name, "mono");
            var value = U.Text(l.Value ?? "default", "mono");
            value.Foreground = l.IsSet ? Tokens.For(Level.Warn) : Tokens.Muted;
            var src = U.Chip(l.Source, l.Source == "env" ? Level.Warn : l.Source == "default" ? Level.Plain : Level.Info);
            name.Margin = new Thickness(0, 3, 18, 3);
            value.Margin = new Thickness(0, 3, 18, 3);
            src.Margin = new Thickness(0, 2, 0, 2);
            value.VerticalAlignment = VerticalAlignment.Center;
            name.VerticalAlignment = VerticalAlignment.Center;
            Grid.SetRow(name, row); Grid.SetRow(value, row); Grid.SetRow(src, row);
            Grid.SetColumn(value, 1); Grid.SetColumn(src, 2);
            g.Children.Add(name); g.Children.Add(value); g.Children.Add(src);
            row++;
        }
        body.Children.Add(g);
        if (perf.UserFile.Length > 0)
            body.Children.Add(U.Wrapped($"Per-user file: {perf.UserFile}{(perf.UserWritten.Length > 0 ? $" (written {perf.UserWritten})" : "")}", "desc"));
        return U.Card(body);
    }
}

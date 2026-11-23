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
/// Performance (#1252): the three shipped controls — "Graphics card for 3D apps" (Target GPU),
/// "If 3D looks wrong" (Mode), "Detailed logging" (Diagnostics) — over <c>displayxr-cli perf</c>, the effective state with its
/// provenance, and the amber "non-default settings" banner with reset. Every
/// write goes through the CLI and the state shown is re-read afterwards.
/// </summary>
public sealed class PerformancePage : Page
{
    public PerformancePage() : base("performance", "Performance") { }

    public override bool HoldsFeed => false;

    public override string Caption =>
        "Settings 3D apps read when they start. A change applies to apps you start afterwards; a running app keeps what it started with.";

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
                ? U.Notice(Level.Critical, "Couldn't read these settings", err)
                : U.Card(U.Wrapped("Reading the settings…", "empty")));
            return page;
        }

        if (perf.AnyNonDefault)
        {
            var lines = U.VStack(2);
            foreach (var l in perf.Levers.Where(l => l.IsSet))
                lines.Children.Add(U.Wrapped($"{l.Name} = {l.Value}   [{l.Source}]", "dev"));
            var reset = U.Button("Reset all to defaults", () => _ = Ctx.RunActionsAsync("perf", CliArgs.Words("perf reset")), "warn", "sm");
            reset.IsEnabled = !Ctx.ActionRunning;
            var notice = U.Notice(Level.Warn,
                $"Some settings are changed from their defaults{(perf.UserWritten.Length > 0 ? " (since " + perf.UserWritten + ")" : "")}",
                "Values marked [env] are set outside the Dashboard and cannot be reset from here.", reset);
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
        var body = U.VStack(10, U.CardTitle("Graphics card for 3D apps",
            "Which graphics card draws 3D apps. Choosing the one your 3D display is plugged into keeps the whole picture on one card."));
        if (Ctx.Info is null)
        {
            body.Children.Add(U.Wrapped(Ctx.InfoLoading ? "Looking at your graphics cards (about 10 seconds)…" : Ctx.InfoError ?? "Graphics cards not read.", "empty"));
            return U.Card(body);
        }
        if (!Ctx.HasMultipleAdapters)
        {
            body.Children.Add(U.Wrapped("This PC has one graphics card, so there is nothing to choose.", "empty"));
            return U.Card(body);
        }
        string now = perf.TargetGpu;
        (string Label, string Value)[] opts =
        {
            ("Automatic (recommended)", ""), ("The one your 3D display is plugged into", "scanout"),
            ("High performance (separate graphics card)", "dgpu"), ("Power saving (built-in graphics)", "igpu"),
        };
        string group = "gpu" + Guid.NewGuid().ToString("N");
        foreach (var (label, value) in opts)
        {
            bool active = value.Length == 0 ? now.Length == 0 : now == value;
            body.Children.Add(Option(group, label, active, () =>
            {
                if (value.Length == 0)
                    _ = Ctx.RunActionsAsync("perf", CliArgs.Words("perf reset DXR_D3D_FORCE_GPU"), CliArgs.Words("perf reset DXR_VK_FORCE_GPU"));
                else
                    _ = Ctx.RunActionsAsync("perf", new CliArgs("perf", "set", "DXR_D3D_FORCE_GPU", value), new CliArgs("perf", "set", "DXR_VK_FORCE_GPU", value));
            }));
        }
        if (perf.SourceOf("DXR_D3D_FORCE_GPU") == "env")
            body.Children.Add(U.WarningRow(new StatusWarning("ENV", WarningLevel.Warn, "A setting outside the Dashboard (an environment variable) chooses this, and it wins over the choice here.")));
        return U.Card(body);
    }

    private Control ModeCard(PerfState perf)
    {
        bool compat = perf.CompatibilityMode;
        string group = "mode" + Guid.NewGuid().ToString("N");
        var body = U.VStack(10, U.CardTitle("If 3D looks wrong", "A switch for finding a problem."),
            Option(group, "Normal (recommended)", !compat, () =>
                _ = Ctx.RunActionsAsync("perf", CliArgs.Words("perf reset DXR_WEAVE_ON_SCANOUT"), CliArgs.Words("perf reset DXR_WEAVE_REPAINT"))),
            Option(group, "Compatibility mode", compat, () =>
                _ = Ctx.RunActionsAsync("perf", CliArgs.Words("perf set DXR_WEAVE_ON_SCANOUT 0"), CliArgs.Words("perf set DXR_WEAVE_REPAINT 0"))),
            U.Wrapped("Compatibility mode turns off two ways DisplayXR keeps the 3D picture quick to follow you. It does not make anything faster: the picture can respond more slowly. Turn it on only while finding a problem, then set it back to Normal. On the spatial desktop only one of the two applies.", "desc"));
        return U.Card(body);
    }

    private Control DiagnosticsCard(PerfState perf)
    {
        bool on = perf.DiagnosticsOn;
        string group = "diag" + Guid.NewGuid().ToString("N");
        var body = U.VStack(10, U.CardTitle("Detailed logging", "For bug reports."),
            Option(group, "Off", !on, () =>
                _ = Ctx.RunActionsAsync("perf", CliArgs.Words("perf reset DXR_FRAME_WITNESS"), CliArgs.Words("perf reset DXR_FRAME_STAGE_TIMING"))),
            Option(group, "On", on, () =>
                _ = Ctx.RunActionsAsync("perf", CliArgs.Words("perf set DXR_FRAME_WITNESS 5"), CliArgs.Words("perf set DXR_FRAME_STAGE_TIMING 1"))),
            U.Wrapped("Each 3D app writes extra timing details to its log, for a bug report. It changes nothing else.", "desc"));
        return U.Card(body);
    }

    private Control EffectiveCard(PerfState perf)
    {
        var body = U.VStack(12, U.CardTitle("Current values",
            "What a 3D app starting now would use: a setting outside the Dashboard first, then yours, then this PC's default."));
        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto,Auto") };
        g.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        string[] heads = { "Setting", "Value", "Comes from" };
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
            var src = U.Chip(SourceWord(l.Source), l.Source == "env" ? Level.Warn : l.Source == "default" ? Level.Plain : Level.Info);
            ToolTip.SetTip(src, l.Source); // the CLI's own word
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
            body.Children.Add(U.Wrapped($"Your settings file: {perf.UserFile}{(perf.UserWritten.Length > 0 ? $" (written {perf.UserWritten})" : "")}", "desc"));
        return U.Card(body);
    }

    /// <summary>Where a value comes from, in plain words (env / user / machine / default).</summary>
    public static string SourceWord(string source) => source switch
    {
        "env" => "outside setting",
        "user" => "you",
        "machine" => "this PC",
        "default" => "default",
        var other => other,
    };
}

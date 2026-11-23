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
/// Settings (page id "settings"; was Performance, #1252): what an owner can change. "Graphics card
/// for 3D apps" (Target GPU; hidden on a PC with one graphics card), the spatial desktop's shortcut
/// and "Open now" (the same controls as its row on Components, <see cref="WorkspaceLaunchEditor"/>;
/// hidden when no spatial desktop is installed), and "If 3D looks wrong" (Compatibility mode,
/// Detailed logging), over <c>displayxr-cli perf</c> and <c>workspace</c>. Every write goes through
/// the CLI and the state shown is re-read afterwards. The table of current values, by variable name,
/// is in Troubleshooting.
/// </summary>
public sealed class SettingsPage : Page
{
    private WorkspaceLaunchEditor? _launchEditor;
    private WorkspaceLaunchEditor Launch => _launchEditor ??= new WorkspaceLaunchEditor(Ctx, force => Update(force));

    public SettingsPage() : base("settings", "Settings") { }

    public override bool HoldsFeed => false;

    public override string Caption =>
        "What you can change. A change applies to 3D apps you start afterwards; a running app keeps what it started with.";

    protected override bool DeferRebuild => Launch.Busy;

    public override void OnShown()
    {
        _ = Ctx.LoadPerfAsync(force: false);
        _ = Ctx.LoadInfoAsync(force: false); // the graphics cards
        _ = Ctx.LoadWorkspaceAsync();        // the spatial desktop's shortcut
    }

    public override void OnHidden() => Launch.OnHidden();

    public override void OnRefresh()
    {
        _ = Ctx.LoadPerfAsync(force: true);
        _ = Ctx.LoadWorkspaceAsync(force: true);
    }

    protected override void UpdateInPlace() => Launch.UpdateInPlace();

    protected override string Key()
    {
        var sb = new StringBuilder();
        sb.Append(Ctx.ActionRunning).Append('|').Append(Ctx.LastActionArea).Append(Ctx.LastAction).Append(Ctx.LastActionFailed).Append('|')
          .Append(Ctx.PerfError).Append('|').Append(Ctx.InfoLoading).Append('|').Append(Ctx.HasMultipleAdapters).Append('|')
          .Append(Ctx.Info is null).Append(Launch.Key).Append('|');
        if (Ctx.Perf is { } p)
        {
            sb.Append(p.UserFile).Append(p.UserWritten);
            foreach (var l in p.Levers) sb.Append(l).Append(';');
        }
        if (Ctx.Info?.Gpu is { } g) foreach (var a in g.Adapters) sb.Append(a.Name);
        foreach (var c in Controllers()) sb.Append(c.Id).Append(c.Name).Append(c.Connected).Append(c.Launch).Append(';');
        return sb.ToString();
    }

    /// <summary>The installed spatial desktops that have launch controls (<c>workspace list --json</c>).</summary>
    private RegisteredController[] Controllers() =>
        (Ctx.Workspace?.Controllers ?? Array.Empty<RegisteredController>()).Where(c => c.Launch is not null).ToArray();

    protected override Control Build()
    {
        Launch.BeginBuild();
        var page = U.VStack(18);
        var perf = Ctx.Perf;

        if (perf is not null && perf.AnyNonDefault)
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
        if (perf is not null && TargetGpuCard(perf) is { } gpu) grid.Add(gpu);
        if (SpatialDesktopCard() is { } desk) grid.Add(desk);
        if (perf is not null) grid.Add(Looks3DWrongCard(perf));
        else
            grid.Add(Ctx.PerfError is { } err
                ? U.Notice(Level.Critical, "Couldn't read these settings", err)
                : U.Card(U.Wrapped("Reading the settings…", "empty")));
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

    /// <summary>Which graphics card draws 3D apps. Null (no card) on a PC with one graphics card: nothing to choose.</summary>
    private Control? TargetGpuCard(PerfState perf)
    {
        var body = U.VStack(10, U.CardTitle("Graphics card for 3D apps",
            "Which graphics card draws 3D apps. Choosing the one your 3D display is plugged into keeps the whole picture on one card."));
        if (Ctx.Info is null)
        {
            if (!Ctx.InfoLoading && Ctx.InfoError is null) return null;
            body.Children.Add(U.Wrapped(Ctx.InfoLoading ? "Looking at your graphics cards (about 10 seconds)…" : Ctx.InfoError!, "empty"));
            return U.Card(body);
        }
        if (!Ctx.HasMultipleAdapters) return null;
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

    /// <summary>
    /// The spatial desktop's shortcut and "Open now": the same controls as its row on Components.
    /// Null when no spatial desktop with launch controls is installed (or the CLI has no launch block).
    /// </summary>
    private Control? SpatialDesktopCard()
    {
        var controllers = Controllers();
        if (controllers.Length == 0) return null;
        var body = U.VStack(10, U.CardTitle("Spatial desktop",
            "Arrange several 3D apps together on your display. Choose the shortcut that opens it, or open it now."));
        foreach (var c in controllers)
        {
            if (controllers.Length > 1) body.Children.Add(U.Text(c.Name.Length > 0 ? c.Name : c.Id, "value"));
            body.Children.Add(Launch.Build(c));
        }
        return U.Card(body);
    }

    /// <summary>"If 3D looks wrong": two switches for finding a problem, one card.</summary>
    private Control Looks3DWrongCard(PerfState perf)
    {
        bool compat = perf.CompatibilityMode;
        string modeGroup = "mode" + Guid.NewGuid().ToString("N");
        var compatTitle = U.Text("Compatibility mode", "value");
        compatTitle.Margin = new Thickness(0, 4, 0, 0);
        var body = U.VStack(10, U.CardTitle("If 3D looks wrong", "Two switches for finding a problem. Turn them on only while you look for it."),
            compatTitle,
            Option(modeGroup, "Normal (recommended)", !compat, () =>
                _ = Ctx.RunActionsAsync("perf", CliArgs.Words("perf reset DXR_WEAVE_ON_SCANOUT"), CliArgs.Words("perf reset DXR_WEAVE_REPAINT"))),
            Option(modeGroup, "Compatibility mode", compat, () =>
                _ = Ctx.RunActionsAsync("perf", CliArgs.Words("perf set DXR_WEAVE_ON_SCANOUT 0"), CliArgs.Words("perf set DXR_WEAVE_REPAINT 0"))),
            U.Wrapped("Compatibility mode turns off two ways DisplayXR keeps the 3D picture quick to follow you. It does not make anything faster: the picture can respond more slowly. Turn it on only while finding a problem, then set it back to Normal. On the spatial desktop only one of the two applies.", "desc"));

        bool on = perf.DiagnosticsOn;
        string diagGroup = "diag" + Guid.NewGuid().ToString("N");
        var logTitle = U.Text("Detailed logging", "value");
        logTitle.Margin = new Thickness(0, 10, 0, 0);
        body.Children.Add(logTitle);
        body.Children.Add(Option(diagGroup, "Off", !on, () =>
            _ = Ctx.RunActionsAsync("perf", CliArgs.Words("perf reset DXR_FRAME_WITNESS"), CliArgs.Words("perf reset DXR_FRAME_STAGE_TIMING"))));
        body.Children.Add(Option(diagGroup, "On", on, () =>
            _ = Ctx.RunActionsAsync("perf", CliArgs.Words("perf set DXR_FRAME_WITNESS 5"), CliArgs.Words("perf set DXR_FRAME_STAGE_TIMING 1"))));
        body.Children.Add(U.Wrapped("For bug reports. Each 3D app writes extra timing details to its log. It changes nothing else.", "desc"));
        return U.Card(body);
    }
}

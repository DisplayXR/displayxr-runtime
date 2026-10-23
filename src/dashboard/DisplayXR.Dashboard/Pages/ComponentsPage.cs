// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using Avalonia.Input;
using Avalonia.Media;
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
/// </summary>
public sealed class ComponentsPage : Page
{
    private readonly HashSet<string> _expanded = new();

    public ComponentsPage() : base("components", "Components") { }

    public override string Caption => "What is plugged into the runtime, by role: registered and connected.";

    public override void OnShown()
    {
        _ = Ctx.LoadInfoAsync(force: false);
        _ = Ctx.LoadWorkspaceAsync();
        // ADR-054 N3e: the policy rows behind the Enabled / Preferred controls.
        foreach (var c in ComponentClasses.All) _ = Ctx.LoadPolicyAsync(c);
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

    protected override bool DeferRebuild => _capturing is not null || _openDropDowns > 0;

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
    }

    protected override void UpdateInPlace()
    {
        // Known only after the first "on" came back: shown without a rebuild.
        if (_suspendNote is not null) _suspendNote.IsVisible = _capturing is not null && Ctx.HotkeySuspend.Unsupported;
    }

    private IReadOnlyList<ComponentSection> Sections() =>
        Components.Build(Ctx.Feed.Snapshot, Ctx.Info?.Components, Ctx.Workspace, Ctx.Policies);

    protected override string Key()
    {
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
        sb.Append(_machine).Append(_calibration is null).Append('|');
        foreach (var s in Sections())
        {
            sb.Append(s.Id).Append(s.Count).Append(s.Connected).Append(s.ConnectedLine);
            foreach (var i in s.Items) sb.Append(i.Name).Append(i.Level).Append(string.Join("/", i.Chips)).Append(string.Join("/", i.Lines)).Append(i.Controller?.Launch).Append(i.Policy);
            sb.Append(';');
        }
        return sb.ToString();
    }

    protected override Control Build()
    {
        _openDropDowns = 0;
        _suspendNote = null;
        var page = U.VStack(18);
        if (Ctx.Info is null)
            page.Children.Add(U.Notice(Ctx.InfoError is null ? Level.Info : Level.Warn,
                Ctx.InfoError is null ? "Reading the runtime's components" : "Could not read the runtime's components",
                Ctx.InfoError ?? "Input providers and the conversion module come from 'displayxr-cli info' (about 10 s); connected clients come from the live feed."));
        var grid = new U.CardGrid(2) { MinColumnWidth = 460 };
        foreach (var s in Sections()) grid.Add(SectionCard(s), s.Id is "display_processors" or "workspace_controller" ? 2 : 1);
        page.Children.Add(grid);
        return page;
    }

    private Control SectionCard(ComponentSection s)
    {
        var count = U.Badge(s.Count.ToString(System.Globalization.CultureInfo.InvariantCulture),
                            s.Connected > 0 ? Level.Ok : Level.Plain);
        var title = U.Flow(10, 4, U.Text(s.Title, "group"), count);
        var head = U.VStack(4, title, U.Wrapped(s.ConnectedLine, "dim"), U.Wrapped(s.Role, "desc"));
        var body = U.VStack(14, head);
        // ADR-054 N3e: HKLM Policy\Lock makes per-user writes of this class inert; say so once, on the class.
        if (PolicyList.ClassOfSection(s.Id) is { } sc && Ctx.Policies.TryGetValue(sc, out var spl) && spl.LockText is { } lockText)
            body.Children.Add(U.Notice(Level.Info, "Locked by machine policy", lockText));

        bool collapsed = s.Collapsed && !_expanded.Contains(s.Id);
        if (s.IsEmpty)
        {
            body.Children.Add(U.Wrapped(EmptyText(s), "empty"));
        }
        else if (collapsed)
        {
            body.Children.Add(U.Button($"Show {StatusText.Plural(s.Items.Count, "client", "clients")}", () => Toggle(s.Id), "link"));
        }
        else
        {
            var list = U.VStack(0);
            for (int i = 0; i < s.Items.Count; i++)
            {
                if (i > 0) list.Children.Add(new Border { Height = 12 });
                list.Children.Add(ItemRow(s.Items[i]));
                if (s.Items[i].Controller is { Launch: not null } rc) list.Children.Add(LaunchRow(rc));
                if (s.Items[i].Policy is { } pr && Ctx.Policies.TryGetValue(pr.Class, out var pl) && pl.Writable) list.Children.Add(PolicyControls(pr, pl));
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
        var col = U.VStack(8, U.Text("Per screen", "group"),
            U.Wrapped("Whose eyes the views on each screen follow. A display that tracks the viewer always keeps its own eyes; a provider serves a screen only when its display allows an external viewer (policy EXTERNAL_OK).", "desc"));
        var rows = TrackingScreens();
        if (rows.Count == 0)
            col.Children.Add(U.Wrapped("No screen reported yet.", "empty"));
        foreach (var r in rows)
        {
            var head = U.Flow(10, 6, U.Dot(r.Level), U.Text(r.Name, "value"));
            if (r.Primary) U.AddFlow(head, U.Chip("runtime default"), 10, 6);
            U.AddFlow(head, U.Chip(r.Source, r.Kind == ViewerSourceKind.Provider ? Level.Ok : Level.Plain), 10, 6);
            var (pt, pl) = r.PolicyChip;
            U.AddFlow(head, U.Chip(pt, pl), 10, 6);
            if (r.CanCalibrate)
            {
                // Camera-pose calibration (tracking-provider-discovery.md §6; design: displayxr-tracking-open
                // docs/design.md §10): a full-screen window on THIS screen that streams
                // `tracking calibrate --screen <key> --watch --json`, solves and applies.
                var screen = Ctx.Feed.Snapshot?.ScreenById(r.ScreenId);
                string key = screen?.Key is { Length: > 0 } k ? k : r.ScreenId;
                var calibrate = U.Button("Calibrate camera…", () => OpenCalibration(key, screen, r.Name), "quiet", "sm");
                calibrate.IsEnabled = _calibration is null;
                ToolTip.SetShowOnDisabled(calibrate, true);
                ToolTip.SetTip(calibrate, _calibration is null
                    ? "Measure where the camera sits relative to this screen, using the screen's reflection, so the tracked viewer lines up with the display."
                    : "A calibration window is already open.");
                Avalonia.Automation.AutomationProperties.SetName(calibrate, $"Calibrate camera for {r.Name}");
                U.AddFlow(head, calibrate, 10, 6);
            }
            var block = U.VStack(4, head, U.Wrapped(r.Reason, "soft"));
            var box = new Border { Child = block, Margin = new Thickness(0, 6, 0, 0) };
            box.Classes.Add("field");
            col.Children.Add(box);
        }
        if (Ctx.Info?.Components.Tracking is { } sum && sum.ScanLine is { Length: > 0 } scan)
            col.Children.Add(U.Wrapped($"Probe (headless, from 'displayxr-cli info'): {scan}.", "meta"));
        return col;
    }

    private CalibrationWindow? _calibration;

    /// <summary>
    /// Open the calibration window for one provider-served screen (one at a time). After a successful
    /// apply the tracking rows and info's tracking block are re-read.
    /// </summary>
    private void OpenCalibration(string screenKey, Screen? screen, string name)
    {
        if (_calibration is not null) { _calibration.Activate(); return; }
        var w = new CalibrationWindow(Ctx.Cli, screenKey, screen, name, () =>
        {
            _ = Ctx.LoadPolicyAsync(ComponentClass.Tracking, force: true);
            _ = Ctx.LoadInfoAsync(force: true);
            Ctx.Toast($"Camera calibration saved for {name}.");
        });
        w.Closed += (_, _) => { _calibration = null; Update(); };
        _calibration = w;
        DashboardLog.Info($"calibration window opened for screen {screenKey}");
        w.Show();
        Update();
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
            U.Wrapped("Which input provider each app uses, per role. A pin makes the app prefer a provider for every role it supplies; other roles, and an absent provider, fall back to the default order.", "desc"));
        if (list.PerAppLockText is { } lt) col.Children.Add(U.Notice(Level.Info, "Locked by machine policy", lt));
        if (list.ClientsDropped is > 0 and var dropped)
            col.Children.Add(U.Notice(Level.Warn, "Some apps are not resolved per app",
                $"{dropped:0} client(s) exceeded the service's per-client table: they use the default order and their pins do not apply."));
        var rows = list.PerAppRows();
        if (rows.Count == 0)
        {
            col.Children.Add(U.Wrapped(list.Holders is null
                ? "No per-app pins. Running apps are listed when the DisplayXR service is reachable."
                : "No per-app pins and no app attached to the service.", "empty"));
        }
        foreach (var app in rows.Select(r => r.App).Distinct())
        {
            var appRows = rows.Where(r => r.App == app).ToArray();
            string? pin = appRows[0].PinId;
            var head = U.Flow(10, 6, U.Text(app, "mono"), U.Chip(pin is null ? "default" : $"pinned: {appRows[0].Pin}", pin is null ? Level.Plain : Level.Info));
            if (!appRows[0].Running) U.AddFlow(head, U.Chip("not running", Level.Plain), 10, 6);

            void Run(CliArgs command) => _ = Ctx.RunActionsAsync(area, command);
            var choices = list.PinChoices();
            var pick = new ComboBox { ItemsSource = choices.Select(list.NameOf).ToArray(), SelectedIndex = pin is null ? -1 : Array.IndexOf(choices.ToArray(), pin), MinWidth = 200, IsEnabled = !busy && !locked };
            pick.Classes.Add("field");
            Avalonia.Automation.AutomationProperties.SetName(pick, $"Input provider for {app}");
            pick.DropDownOpened += (_, _) => _openDropDowns++;
            pick.DropDownClosed += (_, _) =>
            {
                _openDropDowns = System.Math.Max(0, _openDropDowns - 1);
                Avalonia.Threading.Dispatcher.UIThread.Post(() => Update());
            };
            var prefer = U.Button("Prefer", () =>
            {
                if (pick.SelectedIndex >= 0 && pick.SelectedIndex < choices.Count) Run(PolicyCommands.PreferApp(choices[pick.SelectedIndex], app));
            }, "quiet", "sm");
            prefer.IsEnabled = !busy && !locked;
            ToolTip.SetTip(prefer, locked ? list.PerAppLockText : $"displayxr-cli input use <id> --app {app}");
            U.AddFlow(head, pick, 10, 6);
            U.AddFlow(head, prefer, 10, 6);
            if (pin is not null)
            {
                var reset = U.Button("Reset", () => Run(PolicyCommands.ResetApp(app)), "quiet", "sm");
                reset.IsEnabled = !busy && !locked;
                ToolTip.SetTip(reset, locked ? list.PerAppLockText : $"displayxr-cli {PolicyCommands.ResetApp(app)}");
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
        col.Children.Add(U.Wrapped("Per user. Applies at once to a running app when the service is running; in-process apps at their next launch.", "meta"));
        if (Ctx.LastActionArea == area && Ctx.LastAction is { } la)
            col.Children.Add(Ctx.LastActionFailed ? U.WarningRow(new StatusWarning("POLICY", WarningLevel.Warn, la)) : U.Wrapped(la, "soft"));
        return col;
    }

    private static string EmptyText(ComponentSection s) => s.Id switch
    {
        "display_processors" => "No display processor is registered.",
        "workspace_controller" => "No workspace controller is registered or connected.",
        "present_owners" => "No present owner is connected.",
        "input_providers" => "No input provider reported.",
        "conversion" => "No conversion module reported.",
        "conversion_providers" => "No conversion provider registered.",
        "tracking_providers" => "No viewer-tracking provider registered: a display that does not track keeps the nominal viewer.",
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
                                       : launch.Hotkey is { Length: > 0 } hk ? hk : "No hotkey", capturing || launch.Hotkey is not { Length: > 0 } ? "dim" : "mono");
        keyText.VerticalAlignment = VerticalAlignment.Center;
        var capture = new Border
        {
            Child = keyText, Focusable = true, MinWidth = 220, Padding = new Thickness(12, 6),
            Cursor = new Avalonia.Input.Cursor(Avalonia.Input.StandardCursorType.Hand),
            BorderBrush = capturing ? Tokens.Accent : Tokens.Line, BorderThickness = new Thickness(capturing ? 2 : 1),
            Background = Tokens.Field, CornerRadius = Tokens.Radius("FieldRadius"), VerticalAlignment = VerticalAlignment.Center,
        };
        Avalonia.Automation.AutomationProperties.SetName(capture, $"Launch hotkey for {c.Name}");
        ToolTip.SetTip(capture, "Click, then press the combination: at least one of Ctrl / Shift / Alt / Win plus a key.");
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

        // (b) mode.
        var mode = new ComboBox { ItemsSource = new[] { "Auto (on demand)", "Disabled" }, SelectedIndex = launch.Disabled ? 1 : 0, MinWidth = 190, IsEnabled = !busy };
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
        var launchNow = U.Button("Launch now", () => _ = Ctx.RunActionsAsync("workspace", new CliArgs("workspace", "launch", id)), "primary", "sm");
        launchNow.IsEnabled = !busy && !launch.Disabled && !connected;
        ToolTip.SetTip(launchNow, launch.Disabled ? "Launching is disabled." : connected ? "Already connected." : "Start it now, the same way the hotkey does.");

        var label = U.Text("Launch", "label");
        label.VerticalAlignment = VerticalAlignment.Center;
        var row = U.Flow(10, 6, label, capture, clear, mode, launchNow);

        // (d) state chips.
        var chips = U.Flow(6, 4, U.Chip("registered"));
        U.AddFlow(chips, connected ? U.Chip(c.Pid is { } p ? $"connected (pid {p})" : "connected", Level.Ok) : U.Chip("not running"), 6, 4);
        U.AddFlow(chips, U.Chip($"hotkey: {launch.Source}", launch.Source == "user" ? Level.Info : Level.Plain), 6, 4);
        var note = U.Text("Changes take effect immediately.", "meta");
        note.VerticalAlignment = VerticalAlignment.Center;
        U.AddFlow(chips, note, 6, 4);

        var col = U.VStack(6, row, chips);
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
    /// `--machine` command for an administrator instead of attempting it.
    /// </summary>
    private Control PolicyControls(PolicyRow r, PolicyList list)
    {
        var cls = r.Class;
        string area = PolicyCommands.Area(cls);
        bool busy = Ctx.ActionRunning;
        bool elevated = Elevation.IsElevated;
        bool machine = _machine && elevated;
        var row = U.Flow(10, 6);

        void Run(CliArgs command)
        {
            _policyActionRow = r.Id;
            _ = Ctx.RunActionsAsync(area, command);
        }

        // Policy\Lock: a per-user write would be ignored, so the per-user controls are inert
        // (a machine-wide write from an elevated dashboard still applies).
        bool enabledLocked = list.LockHidesUserEnabled && !machine;
        bool prefLocked = list.LockHidesUserPreference && !machine;
        const string lockTip = "Locked by machine policy (HKLM Policy\\Lock): a per-user value is ignored. An administrator changes the machine-wide value (--machine).";

        // (a) Enabled — never on the built-in adapter, which has no Enabled value (EnabledKnown).
        if (r.EnabledKnown)
        {
            var enabled = new CheckBox { Content = "Enabled", IsChecked = r.Enabled, IsEnabled = !busy && !r.Untrusted && !enabledLocked, VerticalAlignment = VerticalAlignment.Center };
            Avalonia.Automation.AutomationProperties.SetName(enabled, $"Enabled: {r.Name}");
            ToolTip.SetTip(enabled, r.Untrusted
                ? "Refused by the provenance gate: allowlist the binary first (the runtime log names the row to write)."
                : enabledLocked ? lockTip
                : $"displayxr-cli {PolicyCommands.Toggle(cls, r.Id, r.Enabled, machine)}");
            enabled.IsCheckedChanged += (_, _) =>
            {
                if (Ctx.ActionRunning) return;
                bool on = enabled.IsChecked == true;
                if (on == r.Enabled) return;
                Run(on ? PolicyCommands.Enable(cls, r.Id, machine) : PolicyCommands.Disable(cls, r.Id, machine));
            };
            U.AddFlow(row, enabled, 10, 6);
        }

        // (b) Preferred: global for display / input, per task for conversion.
        if (cls != ComponentClass.Conversion)
        {
            // Tracking: Policy\Lock also hides the per-user PreferredPlugin (display / input: never locked).
            if (r.Preferred)
            {
                U.AddFlow(row, U.Chip("preferred", Level.Info), 10, 6);
                var reset = U.Button("Reset preference", () => Run(PolicyCommands.Reset(cls, machine)), "quiet", "sm");
                reset.IsEnabled = !busy && !prefLocked;
                ToolTip.SetTip(reset, prefLocked ? lockTip : $"displayxr-cli {PolicyCommands.Reset(cls, machine)}");
                U.AddFlow(row, reset, 10, 6);
            }
            else
            {
                var prefer = U.Button("Prefer", () => Run(PolicyCommands.Prefer(cls, r.Id, machine)), "quiet", "sm");
                prefer.IsEnabled = !busy && r.Enabled && !r.Untrusted && !prefLocked;
                ToolTip.SetTip(prefer, prefLocked ? lockTip : $"Rank this {cls.Noun()} first: displayxr-cli {PolicyCommands.Prefer(cls, r.Id, machine)}");
                U.AddFlow(row, prefer, 10, 6);
            }
        }
        else
        {
            foreach (var task in list.TaskNamesOf(r.Id))
            {
                if (list.IsPreferredFor(task, r.Id))
                {
                    U.AddFlow(row, U.Chip($"preferred for {task}", Level.Info), 10, 6);
                    var reset = U.Button($"Reset {task}", () => Run(PolicyCommands.ResetTask(task, machine)), "quiet", "sm");
                    reset.IsEnabled = !busy && !prefLocked;
                    ToolTip.SetTip(reset, prefLocked ? lockTip : $"displayxr-cli {PolicyCommands.ResetTask(task, machine)}");
                    U.AddFlow(row, reset, 10, 6);
                }
                else if (!r.BuiltIn) // the built-in adapter is never a provider to prefer
                {
                    var prefer = U.Button($"Prefer for {task}", () => Run(PolicyCommands.PreferTask(task, r.Id, machine)), "quiet", "sm");
                    prefer.IsEnabled = !busy && r.Enabled && !r.Untrusted && !prefLocked;
                    ToolTip.SetTip(prefer, prefLocked ? lockTip : $"displayxr-cli {PolicyCommands.PreferTask(task, r.Id, machine)}");
                    U.AddFlow(row, prefer, 10, 6);
                }
            }
        }

        // (c) scope: machine-wide only when elevated (the dashboard runs non-elevated by design).
        if (elevated)
        {
            var mw = new CheckBox { Content = "Machine-wide", IsChecked = _machine, IsEnabled = !busy, VerticalAlignment = VerticalAlignment.Center };
            ToolTip.SetTip(mw, "Write the machine-wide (HKLM) value instead of this user's. Available because the dashboard is running elevated.");
            mw.IsCheckedChanged += (_, _) => { _machine = mw.IsChecked == true; Update(force: true); };
            U.AddFlow(row, mw, 10, 6);
        }

        // (d) what a change means, and how an administrator does it machine-wide.
        var col = U.VStack(6, row);
        if (r.BuiltIn)
            col.Children.Add(U.Wrapped("Built-in adapter: it has no Enabled value and is not a provider to prefer. A registered provider hides it with Supersedes = dp-lift.", "meta"));
        string scope = machine ? "Machine-wide." : elevated ? "Per user." : "Per user; machine-wide needs an administrator prompt.";
        col.Children.Add(U.Wrapped($"{scope} {cls.AppliesNote()}", "meta"));
        // Non-elevated: every control's machine-wide command (Enabled, Prefer, Reset), for an administrator.
        if (!elevated && PolicyCommands.MachineHints(r, list) is { Count: > 0 } hints)
        {
            col.Children.Add(U.Wrapped("Machine-wide, from an administrator prompt:", "meta"));
            foreach (var h in hints) col.Children.Add(U.Wrapped($"displayxr-cli {h}", "mono"));
        }
        if (Ctx.LastActionArea == area && _policyActionRow == r.Id && Ctx.LastAction is { } la)
            col.Children.Add(Ctx.LastActionFailed ? U.WarningRow(new StatusWarning("POLICY", WarningLevel.Warn, la)) : U.Wrapped(la, "soft"));
        var box = new Border { Child = col, Margin = new Thickness(22, 8, 0, 0) };
        box.Classes.Add("field");
        return box;
    }

    private static Control ItemRow(ComponentItem item)
    {
        var dot = U.Dot(item.Level);
        dot.VerticalAlignment = VerticalAlignment.Top;
        dot.Margin = new Thickness(0, 6, 12, 0);
        var name = U.Wrapped(item.Name, "value");
        name.FontWeight = FontWeight.SemiBold;
        var head = U.Flow(8, 4, name);
        foreach (var (text, level) in item.Chips) U.AddFlow(head, U.Chip(text, level), 6, 4);
        var col = U.VStack(2, head);
        for (int i = 0; i < item.Lines.Count; i++)
            col.Children.Add(U.Wrapped(item.Lines[i], i == 0 ? "soft" : "desc"));
        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
        g.Children.Add(dot);
        Grid.SetColumn(col, 1);
        g.Children.Add(col);
        return g;
    }
}

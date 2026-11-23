// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Input;
using Avalonia.Layout;
using DisplayXR.Dashboard.Model;

namespace DisplayXR.Dashboard.Ui;

/// <summary>
/// The spatial desktop's launch controls (phase 8): the shortcut (click, press the combination, Esc
/// cancels; the service's own hotkey is suspended while capturing), Clear, Reset, the launch mode and
/// "Open now". One implementation, shown on Components (the spatial desktop's row) and on Settings.
/// The page that shows it passes its own Update; while a capture or a drop-down is open the page
/// must not rebuild (<see cref="Busy"/>).
/// </summary>
public sealed class WorkspaceLaunchEditor
{
    private readonly DashboardContext _ctx;
    private readonly Action<bool> _update;
    private string? _captureNote;
    private string? _captureNoteFor;
    private TextBlock? _suspendNote;

    /// <param name="update">The page's Update (true: force a rebuild).</param>
    public WorkspaceLaunchEditor(DashboardContext ctx, Action<bool> update)
    {
        _ctx = ctx;
        _update = update;
    }

    /// <summary>The controller whose shortcut is being captured, or null.</summary>
    public string? Capturing { get; private set; }

    public int OpenDropDowns { get; private set; }

    /// <summary>A rebuild now would end a capture or close a drop-down under the pointer.</summary>
    public bool Busy => Capturing is not null || OpenDropDowns > 0;

    /// <summary>What the page's key must include so a capture note shows.</summary>
    public string Key => $"{Capturing}|{_captureNoteFor}|{_captureNote}";

    /// <summary>Call at the start of the page's Build: a forced rebuild discards any open drop-down without a Closed event.</summary>
    public void BeginBuild()
    {
        OpenDropDowns = 0;
        _suspendNote = null;
    }

    /// <summary>Known only after the first "on" came back: shown without a rebuild.</summary>
    public void UpdateInPlace()
    {
        if (_suspendNote is not null) _suspendNote.IsVisible = Capturing is not null && _ctx.HotkeySuspend.Unsupported;
    }

    /// <summary>End a capture: the hotkey hook is handed back on every exit path.</summary>
    public void EndCapture()
    {
        Capturing = null;
        _ctx.HotkeySuspend.End();
    }

    /// <summary>The page left the screen: a capture never survives it.</summary>
    public void OnHidden()
    {
        if (Capturing is not null) EndCapture();
    }

    /// <summary>
    /// How this workspace controller is launched (phase 8): hotkey capture,
    /// mode, Launch now. Written with `displayxr-cli workspace set|launch`, then
    /// `workspace list --json` is re-read once; the CLI makes the service reload,
    /// so a change is live.
    /// </summary>
    public Control Build(RegisteredController c)
    {
        var launch = c.Launch!;
        string id = c.Id;
        bool connected = c.Connected == true;
        bool busy = _ctx.ActionRunning;

        // (a) hotkey capture: click, press the combo, Esc cancels.
        bool capturing = Capturing == id;
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
            Capturing = id;
            _captureNote = null;
            _update(true);
        };
        if (capturing)
        {
            capture.GotFocus += (_, _) => _ctx.HotkeySuspend.Begin();
            capture.AttachedToVisualTree += (_, _) => Avalonia.Threading.Dispatcher.UIThread.Post(() => capture.Focus());
            capture.LostFocus += (_, _) =>
            {
                if (Capturing != id) return;
                EndCapture();
                Avalonia.Threading.Dispatcher.UIThread.Post(() => _update(true));
            };
            capture.AddHandler(InputElement.KeyDownEvent, (_, e) =>
            {
                e.Handled = true;
                if (e.Key == Avalonia.Input.Key.Escape && e.KeyModifiers == Avalonia.Input.KeyModifiers.None)
                {
                    EndCapture();
                    _update(true);
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
                    _update(true);
                    return;
                }
                var combo = new Hotkey(mods, name!).ToString();
                _captureNote = null;
                _ = _ctx.RunActionsAsync("workspace", new CliArgs("workspace", "set", id, "--hotkey", combo));
                _update(true);
            }, Avalonia.Interactivity.RoutingStrategies.Tunnel);
        }

        var clear = U.Button("Clear", () => _ = _ctx.RunActionsAsync("workspace", new CliArgs("workspace", "set", id, "--no-hotkey")), "quiet", "sm");
        clear.IsEnabled = !busy && launch.Hotkey is { Length: > 0 };

        // Back to the runtime's defaults: `workspace reset` drops the controller's
        // service.json entry, so it follows the default again (an upgraded box that
        // stored Ctrl+Space gets Ctrl+Shift+Space this way).
        var reset = U.Button("Reset", () => _ = _ctx.RunActionsAsync("workspace", new CliArgs("workspace", "reset", id)), "quiet", "sm");
        reset.IsEnabled = !busy && launch.Source == "user";
        ToolTip.SetTip(reset, $"Back to the runtime's defaults ({Hotkey.Default}, Auto on current runtimes).");

        // (b) mode.
        var mode = new ComboBox { ItemsSource = new[] { "Opens with the shortcut", "Turned off" }, SelectedIndex = launch.Disabled ? 1 : 0, MinWidth = 190, IsEnabled = !busy };
        mode.Classes.Add("field");
        Avalonia.Automation.AutomationProperties.SetName(mode, $"Launch mode for {c.Name}");
        int modeWas = mode.SelectedIndex;
        mode.DropDownOpened += (_, _) => OpenDropDowns++;
        mode.DropDownClosed += (_, _) =>
        {
            OpenDropDowns = System.Math.Max(0, OpenDropDowns - 1);
            Avalonia.Threading.Dispatcher.UIThread.Post(() => _update(false));
        };
        mode.SelectionChanged += (_, _) =>
        {
            if (mode.SelectedIndex < 0 || mode.SelectedIndex == modeWas || _ctx.ActionRunning) return;
            _ = _ctx.RunActionsAsync("workspace", new CliArgs("workspace", "set", id, "--mode", mode.SelectedIndex == 1 ? "disabled" : "auto"));
        };

        // (c) launch now.
        var launchNow = U.Button("Open now", () => _ = _ctx.RunActionsAsync("workspace", new CliArgs("workspace", "launch", id)), "primary", "sm");
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
            _suspendNote.IsVisible = _ctx.HotkeySuspend.Unsupported;
            col.Children.Add(_suspendNote);
        }
        if (_captureNoteFor == id && _captureNote is { } cn)
            col.Children.Add(U.WarningRow(new StatusWarning("HOTKEY", WarningLevel.Warn, cn)));
        if (_ctx.LastActionArea == "workspace" && _ctx.LastAction is { } la && _ctx.LastActionFailed)
            col.Children.Add(U.Wrapped(la, "soft"));
        var box = new Border { Child = col, Margin = new Thickness(22, 10, 0, 0) };
        box.Classes.Add("field");
        return box;
    }
}

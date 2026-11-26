// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Layout;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;
using DisplayXR.Dashboard.Ui;

namespace DisplayXR.Dashboard.Pages;

/// <summary>
/// Displays (design §8): one card per screen in registry order. Its face: status dot, name, plain
/// role chips, the plain badge, the plain state line, warnings as data, and its actions (the display
/// maker's app, "Calibrate camera…" where camera head tracking serves it, Details). Behind
/// "Details": the plain identity / claim / maker lines, the driver selector with what it does, and
/// the technical lines. The selector stays on the face when more than one driver could drive the
/// display or more than one 3D display is present. The desktop map shows only with several displays.
/// </summary>
public sealed class DisplaysPage : Page
{
    private readonly DesktopMap _map = new();
    private readonly Dictionary<string, string> _launchNotes = new();
    private readonly HashSet<string> _details = new(); // the display cards whose Details are open
    private string? _dpKey; // the screen whose selector ran the last per-screen action
    private string? _modeKey; // the screen whose 3D mode picker ran the last action
    private int _openDropDowns;
    // The choice just made, shown until the re-read dp list reflects it (the
    // re-read loads every plug-in, ~10 s): without it the combo would snap back.
    private (string Key, int Index, DpList? Before)? _pending;

    protected override bool DeferRebuild => _openDropDowns > 0;

    public DisplaysPage() : base("displays", "Displays") { }

    public override string Caption
    {
        get
        {
            var s = Ctx.Feed.Snapshot;
            return s is null ? "Looking for your displays…" : StatusText.DisplaysSummary(s);
        }
    }

    public override Control? HeaderActions
    {
        get
        {
            var s = Ctx.Feed.Snapshot;
            if (s is null || s.Screens.Count == 0) return null;
            return U.Button("Copy list", () =>
            {
                if (Ctx.Feed.Snapshot is { } now) _ = Ctx.CopyText(StatusText.Copy(now));
                Ctx.Toast("Display list copied");
            }, "outline", "sm");
        }
    }

    public override void OnShown() => _ = Ctx.LoadDpAsync();

    protected override string Key()
    {
        var s = Ctx.Feed.Snapshot;
        if (s is null) return "none";
        var sb = new System.Text.StringBuilder(StatusText.Copy(s, includeSource: false));
        sb.Append('|').Append(string.Join(";", _launchNotes)).Append('|').Append(Ctx.ActionRunning).Append(Ctx.DpLoading)
          .Append('|').Append(string.Join(",", _details)).Append(_calibration is null);
        foreach (var x in s.Screens) sb.Append(x.Key).Append(x.Claim.Forced).Append(x.Claim.PreferredPlugin).Append(x.Claim.Apply);
        foreach (var x in s.Screens) sb.Append(StatusText.Mode3DKey(x.Mode3D)).Append(';');
        if (Ctx.LastActionArea == "screen-mode") sb.Append(Ctx.LastAction).Append(Ctx.LastActionFailed);
        if (Ctx.Dp is { } dp)
        {
            foreach (var p in dp.Plugins) sb.Append(p.Id).Append(',');
            if (dp.Screens is { } ds) foreach (var d in ds) sb.Append(d).Append(';');
        }
        if (Ctx.LastActionArea == "screen-dp") sb.Append(Ctx.LastAction).Append(Ctx.LastActionFailed);
        sb.Append(_pending?.Key).Append(_pending?.Index).Append(Ctx.Dp?.GetHashCode());
        return sb.ToString();
    }

    protected override void UpdateInPlace()
    {
        if (Ctx.Feed.Snapshot is { } s)
        {
            var (screens, windows) = DesktopMapGeometry.From(s);
            _map.SetData(screens, windows);
        }
    }

    protected override Control Build()
    {
        _openDropDowns = 0; // a forced rebuild discards any open drop-down without a Closed event
        var s = Ctx.Feed.Snapshot;
        var page = U.VStack(16);
        if (s is null)
        {
            page.Children.Add(U.Card(U.VStack(8, U.Text("Looking for your displays", "group"),
                U.Wrapped("This takes about a second when DisplayXR runs in the background, or about ten seconds when it does not.", "desc"))));
            return page;
        }
        if (s.Screens.Count == 0)
        {
            page.Children.Add(U.Card(U.VStack(8, U.Text("No displays found", "group"),
                U.Wrapped("DisplayXR found no display. Check that your display is connected and awake. In a remote-desktop session the displays of the PC itself are not visible.", "desc"))));
        }
        foreach (var screen in s.Screens) page.Children.Add(ScreenCard(s, screen));

        // The desktop map matters only with more than one display.
        if (s.Screens.Count < 2) return page;
        U.Detach(_map);
        var mapCard = U.VStack(12,
            U.CardTitle("Desktop", "Your displays to scale. Outlines are the windows of 3D apps; the tinted display holds a window; a dashed line is where a window crosses from one display to another."),
            _map);
        var spacer = new Border { Height = 4 };
        page.Children.Add(spacer);
        page.Children.Add(U.Card(mapCard));
        return page;
    }

    /// <summary>
    /// The per-screen display-processor selector (phase 7). Absent when the CLI
    /// predates per-screen overrides: no key on the screen, or no plug-in list.
    /// </summary>
    private Control? DpRow(Screen s)
    {
        if (s.Key is not null && Ctx.Dp is null && Ctx.DpLoading)
        {
            var wait = U.Text("Display driver: reading the installed drivers…", "meta");
            wait.Margin = new Thickness(0, 6, 0, 0);
            return wait;
        }
        if (StatusText.DpSelector(s, Ctx.Dp) is not { } sel || s.Key is null) return null;
        string key = s.Key;
        if (_pending is { } pend && (!ReferenceEquals(pend.Before, Ctx.Dp) && !Ctx.ActionRunning && !Ctx.DpLoading
                                     || Ctx.LastActionArea == "screen-dp" && Ctx.LastActionFailed && !Ctx.ActionRunning))
            _pending = null;
        bool applying = _pending is { } p0 && p0.Key == key;
        if (applying && _pending!.Value.Index < sel.Items.Count) sel = (sel.Items, _pending.Value.Index);
        var combo = new ComboBox
        {
            ItemsSource = sel.Items.Select(i => i.Label).ToList(),
            SelectedIndex = sel.Selected,
            MinWidth = 260,
            IsEnabled = !Ctx.ActionRunning,
            VerticalAlignment = VerticalAlignment.Center,
        };
        combo.Classes.Add("field");
        Avalonia.Automation.AutomationProperties.SetName(combo, $"Display driver for {StatusText.ScreenName(s)}");
        combo.DropDownOpened += (_, _) => _openDropDowns++;
        combo.DropDownClosed += (_, _) =>
        {
            _openDropDowns = System.Math.Max(0, _openDropDowns - 1);
            Avalonia.Threading.Dispatcher.UIThread.Post(() => Update());
        };
        combo.SelectionChanged += (_, _) =>
        {
            int i = combo.SelectedIndex;
            if (i < 0 || i == sel.Selected || i >= sel.Items.Count || Ctx.ActionRunning) return;
            _dpKey = key;
            _pending = (key, i, Ctx.Dp);
            _ = Ctx.RunActionsAsync("screen-dp", StatusText.DpVerb(key, sel.Items[i]));
        };
        var label = U.Text("Display driver", "label");
        label.VerticalAlignment = VerticalAlignment.Center;
        var row = U.Flow(12, 4, label, combo);
        if (applying)
        {
            var ap = U.Text("applying…", "meta");
            ap.VerticalAlignment = VerticalAlignment.Center;
            U.AddFlow(row, ap, 12, 4);
        }
        else if (StatusText.ForcedChip(s, Ctx.Dp) is { } forced) U.AddFlow(row, U.Chip(forced, Level.Warn), 12, 4);
        if (StatusText.ApplyLabel(s, Ctx.Dp) is { } apply)
        {
            var a = U.Text(apply, "meta");
            a.VerticalAlignment = VerticalAlignment.Center;
            U.AddFlow(row, a, 12, 4);
        }
        var col = U.VStack(2, row);
        col.Margin = new Thickness(0, 6, 0, 0);
        if (Ctx.LastActionArea == "screen-dp" && _dpKey == key && Ctx.LastAction is { } last && Ctx.LastActionFailed)
            col.Children.Add(U.Wrapped(last, "soft"));
        return col;
    }

    /// <summary>
    /// The per-display 3D mode row. With two or more modes it is a picker: the display's 3D modes by the
    /// driver's own labels, the stored choice selected (else the one drawn now). A choice runs
    /// `dp mode set &lt;name&gt; --screen &lt;key&gt;`; "Reset to default" runs `dp mode reset --screen
    /// &lt;key&gt;` and is offered for this user's own choice only; the status feed shows the result. A
    /// mode that applies at the next app start can be picked and says so; a mode the display cannot show
    /// is not listed, and the line under the picker names it and says why. With a stored choice and no
    /// modes reported, the row shows the stored choice and the reason, without a picker. Absent when the
    /// feed says there is nothing (null), or lists one mode with nothing to choose.
    /// </summary>
    private Control? Mode3DRow(Screen s)
    {
        if (!StatusText.Mode3DRowShown(s) || s.Mode3D is not { } mode || s.Key is null) return null;
        string key = s.Key;
        var label = U.Text("3D mode", "label");
        label.VerticalAlignment = VerticalAlignment.Center;
        Control value;
        if (StatusText.Mode3DPickerShown(s))
        {
            var items = StatusText.Mode3DItems(mode);
            int selected = StatusText.Mode3DSelected(mode);
            var combo = new ComboBox
            {
                ItemsSource = items.Select(c => StatusText.Mode3DItemText(mode, c)).ToList(),
                SelectedIndex = selected,
                MinWidth = 200,
                IsEnabled = !Ctx.ActionRunning,
                VerticalAlignment = VerticalAlignment.Center,
            };
            combo.Classes.Add("field");
            Avalonia.Automation.AutomationProperties.SetName(combo, $"3D mode for {StatusText.ScreenName(s)}");
            combo.DropDownOpened += (_, _) => _openDropDowns++;
            combo.DropDownClosed += (_, _) =>
            {
                _openDropDowns = System.Math.Max(0, _openDropDowns - 1);
                Avalonia.Threading.Dispatcher.UIThread.Post(() => Update());
            };
            combo.SelectionChanged += (_, _) =>
            {
                int i = combo.SelectedIndex;
                if (i < 0 || i == selected || i >= items.Count || Ctx.ActionRunning) return;
                _modeKey = key;
                _ = Ctx.RunActionsAsync("screen-mode", StatusText.Mode3DVerb(key, items[i]));
            };
            value = combo;
        }
        else
        {
            // A stored choice, no modes reported: nothing to pick from, the choice stays visible.
            var stored = U.Text(StatusText.Mode3DStoredText(mode));
            stored.VerticalAlignment = VerticalAlignment.Center;
            value = stored;
        }
        var row = U.Flow(12, 4, label, value);
        if (StatusText.Mode3DSourceText(mode) is { } source)
        {
            var src = U.Text(source, "meta");
            src.VerticalAlignment = VerticalAlignment.Center;
            U.AddFlow(row, src, 12, 4);
        }
        if (StatusText.Mode3DCanReset(mode))
        {
            var reset = U.Button("Reset to default", () =>
            {
                _modeKey = key;
                _ = Ctx.RunActionsAsync("screen-mode", StatusText.Mode3DResetVerb(key));
            }, "quiet", "sm");
            reset.IsEnabled = !Ctx.ActionRunning;
            reset.VerticalAlignment = VerticalAlignment.Center;
            Avalonia.Automation.AutomationProperties.SetName(reset, $"Reset the 3D mode of {StatusText.ScreenName(s)} to its default");
            U.AddFlow(row, reset, 12, 4);
        }
        var col = U.VStack(2, row);
        col.Margin = new Thickness(0, 6, 0, 0);
        foreach (var note in StatusText.Mode3DNotes(mode)) col.Children.Add(U.Wrapped(note, "soft"));
        if (StatusText.Mode3DPickerShown(s)) col.Children.Add(U.Wrapped(StatusText.Mode3DExplanation, "desc"));
        if (Ctx.LastActionArea == "screen-mode" && _modeKey == key && Ctx.LastAction is { } last && Ctx.LastActionFailed)
            col.Children.Add(U.Wrapped(last, "soft"));
        return col;
    }

    private Control ScreenCard(StatusSnapshot snap, Screen s)
    {
        var level = StatusText.ScreenLevel(s, snap);
        var dot = U.Dot(level, 11);
        dot.VerticalAlignment = VerticalAlignment.Top;
        dot.Margin = new Thickness(0, 6, 14, 0);
        string screenName = StatusText.ScreenName(s);

        var name = U.Wrapped(screenName, "group");
        name.FontSize = 17;
        var title = U.Flow(10, 4, name);
        foreach (var chip in StatusText.PlainChips(s))
            U.AddFlow(title, U.Chip(chip, chip == "Main 3D display" ? Level.Info : Level.Plain), 6, 4);
        if (StatusText.ForcedChip(s, Ctx.Dp, snap) is { } forcedChip) U.AddFlow(title, U.Chip(forcedChip, Level.Warn), 6, 4);

        // Plain words on the face; the technical line each one replaces is in Details and in "Copy list".
        var (badgeText, badgeLevel) = StatusText.PlainBadge(s, snap);
        var badge = U.Badge(badgeText, badgeLevel, tick: badgeLevel == Level.Ok);
        ToolTip.SetTip(badge, StatusText.Badge(s).Text);
        badge.VerticalAlignment = VerticalAlignment.Top;
        badge.Margin = new Thickness(0, 2, 0, 0);

        var head = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
        head.Children.Add(title);
        Grid.SetColumn(badge, 1);
        head.Children.Add(badge);

        var body = U.VStack(5, head);
        var state = U.Wrapped(StatusText.PlainStateLine(s, snap), "soft");
        ToolTip.SetTip(state, StatusText.StateLine(s)); // the technical line, as "Copy list" writes it
        body.Children.Add(state);

        var warnings = StatusText.ScreenWarnings(s);
        if (warnings.Count > 0)
        {
            var list = U.VStack(4);
            list.Margin = new Thickness(0, 6, 0, 0);
            foreach (var w in warnings)
                list.Children.Add(U.WarningRow(w, ReferenceEquals(w, s.Vendor.WorstWarning) ? "vendor " : null));
            body.Children.Add(list);
        }

        // The face's actions: the display maker's own app, camera calibration for this display, Details.
        var actions = U.Flow(10, 6);
        if (s.Vendor.Present && s.Vendor.DashboardCommand is { Length: > 0 } cmd)
        {
            string sid = s.Id;
            var open = U.Button("Open the display maker's app ↗", () =>
            {
                string line = VendorCommand.Expand(cmd, s.Vendor.Serial.Length > 0 ? s.Vendor.Serial : s.Claim.Serial, s.Id);
                string? err = VendorCommand.Launch(line);
                if (err is null) _launchNotes.Remove(sid); else _launchNotes[sid] = err;
                Update();
            }, "outline", "sm");
            U.AddFlow(actions, open, 10, 6);
        }
        if (CalibrateButton(snap, s) is { } calibrate) U.AddFlow(actions, calibrate, 10, 6);
        bool open_ = _details.Contains(s.Id);
        var details = U.Button(open_ ? "Hide details" : "Details", () =>
        {
            if (!_details.Remove(s.Id)) _details.Add(s.Id);
            Update(force: true);
        }, "quiet", "sm");
        Avalonia.Automation.AutomationProperties.SetName(details, $"{(open_ ? "Hide details" : "Details")} of {screenName}");
        U.AddFlow(actions, details, 10, 6);
        actions.Margin = new Thickness(0, 6, 0, 0);
        body.Children.Add(actions);
        if (_launchNotes.TryGetValue(s.Id, out var note)) body.Children.Add(U.Wrapped(note, "soft"));

        // More than one driver could drive this display, or more than one 3D display: the choice is on the face.
        bool choiceOnFace = StatusText.DriverChoiceOnFace(s, snap, Ctx.Dp);
        var dpRow = DpRow(s);
        // The 3D mode picker is always on the face: under the driver selector when that is on the face too.
        var modeRow = Mode3DRow(s);
        if (modeRow is not null && !(choiceOnFace && dpRow is not null)) body.Children.Add(modeRow);
        if (choiceOnFace && dpRow is not null)
        {
            body.Children.Add(dpRow);
            body.Children.Add(U.Wrapped(DriverExplanation, "desc"));
            if (modeRow is not null) body.Children.Add(modeRow);
        }
        // ADR-066 Part B: viewer tracking per display, on the face (greyed out with the reason when not offered).
        if (TrackingRow(s) is { } trackingRow) body.Children.Add(trackingRow);

        if (open_)
        {
            var d = U.VStack(6);
            d.Children.Add(U.Wrapped(StatusText.PlainIdentity(s), "dim"));
            d.Children.Add(U.Wrapped(StatusText.PlainClaimLine(s, id => snap.Plugins.FirstOrDefault(p => p.Id == id)?.Name, snap), "soft"));
            if (StatusText.PlainVendorLine(s) is { } vendor) d.Children.Add(U.Wrapped(vendor, "dim"));
            if (!choiceOnFace && dpRow is not null)
            {
                d.Children.Add(dpRow);
                d.Children.Add(U.Wrapped(DriverExplanation, "desc"));
            }
            // The technical lines, as "Copy list" writes them.
            var tech = U.VStack(2);
            tech.Margin = new Thickness(0, 6, 0, 0);
            tech.Children.Add(U.Wrapped(StatusText.Identity(s), "dev"));
            tech.Children.Add(U.Wrapped(StatusText.ClaimLine(s), "dev"));
            tech.Children.Add(U.Wrapped(StatusText.StateLine(s), "dev"));
            if (StatusText.VendorLine(s) is { } vl) tech.Children.Add(U.Wrapped(vl, "dev"));
            d.Children.Add(tech);
            var box = new Border { Child = d, Padding = new Thickness(12, 10), Margin = new Thickness(0, 4, 0, 0) };
            box.Classes.Add("field");
            body.Children.Add(box);
        }

        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
        g.Children.Add(dot);
        Grid.SetColumn(body, 1);
        g.Children.Add(body);
        var card = U.Card(g);
        if (level is Level.Warn or Level.Critical) card.BorderBrush = Tokens.Alpha(Tokens.ColorKey(level), 0.45);
        return card;
    }

    /// <summary>
    /// ADR-066 Part B: the per-display viewer-tracking selector (Off / In 3D only / Always), greyed out
    /// with the reason where it is not offered (the display tracks the viewer itself, no camera, no face
    /// estimator in this build, …). Writes `tracking set --screen &lt;key&gt; off|3d|always`; the status
    /// feed shows the result.
    /// </summary>
    private Control? TrackingRow(Screen s)
    {
        // A plain monitor (no display driver) has nothing to offer: no row at all.
        if (s.ViewerTracking is not { } vt || s.Key is null || (s.Claim.PluginId is null && !vt.Available)) return null;
        string key = s.Key;
        int selected = StatusText.TrackingSelected(vt);
        var combo = new ComboBox
        {
            ItemsSource = StatusText.TrackingChoices.Select(c => c.Label).ToList(),
            SelectedIndex = selected,
            MinWidth = 200,
            IsEnabled = vt.Available && !Ctx.ActionRunning,
            VerticalAlignment = VerticalAlignment.Center,
        };
        combo.Classes.Add("field");
        string? note = StatusText.TrackingNote(vt);
        if (!vt.Available && note is not null)
        {
            ToolTip.SetShowOnDisabled(combo, true);
            ToolTip.SetTip(combo, note);
        }
        Avalonia.Automation.AutomationProperties.SetName(combo, $"Viewer tracking for {StatusText.ScreenName(s)}");
        combo.DropDownOpened += (_, _) => _openDropDowns++;
        combo.DropDownClosed += (_, _) =>
        {
            _openDropDowns = System.Math.Max(0, _openDropDowns - 1);
            Avalonia.Threading.Dispatcher.UIThread.Post(() => Update());
        };
        combo.SelectionChanged += (_, _) =>
        {
            int i = combo.SelectedIndex;
            if (i < 0 || i == selected || i >= StatusText.TrackingChoices.Count || Ctx.ActionRunning) return;
            _trackingKey = key;
            _ = Ctx.RunActionsAsync("screen-tracking", StatusText.TrackingVerb(key, i));
        };
        var label = U.Text("Viewer tracking", "label");
        label.VerticalAlignment = VerticalAlignment.Center;
        var row = U.Flow(12, 4, label, combo);
        var col = U.VStack(2, row);
        col.Margin = new Thickness(0, 6, 0, 0);
        if (note is not null) col.Children.Add(U.Wrapped(note, "desc"));
        if (Ctx.LastActionArea == "screen-tracking" && _trackingKey == key && Ctx.LastAction is { } last &&
            Ctx.LastActionFailed)
            col.Children.Add(U.Wrapped(last, "soft"));
        return col;
    }

    private string? _trackingKey;

    /// <summary>What the per-display driver selector does, in plain words.</summary>
    private const string DriverExplanation =
        "The display driver turns what a 3D app draws into what this display shows. Automatic picks the one made for it. Choosing one here is for you only.";

    /// <summary>
    /// Camera calibration, on the card of the display it is for: only where camera head tracking
    /// serves this display (its viewer source is a provider; tracking-provider-discovery.md §6).
    /// </summary>
    private Control? CalibrateButton(StatusSnapshot snap, Screen s)
    {
        var row = Tracking.Screens(snap, null).FirstOrDefault(r => r.ScreenId == s.Id);
        if (row is not { CanCalibrate: true }) return null;
        string name = StatusText.ScreenName(s);
        string key = s.Key is { Length: > 0 } k ? k : s.Id;
        var calibrate = U.Button("Calibrate camera…", () => OpenCalibration(key, s, name), "outline", "sm");
        calibrate.IsEnabled = _calibration is null;
        ToolTip.SetShowOnDisabled(calibrate, true);
        ToolTip.SetTip(calibrate, _calibration is null
            ? "Measure where the camera sits next to this display, using your reflection in it, so the picture lines up with where you are."
            : "A calibration window is already open.");
        Avalonia.Automation.AutomationProperties.SetName(calibrate, $"Calibrate camera for {name}");
        return calibrate;
    }

    private CalibrationWindow? _calibration;

    /// <summary>
    /// Open the calibration window for one provider-served display (one at a time): a full-screen window
    /// on THIS display that streams `tracking calibrate --screen &lt;key&gt; --watch --json`, solves and
    /// applies. After a successful apply the tracking rows and info's tracking block are re-read.
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
}

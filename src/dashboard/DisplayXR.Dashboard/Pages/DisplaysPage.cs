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
/// Displays (design §8): one card per screen in registry order — status dot,
/// name, role chips, claim badge, identity / claim / state / vendor lines,
/// warnings as data — and the desktop map under them.
/// </summary>
public sealed class DisplaysPage : Page
{
    private readonly DesktopMap _map = new();
    private readonly Dictionary<string, string> _launchNotes = new();
    private string? _dpKey; // the screen whose selector ran the last per-screen action
    private int _openDropDowns;

    protected override bool DeferRebuild => _openDropDowns > 0;

    public DisplaysPage() : base("displays", "Displays") { }

    public override string Caption
    {
        get
        {
            var s = Ctx.Feed.Snapshot;
            return s is null ? "Waiting for the first status snapshot…" : StatusText.DisplaysHeader(s);
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
        sb.Append('|').Append(string.Join(";", _launchNotes)).Append('|').Append(Ctx.ActionRunning);
        foreach (var x in s.Screens) sb.Append(x.Key).Append(x.Claim.Forced).Append(x.Claim.PreferredPlugin).Append(x.Claim.Apply);
        if (Ctx.Dp is { } dp)
        {
            foreach (var p in dp.Plugins) sb.Append(p.Id).Append(',');
            if (dp.Screens is { } ds) foreach (var d in ds) sb.Append(d).Append(';');
        }
        if (Ctx.LastActionArea == "screen-dp") sb.Append(Ctx.LastAction).Append(Ctx.LastActionFailed);
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
            page.Children.Add(U.Card(U.VStack(8, U.Text("Reading the display status", "group"),
                U.Wrapped("The first snapshot arrives within a second from the service, or after about ten seconds when the dashboard has to build it without one.", "desc"))));
            return page;
        }
        if (s.Screens.Count == 0)
        {
            page.Children.Add(U.Card(U.VStack(8, U.Text("No monitors reported", "group"),
                U.Wrapped("The runtime's screen registry is empty. Check that a display is connected and awake; on a remote-desktop session the physical monitors are not visible.", "desc"))));
        }
        foreach (var screen in s.Screens) page.Children.Add(ScreenCard(s, screen));

        U.Detach(_map);
        var mapCard = U.VStack(12,
            U.CardTitle("Desktop", "Monitors to scale. Outlines are client windows; the tinted monitor owns a window; a dashed line is a seam where a window crosses screens."),
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
        if (StatusText.DpSelector(s, Ctx.Dp) is not { } sel || s.Key is null) return null;
        string key = s.Key;
        var combo = new ComboBox
        {
            ItemsSource = sel.Items.Select(i => i.Label).ToList(),
            SelectedIndex = sel.Selected,
            MinWidth = 260,
            IsEnabled = !Ctx.ActionRunning,
            VerticalAlignment = VerticalAlignment.Center,
        };
        combo.Classes.Add("field");
        Avalonia.Automation.AutomationProperties.SetName(combo, $"Display processor for {StatusText.ScreenName(s)}");
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
            _ = Ctx.RunActionsAsync("screen-dp", StatusText.DpVerb(key, sel.Items[i]));
        };
        var label = U.Text("Display processor", "label");
        label.VerticalAlignment = VerticalAlignment.Center;
        var row = U.Flow(12, 4, label, combo);
        if (StatusText.ForcedChip(s, Ctx.Dp) is { } forced) U.AddFlow(row, U.Chip(forced, Level.Warn), 12, 4);
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

    private Control ScreenCard(StatusSnapshot snap, Screen s)
    {
        var level = StatusText.ScreenLevel(s);
        var dot = U.Dot(level, 11);
        dot.VerticalAlignment = VerticalAlignment.Top;
        dot.Margin = new Thickness(0, 6, 14, 0);

        var name = U.Wrapped(StatusText.ScreenName(s), "group");
        name.FontSize = 17;
        var title = U.Flow(10, 4, name);
        foreach (var chip in StatusText.Chips(s))
            U.AddFlow(title, U.Chip(chip, chip == "vendor primary" ? Level.Info : Level.Plain), 6, 4);

        var (badgeText, badgeLevel) = StatusText.Badge(s);
        var badge = U.Badge(badgeText, badgeLevel, tick: badgeText == "VERIFIED");
        badge.VerticalAlignment = VerticalAlignment.Top;
        badge.Margin = new Thickness(0, 2, 0, 0);

        var head = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
        head.Children.Add(title);
        Grid.SetColumn(badge, 1);
        head.Children.Add(badge);

        var body = U.VStack(5, head);
        body.Children.Add(U.Wrapped(StatusText.Identity(s), "dev"));
        var claim = U.Wrapped(StatusText.ClaimLine(s), "soft");
        body.Children.Add(claim);
        body.Children.Add(U.Wrapped(StatusText.StateLine(s), "dim"));

        if (StatusText.VendorLine(s) is { } vendor)
        {
            var vendorRow = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
            var vt = U.Wrapped(vendor, "dim");
            vt.VerticalAlignment = VerticalAlignment.Center;
            vendorRow.Children.Add(vt);
            if (s.Vendor.DashboardCommand is { Length: > 0 } cmd)
            {
                string id = s.Id;
                var open = U.Button("Open in vendor dashboard ↗", () =>
                {
                    string line = VendorCommand.Expand(cmd, s.Vendor.Serial.Length > 0 ? s.Vendor.Serial : s.Claim.Serial, s.Id);
                    string? err = VendorCommand.Launch(line);
                    if (err is null) _launchNotes.Remove(id); else _launchNotes[id] = err;
                    Update();
                }, "outline", "sm");
                open.Margin = new Thickness(12, 0, 0, 0);
                Grid.SetColumn(open, 1);
                vendorRow.Children.Add(open);
            }
            body.Children.Add(vendorRow);
            if (_launchNotes.TryGetValue(s.Id, out var note)) body.Children.Add(U.Wrapped(note, "soft"));
        }

        if (DpRow(s) is { } dpRow) body.Children.Add(dpRow);

        var warnings = StatusText.ScreenWarnings(s);
        if (warnings.Count > 0)
        {
            var list = U.VStack(4);
            list.Margin = new Thickness(0, 6, 0, 0);
            foreach (var w in warnings)
                list.Children.Add(U.WarningRow(w, ReferenceEquals(w, s.Vendor.WorstWarning) ? "vendor " : null));
            body.Children.Add(list);
        }

        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
        g.Children.Add(dot);
        Grid.SetColumn(body, 1);
        g.Children.Add(body);
        var card = U.Card(g);
        if (level is Level.Warn or Level.Critical) card.BorderBrush = Tokens.Alpha(Tokens.ColorKey(level), 0.45);
        return card;
    }
}

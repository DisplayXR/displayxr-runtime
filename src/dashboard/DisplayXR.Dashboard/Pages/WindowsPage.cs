// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Collections.Generic;
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
/// Troubleshooting > Running 3D apps (was the "windows" page; design §8 "Windows"): one card per service client — class, presenter, lease,
/// window and owner screen, the segment table, view-set counts, and on one row
/// the integrity triple with the weave placement (#1248: never a rate without
/// them). The counters update in place; the cards rebuild only when the
/// structure moves.
/// </summary>
public sealed class WindowsPage : Page
{
    private bool _showDiag;
    private readonly Dictionary<long, TextBlock> _integrity = new();

    public WindowsPage() : base("windows", "Running 3D apps") { }

    private IReadOnlyList<Client> Visible(StatusSnapshot s) => s.Clients.Where(c => _showDiag || !c.IsDiag).ToList();

    public override string Caption
    {
        get
        {
            var s = Ctx.Feed.Snapshot;
            if (s is null) return "Looking for running 3D apps…";
            if (!s.IsService) return "Running apps are listed when DisplayXR runs in the background.";
            int apps = s.Clients.Count(c => !c.IsDiag), diag = s.Clients.Count - apps;
            return $"{StatusText.Plural(apps, "app", "apps")}{StatusText.Sep}{StatusText.Plural(diag, "status reader", "status readers")}";
        }
    }

    public override Control? HeaderActions
    {
        get
        {
            var s = Ctx.Feed.Snapshot;
            int diag = s?.Clients.Count(c => c.IsDiag) ?? 0;
            var cb = new CheckBox { Content = $"Show status readers ({diag})", IsChecked = _showDiag, VerticalAlignment = VerticalAlignment.Center };
            cb.IsCheckedChanged += (_, _) =>
            {
                _showDiag = cb.IsChecked == true;
                Update();
            };
            return cb;
        }
    }

    protected override string Key()
    {
        var s = Ctx.Feed.Snapshot;
        if (s is null) return "none";
        var sb = new StringBuilder();
        sb.Append(_showDiag).Append('|').Append(s.Source).Append('|');
        foreach (var c in Visible(s))
        {
            // Everything but the integrity counters, which tick every frame.
            sb.Append(c.Id).Append(',').Append(c.Pid).Append(',').Append(c.Class).Append(',').Append(c.Name).Append(',')
              .Append(c.Flags).Append(',').Append(c.Presenter).Append(',').Append(c.Lease).Append(',').Append(c.Window)
              .Append(',').Append(StatusText.ScreenRef(s, c.OwnerScreen)).Append(',').Append(c.Views).Append(',')
              .Append(c.Segments.Generation).Append(',').Append(c.Segments.Split).Append(',');
            foreach (var seg in c.Segments.Items) sb.Append(seg).Append(StatusText.ScreenRef(s, seg.Screen));
            sb.Append(';');
        }
        return sb.ToString();
    }

    protected override void UpdateInPlace()
    {
        var s = Ctx.Feed.Snapshot;
        if (s is null) return;
        foreach (var c in s.Clients)
            if (_integrity.TryGetValue(c.Id, out var t)) t.Text = $"{StatusText.ViewsLine(c)}{StatusText.Sep}{StatusText.IntegrityLine(c)}";
    }

    protected override Control Build()
    {
        _integrity.Clear();
        var s = Ctx.Feed.Snapshot;
        var page = U.VStack(16);
        if (s is null)
        {
            page.Children.Add(U.Card(U.VStack(8, U.Text("Looking for running 3D apps", "group"),
                U.Wrapped("This takes about a second when DisplayXR runs in the background.", "desc"))));
            return page;
        }
        var clients = Visible(s);
        if (clients.Count == 0)
        {
            string text = s.IsService
                ? "No 3D app is running through DisplayXR's background service. Many 3D apps run on their own and are not listed here, so an empty list does not mean anything is wrong."
                : "DisplayXR isn't running in the background, so there is no list of running apps. It normally starts when you sign in.";
            page.Children.Add(U.Card(U.VStack(8, U.Text("No 3D apps listed", "group"), U.Wrapped(text, "desc"))));
        }
        foreach (var c in clients) page.Children.Add(ClientCard(s, c));
        page.Children.Add(RolesCard(s));
        return page;
    }

    /// <summary>
    /// The running programs by role (moved here from Components, which lists what you have and can add):
    /// apps with their own window and status readers, each with its process line. Status readers are
    /// listed when "Show status readers" is ticked.
    /// </summary>
    private Control RolesCard(StatusSnapshot s)
    {
        var body = U.VStack(14, U.CardTitle("By role", "Running programs that DisplayXR knows by what they do."));
        foreach (var sec in Components.RunningPrograms(s))
        {
            var head = U.Flow(10, 4, U.Text(sec.Title, "value"), U.Text(sec.ConnectedLine, "meta"));
            var col = U.VStack(4, head, U.Wrapped(sec.Role, "desc"));
            bool hidden = sec.Collapsed && !_showDiag;
            if (hidden && sec.Items.Count > 0)
                col.Children.Add(U.Wrapped($"Tick \"Show status readers\" to list {(sec.Items.Count == 1 ? "it" : "them")}.", "meta"));
            else
                foreach (var item in sec.Items)
                {
                    var line = U.Flow(10, 2, U.Text(item.Name, "soft"));
                    foreach (var l in item.Lines) U.AddFlow(line, U.Text(l, "dev"), 10, 2);
                    col.Children.Add(line);
                }
            body.Children.Add(col);
        }
        return U.Card(body);
    }

    private Control ClientCard(StatusSnapshot s, Client c)
    {
        bool flat = c.Segments.Items.Any(i => !i.HasDp);
        var name = U.Wrapped(StatusText.ClientName(c), "group");
        name.FontSize = 16;
        var title = U.Flow(10, 4, name);
        U.AddFlow(title, U.Chip(c.Class, c.Class == "UNVERIFIED" ? Level.Warn : Level.Plain), 6, 4);
        if (c.Presenter != "NONE") U.AddFlow(title, U.Chip(c.Presenter), 6, 4);
        if (c.Lease != "none") U.AddFlow(title, U.Chip($"lease {c.Lease}", Level.Info), 6, 4);
        foreach (var f in StatusText.ClientFlags(c)) U.AddFlow(title, U.Chip(f, f == "focused" ? Level.Ok : Level.Plain), 6, 4);

        var ids = U.Text($"pid {c.Pid}{StatusText.Sep}id {c.Id}", "meta");
        ids.VerticalAlignment = VerticalAlignment.Top;
        ids.Margin = new Thickness(12, 4, 0, 0);
        var head = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
        head.Children.Add(title);
        Grid.SetColumn(ids, 1);
        head.Children.Add(ids);

        var body = U.VStack(6, head);
        body.Children.Add(U.Wrapped($"window {StatusText.WindowText(c.Window)}{StatusText.Sep}owner {StatusText.ScreenRef(s, c.OwnerScreen)}", "soft"));
        var integrity = U.Wrapped($"{StatusText.ViewsLine(c)}{StatusText.Sep}{StatusText.IntegrityLine(c)}", "dev");
        _integrity[c.Id] = integrity;
        body.Children.Add(integrity);

        if (c.Segments.Items.Count > 0)
        {
            var label = U.Text($"SEGMENTS{StatusText.Sep}GEN {c.Segments.Generation}{(c.Segments.Split ? StatusText.Sep + "SPLIT" : "")}", "section");
            label.Margin = new Thickness(0, 8, 0, 2);
            body.Children.Add(label);
            body.Children.Add(SegmentTable(s, c));
        }
        else
        {
            body.Children.Add(U.Wrapped("No segment table: the app has not reported one (its window is not split, or it has no window).", "desc"));
        }

        if (flat)
            foreach (var seg in c.Segments.Items.Where(i => !i.HasDp))
            {
                var row = U.WarningRow(new StatusWarning("SEGMENT_FLAT_2D", WarningLevel.Warn,
                    StatusText.FlatSegmentText(s, seg).Replace("SEGMENT_FLAT_2D: ", "")));
                row.Margin = new Thickness(0, 4, 0, 0);
                body.Children.Add(row);
            }

        var card = U.Card(body);
        if (flat) card.BorderBrush = Tokens.Alpha("WarnColor", 0.45);
        return card;
    }

    private static Control SegmentTable(StatusSnapshot s, Client c)
    {
        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("2*,1.4*,Auto,Auto,Auto") };
        string[] heads = { "Display", "Canvas", "Display driver", "Woven", "Eyes" };
        g.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
        for (int i = 0; i < heads.Length; i++)
        {
            var h = U.Text(heads[i], "meta");
            h.Margin = new Thickness(i == 0 ? 0 : 0, 0, 22, 4);
            Grid.SetColumn(h, i);
            g.Children.Add(h);
        }
        int row = 1;
        foreach (var seg in c.Segments.Items)
        {
            g.RowDefinitions.Add(new RowDefinition(GridLength.Auto));
            var cells = new Control[]
            {
                U.Wrapped(StatusText.ScreenRef(s, seg.Screen), "value"),
                U.Text(StatusText.CanvasText(seg.Canvas), "mono"),
                YesNo(seg.HasDp, Level.Warn),
                YesNo(seg.Woven, Level.Plain),
                U.Text(seg.EyeSource, "mono"),
            };
            for (int i = 0; i < cells.Length; i++)
            {
                cells[i].Margin = new Thickness(0, 3, 22, 3);
                cells[i].VerticalAlignment = VerticalAlignment.Center;
                Grid.SetRow(cells[i], row);
                Grid.SetColumn(cells[i], i);
                g.Children.Add(cells[i]);
            }
            row++;
        }
        return new Border { Child = g, Classes = { "field" } };
    }

    private static Control YesNo(bool yes, Level noLevel)
    {
        var t = U.Text(yes ? "yes" : "no");
        t.Foreground = yes ? Tokens.For(Level.Ok) : noLevel == Level.Plain ? Tokens.Muted : Tokens.For(noLevel);
        t.FontWeight = FontWeight.Medium;
        return t;
    }
}

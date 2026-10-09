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
/// Windows (design §8): one card per service client — class, presenter, lease,
/// window and owner screen, the segment table, view-set counts, and on one row
/// the integrity triple with the weave placement (#1248: never a rate without
/// them). The counters update in place; the cards rebuild only when the
/// structure moves.
/// </summary>
public sealed class WindowsPage : Page
{
    private bool _showDiag;
    private readonly Dictionary<long, TextBlock> _integrity = new();

    public WindowsPage() : base("windows", "Windows") { }

    private IReadOnlyList<Client> Visible(StatusSnapshot s) => s.Clients.Where(c => _showDiag || !c.IsDiag).ToList();

    public override string Caption
    {
        get
        {
            var s = Ctx.Feed.Snapshot;
            if (s is null) return "Waiting for the first status snapshot…";
            if (!s.IsService) return "Live client rows need the service; this is a headless snapshot.";
            int apps = s.Clients.Count(c => !c.IsDiag), diag = s.Clients.Count - apps;
            return $"{StatusText.Plural(apps, "client", "clients")}{StatusText.Sep}{diag} diagnostic";
        }
    }

    public override Control? HeaderActions
    {
        get
        {
            var s = Ctx.Feed.Snapshot;
            int diag = s?.Clients.Count(c => c.IsDiag) ?? 0;
            var cb = new CheckBox { Content = $"Show diagnostic clients ({diag})", IsChecked = _showDiag, VerticalAlignment = VerticalAlignment.Center };
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
            page.Children.Add(U.Card(U.VStack(8, U.Text("Reading the client list", "group"),
                U.Wrapped("Waiting for the first status snapshot.", "desc"))));
            return page;
        }
        var clients = Visible(s);
        if (clients.Count == 0)
        {
            string text = s.IsService
                ? "No OpenXR app is connected to the service. In-process apps (handle / hosted / texture without the shell) do not appear here: they never talk to the service."
                : "No service was reached, so there are no live clients to list. Start the DisplayXR service (it starts at logon) to see apps that run through it.";
            page.Children.Add(U.Card(U.VStack(8, U.Text("No windows", "group"), U.Wrapped(text, "desc"))));
            return page;
        }
        foreach (var c in clients) page.Children.Add(ClientCard(s, c));
        return page;
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
            body.Children.Add(U.Wrapped("No segment table: the client has not reported one (not split, or not a windowed client).", "desc"));
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
        string[] heads = { "Screen", "Canvas", "DP", "Woven", "Eyes" };
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

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
/// Components: what is plugged into the runtime, grouped by ROLE (display
/// processors, workspace controller, present owners, input providers, the
/// conversion module, a stereo camera source, diagnostics clients), never by
/// product. Clients come from the live feed; the rest from <c>info --json</c>,
/// read once per show and on Refresh.
/// </summary>
public sealed class ComponentsPage : Page
{
    private readonly HashSet<string> _expanded = new();

    public ComponentsPage() : base("components", "Components") { }

    public override string Caption => "What is plugged into the runtime, by role: registered and connected.";

    public override void OnShown() => _ = Ctx.LoadInfoAsync(force: false);

    private IReadOnlyList<ComponentSection> Sections() =>
        Components.Build(Ctx.Feed.Snapshot, Ctx.Info?.Components);

    protected override string Key()
    {
        var sb = new StringBuilder();
        sb.Append(Ctx.InfoLoading).Append(Ctx.InfoError).Append(string.Join(",", _expanded)).Append('|');
        foreach (var s in Sections())
        {
            sb.Append(s.Id).Append(s.Count).Append(s.Connected).Append(s.ConnectedLine);
            foreach (var i in s.Items) sb.Append(i.Name).Append(i.Level).Append(string.Join("/", i.Chips)).Append(string.Join("/", i.Lines));
            sb.Append(';');
        }
        return sb.ToString();
    }

    protected override Control Build()
    {
        var page = U.VStack(18);
        if (Ctx.Info is null)
            page.Children.Add(U.Notice(Ctx.InfoError is null ? Level.Info : Level.Warn,
                Ctx.InfoError is null ? "Reading the runtime's components" : "Could not read the runtime's components",
                Ctx.InfoError ?? "Input providers and the conversion module come from 'displayxr-cli info' (about 10 s); connected clients come from the live feed."));
        var grid = new U.CardGrid(2) { MinColumnWidth = 460 };
        foreach (var s in Sections()) grid.Add(SectionCard(s), s.Id is "display_processors" ? 2 : 1);
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
            }
            body.Children.Add(list);
            if (s.Collapsed) body.Children.Add(U.Button("Hide", () => Toggle(s.Id), "link"));
        }
        return U.Card(body);
    }

    private static string EmptyText(ComponentSection s) => s.Id switch
    {
        "display_processors" => "No display-processor plug-in is registered.",
        "workspace_controller" => "No workspace controller is registered or connected.",
        "present_owners" => "No present owner is connected.",
        "input_providers" => "No input provider reported.",
        "conversion" => "No conversion module reported.",
        _ => "None.",
    };

    private void Toggle(string id)
    {
        if (!_expanded.Remove(id)) _expanded.Add(id);
        Update();
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

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Controls.Shapes;
using Avalonia.Layout;
using Avalonia.Media;
using DisplayXR.Dashboard.Model;

namespace DisplayXR.Dashboard.Ui;

/// <summary>
/// Factories for the components every page is made of. Everything visual
/// comes from Styles/Dashboard.axaml (class names) or Tokens (state colours).
/// </summary>
public static class U
{
    public static TextBlock Text(string text, params string[] classes)
    {
        var t = new TextBlock { Text = text };
        foreach (var c in classes) t.Classes.Add(c);
        return t;
    }

    public static TextBlock Wrapped(string text, params string[] classes)
    {
        var t = Text(text, classes);
        t.TextWrapping = TextWrapping.Wrap;
        return t;
    }

    public static StackPanel VStack(double spacing, params Control[] children)
    {
        var p = new StackPanel { Orientation = Orientation.Vertical, Spacing = spacing };
        foreach (var c in children) p.Children.Add(c);
        return p;
    }

    public static StackPanel HStack(double spacing, params Control[] children)
    {
        var p = new StackPanel { Orientation = Orientation.Horizontal, Spacing = spacing };
        foreach (var c in children) p.Children.Add(c);
        return p;
    }

    /// <summary>A wrapping row: chips and buttons flow onto the next line at narrow widths instead of clipping.</summary>
    public static WrapPanel Flow(double gapX, double gapY, params Control[] children)
    {
        var w = new WrapPanel { Orientation = Orientation.Horizontal };
        foreach (var c in children) AddFlow(w, c, gapX, gapY);
        return w;
    }

    public static void AddFlow(WrapPanel w, Control c, double gapX = 8, double gapY = 6)
    {
        c.Margin = new Thickness(0, 0, gapX, gapY);
        w.Children.Add(c);
    }

    public static Border Card(Control content)
    {
        var b = new Border { Child = content };
        b.Classes.Add("card");
        return b;
    }

    /// <summary>A card title: the heading, an optional caption under it, an optional control at the right.</summary>
    public static Control CardTitle(string title, string? caption = null, Control? right = null)
    {
        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("*,Auto") };
        var left = VStack(3, Text(title, "group"));
        if (caption is not null) left.Children.Add(Wrapped(caption, "desc"));
        g.Children.Add(left);
        if (right is not null)
        {
            Grid.SetColumn(right, 1);
            right.VerticalAlignment = VerticalAlignment.Top;
            right.Margin = new Thickness(16, 0, 0, 0);
            g.Children.Add(right);
        }
        return g;
    }

    public static Button Button(string label, Action onClick, params string[] classes)
    {
        var b = new Button { Content = label };
        foreach (var c in classes) b.Classes.Add(c);
        b.Click += (_, _) =>
        {
            try { onClick(); }
            catch (Exception ex) { DashboardLog.Error($"button '{label}'", ex); }
        };
        return b;
    }

    public static Border Rule()
    {
        var b = new Border();
        b.Classes.Add("rule");
        return b;
    }

    public static Ellipse Dot(Level level, double size = 9)
    {
        return new Ellipse
        {
            Width = size, Height = size, Fill = Tokens.For(level), VerticalAlignment = VerticalAlignment.Center,
        };
    }

    /// <summary>A role chip: outlined, quiet ("OS main", "runtime default").</summary>
    public static Border Chip(string text, Level level = Level.Plain)
    {
        var brush = level == Level.Plain ? Tokens.FgDim : Tokens.For(level);
        var t = Text(text);
        t.FontSize = 11.5;
        t.Foreground = brush;
        t.VerticalAlignment = VerticalAlignment.Center;
        return new Border
        {
            Child = t,
            Background = level == Level.Plain ? Tokens.Field : Tokens.Alpha(Tokens.ColorKey(level), 0.12),
            BorderBrush = level == Level.Plain ? Tokens.Line : Tokens.Alpha(Tokens.ColorKey(level), 0.55),
            BorderThickness = new Thickness(1),
            CornerRadius = Tokens.Radius("ChipRadius"),
            Padding = new Thickness(9, 2.5),
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Center,
        };
    }

    /// <summary>A state badge: filled tint, bold caps, an optional leading tick ("VERIFIED").</summary>
    public static Border Badge(string text, Level level, bool tick = false)
    {
        var brush = level == Level.Plain ? Tokens.FgDim : Tokens.For(level);
        var row = HStack(5);
        if (tick)
        {
            row.Children.Add(new Path
            {
                Data = Geometry.Parse("M 1,5 L 4,8 L 10,1"), Stroke = brush, StrokeThickness = 1.8,
                StrokeLineCap = PenLineCap.Round, StrokeJoin = PenLineJoin.Round, Width = 11, Height = 9,
                VerticalAlignment = VerticalAlignment.Center,
            });
        }
        var t = Text(text);
        t.FontSize = 11;
        t.FontWeight = FontWeight.Bold;
        t.LetterSpacing = 0.5;
        t.Foreground = brush;
        t.VerticalAlignment = VerticalAlignment.Center;
        row.Children.Add(t);
        return new Border
        {
            Child = row,
            Background = level == Level.Plain ? Tokens.Field : Tokens.Alpha(Tokens.ColorKey(level), 0.16),
            BorderBrush = level == Level.Plain ? Tokens.Line : Tokens.Alpha(Tokens.ColorKey(level), 0.7),
            BorderThickness = new Thickness(1),
            CornerRadius = new CornerRadius(6),
            Padding = new Thickness(8, 3),
            HorizontalAlignment = HorizontalAlignment.Left,
            VerticalAlignment = VerticalAlignment.Center,
        };
    }

    /// <summary>A notice strip (banner): a dot, a bold title, a sentence, optional actions.</summary>
    public static Border Notice(Level level, string title, string text, params Control[] actions)
    {
        var body = VStack(3);
        if (title.Length > 0)
        {
            var tt = Wrapped(title);
            tt.FontWeight = FontWeight.SemiBold;
            if (level is Level.Warn or Level.Critical) tt.Foreground = Tokens.For(level);
            body.Children.Add(tt);
        }
        if (text.Length > 0) body.Children.Add(Wrapped(text, "dim"));
        if (actions.Length > 0)
        {
            var row = Flow(10, 0, actions);
            row.Margin = new Thickness(0, 6, 0, 0);
            body.Children.Add(row);
        }
        var dot = Dot(level, 9);
        dot.VerticalAlignment = VerticalAlignment.Top;
        dot.Margin = new Thickness(0, 5, 12, 0);
        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
        g.Children.Add(dot);
        Grid.SetColumn(body, 1);
        g.Children.Add(body);
        var b = new Border { Child = g };
        b.Classes.Add("notice");
        if (level == Level.Warn) b.Classes.Add("warn");
        if (level == Level.Critical) b.Classes.Add("critical");
        return b;
    }

    /// <summary>A warning row: level dot, then "CODE: text" (design §4: Title: fix).</summary>
    public static Control WarningRow(StatusWarning w, string? prefix = null)
    {
        var level = StatusText.ToLevel(w.Level);
        var dot = Dot(level, 7);
        dot.VerticalAlignment = VerticalAlignment.Top;
        dot.Margin = new Thickness(0, 7, 10, 0);
        var text = new TextBlock { TextWrapping = TextWrapping.Wrap };
        text.Classes.Add("soft");
        if (prefix is not null) text.Inlines!.Add(new Avalonia.Controls.Documents.Run(prefix) { Foreground = Tokens.Muted });
        text.Inlines!.Add(new Avalonia.Controls.Documents.Run(w.Code) { Foreground = Tokens.For(level), FontWeight = FontWeight.SemiBold });
        if (w.Text.Length > 0) text.Inlines.Add(new Avalonia.Controls.Documents.Run(": " + w.Text));
        var g = new Grid { ColumnDefinitions = new ColumnDefinitions("Auto,*") };
        g.Children.Add(dot);
        Grid.SetColumn(text, 1);
        g.Children.Add(text);
        return g;
    }

    /// <summary>Two-column key / value list: muted labels, values wrapping.</summary>
    public sealed class KvList : Grid
    {
        private int _row;

        public KvList(double labelWidth = 150)
        {
            ColumnDefinitions = new ColumnDefinitions($"{labelWidth.ToString(System.Globalization.CultureInfo.InvariantCulture)},*");
        }

        public KvList Add(string label, string value, bool mono = false, IBrush? brush = null)
        {
            var v = Wrapped(value, mono ? new[] { "value", "mono" } : new[] { "value" });
            if (brush is not null) v.Foreground = brush;
            return Add(label, v);
        }

        public KvList Add(string label, Control value)
        {
            var l = Wrapped(label, "label");
            RowDefinitions.Add(new RowDefinition(GridLength.Auto));
            double top = _row == 0 ? 0 : 9;
            l.Margin = new Thickness(0, top, 14, 0);
            value.Margin = new Thickness(0, top, 0, 0);
            value.VerticalAlignment = VerticalAlignment.Top;
            Grid.SetRow(l, _row);
            Grid.SetRow(value, _row);
            Grid.SetColumn(value, 1);
            Children.Add(l);
            Children.Add(value);
            _row++;
            return this;
        }
    }

    /// <summary>
    /// A responsive grid of cards: up to <c>maxColumns</c> equal columns, fewer
    /// when a column would be narrower than <see cref="MinColumnWidth"/> (at
    /// 250 % a fixed two-column layout leaves cards too narrow to read).
    /// </summary>
    public sealed class CardGrid : Grid
    {
        private readonly List<(Control Control, int Span)> _items = new();
        private readonly double _gap;
        private readonly int _maxColumns;
        private int _columns = -1;
        public double MinColumnWidth { get; set; } = 400;

        public CardGrid(int maxColumns = 2, double gap = 18)
        {
            _maxColumns = Math.Max(1, maxColumns);
            _gap = gap;
        }

        public CardGrid Add(Control c, int span = 1)
        {
            _items.Add((c, span));
            Children.Add(c);
            _columns = -1;
            return this;
        }

        private void Place(int columns)
        {
            if (columns == _columns) return;
            _columns = columns;
            ColumnDefinitions.Clear();
            RowDefinitions.Clear();
            for (int i = 0; i < columns; i++) ColumnDefinitions.Add(new ColumnDefinition(1, GridUnitType.Star));
            int row = 0, col = 0;
            foreach (var (control, spanWanted) in _items)
            {
                int span = Math.Min(spanWanted, columns);
                if (col + span > columns) { row++; col = 0; }
                while (RowDefinitions.Count <= row) RowDefinitions.Add(new RowDefinition(GridLength.Auto));
                SetRow(control, row);
                SetColumn(control, col);
                SetColumnSpan(control, span);
                control.Margin = new Thickness(col == 0 ? 0 : _gap / 2, row == 0 ? 0 : _gap, col + span == columns ? 0 : _gap / 2, 0);
                control.VerticalAlignment = VerticalAlignment.Stretch;
                col += span;
                if (col >= columns) { col = 0; row++; }
            }
        }

        protected override Size MeasureOverride(Size available)
        {
            int fit = _maxColumns;
            if (double.IsFinite(available.Width) && MinColumnWidth > 0)
                fit = (int)Math.Floor((available.Width + _gap) / (MinColumnWidth + _gap));
            Place(Math.Clamp(fit, 1, _maxColumns));
            return base.MeasureOverride(available);
        }
    }

    /// <summary>Detach a control from whatever holds it, so a long-lived control can move between rebuilt trees.</summary>
    public static void Detach(Control c)
    {
        switch (c.Parent)
        {
            case Panel p: p.Children.Remove(c); break;
            case Decorator d: d.Child = null; break;
            case ContentControl cc: cc.Content = null; break;
        }
    }
}

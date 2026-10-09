// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Globalization;
using Avalonia;
using Avalonia.Controls;
using Avalonia.Media;
using DisplayXR.Dashboard.Model;

namespace DisplayXR.Dashboard.Ui;

/// <summary>
/// The desktop map (design §8): monitors to scale, labelled; client windows as
/// outlines; a dashed seam where a window crosses a monitor edge; the owner
/// screen tinted. Geometry is <see cref="DesktopMapGeometry"/>; this only draws.
/// </summary>
public sealed class DesktopMap : Control
{
    private IReadOnlyList<DesktopMapGeometry.ScreenIn> _screens = Array.Empty<DesktopMapGeometry.ScreenIn>();
    private IReadOnlyList<DesktopMapGeometry.WindowIn> _windows = Array.Empty<DesktopMapGeometry.WindowIn>();
    private string _key = "";
    private const double Pad = 14;

    public DesktopMap()
    {
        ClipToBounds = true;
        MinHeight = 140;
    }

    public void SetData(IReadOnlyList<DesktopMapGeometry.ScreenIn> screens, IReadOnlyList<DesktopMapGeometry.WindowIn> windows)
    {
        string key = string.Join("|", Key(screens)) + "#" + string.Join("|", Key(windows));
        if (key == _key) return;
        bool aspectChanged = Math.Abs(DesktopMapGeometry.Aspect(screens) - DesktopMapGeometry.Aspect(_screens)) > 1e-6;
        _key = key;
        _screens = screens;
        _windows = windows;
        if (aspectChanged) InvalidateMeasure();
        InvalidateVisual();
    }

    private static IEnumerable<string> Key(IReadOnlyList<DesktopMapGeometry.ScreenIn> s)
    {
        foreach (var x in s) yield return $"{x.Id},{x.Label},{x.Detail},{x.Rect},{x.Level}";
    }

    private static IEnumerable<string> Key(IReadOnlyList<DesktopMapGeometry.WindowIn> w)
    {
        foreach (var x in w) yield return $"{x.ClientId},{x.Label},{x.Rect},{x.Owner},{x.Split}";
    }

    protected override Size MeasureOverride(Size availableSize)
    {
        double w = double.IsFinite(availableSize.Width) ? availableSize.Width : 640;
        double h = Math.Clamp((w - 2 * Pad) * DesktopMapGeometry.Aspect(_screens) + 2 * Pad, 140, 420);
        return new Size(w, h);
    }

    private static FormattedText Label(string text, double size, IBrush brush, FontWeight weight = FontWeight.Normal) =>
        new(text, CultureInfo.InvariantCulture, FlowDirection.LeftToRight,
            new Typeface(FontFamily.Default, FontStyle.Normal, weight), size, brush);

    public override void Render(DrawingContext ctx)
    {
        var bounds = new Rect(Bounds.Size);
        ctx.DrawRectangle(Tokens.Field, null, bounds, 8, 8);
        var layout = DesktopMapGeometry.Compute(_screens, _windows, bounds.Width, bounds.Height, Pad);
        if (layout.Screens.Count == 0 && layout.Windows.Count == 0)
        {
            var t = Label("No monitors reported", 12.5, Tokens.Muted);
            ctx.DrawText(t, new Point((bounds.Width - t.Width) / 2, (bounds.Height - t.Height) / 2));
            return;
        }

        var monitorPen = new Pen(Tokens.Line, 1);
        var ownerFill = Tokens.Alpha("AccentColor", 0.14);
        var ownerPen = new Pen(Tokens.Alpha("AccentColor", 0.75), 1.5);
        foreach (var s in layout.Screens)
        {
            var r = Inset(s.Rect, 2);
            if (r.Width <= 0 || r.Height <= 0) continue;
            ctx.DrawRectangle(s.IsOwner ? ownerFill : Tokens.Panel, s.IsOwner ? ownerPen : monitorPen, r, 6, 6);
            // A thin status bar along the top edge: the screen's dot colour.
            if (s.Level != Level.Plain && r.Width > 24)
                ctx.DrawRectangle(Tokens.For(s.Level), null, new Rect(r.X + 10, r.Y + 7, Math.Min(26, r.Width - 20), 3), 1.5, 1.5);

            using (ctx.PushClip(r))
            {
                double size = Math.Clamp(r.Height / 9, 10, 15);
                var name = Label(s.Label, size, Tokens.Fg, FontWeight.SemiBold);
                var detail = Label(s.Detail, Math.Max(9.5, size - 2.5), Tokens.Muted);
                // Bottom-left: client windows usually sit higher up, so the label stays readable.
                bool showDetail = r.Height > 70;
                double y = r.Bottom - 8 - name.Height - (showDetail ? detail.Height + 1 : 0);
                y = Math.Max(r.Y + 14, y);
                ctx.DrawText(name, new Point(r.X + 10, y));
                if (showDetail) ctx.DrawText(detail, new Point(r.X + 10, y + name.Height + 1));
            }
        }

        var windowPen = new Pen(Tokens.Brush("AccentHoverBrush"), 1.6);
        var windowFill = Tokens.Alpha("AccentColor", 0.10);
        var clip = Inset(new Rect(bounds.Size), 2);
        using (ctx.PushClip(clip))
        {
            foreach (var w in layout.Windows)
            {
                var r = new Rect(w.Rect.X, w.Rect.Y, Math.Max(2, w.Rect.W), Math.Max(2, w.Rect.H));
                ctx.DrawRectangle(windowFill, windowPen, r, 3, 3);
                if (r.Width > 30 && r.Height > 18)
                {
                    using (ctx.PushClip(r))
                    {
                        // On a backdrop: the label sits over the monitor's own text.
                        var t = Label(w.Label, 11, Tokens.Fg, FontWeight.Medium);
                        ctx.DrawRectangle(Tokens.Alpha("FieldColor", 0.9), null,
                            new Rect(r.X + 2, r.Y + 2, Math.Min(t.Width + 10, r.Width - 4), t.Height + 4), 2, 2);
                        ctx.DrawText(t, new Point(r.X + 7, r.Y + 4));
                    }
                }
            }

            var seamPen = new Pen(Tokens.Brush("WarnBrush"), 1.6, new DashStyle(new double[] { 3, 2.5 }, 0));
            foreach (var seam in layout.Seams)
                ctx.DrawLine(seamPen, new Point(seam.X1, seam.Y1), new Point(seam.X2, seam.Y2));
        }
    }

    private static Rect Inset(RectD r, double d) => new(r.X + d, r.Y + d, Math.Max(0, r.W - 2 * d), Math.Max(0, r.H - 2 * d));

    private static Rect Inset(Rect r, double d) => new(r.X + d, r.Y + d, Math.Max(0, r.Width - 2 * d), Math.Max(0, r.Height - 2 * d));
}

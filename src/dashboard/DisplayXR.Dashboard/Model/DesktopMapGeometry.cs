// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;

namespace DisplayXR.Dashboard.Model;

public readonly record struct RectD(double X, double Y, double W, double H)
{
    public double Right => X + W;
    public double Bottom => Y + H;
}

public sealed record MapScreen(string Id, string Label, string Detail, RectD Rect, bool IsOwner, Level Level);

public sealed record MapWindow(long ClientId, string Label, RectD Rect, bool Split);

public sealed record MapSeam(double X1, double Y1, double X2, double Y2);

public sealed record MapLayout(double Scale, double OffsetX, double OffsetY, double DesktopLeft, double DesktopTop,
                               IReadOnlyList<MapScreen> Screens, IReadOnlyList<MapWindow> Windows, IReadOnlyList<MapSeam> Seams)
{
    public static readonly MapLayout Empty = new(0, 0, 0, 0, 0, Array.Empty<MapScreen>(), Array.Empty<MapWindow>(), Array.Empty<MapSeam>());
}

/// <summary>
/// The desktop map (design §8): every monitor to scale from its desktop rect,
/// client windows as outlines, the seam where a window crosses a monitor edge,
/// the owner screen tinted. Pure geometry; <c>Ui/DesktopMap</c> only draws it.
/// Desktop coordinates are physical pixels (the CLI is per-monitor DPI aware),
/// the same space the window rects are in.
/// </summary>
public static class DesktopMapGeometry
{
    public readonly record struct ScreenIn(string Id, string Label, string Detail, PixelRect Rect, Level Level);

    public readonly record struct WindowIn(long ClientId, string Label, PixelRect Rect, string? Owner, bool Split);

    /// <summary>Height / width of the desktop's bounding box (0.5 when there is nothing to draw).</summary>
    public static double Aspect(IReadOnlyList<ScreenIn> screens)
    {
        var b = Bounds(screens.Select(s => s.Rect));
        return b is { } r && r.W > 0 ? r.H / r.W : 0.5;
    }

    public static RectD? Bounds(IEnumerable<PixelRect> rects)
    {
        double l = double.MaxValue, t = double.MaxValue, r = double.MinValue, b = double.MinValue;
        bool any = false;
        foreach (var x in rects)
        {
            if (x.IsEmpty) continue;
            any = true;
            l = Math.Min(l, x.Left); t = Math.Min(t, x.Top);
            r = Math.Max(r, x.Right); b = Math.Max(b, x.Bottom);
        }
        return any ? new RectD(l, t, r - l, b - t) : null;
    }

    public static MapLayout Compute(IReadOnlyList<ScreenIn> screens, IReadOnlyList<WindowIn> windows,
                                    double width, double height, double pad)
    {
        if (!(width > 0) || !(height > 0)) return MapLayout.Empty;
        // The monitors set the frame; a window hanging off the desktop is clipped
        // to it when drawn, rather than shrinking every monitor to fit it.
        var frame = Bounds(screens.Select(s => s.Rect)) ?? Bounds(windows.Select(w => w.Rect));
        if (frame is not { } f) return MapLayout.Empty;

        double availW = Math.Max(1, width - 2 * pad), availH = Math.Max(1, height - 2 * pad);
        double scale = Math.Min(availW / f.W, availH / f.H);
        double ox = pad + (availW - f.W * scale) / 2 - f.X * scale;
        double oy = pad + (availH - f.H * scale) / 2 - f.Y * scale;
        RectD Map(PixelRect r) => new(ox + r.Left * scale, oy + r.Top * scale, r.Width * scale, r.Height * scale);

        var owners = new HashSet<string>(windows.Where(w => w.Owner is not null).Select(w => w.Owner!), StringComparer.OrdinalIgnoreCase);
        var mappedScreens = screens.Where(s => !s.Rect.IsEmpty)
            .Select(s => new MapScreen(s.Id, s.Label, s.Detail, Map(s.Rect), owners.Contains(s.Id), s.Level)).ToArray();
        var mappedWindows = windows.Where(w => !w.Rect.IsEmpty)
            .Select(w => new MapWindow(w.ClientId, w.Label, Map(w.Rect), w.Split)).ToArray();

        var seams = new List<MapSeam>();
        var seen = new HashSet<(long, long, long, long)>();
        foreach (var w in windows)
        {
            if (w.Rect.IsEmpty) continue;
            foreach (var s in screens)
            {
                if (s.Rect.IsEmpty) continue;
                // Vertical edges of this monitor strictly inside the window, over the rows both share.
                long top = Math.Max(w.Rect.Top, s.Rect.Top), bottom = Math.Min(w.Rect.Bottom, s.Rect.Bottom);
                if (bottom > top)
                    foreach (long x in new[] { s.Rect.Left, s.Rect.Right })
                        if (x > w.Rect.Left && x < w.Rect.Right && seen.Add((x, top, x, bottom)))
                            seams.Add(new MapSeam(ox + x * scale, oy + top * scale, ox + x * scale, oy + bottom * scale));
                // Horizontal edges, for monitors stacked vertically.
                long left = Math.Max(w.Rect.Left, s.Rect.Left), right = Math.Min(w.Rect.Right, s.Rect.Right);
                if (right > left)
                    foreach (long y in new[] { s.Rect.Top, s.Rect.Bottom })
                        if (y > w.Rect.Top && y < w.Rect.Bottom && seen.Add((left, y, right, y)))
                            seams.Add(new MapSeam(ox + left * scale, oy + y * scale, ox + right * scale, oy + y * scale));
            }
        }
        return new MapLayout(scale, ox, oy, f.X, f.Y, mappedScreens, mappedWindows, seams);
    }

    /// <summary>The map's inputs from a snapshot: monitors, and every non-diagnostic client with a window.</summary>
    public static (IReadOnlyList<ScreenIn> Screens, IReadOnlyList<WindowIn> Windows) From(StatusSnapshot s)
    {
        var screens = s.Screens.Select(x => new ScreenIn(
            x.Id, StatusText.ShortDevice(x.DeviceName) is { Length: > 0 } d ? d : StatusText.ScreenName(x),
            $"{StatusText.ScreenName(x)}{StatusText.Sep}{x.Desktop.Rect.Width}{StatusText.Times}{x.Desktop.Rect.Height}{StatusText.Sep}{StatusText.Scale(x.Desktop.Scale)}",
            x.Desktop.Rect, StatusText.ScreenLevel(x))).ToArray();
        var windows = s.Clients.Where(c => !c.IsDiag && c.Window is { IsEmpty: false })
            .Select(c => new WindowIn(c.Id, StatusText.ClientName(c), c.Window!.Value, c.OwnerScreen, c.Segments.Split)).ToArray();
        return (screens, windows);
    }
}

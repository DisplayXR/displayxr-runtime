// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;

namespace DisplayXR.Dashboard.Model;

// Camera-pose calibration (tracking-provider-discovery.md §6, displayxr-tracking-open
// docs/design.md §10): the pure half of the Dashboard's calibration window. The window
// (Ui/CalibrationWindow.cs) only draws what CalibrationSession says and feeds it keys,
// the `tracking calibrate --watch --json` lines and the solve / apply results. Nothing here
// links runtime code (ADR-051 D4): the solver is `displayxr-cli tracking calibrate solve`.

/// <summary>A plain 3-vector (display or camera space, metres).</summary>
public readonly record struct Vec3(double X, double Y, double Z)
{
    public static Vec3 operator +(Vec3 a, Vec3 b) => new(a.X + b.X, a.Y + b.Y, a.Z + b.Z);
    public static Vec3 operator -(Vec3 a, Vec3 b) => new(a.X - b.X, a.Y - b.Y, a.Z - b.Z);
    public static Vec3 operator *(double s, Vec3 a) => new(s * a.X, s * a.Y, s * a.Z);
    public double Dot(Vec3 b) => X * b.X + Y * b.Y + Z * b.Z;
    public Vec3 Cross(Vec3 b) => new(Y * b.Z - Z * b.Y, Z * b.X - X * b.Z, X * b.Y - Y * b.X);
    public double Length => Math.Sqrt(Dot(this));
    public Vec3 Normalized => Length > 0 ? (1.0 / Length) * this : this;

    /// <summary>The angle between two directions, degrees (0 for a zero vector).</summary>
    public static double AngleDeg(Vec3 a, Vec3 b)
    {
        double la = a.Length, lb = b.Length;
        if (la <= 0 || lb <= 0) return 0;
        double c = Math.Clamp(a.Dot(b) / (la * lb), -1.0, 1.0);
        return Math.Acos(c) * 180.0 / Math.PI;
    }

    internal static Vec3? Read(JsonElement? e, string key)
    {
        if (e is not { } o || o.ValueKind != JsonValueKind.Object || !o.TryGetProperty(key, out var a) ||
            a.ValueKind != JsonValueKind.Array || a.GetArrayLength() != 3) return null;
        var v = new double[3];
        int i = 0;
        foreach (var x in a.EnumerateArray())
        {
            if (x.ValueKind != JsonValueKind.Number || !x.TryGetDouble(out v[i]) || !double.IsFinite(v[i])) return null;
            i++;
        }
        return new Vec3(v[0], v[1], v[2]);
    }
}

/// <summary>
/// The two head distances of the procedure. One distance cannot separate a camera rotation from a
/// camera translation (a small yaw or pitch looks like a sideways or vertical offset at a fixed
/// distance), so every dot is aligned once leaning in and once sitting back.
/// </summary>
public enum CalibPass
{
    Near,
    Far,
}

/// <summary>
/// One target of the procedure: a dot, in one of the two passes. <see cref="Fx"/> / <see cref="Fy"/>
/// are fractions of the screen (0 = left / top edge, 1 = right / bottom edge).
/// </summary>
public sealed record CalibDot(int Index, string Name, double Fx, double Fy, CalibPass Pass)
{
    /// <summary>The name with its pass, for the status line and the result table.</summary>
    public string Label => $"{Name}, {(Pass == CalibPass.Near ? "near" : "far")}";
}

/// <summary>
/// The target screen's pixel grid and physical size, and the dot maths: a dot's display-space
/// position is (pixel − screen centre) × pixel pitch, y up (display space: +X right, +Y up, origin
/// at the screen centre).
/// </summary>
public sealed record CalibGeometry(long PixelWidth, long PixelHeight, double WidthM, double HeightM, string Source)
{
    /// <summary>
    /// The far pass's mean eye distance must be at least this many times the near pass's. Below it
    /// the rotation / translation split is poorly conditioned (in simulation, 0.2° direction and
    /// 2 mm alignment noise: a 1.3x spread leaves ~27 mm of T error, 1.5x ~14 mm, 1.75x ~10 mm).
    /// The distances are the provider's uncalibrated iris estimates, but a per-user scale cancels
    /// in the ratio.
    /// </summary>
    public const double MinDistanceRatio = 1.5;

    public bool IsValid => PixelWidth > 0 && PixelHeight > 0 && WidthM > 0 && HeightM > 0;

    public double PitchX => WidthM / PixelWidth;
    public double PitchY => HeightM / PixelHeight;

    /// <summary>The dot's pixel on the native grid (whole pixels, so the cross is crisp).</summary>
    public (long X, long Y) Pixel(CalibDot d) =>
        ((long)Math.Round(d.Fx * PixelWidth, MidpointRounding.AwayFromZero),
         (long)Math.Round(d.Fy * PixelHeight, MidpointRounding.AwayFromZero));

    /// <summary>Where the window draws the dot: its pixel as a fraction of the screen.</summary>
    public (double Fx, double Fy) Fraction(CalibDot d)
    {
        var (px, py) = Pixel(d);
        return ((double)px / PixelWidth, (double)py / PixelHeight);
    }

    /// <summary>The dot in display space, metres (y up).</summary>
    public (double X, double Y) Metres(CalibDot d)
    {
        var (px, py) = Pixel(d);
        return ((px - PixelWidth / 2.0) * PitchX, (PixelHeight / 2.0 - py) * PitchY);
    }

    /// <summary>A display-space point (metres, y up) as a fraction of the screen (the live-check dot).</summary>
    public (double Fx, double Fy) FromMetres(double x, double y) => (x / WidthM + 0.5, 0.5 - y / HeightM);

    /// <summary>
    /// The geometry to use. Metres: the provider's display space is the runtime's display info,
    /// which the watch's <c>hello.display</c> carries for the screen the provider serves (the runtime
    /// default screen in this release), so it wins there; otherwise the screen's layout metres, then
    /// its physical size. Pixels: the screen's native mode, else the hello's. Null when neither
    /// source has both.
    /// </summary>
    public static CalibGeometry? Choose(Screen? screen, CalibHelloDisplay? hello)
    {
        bool primary = screen?.Roles.RuntimeDefault ?? true;
        long pw = screen?.Native.Width ?? 0, ph = screen?.Native.Height ?? 0;
        if ((pw <= 0 || ph <= 0) && hello is { PixelWidth: > 0, PixelHeight: > 0 }) { pw = hello.PixelWidth; ph = hello.PixelHeight; }
        if (pw <= 0 || ph <= 0) return null;
        if (primary && hello is { WidthM: > 0, HeightM: > 0 } h)
            return new CalibGeometry(pw, ph, h.WidthM, h.HeightM, "display info");
        if (screen is { Layout: { WidthM: > 0, HeightM: > 0 } l })
            return new CalibGeometry(pw, ph, l.WidthM, l.HeightM, "screen layout");
        if (screen is { PhysicalMm: { Width: > 0, Height: > 0 } mm })
            return new CalibGeometry(pw, ph, mm.Width / 1000.0, mm.Height / 1000.0, "physical size (" + mm.Source + ")");
        if (hello is { WidthM: > 0, HeightM: > 0 } h2)
            return new CalibGeometry(pw, ph, h2.WidthM, h2.HeightM, "display info");
        return null;
    }
}

/// <summary>
/// What the camera can see, for the target layout and the live "face in view" guard. No provider
/// reports its field of view today, so <see cref="Default"/> is a conservative 60° horizontal at
/// 16:9 (18° half-height). A face counts as in view when its eye midpoint and the face around it —
/// <see cref="FaceAboveM"/> up to the forehead, <see cref="FaceBelowM"/> down to the mouth (eyes,
/// nose and mouth are what a landmarker needs; a cut-off chin is tolerated: a laptop camera
/// tracked a near-pass eye midpoint 19° below its axis at 42 cm, where the chin is ~31° below),
/// <see cref="FaceSideM"/> to each side — are inside the view less a margin.
/// </summary>
public sealed record CameraView(double HfovDeg, double Aspect)
{
    public static readonly CameraView Default = new(60, 9.0 / 16);

    public const double FaceAboveM = 0.070;
    public const double FaceBelowM = 0.070;
    public const double FaceSideM = 0.075;

    /// <summary>The layout keeps the face this far inside the view; the live guard fires at the edge itself.</summary>
    public const double LayoutMarginDeg = 2.0;

    private static readonly Vec3[] FaceOffsets =
    {
        new(0, FaceAboveM, 0), new(0, -FaceBelowM, 0), new(-FaceSideM, 0, 0), new(FaceSideM, 0, 0),
    };

    public double HalfHorizontalRad => HfovDeg * Math.PI / 360.0;
    public double HalfVerticalRad => Math.Atan(Math.Tan(HalfHorizontalRad) * Aspect);

    /// <summary>A camera-space point inside the view less <paramref name="marginDeg"/> (camera +Z toward the viewer).</summary>
    public bool InView(Vec3 p, double marginDeg)
    {
        double m = marginDeg * Math.PI / 180.0;
        double th = Math.Tan(HalfHorizontalRad - m), tv = Math.Tan(HalfVerticalRad - m);
        return p.Z > 0 && Math.Abs(p.X) <= th * p.Z && Math.Abs(p.Y) <= tv * p.Z;
    }

    /// <summary>The face around a camera-space eye midpoint is in view (the camera held upright).</summary>
    public bool FaceInView(Vec3 eyeCamera, double marginDeg) => FaceOffsets.All(o => InView(eyeCamera + o, marginDeg));

    /// <summary>The face around a display-space eye midpoint is in view of a camera at <paramref name="pose"/>.</summary>
    public bool FaceInView(CameraPose pose, Vec3 eyeDisplay, double marginDeg) =>
        FaceOffsets.All(o => InView(pose.ToCamera(eyeDisplay + o), marginDeg));
}

/// <summary>
/// The targets of one calibration, sized from the geometry (§6 Dashboard): a near pass of the five
/// central dots (centre; left, right, top, bottom at a 30 % inset) and a far pass of a 3 × 3 grid at
/// a 12 % inset, the wide grid being what pins roll and yaw. The eye midpoint sits on the normal
/// through the dot, so every dot is checked against the camera's view from the prior pose at its
/// pass distance (<see cref="CameraView.FaceInView(CameraPose, Vec3, double)"/>, with
/// <see cref="CameraView.LayoutMarginDeg"/>); a dot that would take the face out is moved toward the
/// camera's axis (vertically first, so it keeps its sideways lever), dots that end within 15 mm of
/// another in their pass are dropped, and a pass whose every dot is out moves 5 cm further back.
/// </summary>
public sealed class CalibLayout
{
    public const double NearInset = 0.30;
    public const double FarInset = 0.12;
    public const double NearTargetM = 0.40;
    public const double FarTargetM = 0.70;
    /// <summary>The far pass is at least this many times the near one (above <see cref="CalibGeometry.MinDistanceRatio"/>, for slack).</summary>
    public const double FarOverNear = 1.75;
    public const double StepM = 0.05;
    public const double MaxDistanceM = 1.20;
    public const double MinSeparationM = 0.015;

    /// <summary>The default prior (no stored pose): upright, top centre, 8 mm above the panel (u_camera_calib_default_prior).</summary>
    public static CameraPose DefaultPrior(double heightM) => new(new Vec3(0, heightM / 2 + 0.008, 0), 0, 0, 0, 1);

    private static readonly (string Name, double Fx, double Fy)[] NearIdeal =
    {
        ("centre", 0.5, 0.5), ("left", NearInset, 0.5), ("right", 1 - NearInset, 0.5),
        ("top", 0.5, NearInset), ("bottom", 0.5, 1 - NearInset),
    };

    private static readonly (string Name, double Fx, double Fy)[] FarIdeal =
    {
        ("top left", FarInset, FarInset), ("top", 0.5, FarInset), ("top right", 1 - FarInset, FarInset),
        ("left", FarInset, 0.5), ("centre", 0.5, 0.5), ("right", 1 - FarInset, 0.5),
        ("bottom left", FarInset, 1 - FarInset), ("bottom", 0.5, 1 - FarInset), ("bottom right", 1 - FarInset, 1 - FarInset),
    };

    private CalibLayout(IReadOnlyList<CalibDot> dots, double nearM, double farM, CameraPose prior, string priorSource, CameraView view,
                        IReadOnlyList<string> notes)
    {
        Dots = dots;
        NearM = nearM;
        FarM = farM;
        Prior = prior;
        PriorSource = priorSource;
        View = view;
        Notes = notes;
    }

    public IReadOnlyList<CalibDot> Dots { get; }
    /// <summary>The pass distances the prompts ask for (eye to glass, metres).</summary>
    public double NearM { get; }
    public double FarM { get; }
    public CameraPose Prior { get; }
    public string PriorSource { get; }
    public CameraView View { get; }
    /// <summary>What the geometry changed (a dot moved or dropped, a pass moved back), for the log.</summary>
    public IReadOnlyList<string> Notes { get; }

    public double DistanceM(CalibPass pass) => pass == CalibPass.Near ? NearM : FarM;

    public IReadOnlyList<int> PassIndices(CalibPass pass) => Dots.Where(d => d.Pass == pass).Select(d => d.Index).ToArray();

    /// <summary>The layout for <paramref name="g"/> seen from <paramref name="prior"/> (null: the default prior).</summary>
    public static CalibLayout Build(CalibGeometry g, CameraPose? prior = null, string? priorSource = null, CameraView? view = null)
    {
        var pose = prior ?? DefaultPrior(g.HeightM);
        string source = prior is null ? "default-prior" : priorSource is { Length: > 0 } s ? s : "hello";
        var v = view ?? CameraView.Default;
        var notes = new List<string>();
        var dots = new List<CalibDot>();
        double near = PlacePass(g, pose, v, CalibPass.Near, NearIdeal, NearTargetM, dots, notes);
        double far = PlacePass(g, pose, v, CalibPass.Far, FarIdeal, Math.Max(FarTargetM, FarOverNear * near), dots, notes);
        return new CalibLayout(dots, near, far, pose, source, v, notes);
    }

    private static double PlacePass(CalibGeometry g, CameraPose pose, CameraView v, CalibPass pass, (string Name, double Fx, double Fy)[] ideal,
                                    double z, List<CalibDot> dots, List<string> notes)
    {
        string passName = pass == CalibPass.Near ? "near" : "far";
        double target = z;
        for (; z <= MaxDistanceM + 1e-9; z += StepM)
        {
            var anchor = Anchor(g, pose, z);
            if (!Fits(v, pose, anchor.X, anchor.Y, z)) continue;
            if (z > target + 1e-9)
                notes.Add(string.Format(CultureInfo.InvariantCulture, "{0} pass moved back to {1:0} cm: closer, no target keeps the face in the camera's view",
                                        passName, z * 100));
            var kept = new List<(double X, double Y)>();
            foreach (var (name, fx, fy) in ideal)
            {
                var (ix, iy) = g.Metres(new CalibDot(0, name, fx, fy, pass));
                var (x, y) = Clamp(v, pose, (ix, iy), anchor, z);
                if (kept.Any(k => Math.Sqrt((k.X - x) * (k.X - x) + (k.Y - y) * (k.Y - y)) < MinSeparationM))
                {
                    notes.Add($"{passName} {name}: dropped (moved onto another target)");
                    continue;
                }
                if (Math.Abs(x - ix) > 1e-4 || Math.Abs(y - iy) > 1e-4)
                    notes.Add(string.Format(CultureInfo.InvariantCulture, "{0} {1}: moved ({2:+0;-0;0}, {3:+0;-0;0}) mm to keep the face in view",
                                            passName, name, (x - ix) * 1000, (y - iy) * 1000));
                kept.Add((x, y));
                var (ffx, ffy) = g.FromMetres(x, y);
                dots.Add(new CalibDot(dots.Count, name, ffx, ffy, pass));
            }
            return z;
        }
        // Nothing fits even far back (a camera looking away): lay the ideal targets out, the live guard decides.
        notes.Add($"{passName} pass: no distance up to {MaxDistanceM * 100:0} cm keeps the face in view from the prior; targets not adjusted");
        foreach (var (name, fx, fy) in ideal) dots.Add(new CalibDot(dots.Count, name, fx, fy, pass));
        return target;
    }

    private static bool Fits(CameraView v, CameraPose pose, double x, double y, double z) =>
        v.FaceInView(pose, new Vec3(x, y, z), CameraView.LayoutMarginDeg);

    /// <summary>Where the camera's axis meets the pass distance, clamped into the far inset rectangle.</summary>
    private static (double X, double Y) Anchor(CalibGeometry g, CameraPose pose, double z)
    {
        var a = pose.Rotate(new Vec3(0, 0, 1));
        double fx = pose.T.X, fy = pose.T.Y;
        if (a.Z > 0.1)
        {
            double s = (z - pose.T.Z) / a.Z;
            fx += s * a.X;
            fy += s * a.Y;
        }
        double hx = (0.5 - FarInset) * g.WidthM, hy = (0.5 - FarInset) * g.HeightM;
        return (Math.Clamp(fx, -hx, hx), Math.Clamp(fy, -hy, hy));
    }

    /// <summary>
    /// The ideal point if the face fits there, else the fitting point nearest it along the vertical
    /// toward the anchor's height, else along the line to the anchor. The fitting set is convex
    /// (linear constraints in the eye position), so bisection finds its edge.
    /// </summary>
    private static (double X, double Y) Clamp(CameraView v, CameraPose pose, (double X, double Y) ideal, (double X, double Y) anchor, double z)
    {
        if (Fits(v, pose, ideal.X, ideal.Y, z)) return ideal;
        var to = (ideal.X, anchor.Y);
        if (!Fits(v, pose, to.X, to.Y, z)) to = anchor;
        double lo = 0, hi = 1; // lo: out, hi: in
        for (int i = 0; i < 40; i++)
        {
            double m = (lo + hi) / 2;
            if (Fits(v, pose, ideal.X + (to.X - ideal.X) * m, ideal.Y + (to.Y - ideal.Y) * m, z)) hi = m;
            else lo = m;
        }
        return (ideal.X + (to.X - ideal.X) * hi, ideal.Y + (to.Y - ideal.Y) * hi);
    }

    /// <summary>One log line: the prior, the distances and every target in millimetres.</summary>
    public string Describe(CalibGeometry g)
    {
        var ci = CultureInfo.InvariantCulture;
        var parts = Dots.Select(d =>
        {
            var (x, y) = g.Metres(d);
            return string.Format(ci, "{0} ({1:0}, {2:0})", d.Label, x * 1000, y * 1000);
        });
        return string.Format(ci, "layout: prior {0} T ({1:0.0}, {2:0.0}, {3:0.0}) mm, view {4:0}° H at {5:0.00}; near {6:0} cm, far {7:0} cm; {8}",
                             PriorSource, Prior.T.X * 1000, Prior.T.Y * 1000, Prior.T.Z * 1000, View.HfovDeg, View.Aspect,
                             NearM * 100, FarM * 100, string.Join("; ", parts))
               + (Notes.Count > 0 ? " | " + string.Join("; ", Notes) : "");
    }
}

/// <summary>The monitor the window goes on: the one that best overlaps the screen's desktop rect.</summary>
public static class MonitorMatch
{
    /// <summary>
    /// Index into <paramref name="monitors"/> (their bounds, in the UI toolkit's pixels) of the best
    /// match for <paramref name="desktop"/> (the status feed's desktop rect). The rect is also tried
    /// multiplied by its <paramref name="scale"/>, because one platform reports desktop points where
    /// the toolkit reports pixels. -1 when nothing overlaps.
    /// </summary>
    public static int Pick(IReadOnlyList<PixelRect> monitors, PixelRect desktop, double scale)
    {
        var candidates = new List<PixelRect> { desktop };
        if (scale > 0 && Math.Abs(scale - 1) > 1e-6)
            candidates.Add(new PixelRect((long)Math.Round(desktop.Left * scale), (long)Math.Round(desktop.Top * scale),
                                         (long)Math.Round(desktop.Width * scale), (long)Math.Round(desktop.Height * scale)));
        int best = -1;
        double bestIou = 0;
        for (int i = 0; i < monitors.Count; i++)
            foreach (var c in candidates)
            {
                double iou = Iou(monitors[i], c);
                if (iou > bestIou + 1e-9) { bestIou = iou; best = i; }
            }
        return best;
    }

    private static double Iou(PixelRect a, PixelRect b)
    {
        if (a.IsEmpty || b.IsEmpty) return 0;
        long w = Math.Min(a.Right, b.Right) - Math.Max(a.Left, b.Left);
        long h = Math.Min(a.Bottom, b.Bottom) - Math.Max(a.Top, b.Top);
        if (w <= 0 || h <= 0) return 0;
        double inter = (double)w * h;
        return inter / ((double)a.Width * a.Height + (double)b.Width * b.Height - inter);
    }
}

/// <summary>The camera-space block of a sample (§6): per-eye unit direction and estimated distance.</summary>
public sealed record CameraBlock(string Key, Vec3 LeftDir, double LeftDistanceM, Vec3 RightDir, double RightDistanceM)
{
    /// <summary>
    /// The eye midpoint in camera space — the closed-eye variant's aligned point, computed as the
    /// solver does (<c>m_mirror_calib_eye_point</c>, eye <c>mid</c>): the mean of the two eye points,
    /// unit directions scaled by their distances (unit distances when either is missing).
    /// </summary>
    public Vec3 MidPoint
    {
        get
        {
            bool haveD = LeftDistanceM > 0 && RightDistanceM > 0;
            double dl = haveD ? LeftDistanceM : 1, dr = haveD ? RightDistanceM : 1;
            return 0.5 * (dl * LeftDir.Normalized + dr * RightDir.Normalized);
        }
    }

    /// <summary>Both eyes carry a distance estimate (else <see cref="MidPoint"/> is a unit-distance direction).</summary>
    public bool HasDistance => LeftDistanceM > 0 && RightDistanceM > 0;

    internal static CameraBlock? Read(JsonElement? e)
    {
        if (e is not { ValueKind: JsonValueKind.Object } o) return null;
        if (Vec3.Read(o, "left_dir") is not { } l || Vec3.Read(o, "right_dir") is not { } r) return null;
        return new CameraBlock(o.Str("key"), l, o.Num("left_distance_m"), r, o.Num("right_distance_m"));
    }
}

/// <summary><c>hello.display</c>: the runtime's display info for the screen the provider serves.</summary>
public sealed record CalibHelloDisplay(double WidthM, double HeightM, long PixelWidth, long PixelHeight);

public abstract record CalibLine;

/// <summary>The watch's first line: which provider and camera, and the display it measures in.</summary>
public sealed record CalibHello(string Screen, string Provider, string Source, string CameraKey, bool TracksCameraSpace,
                                string ViewerSource, string ViewerReason, CalibHelloDisplay? Display, string Note) : CalibLine
{
    /// <summary>The camera's current pose answer (<c>hello.camera_pose</c>; null from an older CLI): the layout's prior.</summary>
    public CameraPose? CameraPose { get; init; }
    public string CameraPoseSource { get; init; } = "";
}

/// <summary>One pushed sample. <see cref="Raw"/> is the line verbatim: what the solve input records.</summary>
public sealed record CalibSample(long Seq, long CaptureNs, bool Tracking, double Confidence, Vec3? LeftEye, Vec3? RightEye,
                                 CameraBlock? Camera, string Raw) : CalibLine
{
    public bool Usable => Tracking && Camera is not null;
}

/// <summary>The watch's error line (exit 3): <c>no_provider</c>, <c>start_failed</c>, <c>no_samples</c>.</summary>
public sealed record CalibError(string Error, string Reason) : CalibLine;

public static class CalibLineParser
{
    /// <summary>One NDJSON line of <c>tracking calibrate --watch --json</c>; null for anything else (a log line, a truncated line).</summary>
    public static CalibLine? Parse(string? line)
    {
        if (string.IsNullOrWhiteSpace(line)) return null;
        string t = line.Trim();
        if (!t.StartsWith('{')) return null;
        try
        {
            using var doc = JsonDocument.Parse(t);
            var r = doc.RootElement;
            if (r.ValueKind != JsonValueKind.Object) return null;
            switch (r.Str("type"))
            {
            case "hello":
                CalibHelloDisplay? d = r.Obj("display") is { } dd
                    ? new CalibHelloDisplay(dd.Num("width_m"), dd.Num("height_m"), dd.Long("pixel_width"), dd.Long("pixel_height"))
                    : null;
                CameraPose? pose = null;
                if (r.Obj("camera_pose") is { } cp)
                {
                    var pt = cp.Arr("t_m").Select(x => x.ValueKind == JsonValueKind.Number ? x.GetDouble() : double.NaN).ToArray();
                    var pq = cp.Arr("q").Select(x => x.ValueKind == JsonValueKind.Number ? x.GetDouble() : double.NaN).ToArray();
                    if (pt.Length == 3 && pq.Length == 4 && pt.Concat(pq).All(double.IsFinite) && pq.Sum(x => x * x) > 0.5)
                        pose = new CameraPose(new Vec3(pt[0], pt[1], pt[2]), pq[0], pq[1], pq[2], pq[3]);
                }
                return new CalibHello(r.Str("screen"), r.Str("provider"), r.Str("source"), r.Str("camera_key"),
                                      r.Bool("tracks_camera_space"), r.Str("viewer_source"), r.Str("viewer_reason"), d, r.Str("note"))
                {
                    CameraPose = pose,
                    CameraPoseSource = pose is null ? "" : r.Obj("camera_pose") is { } cs ? cs.Str("source") : "",
                };
            case "sample":
                var disp = r.Obj("display");
                return new CalibSample(r.Long("seq"), r.Long("capture_ns"), r.Bool("tracking"), r.Num("confidence"),
                                       Vec3.Read(disp, "left_eye"), Vec3.Read(disp, "right_eye"), CameraBlock.Read(r.Obj("camera")), t);
            case "error":
                return new CalibError(r.Str("error", "error"), r.Str("reason"));
            default:
                return null;
            }
        }
        catch (JsonException) { return null; }
    }
}

/// <summary>
/// "0.5 s of stable samples": the eye midpoint's camera-space direction stays within
/// <see cref="MaxDeg"/> of where the still run began for at least <see cref="Hold"/>. A sample
/// without a lock (or without a camera block) ends the run.
/// </summary>
public sealed class StabilityTracker
{
    public const double MaxDeg = 0.3;
    public static readonly TimeSpan Hold = TimeSpan.FromMilliseconds(500);

    private Vec3? _anchor;
    private long _anchorNs;
    private long _lastNs;

    public bool Stable => _anchor is not null && _lastNs - _anchorNs >= Hold.Ticks * 100;

    /// <summary>How long the head has been still, 0..1 of <see cref="Hold"/> (the window's progress ring).</summary>
    public double Progress => _anchor is null ? 0 : Math.Clamp((_lastNs - _anchorNs) / (Hold.Ticks * 100.0), 0, 1);

    public void Reset() { _anchor = null; _anchorNs = _lastNs = 0; }

    public void Add(CalibSample s)
    {
        if (!s.Usable) { Reset(); return; }
        var dir = s.Camera!.MidPoint.Normalized;
        if (_anchor is not { } a || s.CaptureNs < _lastNs || Vec3.AngleDeg(a, dir) >= MaxDeg)
        {
            _anchor = dir;
            _anchorNs = s.CaptureNs;
        }
        _lastNs = s.CaptureNs;
    }
}

/// <summary>A camera pose: display point = T + R · camera point (§6), R as a unit quaternion (x, y, z, w).</summary>
public sealed record CameraPose(Vec3 T, double Qx, double Qy, double Qz, double Qw)
{
    public Vec3 Rotate(Vec3 v)
    {
        // v' = v + 2w (q × v) + 2 q × (q × v)
        var q = new Vec3(Qx, Qy, Qz);
        var t = 2.0 * q.Cross(v);
        return v + Qw * t + q.Cross(t);
    }

    /// <summary>A display-space point into the camera frame: Rᵀ · (p − T).</summary>
    public Vec3 ToCamera(Vec3 displayPoint) => new CameraPose(T, -Qx, -Qy, -Qz, Qw).Rotate(displayPoint - T);

    public CameraPose Nudged(double dxM, double dyM) => this with { T = new Vec3(T.X + dxM, T.Y + dyM, T.Z) };

    /// <summary>The <c>CAMERA_POSE_&lt;n&gt;</c> value, <c>tx,ty,tz,qx,qy,qz,qw</c>, as the solver formats it.</summary>
    public string Setting => string.Join(",", new[] { T.X, T.Y, T.Z, Qx, Qy, Qz, Qw }
        .Select(v => v.ToString("F6", CultureInfo.InvariantCulture)));

    /// <summary>
    /// A camera-space eye point into display space the way a provider applies a committed slot:
    /// <c>T + R · (depth_scale · s̃ · normalize(u_x, u_y, k·u_z))</c>.
    /// </summary>
    public Vec3 ToDisplay(Vec3 cameraPoint, double depthScale = 1, double focalScale = 1)
    {
        double s = cameraPoint.Length;
        var u = cameraPoint.Normalized;
        var dir = new Vec3(u.X, u.Y, focalScale * u.Z).Normalized;
        return T + Rotate((depthScale > 0 ? depthScale : 1) * s * dir);
    }
}

/// <summary>One per-sample row of a solve.</summary>
public sealed record SolveSample(int Index, double DotX, double DotY, double ResidualMm, double DistanceM, double EstimatedDistanceM, bool Rejected);

/// <summary><c>tracking calibrate solve --json</c> (§6). <see cref="Pose"/> is null on a refusal.</summary>
public sealed record SolveResult(bool Ok, string Status, string Reason, string Mode, string Eye, string CameraKey, string Screen,
                                 CameraPose? Pose, double PitchDeg, double YawDeg, double RollDeg, double FocalScale, string Setting,
                                 double RmsMm, double MaxMm, double IrisScale, bool IllConditioned, int SampleCount, int UsedCount,
                                 int DistinctDots, IReadOnlyList<SolveSample> Samples)
{
    /// <summary>The procedure's retake threshold (design §10 step 4).</summary>
    public const double WarnRmsMm = 5.0;

    public bool HighRms => Ok && RmsMm > WarnRmsMm;

    /// <summary>
    /// The samples to retake: the solver's rejected ones; else, on a high RMS, those over the
    /// threshold; else (a refusal, or nothing stands out) none — the page offers "start over".
    /// </summary>
    public IReadOnlyList<int> Flagged
    {
        get
        {
            var rej = Samples.Where(s => s.Rejected).Select(s => s.Index).ToArray();
            if (rej.Length > 0) return rej;
            if (HighRms) return Samples.Where(s => s.ResidualMm > WarnRmsMm).Select(s => s.Index).ToArray();
            return Array.Empty<int>();
        }
    }

    public static bool TryParse(string? json, out SolveResult? result)
    {
        result = null;
        if (string.IsNullOrWhiteSpace(json)) return false;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var r = doc.RootElement;
            if (r.ValueKind != JsonValueKind.Object || !r.Has("ok")) return false;
            CameraPose? pose = null;
            double pitch = 0, yaw = 0, roll = 0, focal = 1;
            if (r.Obj("pose") is { } p)
            {
                var t = p.Arr("t_m").Select(x => x.ValueKind == JsonValueKind.Number ? x.GetDouble() : double.NaN).ToArray();
                var q = p.Arr("q").Select(x => x.ValueKind == JsonValueKind.Number ? x.GetDouble() : double.NaN).ToArray();
                if (t.Length == 3 && q.Length == 4 && t.Concat(q).All(double.IsFinite))
                    pose = new CameraPose(new Vec3(t[0], t[1], t[2]), q[0], q[1], q[2], q[3]);
                pitch = p.Num("pitch_deg"); yaw = p.Num("yaw_deg"); roll = p.Num("roll_deg"); focal = p.Num("focal_scale", 1);
            }
            var samples = r.Arr("samples").Where(s => s.ValueKind == JsonValueKind.Object).Select(s =>
            {
                var dot = s.Arr("dot_m").Select(x => x.ValueKind == JsonValueKind.Number ? x.GetDouble() : 0).ToArray();
                return new SolveSample(s.Int("index"), dot.Length > 0 ? dot[0] : 0, dot.Length > 1 ? dot[1] : 0, s.Num("residual_mm"),
                                       s.Num("distance_m"), s.Num("estimated_distance_m"), s.Bool("rejected"));
            }).ToArray();
            bool ok = r.Bool("ok") && pose is not null;
            result = new SolveResult(ok, r.Str("status", ok ? "ok" : "?"), r.Str("reason"), r.Str("mode"), r.Str("eye"),
                                     r.Str("camera_key"), r.Str("screen"), pose, pitch, yaw, roll, focal, r.Str("setting"),
                                     r.Num("rms_mm"), r.Num("max_mm"), r.Num("iris_scale", 1), r.Bool("ill_conditioned"),
                                     r.Int("sample_count"), r.Int("used_count"), r.Int("distinct_dots"), samples);
            return true;
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException) { return false; }
    }
}

/// <summary>The CLI lines the window runs (argument vectors, never parsed strings).</summary>
public static class CalibrationCommands
{
    /// <summary>
    /// The solver mode: the full rotation (pitch, yaw, roll) + T_x, T_y. Two head distances are what
    /// make it solvable; at one distance a yaw (or a principal point off the image centre) is absorbed
    /// as a sideways offset of T, and a pitch as a vertical one. The focal scale is NOT solved
    /// (<c>--focal</c>): with this dot layout it is ill-conditioned against the rotation (in
    /// simulation half the solves flag <c>ill_conditioned</c> and the yaw error grows ~40x).
    /// </summary>
    public const string Mode = "c";

    /// <summary>The fallback when mode <see cref="Mode"/> is refused as degenerate: translation + pitch.</summary>
    public const string FallbackMode = "b";
    public const string RejectMm = "5";

    public static CliArgs Watch(string screenKey) =>
        new("tracking", "calibrate", "--screen", screenKey, "--watch", "--json", "--interval-ms", "20");

    public static CliArgs Solve(string inputPath, string mode = Mode) =>
        new("tracking", "calibrate", "solve", "--in", inputPath, "--mode", mode, "--eye", "mid", "--reject-mm", RejectMm, "--json");

    /// <summary>
    /// Where the window keeps the last solve's input (overwritten by each solve), beside the
    /// Dashboard log, so a run can be re-solved and inspected afterwards
    /// (<c>displayxr-cli tracking calibrate solve --in &lt;this&gt; --mode c --json</c>).
    /// </summary>
    public static string LastSolveInputPath { get; } = System.IO.Path.Combine(
        Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "DisplayXR", "calibration-last-solve.json");

    /// <summary>The one-line log record of a solve: the pose and its quality.</summary>
    public static string Describe(SolveResult r)
    {
        var ci = CultureInfo.InvariantCulture;
        if (!r.Ok || r.Pose is null)
            return string.Format(ci, "solve refused: mode {0}, {1} samples, status {2}: {3}", r.Mode, r.SampleCount, r.Status, r.Reason);
        return string.Format(ci,
            "solve ok: mode {0}, T ({1:0.0}, {2:0.0}, {3:0.0}) mm, pitch {4:0.00} yaw {5:0.00} roll {6:0.00} deg, " +
            "focal scale {7:0.0000}, iris scale {8:0.0000}, RMS {9:0.00} mm (max {10:0.00}), {11}/{12} samples, {13} dots{14}",
            r.Mode, r.Pose.T.X * 1000, r.Pose.T.Y * 1000, r.Pose.T.Z * 1000, r.PitchDeg, r.YawDeg, r.RollDeg, r.FocalScale,
            r.IrisScale, r.RmsMm, r.MaxMm, r.UsedCount, r.SampleCount, r.DistinctDots, r.IllConditioned ? ", ILL-CONDITIONED" : "");
    }

    public static CliArgs Apply(string screenKey, string cameraKey, CameraPose pose, string? provider, double irisScale, double focalScale,
                                double rmsMm = -1, int samples = 0, string? mode = null)
    {
        var a = new List<string> { "tracking", "calibrate", "apply", "--screen", screenKey, "--camera", cameraKey, "--pose", pose.Setting };
        // Accepted and ignored since the runtime-owned camera calibration store: the pose belongs to the
        // camera + screen, not to a provider. Still passed so an older CLI keeps working.
        if (!string.IsNullOrEmpty(provider)) { a.Add("--provider"); a.Add(provider); }
        // The solve's quality, recorded with the calibration (the "calibrated (rms 2.1 mm)" label).
        if (rmsMm >= 0 && double.IsFinite(rmsMm)) { a.Add("--rms-mm"); a.Add(rmsMm.ToString("F3", CultureInfo.InvariantCulture)); }
        if (samples > 0) { a.Add("--samples"); a.Add(samples.ToString(CultureInfo.InvariantCulture)); }
        if (mode is "a" or "b" or "c") { a.Add("--mode"); a.Add(mode); }
        if (irisScale > 0 && double.IsFinite(irisScale) && Math.Abs(irisScale - 1) > 1e-6)
        {
            a.Add("--depth-scale");
            a.Add(irisScale.ToString("F6", CultureInfo.InvariantCulture));
        }
        if (focalScale > 0 && double.IsFinite(focalScale) && Math.Abs(focalScale - 1) > 1e-6)
        {
            a.Add("--focal-scale");
            a.Add(focalScale.ToString("F6", CultureInfo.InvariantCulture));
        }
        a.Add("--json");
        return new CliArgs(a.ToArray());
    }

    /// <summary>
    /// <c>calibrate apply --json</c>. Current CLI (the runtime-owned camera calibration store):
    /// <c>{"ok","camera","screen","source":"user-calibration","layer","store",...,"failed","reason"}</c>;
    /// older CLI (#2303, a provider's settings): <c>{"ok","provider","slot","store","keys":{...}}</c>.
    /// The report names where it was saved; a failure says what did not write.
    /// </summary>
    public static bool ParseApply(string? json, out string report)
    {
        report = "";
        if (string.IsNullOrWhiteSpace(json)) return false;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var r = doc.RootElement;
            if (r.ValueKind != JsonValueKind.Object || !r.Has("ok")) return false;
            bool ok = r.Bool("ok");
            string store = r.Str("store");
            if (r.Has("source"))
            {
                report = ok
                    ? $"Saved as the calibration of camera '{r.Str("camera")}' on this screen, shared by every provider" + (store.Length > 0 ? $" ({store})." : ".")
                    : $"writing the {r.Str("failed", "calibration").Replace('_', ' ')} failed: {r.Str("reason", "store write failed")}" + (store.Length > 0 ? $" ({store})." : ".");
                return ok;
            }
            report = ok
                ? $"Saved as CAMERA_POSE_{r.Int("slot")} of '{r.Str("provider")}'" + (store.Length > 0 ? $" in {store}." : ".")
                : $"writing {r.Str("failed_key", "a setting")} of '{r.Str("provider")}' failed" + (store.Length > 0 ? $" ({store})." : ".");
            return ok;
        }
        catch (JsonException) { return false; }
    }

    /// <summary>
    /// The solve input (§6, the object form): <c>{"mode","eye","camera_key","screen","samples":[{"dot_m":[x,y],"sample":&lt;the watch line&gt;}]}</c>.
    /// </summary>
    public static string SolveInput(string screenKey, string cameraKey, IEnumerable<(double X, double Y, CalibSample Sample)> pairs,
                                    string mode = Mode)
    {
        var arr = new JsonArray();
        foreach (var (x, y, s) in pairs)
            arr.Add(new JsonObject
            {
                ["dot_m"] = new JsonArray(x, y),
                ["sample"] = JsonNode.Parse(s.Raw),
            });
        var root = new JsonObject
        {
            ["mode"] = mode,
            ["eye"] = "mid",
            ["camera_key"] = cameraKey,
            ["screen"] = screenKey,
            ["samples"] = arr,
        };
        return root.ToJsonString();
    }
}

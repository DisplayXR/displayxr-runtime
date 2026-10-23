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
/// One target of the procedure. <see cref="Fx"/> / <see cref="Fy"/> are fractions of the screen
/// (0 = left / top edge, 1 = right / bottom edge).
/// </summary>
public sealed record CalibDot(int Index, string Name, double Fx, double Fy, bool LeanBack);

/// <summary>
/// The target screen's pixel grid and physical size, and the dot maths: a dot's display-space
/// position is (pixel − screen centre) × pixel pitch, y up (display space: +X right, +Y up, origin
/// at the screen centre).
/// </summary>
public sealed record CalibGeometry(long PixelWidth, long PixelHeight, double WidthM, double HeightM, string Source)
{
    /// <summary>The inset of the left / right / top / bottom dots (design §10: an inset, not the corners).</summary>
    public const double Inset = 0.30;

    /// <summary>Centre; left, right, top, bottom at a 30 % inset; centre again, leaning back.</summary>
    public static readonly IReadOnlyList<CalibDot> Dots = new[]
    {
        new CalibDot(0, "centre", 0.5, 0.5, false),
        new CalibDot(1, "left", Inset, 0.5, false),
        new CalibDot(2, "right", 1 - Inset, 0.5, false),
        new CalibDot(3, "top", 0.5, Inset, false),
        new CalibDot(4, "bottom", 0.5, 1 - Inset, false),
        new CalibDot(5, "centre, leaning back", 0.5, 0.5, true),
    };

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
                                string ViewerSource, string ViewerReason, CalibHelloDisplay? Display, string Note) : CalibLine;

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
                return new CalibHello(r.Str("screen"), r.Str("provider"), r.Str("source"), r.Str("camera_key"),
                                      r.Bool("tracks_camera_space"), r.Str("viewer_source"), r.Str("viewer_reason"), d, r.Str("note"));
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
    /// <summary>The solver mode: translation + pitch (a clip-on or tilted camera; exact R = I is a special case).</summary>
    public const string Mode = "b";
    public const string RejectMm = "5";

    public static CliArgs Watch(string screenKey) =>
        new("tracking", "calibrate", "--screen", screenKey, "--watch", "--json", "--interval-ms", "20");

    public static CliArgs Solve(string inputPath) =>
        new("tracking", "calibrate", "solve", "--in", inputPath, "--mode", Mode, "--eye", "mid", "--reject-mm", RejectMm, "--json");

    public static CliArgs Apply(string screenKey, string cameraKey, CameraPose pose, string? provider, double irisScale, double focalScale)
    {
        var a = new List<string> { "tracking", "calibrate", "apply", "--screen", screenKey, "--camera", cameraKey, "--pose", pose.Setting };
        if (!string.IsNullOrEmpty(provider)) { a.Add("--provider"); a.Add(provider); }
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
    /// <c>calibrate apply --json</c>: <c>{"ok","provider","slot","store","keys":{...}}</c>. The report
    /// names the slot and the store; a failure names the key that did not write.
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
    public static string SolveInput(string screenKey, string cameraKey, IEnumerable<(double X, double Y, CalibSample Sample)> pairs)
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
            ["mode"] = Mode,
            ["eye"] = "mid",
            ["camera_key"] = cameraKey,
            ["screen"] = screenKey,
            ["samples"] = arr,
        };
        return root.ToJsonString();
    }
}

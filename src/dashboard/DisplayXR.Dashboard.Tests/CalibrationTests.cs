// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Linq;
using System.Text.Json.Nodes;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>
/// The camera-pose calibration window's model (tracking-provider-discovery.md §6, design §10).
///
/// Fixtures, captured from this branch's <c>displayxr-cli</c> (macOS, sim-tracking with
/// <c>CAMERA=1</c>, <c>SCRIPT=offset</c>, hidden <c>CAMERA_TRUE_POSE</c> = (35, 183, 0) mm + 9° pitch,
/// <c>CAMERA_DEPTH_BIAS=1.08</c>; screen key and store path neutralised):
/// - <c>calibrate-watch.ndjson</c>: the hello, then one sample per Dashboard target (centre, left,
///   right, top, bottom at 40 cm — the near pass; the same five at 70 cm — the far pass), each with
///   the eye midpoint on that dot's normal (<c>SCRIPT=offset</c>, one <c>--count 1</c> watch per target);
/// - <c>calibrate-solve-input.json</c>: the pairs the CLI solved; <c>calibrate-solve-ok.json</c> its
///   mode <c>c</c> result with <c>--reject-mm 5</c>; <c>-rejected</c>: the near left sample taken 15 mm
///   off its normal; <c>-highrms</c>: 35 mm off, without rejection (RMS 7.9 mm);
///   <c>-c-degenerate</c>: mode <c>c</c> on the five dots twice at one distance (55 cm, rank 3 of 5);
///   <c>-b-ok</c>: an earlier mode <c>b</c> result (six samples, 55 and 75 cm); <c>-degenerate</c>:
///   mode <c>b</c> on three samples at one dot; <c>calibrate-apply.json</c>: <c>calibrate apply --json</c>;
///   <c>calibrate-watch-no-provider.ndjson</c>: the watch with the provider disabled (exit 3).
/// </summary>
public class CalibrationTests
{
    private const string Key = "DXR-0001-00000001";

    private static string[] Watch => TestData.Lines("calibrate-watch.ndjson");

    /// <summary>The same sample line at another capture time (the stability window's clock).</summary>
    private static string At(string line, long ns)
    {
        var o = JsonNode.Parse(line)!.AsObject();
        o["capture_ns"] = ns;
        return o.ToJsonString();
    }

    private static CalibrationSession Started()
    {
        var s = new CalibrationSession(Key, null);
        s.OnLine(Watch[0]);
        return s;
    }

    /// <summary>Hold the head still on sample <paramref name="i"/> for 0.6 s, pressing Space first.</summary>
    private static void CaptureSample(CalibrationSession s, string line, ref long t)
    {
        s.Space();
        s.OnLine(At(line, t));
        t += 300_000_000;
        s.OnLine(At(line, t));
        t += 300_000_000;
        s.OnLine(At(line, t));
        t += 100_000_000;
    }

    private static CalibrationSession Captured()
    {
        var s = Started();
        long t = 1_000_000_000;
        foreach (var line in Watch.Skip(1)) CaptureSample(s, line, ref t);
        return s;
    }

    // --- NDJSON ---------------------------------------------------------------------------

    [Fact]
    public void Watch_Hello_AndSamples_Parse()
    {
        var h = Assert.IsType<CalibHello>(CalibLineParser.Parse(Watch[0]));
        Assert.Equal(Key, h.Screen);
        Assert.Equal("sim-tracking", h.Provider);
        Assert.Equal("sim-tracking:webcam0", h.CameraKey);
        Assert.True(h.TracksCameraSpace);
        Assert.Equal("provider:sim-tracking", h.ViewerSource);
        Assert.Equal(3024, h.Display!.PixelWidth);
        Assert.Equal(0.30121409893035889, h.Display.WidthM, 12);

        var smp = Watch.Skip(1).Select(CalibLineParser.Parse).Cast<CalibSample>().ToArray();
        Assert.Equal(10, smp.Length);
        Assert.All(smp, x => Assert.True(x.Usable));
        Assert.Equal("sim-tracking:webcam0", smp[0].Camera!.Key);
        Assert.NotNull(smp[0].LeftEye);
        Assert.Equal(1.0, smp[0].Camera!.LeftDir.Length, 5);
    }

    [Fact]
    public void Watch_ErrorLine_And_Junk()
    {
        var e = Assert.IsType<CalibError>(CalibLineParser.Parse(TestData.Lines("calibrate-watch-no-provider.ndjson")[0]));
        Assert.Equal("no_provider", e.Error);
        Assert.Null(CalibLineParser.Parse(" WARN [x] a log line"));
        Assert.Null(CalibLineParser.Parse("{\"type\":\"sample\",\"seq\":1"));
        Assert.Null(CalibLineParser.Parse("{\"type\":\"other\"}"));
        Assert.Null(CalibLineParser.Parse(""));
        var noCam = Assert.IsType<CalibSample>(CalibLineParser.Parse("{\"type\":\"sample\",\"seq\":2,\"tracking\":true,\"camera\":null}"));
        Assert.False(noCam.Usable);
    }

    [Fact]
    public void Watch_NoProvider_FailsTheSession()
    {
        var s = new CalibrationSession(Key, null);
        s.OnLine(TestData.Lines("calibrate-watch-no-provider.ndjson")[0]);
        s.OnExit(3);
        Assert.Equal(CalibPhase.Failed, s.Phase);
        Assert.StartsWith("No viewer-tracking provider is running", s.Error);
        Assert.False(s.WantsStream);
    }

    [Fact]
    public void Hello_WithoutCameraSpace_Fails()
    {
        var o = JsonNode.Parse(Watch[0])!.AsObject();
        o["tracks_camera_space"] = false;
        var s = new CalibrationSession(Key, null);
        s.OnLine(o.ToJsonString());
        Assert.Equal(CalibPhase.Failed, s.Phase);
        Assert.Contains("no camera-space samples", s.Error);
    }

    [Fact]
    public void StreamExit_WhileAligning_Fails()
    {
        var s = Started();
        Assert.Equal(CalibPhase.Aligning, s.Phase);
        s.OnExit(0);
        Assert.Equal(CalibPhase.Failed, s.Phase);
        Assert.Contains("exit code 0", s.Error);
    }

    // --- dot geometry ---------------------------------------------------------------------

    [Fact]
    public void Dots_AreCentre_FourAt30PercentInset_NearThenFar()
    {
        var d = CalibGeometry.Dots;
        Assert.Equal(10, d.Count);
        for (int p = 0; p < 2; p++)
        {
            int o = p * CalibGeometry.DotsPerPass;
            Assert.Equal((0.5, 0.5), (d[o].Fx, d[o].Fy));
            Assert.Equal((0.3, 0.5), (d[o + 1].Fx, d[o + 1].Fy));
            Assert.Equal((0.7, 0.5), (d[o + 2].Fx, d[o + 2].Fy));
            Assert.Equal((0.5, 0.3), (d[o + 3].Fx, d[o + 3].Fy));
            Assert.Equal((0.5, 0.7), (d[o + 4].Fx, d[o + 4].Fy));
            Assert.All(d.Skip(o).Take(CalibGeometry.DotsPerPass), x => Assert.Equal(p == 0 ? CalibPass.Near : CalibPass.Far, x.Pass));
        }
        Assert.Equal(Enumerable.Range(0, 10), d.Select(x => x.Index));
        Assert.Equal("left, near", d[1].Label);
        Assert.Equal("left, far", d[6].Label);
        Assert.Equal(new[] { 5, 6, 7, 8, 9 }, CalibGeometry.PassIndices(CalibPass.Far));
    }

    [Fact]
    public void DotMetres_ArePixelOffsetTimesPitch_YUp()
    {
        var g = new CalibGeometry(3840, 2160, 0.60, 0.3375, "test");
        // left: pixel 1152 -> (1152 - 1920) * 0.60/3840 = -0.12
        var (lx, ly) = g.Metres(CalibGeometry.Dots[1]);
        Assert.Equal(-0.12, lx, 9);
        Assert.Equal(0, ly, 9);
        // top: pixel y 648 -> (1080 - 648) * 0.3375/2160 = +0.0675 (y up)
        var (tx, ty) = g.Metres(CalibGeometry.Dots[3]);
        Assert.Equal(0, tx, 9);
        Assert.Equal(0.0675, ty, 9);
        var (bx, by) = g.Metres(CalibGeometry.Dots[4]);
        Assert.Equal(-0.0675, by, 9);
        // centre
        Assert.Equal((0.0, 0.0), g.Metres(CalibGeometry.Dots[0]));
        // the inverse (the live-check dot) lands back on the dot's fraction
        Assert.Equal((0.3, 0.5), g.FromMetres(-0.12, 0));
        Assert.Equal((0.5, 0.3), g.FromMetres(0, 0.0675));
    }

    [Fact]
    public void DotPixels_AreWhole_AndTheMetresFollowTheRoundedPixel()
    {
        var g = new CalibGeometry(3024, 1964, 0.30121409893035889, 0.19562980532646179, "test");
        Assert.Equal((907L, 982L), g.Pixel(CalibGeometry.Dots[1]));
        Assert.Equal((1512L, 589L), g.Pixel(CalibGeometry.Dots[3]));
        var (x, _) = g.Metres(CalibGeometry.Dots[1]);
        Assert.Equal(-605 * 0.30121409893035889 / 3024, x, 12);
        Assert.Equal(907.0 / 3024, g.Fraction(CalibGeometry.Dots[1]).Fx, 12);
    }

    [Fact]
    public void Geometry_Choose_PrefersDisplayInfoOnThePrimary_ThenLayout_ThenPhysical()
    {
        var hello = new CalibHelloDisplay(0.3012, 0.1956, 3024, 1964);
        var snap = TestData.Parse(TestData.DesignExample);
        var sc = snap.Screens[0];
        var primary = sc with { Roles = sc.Roles with { RuntimeDefault = true } };
        var other = sc with { Roles = sc.Roles with { RuntimeDefault = false } };

        var g1 = CalibGeometry.Choose(primary, hello)!;
        Assert.Equal("display info", g1.Source);
        Assert.Equal(0.3012, g1.WidthM);
        Assert.Equal(sc.Native.Width, g1.PixelWidth); // the screen's native grid

        var g2 = CalibGeometry.Choose(other, hello)!;
        Assert.Equal(other.Layout.WidthM > 0 ? "screen layout" : "physical size (" + other.PhysicalMm.Source + ")", g2.Source);

        var noLayout = other with { Layout = other.Layout with { WidthM = 0, HeightM = 0 } };
        var g3 = CalibGeometry.Choose(noLayout, null)!;
        Assert.StartsWith("physical size", g3.Source);
        Assert.Equal(noLayout.PhysicalMm.Width / 1000.0, g3.WidthM, 9);

        // No screen: the hello alone.
        var g4 = CalibGeometry.Choose(null, hello)!;
        Assert.Equal((3024L, 1964L), (g4.PixelWidth, g4.PixelHeight));
        Assert.Null(CalibGeometry.Choose(null, null));
    }

    [Fact]
    public void MonitorMatch_PicksTheBestOverlap_AlsoForPointsTimesScale()
    {
        var monitors = new[] { new PixelRect(0, 0, 3024, 1964), new PixelRect(3024, 0, 3840, 2160) };
        Assert.Equal(1, MonitorMatch.Pick(monitors, new PixelRect(3024, 0, 3840, 2160), 1));
        // The status feed in points (1512 x 982 @2) against monitors in pixels.
        Assert.Equal(0, MonitorMatch.Pick(monitors, new PixelRect(0, 0, 1512, 982), 2));
        Assert.Equal(-1, MonitorMatch.Pick(monitors, new PixelRect(-5000, -5000, 100, 100), 1));
    }

    // --- stability ------------------------------------------------------------------------

    [Fact]
    public void Stability_Needs500msWithin0_3Degrees()
    {
        var line = Watch[1];
        var st = new StabilityTracker();
        st.Add((CalibSample)CalibLineParser.Parse(At(line, 0))!);
        Assert.False(st.Stable);
        st.Add((CalibSample)CalibLineParser.Parse(At(line, 400_000_000))!);
        Assert.False(st.Stable);
        Assert.Equal(0.8, st.Progress, 6);
        st.Add((CalibSample)CalibLineParser.Parse(At(line, 500_000_000))!);
        Assert.True(st.Stable);

        // A 1° turn restarts the run.
        var o = JsonNode.Parse(At(line, 600_000_000))!.AsObject();
        double a = Math.PI / 180;
        var cam = o["camera"]!.AsObject();
        foreach (var k in new[] { "left_dir", "right_dir" })
        {
            var d = cam[k]!.AsArray();
            double x = d[0]!.GetValue<double>(), z = d[2]!.GetValue<double>();
            cam[k] = new JsonArray(x * Math.Cos(a) + z * Math.Sin(a), d[1]!.GetValue<double>(), -x * Math.Sin(a) + z * Math.Cos(a));
        }
        st.Add((CalibSample)CalibLineParser.Parse(o.ToJsonString())!);
        Assert.False(st.Stable);

        // A lost lock restarts it too.
        st.Add((CalibSample)CalibLineParser.Parse("{\"type\":\"sample\",\"seq\":9,\"capture_ns\":2000000000,\"tracking\":false,\"camera\":null}")!);
        Assert.False(st.Stable);
        Assert.Equal(0, st.Progress);
    }

    // --- the state machine ----------------------------------------------------------------

    [Fact]
    public void Space_WhileMoving_Arms_ThenCapturesWhenStill()
    {
        var s = Started();
        Assert.Equal(CalibPhase.Aligning, s.Phase);
        Assert.Equal("centre", s.CurrentDot!.Name);
        Assert.Equal(CalibrationSession.NearPrompt + CalibrationSession.AlignPrompt, s.Prompt);
        s.OnLine(At(Watch[1], 1_000_000_000));
        s.Space();
        Assert.True(s.Armed);
        Assert.Equal(0, s.CapturedCount);
        s.OnLine(At(Watch[1], 1_600_000_000));
        Assert.Equal(1, s.CapturedCount);
        Assert.False(s.Armed);
        Assert.Equal("left", s.CurrentDot!.Name);
    }

    [Fact]
    public void Space_WhenAlreadyStill_CapturesAtOnce()
    {
        var s = Started();
        s.OnLine(At(Watch[1], 1_000_000_000));
        s.OnLine(At(Watch[1], 1_600_000_000));
        Assert.True(s.Stable);
        Assert.Equal(0, s.CapturedCount); // still, but no Space yet
        s.Space();
        Assert.Equal(1, s.CapturedCount);
    }

    [Fact]
    public void SecondPass_PromptsSittingBack()
    {
        var s = Started();
        Assert.StartsWith("Near pass: lean in to about 35 cm", s.Prompt);
        long t = 1_000_000_000;
        foreach (var line in Watch.Skip(1).Take(5)) CaptureSample(s, line, ref t);
        Assert.Equal(CalibPass.Far, s.CurrentDot!.Pass);
        Assert.Equal("centre", s.CurrentDot.Name);
        Assert.StartsWith("Far pass: sit back to about 65 cm", s.Prompt);
        Assert.Contains("Target 6 of 10: centre, far", s.Status);
        // The camera-to-eye distance (the camera sits 186 mm from the screen centre), with the 1.08 depth bias.
        Assert.InRange(s.PassDistanceM(CalibPass.Near)!.Value / 1.08, 0.44, 0.45);
        Assert.Null(s.PassDistanceM(CalibPass.Far));
    }

    [Fact]
    public void PassesTooCloseInDistance_RetakeTheFarPass()
    {
        var s = Started();
        long t = 1_000_000_000;
        // The "far" pass taken at the near distance.
        foreach (var line in Watch.Skip(1).Take(5).Concat(Watch.Skip(1).Take(5))) CaptureSample(s, line, ref t);
        Assert.Equal(CalibPhase.Aligning, s.Phase);
        Assert.Null(s.TakeRequest());
        Assert.Equal(5, s.CapturedCount);
        Assert.Equal(CalibPass.Far, s.CurrentDot!.Pass);
        Assert.Equal("centre", s.CurrentDot.Name);
        Assert.Contains("Sit further back and retake the far pass", s.Error);
        Assert.Contains("near about 48 cm, far about 48 cm", s.Error);
        // Now really far: the solve runs, and the warning is gone.
        foreach (var line in Watch.Skip(6)) CaptureSample(s, line, ref t);
        Assert.Equal(CalibPhase.Solving, s.Phase);
        Assert.Null(s.Error);
        Assert.NotNull(s.TakeRequest());
        Assert.True(s.PassDistanceM(CalibPass.Far) >= CalibGeometry.MinDistanceRatio * s.PassDistanceM(CalibPass.Near));
    }

    [Fact]
    public void WithoutDistanceEstimates_TheDistanceCheckIsSkipped()
    {
        static string NoDistance(string line)
        {
            var o = JsonNode.Parse(line)!.AsObject();
            o["camera"]!["left_distance_m"] = 0;
            o["camera"]!["right_distance_m"] = 0;
            return o.ToJsonString();
        }
        var s = Started();
        long t = 1_000_000_000;
        foreach (var line in Watch.Skip(1).Take(5).Concat(Watch.Skip(1).Take(5))) CaptureSample(s, NoDistance(line), ref t);
        Assert.Null(s.PassDistanceM(CalibPass.Near));
        Assert.Equal(CalibPhase.Solving, s.Phase);
    }

    [Fact]
    public void TenCaptures_RequestAModeCSolve_WithTheCliShapedInput()
    {
        var s = Captured();
        Assert.Equal(CalibPhase.Solving, s.Phase);
        var req = s.TakeRequest()!;
        Assert.True(req.IsSolve);
        Assert.Equal("c", req.Mode);
        Assert.Null(s.TakeRequest()); // once

        var mine = JsonNode.Parse(req.SolveInput!)!.AsObject();
        var cli = JsonNode.Parse(TestData.Text("calibrate-solve-input.json"))!.AsObject();
        Assert.Equal("c", mine["mode"]!.GetValue<string>());
        Assert.Equal("mid", mine["eye"]!.GetValue<string>());
        Assert.Equal("sim-tracking:webcam0", mine["camera_key"]!.GetValue<string>());
        Assert.Equal(Key, mine["screen"]!.GetValue<string>());
        var a = mine["samples"]!.AsArray();
        var b = cli["samples"]!.AsArray();
        Assert.Equal(b.Count, a.Count);
        for (int i = 0; i < a.Count; i++)
        {
            for (int k = 0; k < 2; k++)
                Assert.Equal(b[i]!["dot_m"]![k]!.GetValue<double>(), a[i]!["dot_m"]![k]!.GetValue<double>(), 6);
            // the sample is the watch line, as recorded on Space
            var ca = a[i]!["sample"]!["camera"]!;
            var cb = b[i]!["sample"]!["camera"]!;
            Assert.Equal(cb["key"]!.GetValue<string>(), ca["key"]!.GetValue<string>());
            foreach (var k in new[] { "left_dir", "right_dir" })
                for (int j = 0; j < 3; j++)
                    Assert.Equal(cb[k]![j]!.GetValue<double>(), ca[k]![j]!.GetValue<double>(), 12);
            foreach (var k in new[] { "left_distance_m", "right_distance_m" })
                Assert.Equal(cb[k]!.GetValue<double>(), ca[k]!.GetValue<double>(), 12);
            Assert.Equal("sample", a[i]!["sample"]!["type"]!.GetValue<string>());
        }
    }

    [Fact]
    public void SolveArgs_And_ApplyArgs()
    {
        Assert.Equal(new CliArgs("tracking", "calibrate", "--screen", Key, "--watch", "--json", "--interval-ms", "20"),
                     CalibrationCommands.Watch(Key));
        Assert.Equal(new CliArgs("tracking", "calibrate", "solve", "--in", "/tmp/x.json", "--mode", "c", "--eye", "mid",
                                 "--reject-mm", "5", "--json"), CalibrationCommands.Solve("/tmp/x.json"));
        Assert.Equal(new CliArgs("tracking", "calibrate", "solve", "--in", "/tmp/x.json", "--mode", "b", "--eye", "mid",
                                 "--reject-mm", "5", "--json"), CalibrationCommands.Solve("/tmp/x.json", "b"));
        // The focal scale is not solved (ill-conditioned with this dot layout).
        Assert.DoesNotContain("--focal", CalibrationCommands.Solve("/tmp/x.json").Args);
        Assert.EndsWith(System.IO.Path.Combine("DisplayXR", "calibration-last-solve.json"), CalibrationCommands.LastSolveInputPath);
        var pose = new CameraPose(new Vec3(0.035, 0.183, 0), 0.078459, 0, 0, 0.996917);
        Assert.Equal(new CliArgs("tracking", "calibrate", "apply", "--screen", Key, "--camera", "cam:0", "--pose",
                                 "0.035000,0.183000,0.000000,0.078459,0.000000,0.000000,0.996917", "--provider", "sim-tracking",
                                 "--depth-scale", "0.925926", "--json"),
                     CalibrationCommands.Apply(Key, "cam:0", pose, "sim-tracking", 0.9259259, 1));
        // unit scales are not written (apply then clears them)
        Assert.DoesNotContain("--depth-scale", CalibrationCommands.Apply(Key, "cam:0", pose, null, 1, 1).Args);
        Assert.Contains("--focal-scale", CalibrationCommands.Apply(Key, "cam:0", pose, null, 1, 1.02).Args);
        // The solve's quality is recorded with the calibration when given.
        var q = CalibrationCommands.Apply(Key, "cam:0", pose, null, 1, 1, 2.1, 10, "b").Args;
        Assert.Contains("--rms-mm", q);
        Assert.Contains("2.100", q);
        Assert.Contains("--samples", q);
        Assert.Contains("--mode", q);
    }

    [Fact]
    public void ApplyStoreFailure_SaysWhatDidNotWrite()
    {
        var o = JsonNode.Parse(TestData.Text("calibrate-apply-store.json"))!.AsObject();
        o["ok"] = false;
        o["failed"] = "camera_pose";
        o["reason"] = "denied";
        Assert.False(CalibrationCommands.ParseApply(o.ToJsonString(), out var report));
        Assert.Contains("camera pose failed: denied", report);
    }

    [Fact]
    public void SolveOk_Parses()
    {
        Assert.True(SolveResult.TryParse(TestData.Text("calibrate-solve-ok.json"), out var r));
        Assert.True(r!.Ok);
        Assert.Equal("c", r.Mode);
        Assert.Equal(0.035, r.Pose!.T.X, 6);
        Assert.Equal(0.183, r.Pose.T.Y, 6);
        Assert.Equal(9.0, r.PitchDeg, 3);
        Assert.Equal(0.0, r.YawDeg, 3);
        Assert.Equal(0.0, r.RollDeg, 3);
        Assert.Equal(1 / 1.08, r.IrisScale, 5);
        Assert.Equal(10, r.Samples.Count);
        Assert.Equal(5, r.DistinctDots);
        Assert.All(r.Samples, x => Assert.True(x.ResidualMm < 0.01));
        Assert.False(r.HighRms);
        Assert.Empty(r.Flagged);
        Assert.Equal(r.Pose.Setting, r.Setting); // client-side formatting = the solver's
    }

    [Fact]
    public void Describe_LogsThePoseAndItsQuality()
    {
        Assert.True(SolveResult.TryParse(TestData.Text("calibrate-solve-ok.json"), out var r));
        var d = CalibrationCommands.Describe(r!);
        Assert.StartsWith("solve ok: mode c, T (35.0, 183.0, 0.0) mm, pitch 9.00 yaw", d);
        Assert.Contains("roll", d);
        Assert.Contains("focal scale 1.0000, iris scale 0.9259", d);
        Assert.Contains("RMS 0.00 mm", d);
        Assert.Contains("10/10 samples, 5 dots", d);
        Assert.True(SolveResult.TryParse(TestData.Text("calibrate-solve-c-degenerate.json"), out var refused));
        Assert.StartsWith("solve refused: mode c, 10 samples, status degenerate: the samples constrain only 3 of the 5", CalibrationCommands.Describe(refused!));
    }

    [Fact]
    public void ModeCDegenerate_FallsBackToModeB_AndSaysSo()
    {
        var s = Captured();
        s.TakeRequest();
        s.SolveFinished(true, 2, TestData.Text("calibrate-solve-c-degenerate.json"), "");
        Assert.Equal(CalibPhase.Solving, s.Phase);
        Assert.Null(s.Solve);
        Assert.Equal("b", s.SolveMode);
        Assert.Contains("refused as degenerate", s.FallbackNote);
        Assert.Contains("cannot correct a camera yaw", s.FallbackNote);
        var req = s.TakeRequest()!;
        Assert.True(req.IsSolve);
        Assert.Equal("b", req.Mode);
        Assert.Equal("b", JsonNode.Parse(req.SolveInput!)!["mode"]!.GetValue<string>());

        s.SolveFinished(true, 0, TestData.Text("calibrate-solve-b-ok.json"), "");
        Assert.Equal(CalibPhase.Review, s.Phase);
        Assert.True(s.Solve!.Ok);
        Assert.Equal("b", s.Solve.Mode);
        Assert.Contains("reduced model", s.Prompt);
        // A mode-b refusal does not fall back again.
        var s2 = Captured();
        s2.TakeRequest();
        s2.SolveFinished(true, 2, TestData.Text("calibrate-solve-c-degenerate.json"), "");
        s2.TakeRequest();
        s2.SolveFinished(true, 2, TestData.Text("calibrate-solve-degenerate.json"), "");
        Assert.Equal(CalibPhase.Review, s2.Phase);
        Assert.Null(s2.TakeRequest());
        // A fresh solve starts at mode c again, without the note.
        s2.RetakeAll();
        long t = 20_000_000_000;
        foreach (var line in Watch.Skip(1)) CaptureSample(s2, line, ref t);
        Assert.Equal("c", s2.TakeRequest()!.Mode);
        Assert.Null(s2.FallbackNote);
    }

    [Fact]
    public void SolveRejected_FlagsTheLeftSample_RetakeQueuesOnlyIt()
    {
        var s = Captured();
        s.TakeRequest();
        s.SolveFinished(true, 0, TestData.Text("calibrate-solve-rejected.json"), "");
        Assert.Equal(CalibPhase.Review, s.Phase);
        Assert.Equal(new[] { 1 }, s.Solve!.Flagged);
        Assert.True(s.Solve.Samples[1].Rejected);
        Assert.False(s.Solve.HighRms);
        s.RetakeFlagged();
        Assert.Equal(CalibPhase.Aligning, s.Phase);
        Assert.Equal("left, near", s.CurrentDot!.Label);
        Assert.Equal(9, s.CapturedCount);
        long t = 9_000_000_000;
        CaptureSample(s, Watch[2], ref t);
        Assert.Equal(CalibPhase.Solving, s.Phase);
        Assert.NotNull(s.TakeRequest());
    }

    [Fact]
    public void SolveHighRms_Warns_AndFlagsSamplesOver5mm()
    {
        Assert.True(SolveResult.TryParse(TestData.Text("calibrate-solve-highrms.json"), out var r));
        Assert.True(r!.Ok);
        Assert.True(r.HighRms);
        Assert.True(r.RmsMm > 5);
        Assert.Equal(r.Samples.Where(x => x.ResidualMm > 5).Select(x => x.Index), r.Flagged);
        Assert.Contains(1, r.Flagged);

        var s = Captured();
        s.TakeRequest();
        s.SolveFinished(true, 0, TestData.Text("calibrate-solve-highrms.json"), "");
        Assert.Contains("RMS above 5 mm", s.Prompt);
        // the user may still continue to the live check
        s.Space();
        Assert.Equal(CalibPhase.LiveCheck, s.Phase);
    }

    [Fact]
    public void SolveRefused_ShowsTheReason_RetakeStartsOver()
    {
        var s = Captured();
        s.TakeRequest();
        s.SolveFinished(true, 2, TestData.Text("calibrate-solve-c-degenerate.json"), "");
        s.TakeRequest(); // the mode-b fallback, refused too
        s.SolveFinished(true, 2, TestData.Text("calibrate-solve-degenerate.json"), "");
        Assert.Equal(CalibPhase.Review, s.Phase);
        Assert.False(s.Solve!.Ok);
        Assert.Equal("degenerate", s.Solve.Status);
        Assert.Null(s.Solve.Pose);
        Assert.Contains("scale ambiguity", s.Prompt);
        s.Space(); // no live check on a refusal
        Assert.Equal(CalibPhase.Review, s.Phase);
        s.RetakeFlagged(); // nothing flagged -> all
        Assert.Equal(CalibPhase.Aligning, s.Phase);
        Assert.Equal(0, s.CapturedCount);
        Assert.Equal("centre", s.CurrentDot!.Name);
    }

    [Fact]
    public void SolveWithNoJson_StaysInReview_WithTheCliSummary()
    {
        var s = Captured();
        s.TakeRequest();
        s.SolveFinished(true, 1, "error: cannot read", "error: cannot read 'x'.");
        Assert.Equal(CalibPhase.Review, s.Phase);
        Assert.Null(s.Solve);
        Assert.Contains("cannot read", s.Error);
        s.RetakeAll();
        Assert.Equal(CalibPhase.Aligning, s.Phase);
    }

    [Fact]
    public void LiveCheck_PutsTheDotOnEachTarget_ThroughTheSolvedPose()
    {
        var s = Captured();
        s.TakeRequest();
        s.SolveFinished(true, 0, TestData.Text("calibrate-solve-ok.json"), "");
        s.Space();
        Assert.Equal(CalibPhase.LiveCheck, s.Phase);
        var g = s.Geometry!;
        for (int i = 0; i < 10; i++)
        {
            s.OnLine(Watch[1 + i]);
            var (fx, fy) = s.LiveFoot!.Value;
            var (dx, dy) = g.Fraction(CalibGeometry.Dots[i]);
            // within 0.1 mm of the target
            Assert.True(Math.Abs((fx - dx) * g.WidthM) < 1e-4, $"dot {i} x: {(fx - dx) * g.WidthM * 1000:0.000} mm");
            Assert.True(Math.Abs((fy - dy) * g.HeightM) < 1e-4, $"dot {i} y: {(fy - dy) * g.HeightM * 1000:0.000} mm");
        }
        // the depth uses the iris scale: the near centre sample at 40 cm, the far one at 70 cm
        s.OnLine(Watch[1]);
        Assert.Equal(0.40, s.LivePoint!.Value.Z, 3);
        s.OnLine(Watch[6]);
        Assert.Equal(0.70, s.LivePoint!.Value.Z, 3);
        Assert.Contains("near AND far", s.Prompt);
    }

    [Fact]
    public void LiveCheck_ArrowsNudgeT_ByOneMillimetre_AndApplyCarriesIt()
    {
        var s = Captured();
        s.TakeRequest();
        s.SolveFinished(true, 0, TestData.Text("calibrate-solve-ok.json"), "");
        s.Space();
        s.OnLine(Watch[1]);
        var before = s.LiveFoot!.Value;
        s.Nudge(1, 0);
        s.Nudge(1, 0);
        s.Nudge(0, -1);
        var after = s.LiveFoot!.Value;
        Assert.Equal(0.002, (after.Fx - before.Fx) * s.Geometry!.WidthM, 9);
        Assert.Equal(0.001, (after.Fy - before.Fy) * s.Geometry.HeightM, 9); // y down on screen
        Assert.Equal(0.037, s.Pose!.T.X, 6);
        Assert.Equal(0.182, s.Pose.T.Y, 6);

        s.Enter();
        Assert.Equal(CalibPhase.Applying, s.Phase);
        var req = s.TakeRequest()!;
        Assert.False(req.IsSolve);
        Assert.Equal(new CliArgs("tracking", "calibrate", "apply", "--screen", Key, "--camera", "sim-tracking:webcam0", "--pose",
                                 "0.037000,0.182000,0.000000,0.078459,-0.000000,0.000000,0.996917", "--provider", "sim-tracking",
                                 "--rms-mm", "0.000", "--samples", "10", "--mode", "c",
                                 "--depth-scale", "0.925926", "--json"), req.Args);

        // The current CLI: the runtime-owned camera calibration store.
        Assert.True(CalibrationCommands.ParseApply(TestData.Text("calibrate-apply-store.json"), out var report));
        Assert.Contains("camera 'sim-tracking:webcam0'", report);
        Assert.Contains("shared by every provider", report);
        // An older CLI (#2303, provider settings) still reads.
        Assert.True(CalibrationCommands.ParseApply(TestData.Text("calibrate-apply.json"), out var legacy));
        Assert.Contains("CAMERA_POSE_0 of 'sim-tracking'", legacy);
        s.ApplyFinished(true, report);
        Assert.Equal(CalibPhase.Done, s.Phase);
        Assert.False(s.WantsStream);
    }

    [Fact]
    public void ApplyFailure_ReturnsToTheLiveCheck_WithTheReason()
    {
        var s = Captured();
        s.TakeRequest();
        s.SolveFinished(true, 0, TestData.Text("calibrate-solve-ok.json"), "");
        s.Space();
        s.Enter();
        s.TakeRequest();
        var o = JsonNode.Parse(TestData.Text("calibrate-apply.json"))!.AsObject();
        o["ok"] = false;
        o["failed_key"] = "CAMERA_POSE_0_CAMERA";
        Assert.False(CalibrationCommands.ParseApply(o.ToJsonString(), out var report));
        Assert.Contains("CAMERA_POSE_0_CAMERA", report);
        s.ApplyFinished(false, report);
        Assert.Equal(CalibPhase.LiveCheck, s.Phase);
        Assert.Contains("Could not save", s.Error);
    }

    [Fact]
    public void Escape_CancelsFromAnyPhase_AndDropsAPendingRun()
    {
        var s = Captured();
        Assert.Equal(CalibPhase.Solving, s.Phase);
        s.Escape();
        Assert.Equal(CalibPhase.Cancelled, s.Phase);
        Assert.Null(s.TakeRequest());
        Assert.False(s.WantsStream);
        // late results are ignored
        s.SolveFinished(true, 0, TestData.Text("calibrate-solve-ok.json"), "");
        Assert.Equal(CalibPhase.Cancelled, s.Phase);

        var s2 = new CalibrationSession(Key, null);
        s2.Escape();
        Assert.Equal(CalibPhase.Cancelled, s2.Phase);
    }

    [Fact]
    public void Back_FromLiveCheck_ReturnsToReview()
    {
        var s = Captured();
        s.TakeRequest();
        s.SolveFinished(true, 0, TestData.Text("calibrate-solve-ok.json"), "");
        s.Space();
        s.Back();
        Assert.Equal(CalibPhase.Review, s.Phase);
        Assert.Null(s.LiveFoot);
    }

    [Fact]
    public void Quaternion_RotatesAsTheSolverReports()
    {
        // +9° about X (pitch, optical axis tilted down): +Z (toward the viewer) tilts to -Y.
        var p = new CameraPose(new Vec3(0, 0, 0), Math.Sin(4.5 * Math.PI / 180), 0, 0, Math.Cos(4.5 * Math.PI / 180));
        var v = p.Rotate(new Vec3(0, 0, 1));
        Assert.Equal(-Math.Sin(9 * Math.PI / 180), v.Y, 9);
        Assert.Equal(Math.Cos(9 * Math.PI / 180), v.Z, 9);
    }
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Linq;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

public class VerdictTests
{
    private static readonly StatusSnapshot Design = TestData.Parse(TestData.DesignExample);

    /// <summary>One healthy 3D display with an app drawing on it.</summary>
    private static StatusSnapshot OneLive => Design with { Screens = new[] { Design.Screens[0] } };

    /// <summary>The same display with no app running: no live state (what real hardware reports at idle).</summary>
    private static StatusSnapshot OneIdle => Design with
    {
        Screens = new[]
        {
            Design.Screens[0] with
            {
                Dps = Array.Empty<ScreenDp>(), Mode = null,
                EyeTracking = Design.Screens[0].EyeTracking with { State = "NO_DP" },
                Vendor = Design.Screens[0].Vendor with { Lens = "UNKNOWN" },
            },
        },
        Clients = Array.Empty<Client>(),
    };

    private static readonly (bool, string?) DxrRuntime = (true, @"C:\Program Files\DisplayXR\Runtime\DisplayXR_win64.json");

    [Fact]
    public void Healthy_WithAnApp_SaysWorking()
    {
        var v = Verdicts.From(OneLive, null, DxrRuntime);
        Assert.Equal(VerdictKind.Working, v.Kind);
        Assert.Equal("3D is working on 1 display", v.Headline);
        Assert.Equal("", v.Subline);
        Assert.Equal(Level.Ok, v.Level);
        Assert.Empty(v.Attention);
        var row = Assert.Single(v.Rows);
        Assert.Equal(new[] { "3D display recognised", "3D on", "Sees you" }, row.Facts.Select(f => f.Text));
        Assert.All(row.Facts, f => Assert.Equal(Tick.Yes, f.Tick));
        Assert.Equal("DisplayXR Leia SR", row.Driver); // the driver's own name; the id is the tooltip
        Assert.Equal("leia-sr", row.DriverId);
        Assert.Equal("Main display", row.Role);
    }

    [Fact]
    public void Idle_NoApp_NeverClaims3DOnOrSeesYou()
    {
        // A tracker that happens to report TRACKING with no app drawing still does not count: no live state.
        var idle = OneIdle with { Screens = new[] { OneIdle.Screens[0] with { EyeTracking = OneIdle.Screens[0].EyeTracking with { State = "TRACKING" } } } };
        foreach (var snap in new[] { OneIdle, idle })
        {
            var v = Verdicts.From(snap, null, DxrRuntime);
            Assert.Equal(VerdictKind.Ready, v.Kind);
            Assert.Equal("Your 3D display is ready", v.Headline);
            Assert.Equal("3D turns on when a 3D app runs.", v.Subline);
            Assert.DoesNotContain("working", v.Headline);
            var facts = Assert.Single(v.Rows).Facts;
            Assert.Equal(new[] { Tick.Yes, Tick.NotKnownYet, Tick.NotKnownYet }, facts.Select(f => f.Tick));
            Assert.Equal(new[] { "3D display recognised", "3D on: not known yet", "Sees you: not known yet" }, facts.Select(f => f.Text));
        }
    }

    [Fact]
    public void Live_3DOffAndNotSeeing_AreNo_NotProblems()
    {
        var s = OneLive with
        {
            Screens = new[]
            {
                OneLive.Screens[0] with
                {
                    Vendor = OneLive.Screens[0].Vendor with { Lens = "2D" },
                    EyeTracking = OneLive.Screens[0].EyeTracking with { State = "NOT_TRACKING" },
                },
            },
        };
        var v = Verdicts.From(s, null, DxrRuntime);
        Assert.Equal(new[] { "3D display recognised", "3D off", "Doesn't see you right now" }, v.Rows[0].Facts.Select(f => f.Text));
        Assert.Equal(new[] { Tick.Yes, Tick.No, Tick.No }, v.Rows[0].Facts.Select(f => f.Tick));
        Assert.Equal(VerdictKind.Ready, v.Kind); // an app may show 2D on purpose: nothing invented to fix
        Assert.Empty(v.Attention);
    }

    [Fact]
    public void SomethingWrong_SaysOffAndCountsWhatNeedsFixing()
    {
        var bad = OneIdle with
        {
            Screens = new[] { OneIdle.Screens[0] with { Warnings = new[] { new StatusWarning("NOT_NATIVE", WarningLevel.Critical, "tech") } } },
        };
        var v = Verdicts.From(bad, null, DxrRuntime);
        Assert.Equal(VerdictKind.Off, v.Kind);
        Assert.Equal("3D is off: 1 thing needs fixing", v.Headline);
        Assert.Equal(Level.Critical, v.Level);
        var item = Assert.Single(v.Attention);
        Assert.StartsWith("AUO B194 is not at its full resolution", item.Text);
        Assert.Equal("NOT_NATIVE: tech", item.Technical);
        Assert.Equal(AttentionAction.OpenDisplays, item.Action);

        var two = Verdicts.From(bad, null, (true, @"C:\Other\other_runtime.json"));
        Assert.Equal("3D is off: 2 things need fixing", two.Headline);
        Assert.Contains(two.Attention, a => a.Action == AttentionAction.ActivateRuntime && a.Text == Verdicts.RuntimeOtherSentence);
    }

    [Fact]
    public void Working_WithAWarning_StaysWorking_AndSaysTheCount()
    {
        var v = Verdicts.From(Design, null, DxrRuntime); // the second display carries SEGMENT_FLAT_2D
        Assert.Equal(VerdictKind.Working, v.Kind);
        Assert.Equal("3D is working on 2 displays", v.Headline);
        Assert.Equal("1 thing needs attention.", v.Subline);
        Assert.Equal(Level.Warn, v.Level);
        Assert.Equal("Part of an app's window is on Acer SpatialLabs DS1, which has no display driver, so that part shows flat 2D.",
                     Assert.Single(v.Attention).Text);
    }

    [Fact]
    public void ServiceNotRunning_IsRecognisedNotWorking()
    {
        var headless = OneLive with { Source = "headless", Clients = Array.Empty<Client>() };
        var v = Verdicts.From(headless, null, DxrRuntime);
        Assert.Equal(VerdictKind.NotRunning, v.Kind);
        Assert.Equal("Your 3D display is recognised", v.Headline);
        Assert.Equal("Whether 3D is on is not known yet: DisplayXR isn't running in the background.", v.Subline);
        Assert.Equal(new[] { Tick.Yes, Tick.NotKnownYet, Tick.NotKnownYet }, v.Rows[0].Facts.Select(f => f.Tick)); // dps in a headless snapshot are not live
        Assert.Equal(Verdicts.NotRunningSentence, Assert.Single(v.Attention).Text);
        Assert.DoesNotContain("work", Verdicts.NotRunningSentence); // no promise the Dashboard cannot check
    }

    [Fact]
    public void NoDisplay_And_No3DDisplay()
    {
        var none = Verdicts.From(Design with { Screens = Array.Empty<Screen>() }, null, DxrRuntime);
        Assert.Equal(VerdictKind.NoDisplay, none.Kind);
        Assert.Equal("No display found", none.Headline);

        var flat = OneIdle.Screens[0] with { Claim = OneIdle.Screens[0].Claim with { PluginId = null, Confidence = "NONE" } };
        var v = Verdicts.From(OneIdle with { Screens = new[] { flat } }, null, DxrRuntime);
        Assert.Equal(VerdictKind.No3DDisplay, v.Kind);
        Assert.Equal("No 3D display found", v.Headline);
        var row = Assert.Single(v.Rows);
        Assert.False(row.Is3D);
        Assert.Empty(row.Facts);
        Assert.Null(row.Driver);
    }

    /// <summary>
    /// A PC with no 3D display: the simulated display driver claims every display (at FALLBACK, so apps
    /// run anywhere). That is not a 3D display: Home says "No 3D display found", calmly, with a link to
    /// the simulated display's options.
    /// </summary>
    private static StatusSnapshot SimOnly()
    {
        var sim = OneIdle.Plugins.Single(p => p.Id == "sim-display") with { Load = "ACTIVE" };
        var screen = OneIdle.Screens[0] with { Claim = OneIdle.Screens[0].Claim with { PluginId = "sim-display", Confidence = "FALLBACK" } };
        return OneIdle with { Plugins = new[] { sim }, Screens = new[] { screen } };
    }

    [Fact]
    public void OnlyTheSimulatedDriver_IsNo3DDisplay_AndNothingIsWrong()
    {
        var v = Verdicts.From(SimOnly(), null, DxrRuntime);
        Assert.Equal(VerdictKind.No3DDisplay, v.Kind);
        Assert.Equal("No 3D display found", v.Headline);
        Assert.Equal("DisplayXR apps show in normal 2D on this PC. Nothing is wrong.", v.Subline);
        Assert.Equal(Level.Info, v.Level); // not a warning
        Assert.Equal("sim-display", v.SimulatedDriverId);
        var row = Assert.Single(v.Rows);
        Assert.False(row.Is3D); // an ordinary display, not "3D display recognised"
        Assert.Empty(row.Facts);

        // Something that does need attention is still counted after the sentence.
        var off = Verdicts.From(SimOnly() with { Source = "headless" }, null, DxrRuntime);
        Assert.Equal(VerdictKind.No3DDisplay, off.Kind);
        Assert.StartsWith(Verdicts.SimulatedOnlyLine + " 1 thing needs attention.", off.Subline);
    }

    [Fact]
    public void ARealDriverRecognised_IsNeverReportedMissing()
    {
        // The real driver claims the display, the simulated one is installed beside it: the usual verdict.
        var v = Verdicts.From(OneIdle, null, DxrRuntime);
        Assert.Equal(VerdictKind.Ready, v.Kind);
        Assert.Null(v.SimulatedDriverId);

        // Two displays: the 3D one claimed by the real driver, the other by the simulated one.
        var two = OneIdle with
        {
            Plugins = Design.Plugins.Select(p => p with { Load = "ACTIVE" }).ToArray(),
            Screens = new[] { OneIdle.Screens[0], SimOnly().Screens[0] with { Id = "other", Roles = OneIdle.Screens[0].Roles with { OsMain = false } } },
        };
        var tv = Verdicts.From(two, null, DxrRuntime);
        Assert.Equal(VerdictKind.Ready, tv.Kind);
        Assert.Equal("Your 3D display is ready", tv.Headline); // one 3D display; the other is ordinary
        Assert.Equal(new[] { true, false }, tv.Rows.Select(r => r.Is3D));
        Assert.Null(tv.SimulatedDriverId);

        // The simulated driver chosen for a display on purpose (a per-display override): a display DisplayXR
        // found, kept as recognised.
        var forced = SimOnly() with { Screens = new[] { SimOnly().Screens[0] with { Claim = SimOnly().Screens[0].Claim with { Forced = true } } } };
        Assert.NotEqual(VerdictKind.No3DDisplay, Verdicts.From(forced, null, DxrRuntime).Kind);

        // A real driver is active but claims nothing (and no display at all is claimed): the general sentence.
        var flat = OneIdle.Screens[0] with { Claim = OneIdle.Screens[0].Claim with { PluginId = null, Confidence = "NONE" } };
        var none = Verdicts.From(OneIdle with { Screens = new[] { flat } }, null, DxrRuntime);
        Assert.Equal(VerdictKind.No3DDisplay, none.Kind);
        Assert.StartsWith("None of your displays is recognised as a 3D display.", none.Subline);
        Assert.Null(none.SimulatedDriverId);
    }

    /// <summary>
    /// Review finding 1: a REAL 3D display whose vendor driver is broken also ends up claimed by the
    /// simulated driver at FALLBACK. That must keep reading as something to fix, never "Nothing is wrong",
    /// on Home and on Displays.
    /// </summary>
    private static StatusSnapshot BrokenVendor(string load, string platform, params StatusWarning[] screenWarnings)
    {
        var vendor = Design.Plugins.Single(p => p.Id == "leia-sr") with { Load = load, PlatformState = platform };
        var sim = SimOnly();
        return sim with
        {
            Plugins = new[] { vendor, sim.Plugins[0] },
            Screens = new[] { sim.Screens[0] with { Warnings = screenWarnings } },
        };
    }

    [Fact]
    public void VendorDriverFailedToLoad_IsNotCalm()
    {
        var s = BrokenVendor("LOAD_FAILED", "UNKNOWN",
            new StatusWarning("CLAIM_FALLBACK", WarningLevel.Warn, "Only the fallback 'sim-display' claims this screen"));
        Assert.Null(Verdicts.OnlySimulated(s));
        var v = Verdicts.From(s, null, DxrRuntime);
        Assert.NotEqual(VerdictKind.No3DDisplay, v.Kind);
        Assert.DoesNotContain("Nothing is wrong", v.Subline);
        Assert.Null(v.SimulatedDriverId);
        Assert.True(Assert.Single(v.Rows).Is3D);
        // Displays: a 3D display that needs attention, not "Ordinary 2D display".
        Assert.True(StatusText.IsReal3D(s, s.Screens[0]));
        Assert.NotEqual(Verdicts.Plain2D, StatusText.PlainBadge(s.Screens[0], s).Text);
        Assert.Equal(Level.Warn, StatusText.ScreenLevel(s.Screens[0], s));
    }

    [Fact]
    public void VendorDriverPlatformNotReady_IsNotCalm()
    {
        // Even without the screen warning (an older snapshot), the vendor driver's platform state says it.
        var s = BrokenVendor("LOADED", "NOT_RUNNING");
        Assert.Null(Verdicts.OnlySimulated(s));
        var v = Verdicts.From(s, null, DxrRuntime);
        Assert.NotEqual(VerdictKind.No3DDisplay, v.Kind);
        Assert.DoesNotContain("Nothing is wrong", v.Subline);
        Assert.True(StatusText.IsReal3D(s, s.Screens[0]));
        Assert.NotEqual(StatusText.SimulatedPreviewLine, StatusText.PlainStateLine(s.Screens[0], s));

        // A PLUGIN_NOT_READY warning anywhere is enough too.
        var warned = SimOnly() with { Warnings = new[] { new StatusWarning("PLUGIN_NOT_READY", WarningLevel.Critical, "x") } };
        Assert.Null(Verdicts.OnlySimulated(warned));
    }

    [Fact]
    public void VendorDriverRegisteredButDeclined_IsNotCalmEither()
    {
        // A healthy vendor driver that found no display of its own: no fallback warning, but a registered
        // vendor driver means the calm "only the simulated driver" sentence does not apply.
        var s = BrokenVendor("DECLINED", "READY");
        Assert.Null(Verdicts.OnlySimulated(s));
        var v = Verdicts.From(s, null, DxrRuntime);
        Assert.Equal(VerdictKind.No3DDisplay, v.Kind);
        Assert.StartsWith("None of your displays is recognised as a 3D display.", v.Subline);
        Assert.Null(v.SimulatedDriverId);
    }

    [Fact]
    public void OnlyTheSimulatedDriverRegistered_IsCalm_AndARealActiveDriverIsNotAffected()
    {
        Assert.Equal("sim-display", Verdicts.OnlySimulated(SimOnly()));
        Assert.Equal(VerdictKind.Ready, Verdicts.From(OneIdle, null, DxrRuntime).Kind); // leia-sr ACTIVE, healthy
        Assert.Null(Verdicts.OnlySimulated(OneIdle));
    }

    [Fact]
    public void IsSimulated_ByTheSnapshotsFallbackFlag_ElseByTheId()
    {
        var s = SimOnly();
        Assert.True(StatusText.IsSimulatedDriver(s, "sim-display"));
        Assert.False(StatusText.IsSimulatedDriver(Design, "leia-sr"));
        Assert.True(StatusText.IsSimulatedDriver(s with { Plugins = Array.Empty<PluginInfo>() }, "sim-display")); // not listed: by id
        Assert.False(StatusText.IsSimulatedDriver(s, null));
    }

    [Fact]
    public void Checking_BeforeTheFirstSnapshot()
    {
        var v = Verdicts.From(null);
        Assert.Equal(VerdictKind.Checking, v.Kind);
        Assert.Equal("Checking your 3D setup…", v.Headline);
    }

    [Fact]
    public void AFailedSelftest_IsOneItem_AndBlocks()
    {
        var st = new SelftestResult("FAIL", 3, new[] { new SelftestCheck("a", false, ""), new SelftestCheck("b", true, ""), new SelftestCheck("c", false, "") }, DateTime.Now);
        var v = Verdicts.From(OneIdle, st, DxrRuntime);
        Assert.Equal(VerdictKind.Off, v.Kind);
        var item = Assert.Single(v.Attention);
        Assert.Equal("Check my setup found 2 problems. The list is under Check my setup.", item.Text);
        Assert.Equal(AttentionAction.ShowChecks, item.Action);
    }

    [Fact]
    public void UnknownActiveRuntime_FlagsNothing()
    {
        var idle = OneIdle with { Runtime = OneIdle.Runtime with { ActiveOpenXrRuntime = null } };
        Assert.Equal((false, null), Verdicts.ActiveRuntime(null, idle));
        Assert.Empty(Verdicts.Attention(idle, null, Verdicts.ActiveRuntime(null, idle)));
        Assert.Equal((true, (string?)null), Verdicts.ActiveRuntime(Info(queried: true, set: false), idle));
        Assert.Equal(Verdicts.RuntimeUnsetSentence, Assert.Single(Verdicts.Attention(idle, null, (true, null))).Text);
    }

    [Fact]
    public void Warnings_InPlainWords_DisplayMakerQuoted_UnknownCodeGeneral()
    {
        var w = new StatusWarning("SOMETHING_NEW", WarningLevel.Warn, "internal words");
        Assert.Equal("DisplayXR reports a problem with Panel. Displays has the details.", Verdicts.PlainWarning(w, "Panel"));
        Assert.Equal("The display maker's software reports a problem on Panel: camera", Verdicts.PlainWarning(new StatusWarning("X", WarningLevel.Warn, "camera"), "Panel", true));
        foreach (var code in new[] { "NOT_NATIVE", "PLUGIN_NOT_READY", "CLAIM_FALLBACK", "NO_PHYSICAL_SIZE", "SEGMENT_FLAT_2D", "TRACKER_DOWN", "DP_DEGRADED", "DP_STALE", "CONV_CRASH_LOOP", "CONV_TASK_UNSERVED", "CONV_RESEARCH_MODEL_ENABLED", "SOMETHING_NEW" })
        {
            string text = Verdicts.PlainWarning(new StatusWarning(code, WarningLevel.Warn, ""), "Panel");
            Assert.DoesNotContain("_", text);
            Assert.DoesNotContain("DXR", text);
            Assert.DoesNotContain("!", text);
        }
        var vendor = OneIdle.Screens[0] with { Vendor = OneIdle.Screens[0].Vendor with { WorstWarning = new StatusWarning("V1", WarningLevel.Warn, "lens") } };
        var item = Assert.Single(Verdicts.Attention(OneIdle with { Screens = new[] { vendor } }, null, DxrRuntime));
        Assert.Equal("The display maker's software reports a problem on AUO B194: lens", item.Text);
    }

    [Fact]
    public void Counts_NeverZero_LastCheckedIsSessionOnly()
    {
        Assert.Equal("1 thing needs", Verdicts.Things(1, "needs", "need"));
        Assert.Equal("3 things need", Verdicts.Things(3, "needs", "need"));
        Assert.Equal("Last checked: not yet", Verdicts.LastChecked(null, false));
        Assert.Equal("Checking now…", Verdicts.LastChecked(null, true));
        Assert.Equal("Last checked: 14:05", Verdicts.LastChecked(new SelftestResult("PASS", 0, Array.Empty<SelftestCheck>(), new DateTime(2026, 11, 22, 14, 5, 0)), false));
    }

    private static InfoResult Info(bool queried, bool set) =>
        new("", "", 5, queried, set, set ? "x" : "", null, "", "", "", "", Array.Empty<InfoPlugin>(), null, PerfState.Empty);
}

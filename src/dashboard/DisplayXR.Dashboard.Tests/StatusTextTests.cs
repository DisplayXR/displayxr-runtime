// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Linq;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

public class StatusTextTests
{
    private static readonly StatusSnapshot Design = TestData.Parse(TestData.DesignExample);

    private static Screen WithWarnings(Screen s, params StatusWarning[] w) => s with { Warnings = w };

    [Fact]
    public void Dot_FollowsTheWorstWarning()
    {
        var a = Design.Screens[0];
        Assert.Equal(Level.Ok, StatusText.ScreenLevel(a)); // claimed, no warnings
        Assert.Equal(Level.Ok, StatusText.ScreenLevel(WithWarnings(a, new StatusWarning("CLAIM_EDID_ONLY", WarningLevel.Info, "x")))); // info stays quiet
        Assert.Equal(Level.Warn, StatusText.ScreenLevel(Design.Screens[1]));
        Assert.Equal(Level.Critical, StatusText.ScreenLevel(WithWarnings(a,
            new StatusWarning("SEGMENT_FLAT_2D", WarningLevel.Warn, "x"), new StatusWarning("NOT_NATIVE", WarningLevel.Critical, "y"))));
        var unclaimed = a with { Claim = a.Claim with { PluginId = null, Confidence = "NONE" } };
        Assert.Equal(Level.Plain, StatusText.ScreenLevel(unclaimed));
    }

    [Fact]
    public void VendorWorstWarning_CountsOnce()
    {
        var a = Design.Screens[0];
        var vw = new StatusWarning("TRACKER_DOWN", WarningLevel.Critical, "camera");
        var withVendor = a with { Vendor = a.Vendor with { WorstWarning = vw } };
        Assert.Equal(Level.Critical, StatusText.ScreenLevel(withVendor));
        Assert.Single(StatusText.ScreenWarnings(withVendor));
        var dup = withVendor with { Warnings = new[] { new StatusWarning("TRACKER_DOWN", WarningLevel.Warn, "runtime") } };
        Assert.Single(StatusText.ScreenWarnings(dup)); // same code: the runtime's own wins
    }

    [Theory]
    [InlineData("VERIFIED", "VERIFIED", Level.Ok)]
    [InlineData("EDID", "EDID", Level.Info)]
    [InlineData("FALLBACK", "FALLBACK", Level.Warn)]
    [InlineData("SOMETHING_NEW", "SOMETHING_NEW", Level.Plain)]
    public void Badge_IsTheClaimConfidence(string confidence, string text, Level level)
    {
        var a = Design.Screens[0];
        var s = a with { Claim = a.Claim with { Confidence = confidence } };
        Assert.Equal((text, level), StatusText.Badge(s));
    }

    [Fact]
    public void Badge_Unclaimed()
    {
        var a = Design.Screens[0];
        Assert.Equal(("UNCLAIMED", Level.Plain), StatusText.Badge(a with { Claim = a.Claim with { PluginId = null } }));
    }

    [Fact]
    public void Lines_MatchTheDesign()
    {
        var a = Design.Screens[0];
        Assert.Equal(new[] { "OS main", "runtime default", "vendor primary" }, StatusText.Chips(a));
        Assert.Empty(StatusText.Chips(Design.Screens[1]));
        Assert.Equal(@"\\.\DISPLAY1 · 3840×2160 @ 60 Hz · (0,0) · ×2.5 · 344×194 mm", StatusText.Identity(a));
        Assert.Equal("leia-sr · VERIFIED · QALA2137AL0011 · d3d11 d3d12 vk gl", StatusText.ClaimLine(a));
        Assert.StartsWith("tracking · lens 3D · mode LeiaSR (2 views, 3D) · DP: client 3 d3d11 primary OK", StatusText.StateLine(a));
        Assert.Equal("Vendor: ready · verified · calibrated · tracker RUNNING · model AL", StatusText.VendorLine(a));
        Assert.Equal("2 monitors · 2 claimed (2 verified) · 2 tracking", StatusText.DisplaysHeader(Design));
        Assert.Equal("DISPLAY5", StatusText.ShortDevice(@"\\.\DISPLAY5"));
        Assert.Equal("SEGMENT_FLAT_2D: A window spans", StatusText.WarningLine(Design.Screens[1].Warnings[0])[..31]);
        Assert.Contains("size unknown", StatusText.Identity(Design.Screens[1]));
    }

    [Fact]
    public void HomeLine_OnlyWithSeveralScreensOrAWarning()
    {
        Assert.Equal("2 screens · 2 tracking · 1 warning", StatusText.HomeLine(Design));
        var one = Design with { Screens = new[] { Design.Screens[0] } };
        Assert.Null(StatusText.HomeLine(one));
        var oneInfo = Design with { Screens = new[] { WithWarnings(Design.Screens[0], new StatusWarning("CLAIM_EDID_ONLY", WarningLevel.Info, "")) } };
        Assert.Null(StatusText.HomeLine(oneInfo)); // info alone shows nothing new
        var oneWarn = Design with { Screens = new[] { Design.Screens[1] } };
        Assert.Equal("1 screen · 1 tracking · 1 warning", StatusText.HomeLine(oneWarn));
    }

    [Fact]
    public void NavBadges()
    {
        Assert.Equal(new NavBadge(1, Level.Warn), StatusText.DisplaysBadge(Design));
        Assert.Null(StatusText.DisplaysBadge(null));
        var critical = Design with { Warnings = new[] { new StatusWarning("PLUGIN_NOT_READY", WarningLevel.Critical, "") } };
        Assert.Equal(new NavBadge(2, Level.Critical), StatusText.DisplaysBadge(critical));
        var infoOnly = Design with { Screens = new[] { Design.Screens[0] },
                                     Warnings = new[] { new StatusWarning("SERVICE_HEADLESS", WarningLevel.Info, "") } };
        Assert.Null(StatusText.DisplaysBadge(infoOnly));

        Assert.Equal(new NavBadge(1, Level.Plain), StatusText.WindowsBadge(Design));
        var diagOnly = Design with { Clients = new[] { Design.Clients[0] with { Class = "DIAG" } } };
        Assert.Null(StatusText.WindowsBadge(diagOnly));
    }

    [Fact]
    public void Integrity_AlwaysCarriesTheTriple()
    {
        var c = Design.Clients[0];
        Assert.Equal("paint 1811 · present 1809 · skip 2 · weave scanout", StatusText.IntegrityLine(c));
        Assert.Equal("paint 0 · present 0 · skip 0 · weave —",
            StatusText.IntegrityLine(c with { Integrity = new Integrity(0, 0, 0, null) }));
        Assert.Equal("DISPLAY5 (Acer SpatialLabs DS1)", StatusText.ScreenRef(Design, c.OwnerScreen));
        Assert.Equal("—", StatusText.ScreenRef(Design, null));
        Assert.Equal("0xdead", StatusText.ScreenRef(Design, "0xdead"));
    }

    [Fact]
    public void FlatSegment_UsesTheScreenRowText()
    {
        var seg = Design.Clients[0].Segments.Items[1] with { HasDp = false };
        Assert.StartsWith("SEGMENT_FLAT_2D: A window spans this screen", StatusText.FlatSegmentText(Design, seg));
    }

    [Fact]
    public void Copy_ListsEveryScreen()
    {
        string text = StatusText.Copy(Design);
        Assert.Contains("1. AUO B194 [OS main, runtime default, vendor primary] - VERIFIED", text);
        Assert.Contains("2. Acer SpatialLabs DS1 - VERIFIED", text);
        Assert.Contains("(warn) SEGMENT_FLAT_2D", text);
        Assert.DoesNotContain("gen 17", StatusText.Copy(Design, includeSource: false));
    }

    [Fact]
    public void DpSelector_HiddenWithoutPhase7Keys()
    {
        var dp = new DpList(null, new[] { new DpRow("leia-sr", "DisplayXR Leia SR", "", "", 50, true, false) });
        Assert.Null(StatusText.DpSelector(Design.Screens[0], dp));          // no key: older CLI
        var keyed = Design.Screens[0] with { Key = "k0" };
        Assert.Null(StatusText.DpSelector(keyed, null));                     // dp list not read
        Assert.Null(StatusText.DpSelector(keyed, new DpList(null, System.Array.Empty<DpRow>(), System.Array.Empty<DpScreen>())));
        // Keys in the snapshot but a dp list without screens[]: an older CLI would
        // ignore --screen and write the global override, so no selector.
        Assert.Null(StatusText.DpSelector(keyed, dp));
    }

    [Fact]
    public void DpSelector_AutoOrPreferred()
    {
        var plugins = new[]
        {
            new DpRow("leia-sr", "DisplayXR Leia SR", "", "", 50, true, false),
            new DpRow("sim-display", "", "", "", 200, false, false),
        };
        var keyed = Design.Screens[1] with { Key = "k1" };
        var auto = StatusText.DpSelector(keyed, new DpList(null, plugins, System.Array.Empty<DpScreen>()))!.Value;
        Assert.Equal(new[] { "Auto (leia-sr)", "DisplayXR Leia SR (leia-sr)", "sim-display" }, auto.Items.Select(i => i.Label));
        Assert.Equal(0, auto.Selected);

        var dp = new DpList(null, plugins, new[] { new DpScreen("k1", "", "", "sim-display", "sim-display", "user", true, "live") });
        var forced = StatusText.DpSelector(keyed, dp)!.Value;
        Assert.Equal(2, forced.Selected);
        Assert.Equal("forced (user)", StatusText.ForcedChip(keyed, dp));
        Assert.Equal("applies live", StatusText.ApplyLabel(keyed, dp));
        Assert.Equal("dp use sim-display --screen k1", StatusText.DpVerb("k1", forced.Items[2]));
        Assert.Equal("dp reset --screen k1", StatusText.DpVerb("k1", forced.Items[0]));
        Assert.Equal("dp reset --screen \"a b\"", StatusText.DpVerb("a b", forced.Items[0]));

        var unknown = new DpList(null, plugins, new[] { new DpScreen("k1", "", "", "x", "gone-plugin", "machine", true, "next_session") });
        var u = StatusText.DpSelector(keyed, unknown)!.Value;
        Assert.Equal("gone-plugin (not registered)", u.Items[u.Selected].Label);
        Assert.StartsWith("applies to new sessions", StatusText.ApplyLabel(keyed, unknown));
        Assert.Null(StatusText.ForcedChip(Design.Screens[0], null));
    }

    [Fact]
    public void DpList_ReadsTodayAndPhase7Shapes()
    {
        Assert.True(DpList.TryParse(TestData.Text("dp-list.json"), out var today));
        Assert.Null(today!.Screens);
        Assert.Equal(new[] { "leia-sr", "sim-display" }, today.Plugins.Select(p => p.Id));
        Assert.True(today.Plugins[0].Active);

        Assert.True(DpList.TryParse("""
        { "plugins": ["leia-sr", "sim-display"], "screens": [
          { "key": "k1", "device_name": "\\\\.\\DISPLAY5", "friendly_name": "DS1", "effective_plugin": "leia-sr",
            "preferred_plugin": null, "preferred_source": null, "forced": false, "apply": "live" } ] }
        """, out var p7));
        Assert.Equal(new[] { "leia-sr", "sim-display" }, p7!.Plugins.Select(p => p.Id));
        var sc = Assert.Single(p7.Screens!);
        Assert.Equal(("k1", "leia-sr", (string?)null, false), (sc.Key, sc.EffectivePlugin, sc.PreferredPlugin, sc.Forced));
    }

    [Fact]
    public void VendorCommand_IsNotParsed()
    {
        Assert.Equal("v.exe --page displays --display SN1 --id 0x1",
            VendorCommand.Expand("v.exe --page displays --display {serial} --id {monitor_id}", "SN1", "0x1"));
        Assert.Equal(("v.exe", "--a \"b c\" d"), VendorCommand.SplitFirst("v.exe --a \"b c\" d"));
        Assert.Equal((@"C:\Program Files\V\v.exe", "--page x"), VendorCommand.SplitFirst("\"C:\\Program Files\\V\\v.exe\" --page x"));
        Assert.Equal(("v.exe", ""), VendorCommand.SplitFirst("v.exe"));
    }

    [Fact]
    public void CliModels_ReadRealOutput()
    {
        Assert.True(InfoResult.TryParse(TestData.Text("info.json"), out var info, out _));
        Assert.True(info!.ActiveRuntimeIsDisplayXR);
        Assert.Equal(5, info.PluginAbi);
        Assert.Equal("leia-sr", info.ActivePluginId);
        Assert.Equal(2, info.Gpu!.Adapters.Count);
        Assert.True(info.Gpu.SplitApplies);
        Assert.True(info.Gpu.Scanout.Resolved);
        Assert.Equal(6, info.Perf.Levers.Count);
        Assert.False(info.Perf.AnyNonDefault);

        Assert.True(SelftestResult.TryParse(TestData.Text("selftest.json"), System.DateTime.Now, out var st));
        Assert.True(st!.Passed);
        Assert.Equal(16, st.Checks.Count);

        Assert.True(PerfStateParser.TryParse(TestData.Text("perf-list.json"), out var perf));
        Assert.False(perf!.CompatibilityMode);
        var compat = perf with { Levers = new[] { new PerfLever("DXR_WEAVE_ON_SCANOUT", "0", "user"), new PerfLever("DXR_WEAVE_REPAINT", "0", "user") } };
        Assert.True(compat.CompatibilityMode);
        Assert.True(compat.AnyNonDefault);
        Assert.False(InfoResult.TryParse("{\"runtime\":", out _, out var err));
        Assert.NotNull(err);
    }
}

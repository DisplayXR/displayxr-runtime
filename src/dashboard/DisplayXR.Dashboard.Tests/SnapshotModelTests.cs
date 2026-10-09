// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Linq;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

public class SnapshotModelTests
{
    [Fact]
    public void DesignExample_ReadsEveryKey()
    {
        var s = TestData.Parse(TestData.DesignExample);
        Assert.Equal(1, s.Schema);
        Assert.Equal("service", s.Source);
        Assert.True(s.IsService);
        Assert.Equal(new Generation(17, 2412), s.Generation);
        Assert.Equal("2.32.0", s.Runtime.Version);
        Assert.Equal("v2.32.0", s.Runtime.GitTag);
        Assert.Equal(5, s.Runtime.PluginAbi);
        Assert.EndsWith("DisplayXR_win64.json", s.Runtime.ActiveOpenXrRuntime);

        Assert.Equal(2, s.Plugins.Count);
        var p = s.Plugins[0];
        Assert.Equal(("leia-sr", "DisplayXR Leia SR", "Leia Inc.", "2.13.0", "ACTIVE", "READY", "", false, 50),
                     (p.Id, p.Name, p.Vendor, p.Version, p.Load, p.PlatformState, p.Hint, p.Fallback, p.ProbeOrder));
        Assert.Equal("", s.Plugins[1].Vendor); // absent key
        Assert.True(s.Plugins[1].Fallback);

        var a = s.Screens[0];
        Assert.Equal("0x8c413a2f61152ce7", a.Id);
        Assert.Null(a.Key); // pre-phase-7 JSON
        Assert.Equal(0, a.Index);
        Assert.Equal(@"\\.\DISPLAY1", a.DeviceName);
        Assert.Equal("AUO B194", a.FriendlyName);
        Assert.Equal(new EdidInfo("AUO", "B194", 0), a.Edid);
        Assert.Equal(new PixelRect(0, 0, 3840, 2160), a.Desktop.Rect);
        Assert.Equal(2.5, a.Desktop.Scale);
        Assert.Equal(new NativeMode(3840, 2160, 60000, true), a.Native);
        Assert.Equal(new PhysicalMm(344, 194, "plugin"), a.PhysicalMm);
        Assert.Equal(new Roles(true, true, true), a.Roles);
        Assert.Equal("leia-sr", a.Claim.PluginId);
        Assert.Equal("VERIFIED", a.Claim.Confidence);
        Assert.Equal(100, a.Claim.ConfidenceValue);
        Assert.Equal("QALA2137AL0011", a.Claim.Serial);
        Assert.Equal(new[] { "d3d11", "d3d12", "vk", "gl" }, a.Claim.Apis);
        Assert.Null(a.Claim.Forced);
        Assert.Equal(0.3442, a.Layout.WidthM);
        Assert.Equal(new NominalViewer(0, 0.10, 0.60), a.Layout.Viewer);
        Assert.Equal("plugin", a.Layout.Source);
        Assert.Equal(new[] { "MANAGED" }, a.EyeTracking.Supported);
        Assert.Equal("MANAGED", a.EyeTracking.Default);
        Assert.Equal("TRACKING", a.EyeTracking.State);
        Assert.Equal(new ScreenMode(1, "LeiaSR", 2, true), a.Mode);
        Assert.Equal(new ScreenDp(3, "d3d11", "primary", "OK"), Assert.Single(a.Dps));
        Assert.True(a.Vendor.Present && a.Vendor.Ready && a.Vendor.Verified && a.Vendor.Calibrated);
        Assert.Equal(("RUNNING", "3D", "AL", "QALA2137AL0011"), (a.Vendor.Tracker, a.Vendor.Lens, a.Vendor.Model, a.Vendor.Serial));
        Assert.Null(a.Vendor.WorstWarning);
        Assert.StartsWith("vendor.exe", a.Vendor.DashboardCommand);

        var b = s.Screens[1];
        Assert.Equal(3.0, b.Desktop.Scale);
        Assert.Equal(new PhysicalMm(0, 0, "none"), b.PhysicalMm); // physical_mm absent
        Assert.Null(b.Mode);                                      // mode absent
        var w = Assert.Single(b.Warnings);
        Assert.Equal("SEGMENT_FLAT_2D", w.Code);
        Assert.Equal(WarningLevel.Warn, w.Level);

        var c = Assert.Single(s.Clients);
        Assert.Equal((3L, 24416L, "APP", "cube_handle_d3d11_win.exe"), (c.Id, c.Pid, c.Class, c.Name));
        Assert.Equal(new ClientFlags(true, true, true, false), c.Flags);
        Assert.Equal("APP_HWND", c.Presenter);
        Assert.Equal("slot", c.Lease);
        Assert.Equal(new PixelRect(3018, 285, 1664, 2093), c.Window);
        Assert.Equal("0xb72ea4c616544d01", c.OwnerScreen);
        Assert.Equal(41UL, c.Segments.Generation);
        Assert.True(c.Segments.Split);
        Assert.Equal(2, c.Segments.Items.Count);
        Assert.Equal(new Segment("0xb72ea4c616544d01", new CanvasRect(822, 0, 842, 1875), true, true, "DP"), c.Segments.Items[1]);
        Assert.Equal(new Views(2, 2, 4), c.Views);
        Assert.Equal(new Integrity(1811, 1809, 2, "scanout"), c.Integrity);
        Assert.False(c.IsDiag);

        Assert.Equal(new Workspace(false, null), s.Workspace);
        Assert.Empty(s.Warnings);
        Assert.Same(TestData.DesignExample, s.RawJson);
    }

    [Fact]
    public void RealHeadlessCliOutput_Parses()
    {
        var s = TestData.Parse(TestData.Text("status-headless-cli.json"));
        Assert.Equal("headless", s.Source);
        Assert.False(s.IsService);
        Assert.Equal(2, s.Screens.Count);
        Assert.All(s.Screens, x => Assert.Null(x.Mode));
        Assert.All(s.Screens, x => Assert.Null(x.Vendor.DashboardCommand));
        Assert.All(s.Screens, x => Assert.Equal("NO_DP", x.EyeTracking.State));
        Assert.Null(s.Screens[1].EyeTracking.Default);
        Assert.Empty(s.Clients);
        Assert.Equal("SERVICE_HEADLESS", Assert.Single(s.Warnings).Code);
    }

    [Fact]
    public void Nulls_AreTolerated()
    {
        const string json = """
        { "schema": 1, "source": "service", "generation": null, "runtime": null,
          "plugins": null, "workspace": { "enabled": true, "controller": null },
          "screens": [ { "id": "0x1", "mode": null, "claim": { "plugin_id": null, "confidence": "NONE" },
                         "vendor": { "present": true, "worst_warning": null, "dashboard_command": null },
                         "eye_tracking": { "supported": [], "default": null } } ],
          "clients": [ { "id": 1, "window": null, "owner_screen": null,
                         "integrity": { "weave_placement": null }, "segments": { "items": [ { "screen": null } ] } } ],
          "warnings": null }
        """;
        var s = TestData.Parse(json);
        Assert.Equal(new Generation(0, 0), s.Generation);
        Assert.Equal("", s.Runtime.Version);
        Assert.Null(s.Runtime.ActiveOpenXrRuntime);
        Assert.Empty(s.Plugins);
        Assert.True(s.Workspace.Enabled);
        var sc = Assert.Single(s.Screens);
        Assert.Null(sc.Mode);
        Assert.False(sc.Claim.IsClaimed);
        Assert.Null(sc.Vendor.WorstWarning);
        Assert.Null(sc.Vendor.DashboardCommand);
        Assert.Null(sc.EyeTracking.Default);
        var c = Assert.Single(s.Clients);
        Assert.Null(c.Window);
        Assert.Null(c.OwnerScreen);
        Assert.Null(c.Integrity.WeavePlacement);
        Assert.Null(Assert.Single(c.Segments.Items).Screen);
        Assert.Empty(s.Warnings);
    }

    [Fact]
    public void MinimalSnapshot_HasEmptyLists()
    {
        var s = TestData.Parse("{\"schema\":1}");
        Assert.Equal("headless", s.Source);
        Assert.Empty(s.Screens);
        Assert.Empty(s.Clients);
        Assert.Empty(s.Plugins);
    }

    [Fact]
    public void WrongTypes_FallBack()
    {
        var s = TestData.Parse("""
        { "schema": "1", "screens": [ 7, { "id": 12, "index": "x", "desktop": { "width": "3840", "scale": "bad" },
          "claim": { "apis": [ "vk", 3, null ] } } ], "clients": "nope" }
        """);
        Assert.Equal(1, s.Schema);
        var sc = Assert.Single(s.Screens); // the bare number is skipped
        Assert.Equal("12", sc.Id);
        Assert.Equal(0, sc.Index);
        Assert.Equal(3840, sc.Desktop.Rect.Width);
        Assert.Equal(1, sc.Desktop.Scale);
        Assert.Equal(new[] { "vk" }, sc.Claim.Apis);
        Assert.Empty(s.Clients);
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("{\"schema\":1,\"screens\":[{\"id\":")]
    [InlineData("[1,2,3]")]
    [InlineData("{\"no_schema\":true}")]
    [InlineData(" WARN [load_and_probe_one] plugin loader: something")]
    public void Unparsable_IsRejectedWithoutThrowing(string text)
    {
        Assert.False(StatusSnapshotParser.TryParse(text, out var s, out var err));
        Assert.Null(s);
        Assert.False(string.IsNullOrEmpty(err));
    }

    [Fact]
    public void Fixtures_ParseExceptTheTruncatedLine()
    {
        var lines = TestData.Lines("two-panels.ndjson");
        Assert.Equal(5, lines.Length);
        Assert.Equal(4, lines.Count(l => StatusSnapshotParser.TryParse(l, out _, out _)));
        var stress = TestData.Parse(TestData.Lines("stress-16x32.ndjson")[0]);
        Assert.Equal(16, stress.Screens.Count);
        Assert.Equal(32, stress.Clients.Count);
        var empty = TestData.Parse(TestData.Lines("empty-headless.ndjson")[0]);
        Assert.Empty(empty.Screens);
    }

    [Fact]
    public void Phase7Keys_AreRead()
    {
        var s = TestData.Parse("""
        { "schema": 1, "screens": [ { "id": "0x1", "key": "edid:AUO-B194-1",
          "claim": { "plugin_id": "sim-display", "confidence": "VERIFIED", "forced": true,
                     "preferred_plugin": "sim-display", "preferred_source": "user", "apply": "next_session" } } ] }
        """);
        var sc = s.Screens[0];
        Assert.Equal("edid:AUO-B194-1", sc.Key);
        Assert.True(sc.Claim.Forced);
        Assert.Equal("sim-display", sc.Claim.PreferredPlugin);
        Assert.Equal("user", sc.Claim.PreferredSource);
        Assert.Equal("next_session", sc.Claim.Apply);
    }
}

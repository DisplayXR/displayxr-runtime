// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Linq;
using DisplayXR.Dashboard.Model;
using Xunit;
using G = DisplayXR.Dashboard.Model.DesktopMapGeometry;

namespace DisplayXR.Dashboard.Tests;

public class DesktopMapTests
{
    private static readonly G.ScreenIn[] TwoMonitors =
    {
        new("A", "DISPLAY1", "", new PixelRect(0, 0, 3840, 2160), Level.Ok),
        new("B", "DISPLAY5", "", new PixelRect(3840, 0, 3840, 2160), Level.Ok),
    };

    [Fact]
    public void TwoMonitors_FitToScaleAndCentre()
    {
        // 7680 x 2160 into 800 x 400 with 10 px padding: width-bound, scale 780/7680.
        var m = G.Compute(TwoMonitors, System.Array.Empty<G.WindowIn>(), 800, 400, 10);
        double scale = 780.0 / 7680;
        Assert.Equal(scale, m.Scale, 9);
        Assert.Equal(new RectD(10, 10 + (380 - 2160 * scale) / 2, 3840 * scale, 2160 * scale), m.Screens[0].Rect);
        Assert.Equal(10 + 3840 * scale, m.Screens[1].Rect.X, 9);
        Assert.Equal(m.Screens[0].Rect.Right, m.Screens[1].Rect.X, 9); // adjacent stays adjacent
        Assert.Empty(m.Seams);
        Assert.All(m.Screens, s => Assert.False(s.IsOwner));
    }

    [Fact]
    public void StraddlingWindow_HasOneSeamAtTheSharedEdge()
    {
        var win = new G.WindowIn(3, "cube", new PixelRect(3018, 285, 1664, 1400), "B", true);
        var m = G.Compute(TwoMonitors, new[] { win }, 800, 400, 10);
        var seam = Assert.Single(m.Seams);
        double x = m.OffsetX + 3840 * m.Scale;
        Assert.Equal(x, seam.X1, 9);
        Assert.Equal(x, seam.X2, 9);
        Assert.Equal(m.OffsetY + 285 * m.Scale, seam.Y1, 9);
        Assert.Equal(m.OffsetY + 1685 * m.Scale, seam.Y2, 9);
        var w = Assert.Single(m.Windows);
        Assert.Equal(m.OffsetX + 3018 * m.Scale, w.Rect.X, 9);
        Assert.Equal(1664 * m.Scale, w.Rect.W, 9);
        Assert.True(m.Screens.Single(s => s.Id == "B").IsOwner);
        Assert.False(m.Screens.Single(s => s.Id == "A").IsOwner);
    }

    [Fact]
    public void WindowInsideOneMonitor_HasNoSeam()
    {
        var win = new G.WindowIn(3, "cube", new PixelRect(100, 100, 800, 600), "A", false);
        Assert.Empty(G.Compute(TwoMonitors, new[] { win }, 800, 400, 10).Seams);
    }

    [Fact]
    public void StackedMonitors_GiveAHorizontalSeam()
    {
        var stacked = new G.ScreenIn[]
        {
            new("A", "1", "", new PixelRect(0, 0, 1920, 1080), Level.Ok),
            new("B", "2", "", new PixelRect(0, 1080, 1920, 1080), Level.Ok),
        };
        var m = G.Compute(stacked, new[] { new G.WindowIn(1, "w", new PixelRect(100, 900, 400, 400), "A", true) }, 400, 400, 0);
        var seam = Assert.Single(m.Seams);
        Assert.Equal(seam.Y1, seam.Y2);
        Assert.Equal(m.OffsetY + 1080 * m.Scale, seam.Y1, 9);
    }

    [Fact]
    public void NegativeOrigin_IsHandled()
    {
        var left = new G.ScreenIn[]
        {
            new("L", "2", "", new PixelRect(-1920, 0, 1920, 1080), Level.Ok),
            new("M", "1", "", new PixelRect(0, 0, 3840, 2160), Level.Ok),
        };
        var m = G.Compute(left, System.Array.Empty<G.WindowIn>(), 600, 300, 0);
        Assert.Equal(-1920, m.DesktopLeft);
        Assert.True(m.Screens.All(s => s.Rect.X >= -1e-9 && s.Rect.Right <= 600 + 1e-9));
    }

    [Fact]
    public void Degenerate_InputsGiveAnEmptyLayout()
    {
        Assert.Same(MapLayout.Empty, G.Compute(System.Array.Empty<G.ScreenIn>(), System.Array.Empty<G.WindowIn>(), 800, 400, 10));
        Assert.Same(MapLayout.Empty, G.Compute(TwoMonitors, System.Array.Empty<G.WindowIn>(), 0, 400, 10));
        Assert.Same(MapLayout.Empty, G.Compute(TwoMonitors, System.Array.Empty<G.WindowIn>(), double.NaN, 400, 10));
        var zero = new[] { new G.ScreenIn("Z", "", "", new PixelRect(0, 0, 0, 0), Level.Plain) };
        Assert.Same(MapLayout.Empty, G.Compute(zero, System.Array.Empty<G.WindowIn>(), 800, 400, 10));
    }

    [Fact]
    public void FromSnapshot_SkipsDiagClientsAndWindowlessOnes()
    {
        var s = TestData.Parse(TestData.Lines("two-panels.ndjson")[0]);
        var (screens, windows) = G.From(s);
        Assert.Equal(2, screens.Count);
        Assert.Equal("DISPLAY1", screens[0].Label);
        var w = Assert.Single(windows);
        Assert.Equal("cube_handle_d3d11_win.exe", w.Label);

        var stress = TestData.Parse(TestData.Lines("stress-16x32.ndjson")[0]);
        var (s16, w32) = G.From(stress);
        var m = G.Compute(s16, w32, 1000, 600, 12);
        Assert.Equal(16, m.Screens.Count);
        Assert.Equal(32, m.Windows.Count);
        Assert.Equal(0.5625, G.Aspect(s16), 6);
    }
}

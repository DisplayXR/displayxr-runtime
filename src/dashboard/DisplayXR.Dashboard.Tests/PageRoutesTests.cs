// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Linq;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>Every page id a Dashboard ever answered to still lands where its content is now.</summary>
public class PageRoutesTests
{
    [Theory]
    [InlineData("home", "home", null)]
    [InlineData("displays", "displays", null)]
    [InlineData("components", "components", null)]
    [InlineData("catalog", "components", null)]                 // the catalog tab was merged into Components
    [InlineData("settings", "settings", null)]
    [InlineData("performance", "settings", null)]               // Performance became Settings
    [InlineData("troubleshooting", "troubleshooting", null)]    // the part last shown (Running 3D apps at first)
    [InlineData("windows", "troubleshooting", "apps")]           // the page shown as "Running 3D apps"
    [InlineData("developer", "troubleshooting", "advanced")]     // the Developer page is Advanced
    [InlineData("troubleshooting/apps", "troubleshooting", "apps")]
    [InlineData("troubleshooting/graphics", "troubleshooting", "graphics")]
    [InlineData("troubleshooting/advanced", "troubleshooting", "advanced")]
    [InlineData("troubleshooting/nonsense", "troubleshooting", null)]
    [InlineData("Settings", "settings", null)]
    [InlineData(" Developer ", "troubleshooting", "advanced")]
    [InlineData("no-such-page", "no-such-page", null)]           // the shell then opens Home
    public void OldAndNewIds_LandOnTheRightPlace(string id, string page, string? section)
    {
        Assert.Equal(new Route(page, section), PageRoutes.Route(id));
        Assert.Equal(page, PageRoutes.Resolve(id));
    }

    [Fact]
    public void TheRail_HasFiveEntries_TroubleshootingLast()
    {
        Assert.Equal(new[] { "home", "displays", "components", "settings", "troubleshooting" }, PageRoutes.Rail);
        Assert.Equal(new[] { "apps", "graphics", "advanced" }, PageRoutes.TroubleshootingSections);
    }

    [Fact]
    public void EveryOldId_LandsOnARailPage()
    {
        foreach (var id in PageRoutes.LegacyIds.Concat(new[] { "settings", "troubleshooting" }))
            Assert.Contains(PageRoutes.Resolve(id), PageRoutes.Rail);
        Assert.Null(PageRoutes.Route("troubleshooting").Section);
        Assert.All(PageRoutes.TroubleshootingSections, s => Assert.Equal(s, PageRoutes.Route("troubleshooting/" + s).Section));
    }
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

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
    [InlineData("catalog", "components", null)]       // the catalog tab was merged into Components
    [InlineData("settings", "settings", null)]
    [InlineData("performance", "settings", null)]     // Performance became Settings
    [InlineData("Settings", "settings", null)]
    [InlineData("no-such-page", "no-such-page", null)] // the shell then opens Home
    public void OldAndNewIds_LandOnTheRightPlace(string id, string page, string? section)
    {
        Assert.Equal(new Route(page, section), PageRoutes.Route(id));
        Assert.Equal(page, PageRoutes.Resolve(id));
    }
}

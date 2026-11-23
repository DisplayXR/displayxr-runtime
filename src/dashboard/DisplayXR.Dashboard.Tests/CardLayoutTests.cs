// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using DisplayXR.Dashboard.Ui;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>The page grid (U.CardGrid): no row ends in an empty half.</summary>
public class CardLayoutTests
{
    [Fact]
    public void ACardAloneInItsRow_TakesTheWholeRow()
    {
        // Components' order: full, half, half, half, full. The third half card would sit beside nothing.
        var cells = U.CardGrid.Layout(new[] { 2, 1, 1, 1, 2 }, 2);
        Assert.Equal(new[] { (0, 0, 2), (1, 0, 1), (1, 1, 1), (2, 0, 2), (3, 0, 2) }, cells);
    }

    [Fact]
    public void TheLastCard_FillsItsRow_AndPairsAreLeftAsTheyAre()
    {
        Assert.Equal(new[] { (0, 0, 1), (0, 1, 1), (1, 0, 2) }, U.CardGrid.Layout(new[] { 1, 1, 1 }, 2));
        Assert.Equal(new[] { (0, 0, 1), (0, 1, 1) }, U.CardGrid.Layout(new[] { 1, 1 }, 2));
    }

    [Fact]
    public void OneColumn_IsOneCardPerRow()
    {
        Assert.Equal(new[] { (0, 0, 1), (1, 0, 1), (2, 0, 1) }, U.CardGrid.Layout(new[] { 2, 1, 1 }, 1));
    }
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Linq;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>The per-screen 3D mode picker: what is read from <c>screens[i].mode_3d</c> and what the Displays page shows of it.</summary>
public class Mode3DTests
{
    private static StatusSnapshot Snap() => TestData.Parse(TestData.Text("status-mode-3d.json"));

    private static Screen Screen(string key) => Snap().Screens.Single(s => s.Key == key);

    private const string RestartReason = "declared only at start-up: restart the app to apply";

    [Fact]
    public void Null_AndAMissingKey_ReadAsNothingToChoose()
    {
        Assert.Null(Screen("LEI-0003-C").Mode3D); // "mode_3d": null
        Assert.Null(Screen("OLD-0009-I").Mode3D); // a feed that predates it
        Assert.False(StatusText.Mode3DPickerShown(Screen("LEI-0003-C")));
        Assert.False(StatusText.Mode3DPickerShown(Screen("OLD-0009-I")));
    }

    [Fact]
    public void DefaultState_IsRead()
    {
        var m = Screen("SIM-0001-A").Mode3D!;
        Assert.Equal(("same_as_2d", null, "driver_default"), (m.Active, m.Setting, m.Source));
        Assert.Equal(new[] { "same_as_2d", "anaglyph", "quad" }, m.Choices.Select(c => c.Name));
        Assert.Equal(new[] { "Same as 2D", "Anaglyph", "Quad" }, m.Choices.Select(c => c.Label)); // the driver's words, as given
        Assert.Equal(new Mode3DChoice("same_as_2d", "Same as 2D", 1, false, true, "now", null), m.Choices[0]);
        Assert.Equal(new Mode3DChoice("quad", "Quad", 4, false, false, "restart_app", RestartReason), m.Choices[2]);
        Assert.True(m.Choices[2].NeedsRestart);
        Assert.False(m.Choices[2].NotSupported);

        Assert.Equal(0, StatusText.Mode3DSelected(m));
        Assert.False(StatusText.Mode3DCanReset(m));
        Assert.Null(StatusText.Mode3DSourceText(m));
        Assert.Empty(StatusText.Mode3DNotes(m)); // a restart entry says nothing until it is the selected one
    }

    [Fact]
    public void UserChoice_IsRead_AndOffersReset()
    {
        var m = Screen("SIM-0002-B").Mode3D!;
        Assert.Equal(("anaglyph", "anaglyph", "user"), (m.Active, m.Setting, m.Source));
        Assert.Equal(1, StatusText.Mode3DSelected(m));
        Assert.True(StatusText.Mode3DCanReset(m));
        Assert.Equal("Chosen by you", StatusText.Mode3DSourceText(m));
        Assert.Empty(StatusText.Mode3DNotes(m));
    }

    [Fact]
    public void MachineChoice_SaysItIsForEveryone_AndOffersNoPerUserReset()
    {
        var m = Screen("SIM-0005-E").Mode3D!;
        Assert.Equal("machine", m.Source);
        Assert.StartsWith("Chosen for everyone on this PC.", StatusText.Mode3DSourceText(m));
        Assert.False(StatusText.Mode3DCanReset(m)); // the per-user verb does not clear it
    }

    [Fact]
    public void NotSupportedEntry_IsLeftOut_AndItsReasonIsShown()
    {
        var m = Screen("SIM-0005-E").Mode3D!;
        Assert.True(m.Find("interlaced")!.NotSupported);
        Assert.Equal(new[] { "Same as 2D", "Side by side" }, StatusText.Mode3DItems(m).Select(c => c.Label));
        Assert.Equal(1, StatusText.Mode3DSelected(m)); // an index into the listed entries, not into choices[]
        Assert.Equal(new[] { "Not available on this display: Interlaced (this display is rotated)." }, StatusText.Mode3DNotes(m));

        // Were it the mode in use all the same, it stays listed so the picker shows what is on.
        var inUse = m with { Active = "interlaced" };
        Assert.Equal(3, StatusText.Mode3DItems(inUse).Count);
        Assert.Equal(1, StatusText.Mode3DSelected(inUse));
        Assert.Empty(StatusText.Mode3DNotes(inUse));
    }

    [Fact]
    public void RestartEntry_ShowsItsReason_WhenSelected_OrWhenStoredAndNotInUse()
    {
        var selected = Screen("SIM-0007-G").Mode3D!;
        Assert.Equal(1, StatusText.Mode3DSelected(selected));
        Assert.Equal(new[] { "Declared only at start-up: restart the app to apply." }, StatusText.Mode3DNotes(selected));

        var stored = Screen("SIM-0006-F").Mode3D!;
        Assert.Equal(("anaglyph", "quad"), (stored.Active, stored.Setting));
        Assert.Equal(0, StatusText.Mode3DSelected(stored)); // the mode in use, not the stored one
        Assert.Equal(new[] { $"Quad is chosen but not in use: {RestartReason}." }, StatusText.Mode3DNotes(stored));
        Assert.Equal(2, StatusText.Mode3DItems(stored).Count); // a restart entry can be picked
    }

    [Fact]
    public void Picker_IsShown_OnlyWithAtLeastTwoChoices()
    {
        Assert.False(StatusText.Mode3DPickerShown(Screen("LEI-0003-C"))); // null
        Assert.False(StatusText.Mode3DPickerShown(Screen("LEI-0004-D"))); // one choice
        Assert.False(StatusText.Mode3DPickerShown(Screen("SIM-0008-H"))); // a stored choice, no modes reported
        Assert.True(StatusText.Mode3DPickerShown(Screen("SIM-0002-B")));  // two
        Assert.True(StatusText.Mode3DPickerShown(Screen("SIM-0001-A")));  // three
        // Without a screen key there is nothing to address the verb to.
        Assert.False(StatusText.Mode3DPickerShown(Snap().Screens.Single(s => s.Key is null)));
    }

    [Fact]
    public void Verbs()
    {
        var m = Screen("SIM-0001-A").Mode3D!;
        Assert.Equal("dp mode set anaglyph --screen SIM-0001-A", StatusText.Mode3DVerb("SIM-0001-A", m.Choices[1]).ToString());
        Assert.Equal(new[] { "dp", "mode", "set", "quad", "--screen", "SIM-0001-A" }, StatusText.Mode3DVerb("SIM-0001-A", m.Choices[2]).Args);
        Assert.Equal("dp mode reset --screen SIM-0001-A", StatusText.Mode3DResetVerb("SIM-0001-A").ToString());
    }

    [Fact]
    public void Parsing_ToleratesMissingFields()
    {
        var bare = Snap().Screens.Single(s => s.Key is null).Mode3D!;
        Assert.Equal(("a", null, "driver_default"), (bare.Active, bare.Setting, bare.Source));
        // No label: the name; no applies: "now"; an entry without a name, or one that is not an object, is dropped.
        Assert.Equal(new[] { new Mode3DChoice("a", "a", 0, false, false, "now", null), new Mode3DChoice("b", "B", 0, false, false, "now", null) },
                     bare.Choices);

        var empty = Screen("SIM-0008-H").Mode3D!;
        Assert.Equal((null, "anaglyph", "user"), (empty.Active, empty.Setting, empty.Source));
        Assert.Empty(empty.Choices);
        Assert.Equal(-1, StatusText.Mode3DSelected(empty));

        var odd = TestData.Parse("""{ "schema": 1, "screens": [ { "id": "0x1", "key": "K", "mode_3d": { "choices": "no" } }, { "id": "0x2", "key": "L", "mode_3d": 3 } ] }""");
        Assert.Equal((null, null, "runtime_default", 0), (odd.Screens[0].Mode3D!.Active, odd.Screens[0].Mode3D!.Setting,
                                                          odd.Screens[0].Mode3D!.Source, odd.Screens[0].Mode3D!.Choices.Count));
        Assert.Null(odd.Screens[1].Mode3D);
    }

    [Fact]
    public void RebuildKey_FollowsWhatThePickerShows()
    {
        var a = Screen("SIM-0001-A").Mode3D!;
        Assert.Equal("-", StatusText.Mode3DKey(null));
        Assert.NotEqual(StatusText.Mode3DKey(a), StatusText.Mode3DKey(a with { Active = "anaglyph" }));
        Assert.NotEqual(StatusText.Mode3DKey(a), StatusText.Mode3DKey(a with { Source = "user" }));
        Assert.NotEqual(StatusText.Mode3DKey(a), StatusText.Mode3DKey(a with { Choices = a.Choices.Take(2).ToArray() }));
    }

    [Fact]
    public void OptionsBlock_LeavesOutTheDriversOwn3DModeSetting_WhereThePickerShows()
    {
        var snap = Snap();
        Assert.True(StatusText.DriverHasMode3DPicker(snap, "sim-display"));
        Assert.True(StatusText.DriverHasMode3DPicker(snap, "SIM-DISPLAY"));
        Assert.False(StatusText.DriverHasMode3DPicker(snap, "leia-sr")); // its screens show no picker
        Assert.False(StatusText.DriverHasMode3DPicker(null, "sim-display"));

        Assert.True(DriverOptions.TryParse(TestData.Text("dp-options-sim-display.json"), out var options));
        var kept = OptionViews.Build(FriendlyOptions.SimDisplay, options!);
        Assert.Contains(kept.Friendly, v => v.Key == "OUTPUT");
        var hidden = OptionViews.Build(FriendlyOptions.SimDisplay, options!, modeOnDisplays: true);
        Assert.DoesNotContain(hidden.Friendly.Concat(hidden.More), v => v.Key == "OUTPUT");
        Assert.Equal(kept.Friendly.Count - 1, hidden.Friendly.Count); // nothing else goes
        Assert.Equal(kept.More.Select(v => v.Key), hidden.More.Select(v => v.Key));
        // Only the setting marked as the 3D mode: another driver's block is the same either way.
        Assert.Equal("OUTPUT", FriendlyOptions.Table.Single(f => f.Is3DMode).Key);
        Assert.True(DriverOptions.TryParse(TestData.Text("dp-options-leia-sr.json"), out var leia));
        Assert.Equal(OptionViews.Build("leia-sr", leia!).Friendly.Select(v => v.Key),
                     OptionViews.Build("leia-sr", leia!, modeOnDisplays: true).Friendly.Select(v => v.Key));
    }
}

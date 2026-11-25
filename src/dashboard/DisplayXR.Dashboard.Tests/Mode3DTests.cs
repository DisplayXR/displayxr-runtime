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
        Assert.Null(m.Reason);
        Assert.Empty(StatusText.Mode3DNotes(m)); // a mode that waits for an app start says nothing until it is stored
        // In the list it can be picked, and says in plain words when it applies.
        Assert.Equal(new[] { "Same as 2D", "Anaglyph", "Quad (applies the next time an app starts)" },
                     StatusText.Mode3DItems(m).Select(c => StatusText.Mode3DItemText(m, c)));
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
        Assert.Equal(new[] { "Not available on this display: Interlaced (needs 16 views; at most 8 can be presented)." },
                     StatusText.Mode3DNotes(m));
        Assert.DoesNotContain(StatusText.Mode3DItems(m), c => c.NotSupported); // it cannot be picked

        // Were it the mode in use all the same, it stays listed so the picker shows what is on.
        var inUse = m with { Active = "interlaced", Setting = "interlaced" };
        Assert.Equal(3, StatusText.Mode3DItems(inUse).Count);
        Assert.Equal(1, StatusText.Mode3DSelected(inUse));
        Assert.Equal(new[] { "Needs 16 views; at most 8 can be presented." }, StatusText.Mode3DNotes(inUse));
    }

    [Fact]
    public void ModeForTheNextAppStart_CanBePicked_IsStored_AndSaysWhenItApplies()
    {
        // `dp mode set quad` stores it (exit 0): setting = the stored name, active = what is drawn now.
        var stored = Screen("SIM-0006-F").Mode3D!;
        Assert.Equal(("anaglyph", "quad", "user"), (stored.Active, stored.Setting, stored.Source));
        var items = StatusText.Mode3DItems(stored);
        Assert.Equal(2, items.Count); // listed, so it can be picked
        Assert.Equal(1, StatusText.Mode3DSelected(stored)); // the picker keeps what was picked
        Assert.Equal("Quad (applies the next time an app starts)", StatusText.Mode3DItemText(stored, items[1]));
        Assert.Equal(new[] { "Quad is stored. It applies the next time an app starts. Apps that are running keep Anaglyph." },
                     StatusText.Mode3DNotes(stored));
        Assert.True(StatusText.Mode3DCanReset(stored));
        Assert.Equal("dp mode set quad --screen SIM-0006-F", StatusText.Mode3DVerb("SIM-0006-F", items[1]).ToString());

        // Once it is the mode drawn (after the restart), the list shows the label alone and the runtime's note.
        var inUse = Screen("SIM-0007-G").Mode3D!;
        Assert.Equal(1, StatusText.Mode3DSelected(inUse));
        Assert.Equal("Quad", StatusText.Mode3DItemText(inUse, StatusText.Mode3DItems(inUse)[1]));
        Assert.Equal(new[] { "Declared only at start-up: restart the app to apply." }, StatusText.Mode3DNotes(inUse));

        // The mode drawn now is unknown: the sentence about running apps is left out.
        Assert.Equal(new[] { "Quad is stored. It applies the next time an app starts." },
                     StatusText.Mode3DNotes(stored with { Active = null }));
    }

    [Fact]
    public void MachineChoice_ThatWaitsForAnAppStart_OffersNoReset()
    {
        var m = Screen("SIM-000E-N").Mode3D!;
        Assert.Equal(("anaglyph", "quad", "machine"), (m.Active, m.Setting, m.Source));
        Assert.False(StatusText.Mode3DCanReset(m));
        Assert.StartsWith("Chosen for everyone on this PC.", StatusText.Mode3DSourceText(m));
        Assert.Equal(1, StatusText.Mode3DSelected(m));
        Assert.Single(StatusText.Mode3DNotes(m));
    }

    [Fact]
    public void StoredChoice_WithNoModeTable_KeepsTheRow_WithTheChoiceAndTheReason()
    {
        var s = Screen("SIM-0008-H");
        var m = s.Mode3D!;
        Assert.Equal((null, "anaglyph", "user"), (m.Active, m.Setting, m.Source));
        Assert.Empty(m.Choices);
        Assert.Equal("this display's driver declares no 3D modes now", m.Reason);
        Assert.False(StatusText.Mode3DPickerShown(s)); // nothing to pick from
        Assert.True(StatusText.Mode3DStoredOnly(s));
        Assert.True(StatusText.Mode3DRowShown(s));     // the row is not hidden
        Assert.Equal("anaglyph", StatusText.Mode3DStoredText(m)); // no label is known: the stored name
        Assert.Equal(new[] { "This display's driver declares no 3D modes now." }, StatusText.Mode3DNotes(m));
        Assert.Equal("Chosen by you", StatusText.Mode3DSourceText(m));
        Assert.True(StatusText.Mode3DCanReset(m)); // `dp mode reset` needs no mode table
        Assert.Equal(-1, StatusText.Mode3DSelected(m));

        // The same, stored for everyone on the PC: shown, no reset.
        var everyone = Screen("SIM-000B-K");
        Assert.True(StatusText.Mode3DRowShown(everyone));
        Assert.False(StatusText.Mode3DCanReset(everyone.Mode3D!));
        Assert.Equal("Chosen for everyone on this PC.", StatusText.Mode3DSourceText(everyone.Mode3D!));

        // It does not make the driver's own setting leave the Options block: there is no picker.
        Assert.NotEqual(StatusText.Mode3DKey(m), StatusText.Mode3DKey(m with { Reason = null }));
    }

    [Fact]
    public void Row_IsShown_ForAPicker_OrAStoredChoiceAlone_AndNeverForNull()
    {
        Assert.False(StatusText.Mode3DRowShown(Screen("LEI-0003-C"))); // "mode_3d": null
        Assert.False(StatusText.Mode3DRowShown(Screen("OLD-0009-I"))); // the key is missing
        Assert.False(StatusText.Mode3DRowShown(Screen("LEI-0004-D"))); // one mode, nothing stored
        Assert.True(StatusText.Mode3DRowShown(Screen("SIM-0001-A")));
        Assert.True(StatusText.Mode3DRowShown(Screen("SIM-0008-H")));
        // No modes and nothing stored (the runtime writes null for it; an object is tolerated): no row.
        var none = TestData.Parse("""{ "schema": 1, "screens": [ { "id": "0x1", "key": "K", "mode_3d": { "active": null, "setting": null, "source": "runtime_default", "reason": null, "choices": [] } } ] }""");
        Assert.False(StatusText.Mode3DRowShown(none.Screens[0]));
    }

    [Fact]
    public void LiveMode_WithANote_ShowsIt_WhenItIsTheModeDrawn()
    {
        var m = Screen("SIM-000C-L").Mode3D!;
        Assert.False(m.Find("quad")!.NeedsRestart);
        Assert.Equal("Quad", StatusText.Mode3DItemText(m, m.Find("quad")!));
        Assert.Equal(new[] { "4 views: stereo apps show 2D." }, StatusText.Mode3DNotes(m));
        Assert.Empty(StatusText.Mode3DNotes(m with { Active = "anaglyph", Setting = null })); // not the mode drawn: no note
    }

    [Fact]
    public void StoredChoice_ThatIsNotUsed_IsNamed()
    {
        // A stored name the driver does not declare: the source stays a default, the mode drawn stays selected.
        var unknown = Screen("SIM-000D-M").Mode3D!;
        Assert.Equal(("anaglyph", "lenticular", "driver_default"), (unknown.Active, unknown.Setting, unknown.Source));
        Assert.Equal(1, StatusText.Mode3DSelected(unknown));
        Assert.Equal(new[] { "The stored choice \"lenticular\" is not one of this display's 3D modes and is not used." },
                     StatusText.Mode3DNotes(unknown));

        // A stored mode this display cannot show: not listed, named once with its reason.
        var cannot = Screen("SIM-0005-E").Mode3D! with { Active = "same_as_2d", Setting = "interlaced" };
        Assert.Equal(0, StatusText.Mode3DSelected(cannot));
        Assert.Equal(new[] { "Interlaced is stored, but this display cannot show it: needs 16 views; at most 8 can be presented." },
                     StatusText.Mode3DNotes(cannot));
    }

    [Fact]
    public void Picker_IsShown_OnlyWithAtLeastTwoChoices()
    {
        Assert.False(StatusText.Mode3DPickerShown(Screen("LEI-0003-C"))); // null
        Assert.False(StatusText.Mode3DPickerShown(Screen("LEI-0004-D"))); // one choice
        Assert.False(StatusText.Mode3DPickerShown(Screen("SIM-0008-H"))); // a stored choice, no modes reported: a row, no picker
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

        Assert.Null(bare.Reason); // a feed without the top-level reason

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
        // A stored choice with no picker does not count: the driver's own setting stays in its block.
        Assert.DoesNotContain(snap.Screens, s => StatusText.Mode3DStoredOnly(s) && StatusText.Mode3DPickerShown(s));

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

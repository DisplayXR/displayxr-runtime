// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Linq;
using System.Threading.Tasks;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>
/// A display driver's settings (ADR-065), from <c>dp options &lt;id&gt; --json</c>: the parse, the
/// friendly table, the words, the commands. Fixtures: <c>fixtures/dp-options-*.json</c>, written by
/// hand in the CLI's shape (cli_component_options.c), with one row of each kind the parse must handle.
/// </summary>
public class DriverOptionsTests
{
    private static DriverOptions Sim()
    {
        Assert.True(DriverOptions.TryParse(TestData.Text("dp-options-sim-display.json"), out var o));
        return o!;
    }

    private static OptionsBlock SimBlock() => OptionViews.Build(FriendlyOptions.SimDisplay, Sim());

    [Fact]
    public void Parse_KeepsDeclaredKnownTypes_DropsUndeclaredAndUnknown()
    {
        var o = Sim();
        Assert.Equal("sim-display", o.Id);
        Assert.Equal("DXR_DP_SIM_DISPLAY_", o.EnvPrefix);
        Assert.Equal(new[] { "TRACKING", "TRACKING_PERIOD_MS", "OUTPUT", "START_3D", "FORCE_MODE", "WIDTH_M", "WEAVE_SCOPE",
                             "STEREO_CAMERA_SIZE", "LICENCE_KEY" }, o.Settings.Select(s => s.Key));
        Assert.Null(o.Find("FUTURE_THING"));      // a type this Dashboard does not know
        Assert.Null(o.Find("TRACKING_TURN_DEG")); // stored, not declared (type null)

        var output = o.Find("OUTPUT")!;
        Assert.Equal(SettingType.Enum, output.Type);
        Assert.Equal("quad", output.Value);
        Assert.Equal(SettingSource.User, output.Source);
        var start = o.Find("START_3D")!;
        Assert.Null(start.Value);
        Assert.Equal("0", start.Effective); // the default stands
        Assert.Equal(SettingSource.Absent, start.Source);

        var secret = o.Find("LICENCE_KEY")!;
        Assert.Equal(SettingType.Secret, secret.Type);
        Assert.Null(secret.Value);
        Assert.True(secret.IsSet);
    }

    [Theory]
    [InlineData("")]
    [InlineData("not json")]
    [InlineData("[]")]
    [InlineData("{\"id\":\"x\"}")]
    [InlineData("{\"settings\":5}")]
    public void ANonDocument_IsNoOptions(string json)
    {
        Assert.False(DriverOptions.TryParse(json, out _));
        var o = DriverOptions.From("x", ok: true, json);
        Assert.True(o.IsEmpty);
        Assert.True(OptionViews.Build("x", o).IsEmpty);
    }

    [Fact]
    public void AFailingVerb_IsNoOptions_NotAnError()
    {
        // `dp options nosuch --json` exits 1 with a FAIL line; an older CLI has no verb at all.
        var r = new CliResult(1, "FAIL: 'nosuch' is not a registered display processor (see 'dp list') — nothing to show.", "", false, null);
        Assert.True(DriverOptions.From("nosuch", r.Ok, r.Json).IsEmpty);
        var empty = DriverOptions.From("leia-sr", true, """{"id":"leia-sr","settings":[]}""");
        Assert.True(empty.IsEmpty);
        // A log line printed around the document does not break it.
        var noisy = new CliResult(0, " WARN [x] loading\n" + TestData.Text("dp-options-leia-sr.json") + "\n", "", false, null);
        Assert.Equal("MULTI_VIEWER", Assert.Single(DriverOptions.From("leia-sr", noisy.Ok, noisy.Json).Settings).Key);
    }

    [Fact]
    public void FriendlyTable_IsData_OrderAndLabels()
    {
        var f = FriendlyOptions.Table;
        Assert.Equal(new[] { "OUTPUT", "START_3D", "FORCE_MODE", "MULTI_VIEWER" }, f.OrderBy(x => x.Order).Select(x => x.Key));
        Assert.Equal(new[] { "3D preview kind", "Start in", "Always 2D", "Several viewers" }, f.OrderBy(x => x.Order).Select(x => x.Label));

        var output = FriendlyOptions.Find("sim-display", "OUTPUT")!;
        Assert.Equal(new[] { "Red / cyan (anaglyph)", "Side by side", "Side by side, squeezed", "Four views (2x2)", "Interlaced", "Blend" },
                     output.Choices.Select(c => c.Label));
        Assert.DoesNotContain(output.Choices, c => c.Value is "2d" or "passthrough"); // covered by Start in / Always 2D
        Assert.Equal(new[] { "2D", "3D preview" }, FriendlyOptions.Find("sim-display", "START_3D")!.Choices.Select(c => c.Label));
        // The simulated display's keys are its own; MULTI_VIEWER is friendly on any driver that declares it.
        Assert.Null(FriendlyOptions.Find("another-driver", "OUTPUT"));
        var mv = FriendlyOptions.Find("another-driver", "MULTI_VIEWER")!;
        Assert.Equal(new[] { ("system", "Follow the display's own setting"), ("on", "On"), ("off", "Off") },
                     mv.Choices.Select(c => (c.Value, c.Label)));
    }

    [Fact]
    public void Block_FriendlyFirst_InTableOrder_RestByKey()
    {
        var b = SimBlock();
        Assert.Equal(new[] { "3D preview kind", "Start in", "Always 2D" }, b.Friendly.Select(v => v.Label));
        Assert.All(b.Friendly, v => Assert.True(v.Friendly));
        Assert.Equal(new[] { "TRACKING", "TRACKING_PERIOD_MS", "WIDTH_M", "WEAVE_SCOPE", "STEREO_CAMERA_SIZE", "LICENCE_KEY" },
                     b.More.Select(v => v.Label)); // the raw key is the label
        Assert.All(b.More, v => Assert.False(v.Friendly));
    }

    [Fact]
    public void Output_FourViewKind_CarriesItsNote()
    {
        var v = SimBlock().Friendly[0];
        Assert.Equal(OptionControl.Choice, v.Control);
        Assert.Equal("quad", v.Selected);
        Assert.Equal("Four views (2x2)", v.ValueText);
        Assert.Equal("Your choice", v.SourceText);
        Assert.True(v.CanReset);
        Assert.Equal("For checking four-view apps. Most apps show only two of the views, or stay in 2D.", v.Note);

        // Another kind: no note. An alias the driver accepts selects its offered value.
        var s = Sim().Find("OUTPUT")!;
        var entry = FriendlyOptions.Find("sim-display", "OUTPUT");
        Assert.Null(OptionViews.View(s with { Value = "sbs" }, entry).Note);
        Assert.Equal("squeezed", OptionViews.View(s with { Value = "squeezed_sbs" }, entry).Selected);
        // `2d` is not offered: nothing selected, the stored value quoted.
        var flat = OptionViews.View(s with { Value = "2d" }, entry);
        Assert.Null(flat.Selected);
        Assert.Equal("“2d”", flat.ValueText);
        // Unset: the default kind, "Default", nothing to reset.
        var dflt = OptionViews.View(s with { Value = null, Source = SettingSource.Absent }, entry);
        Assert.Equal("anaglyph", dflt.Selected);
        Assert.Equal("Default", dflt.SourceText);
        Assert.False(dflt.CanReset);
    }

    [Fact]
    public void StartIn_IsATwoWayChoice()
    {
        var v = SimBlock().Friendly[1];
        Assert.Equal(OptionControl.TwoWay, v.Control);
        Assert.Equal("0", v.Selected);
        Assert.Equal("2D", v.ValueText);
        Assert.Equal("Default", v.SourceText);
        var on = OptionViews.View(Sim().Find("START_3D")! with { Value = "1", Source = SettingSource.User }, FriendlyOptions.Find("sim-display", "START_3D"));
        Assert.Equal("3D preview", on.ValueText);
        Assert.Equal("1", on.Selected);
    }

    [Fact]
    public void Always2D_IsAPin_AndAStored1IsItsOwnState()
    {
        var s = Sim().Find("FORCE_MODE")!;
        var entry = FriendlyOptions.Find("sim-display", "FORCE_MODE");
        var three = SimBlock().Friendly[2];
        Assert.Equal(OptionControl.Pin, three.Control);
        Assert.False(three.On);
        Assert.Equal("Always 3D preview", three.State);
        Assert.Equal("Always 3D preview", three.ValueText);
        Assert.True(three.CanReset);

        var pinned = OptionViews.View(s with { Value = "0" }, entry);
        Assert.True(pinned.On);
        Assert.Null(pinned.State);
        var off = OptionViews.View(s with { Value = null, Source = SettingSource.Absent }, entry);
        Assert.False(off.On);
        Assert.Equal("Off", off.ValueText);

        // On writes 0; off clears this user's value, or writes the default over a machine-wide one.
        Assert.Equal("0", OptionViews.PinWrite(off, on: true));
        Assert.Null(OptionViews.PinWrite(pinned, on: false));
        Assert.Equal("-1", OptionViews.PinWrite(OptionViews.View(s with { Value = "0", Source = SettingSource.Machine }, entry), on: false));
    }

    [Fact]
    public void SeveralViewers_OnAVendorDriver()
    {
        Assert.True(DriverOptions.TryParse(TestData.Text("dp-options-leia-sr.json"), out var o));
        var b = OptionViews.Build("leia-sr", o!);
        var v = Assert.Single(b.Friendly);
        Assert.Empty(b.More);
        Assert.Equal("Several viewers", v.Label);
        Assert.Equal("system", v.Selected);
        Assert.Equal("Follow the display's own setting", v.ValueText);
    }

    [Fact]
    public void SourceWords_AndTheEnvironmentIsReadOnly()
    {
        Assert.Equal("Default", OptionViews.SourceText(SettingSource.Absent));
        Assert.Equal("Your choice", OptionViews.SourceText(SettingSource.User));
        Assert.Equal("Set for everyone on this PC", OptionViews.SourceText(SettingSource.Machine));
        Assert.Equal("Set for everyone on this PC", OptionViews.SourceText(SettingSource.Manifest));
        Assert.Equal("Set outside the Dashboard", OptionViews.SourceText(SettingSource.Environment));

        var b = SimBlock();
        var width = b.More.Single(v => v.Key == "WIDTH_M");
        Assert.True(width.ReadOnly);
        Assert.False(width.CanReset);
        Assert.Equal("0.5", width.ValueText);
        Assert.Contains("DXR_DP_SIM_DISPLAY_WIDTH_M", width.Tooltip); // the variable is in the tooltip only

        var machine = b.More.Single(v => v.Key == "STEREO_CAMERA_SIZE");
        Assert.False(machine.ReadOnly);
        Assert.False(machine.CanReset); // `dp unset` clears the user's value, and there is none
        Assert.Equal("Set for everyone on this PC", machine.SourceText);
    }

    [Fact]
    public void RawControls_ByType_EnumWithoutValuesIsAText()
    {
        var b = SimBlock();
        Assert.Equal(OptionControl.Switch, b.More.Single(v => v.Key == "TRACKING").Control);
        Assert.True(b.More.Single(v => v.Key == "TRACKING").On);
        Assert.Equal(OptionControl.Text, b.More.Single(v => v.Key == "TRACKING_PERIOD_MS").Control);
        Assert.Equal(OptionControl.Text, b.More.Single(v => v.Key == "WEAVE_SCOPE").Control); // the CLI lists no enum values
        Assert.Equal("canvas", b.More.Single(v => v.Key == "WEAVE_SCOPE").ValueText);
        var secret = b.More.Single(v => v.Key == "LICENCE_KEY");
        Assert.Equal(OptionControl.Secret, secret.Control);
        Assert.Equal("Set", secret.ValueText);
        Assert.True(secret.CanReset);
        Assert.DoesNotContain(b.More.Concat(b.Friendly), v => v.Key == "LICENCE_KEY" && v.Friendly);
    }

    [Theory]
    [InlineData(SettingType.Int, "42", "42")]
    [InlineData(SettingType.Int, " -1 ", "-1")]
    [InlineData(SettingType.Int, "4.5", null)]
    [InlineData(SettingType.Float, "0,344", "0.344")]
    [InlineData(SettingType.Float, "1e400", null)]
    [InlineData(SettingType.String, "1280x720", "1280x720")]
    [InlineData(SettingType.String, "--machine", null)]
    [InlineData(SettingType.Enum, "", null)]
    [InlineData(SettingType.Bool, "true", "1")]
    [InlineData(SettingType.Bool, "maybe", null)]
    public void TextValues_AreCheckedBeforeTheyAreWritten(SettingType type, string text, string? written)
    {
        var v = OptionViews.Validate(type, text, out var error);
        Assert.Equal(written, v);
        Assert.Equal(written is null, error is not null);
    }

    [Fact]
    public void Commands_AreArgumentVectors()
    {
        Assert.Equal(new[] { "dp", "options", "sim-display", "--json" }, OptionCommands.Read("sim-display").Args);
        Assert.Equal(new[] { "dp", "set", "sim-display", "OUTPUT", "side by side" }, OptionCommands.Set("sim-display", "OUTPUT", "side by side").Args);
        Assert.Equal(new[] { "dp", "unset", "sim-display", "FORCE_MODE" }, OptionCommands.Unset("sim-display", "FORCE_MODE").Args);
    }

    /// <summary>
    /// The context reads a driver's settings lazily, writes through the shared action path, re-reads
    /// after a write, and an unknown driver is no options (the fixture source simulates the verbs from
    /// <c>dp-options-&lt;id&gt;.json</c>, so nothing here can reach a real store).
    /// </summary>
    [Fact]
    public async Task Context_ReadsLazily_WritesThroughTheCli_ThenReReads()
    {
        var fx = new FixtureProcessSource(TestData.Path("components.ndjson"), new FakeSource());
        var ctx = new DashboardContext(new StatusFeed(new FakeSource(), new ManualScheduler()), new CliProcessSource("no-such-cli"), fx);
        Assert.Null(ctx.DriverOptionsOf("sim-display")); // nothing read until asked

        await ctx.LoadDriverOptionsAsync("sim-display");
        Assert.Equal("quad", ctx.DriverOptionsOf("sim-display")!.Find("OUTPUT")!.Value);

        await ctx.RunOptionWriteAsync("sim-display", "OUTPUT", OptionCommands.Set("sim-display", "OUTPUT", "sbs"));
        Assert.Equal(("sim-display", "OUTPUT"), ctx.LastOptionWrite);
        Assert.Equal(OptionCommands.Area, ctx.LastActionArea);
        Assert.False(ctx.LastActionFailed);
        Assert.Contains("'sbs'", ctx.LastAction); // the CLI's own reply
        Assert.Equal("sbs", ctx.DriverOptionsOf("sim-display")!.Find("OUTPUT")!.Value);

        await ctx.RunOptionWriteAsync("sim-display", "OUTPUT", OptionCommands.Unset("sim-display", "OUTPUT"));
        var output = ctx.DriverOptionsOf("sim-display")!.Find("OUTPUT")!;
        Assert.Equal(SettingSource.Absent, output.Source);
        Assert.Equal("anaglyph", output.Effective);

        // A driver the CLI does not know (exit 1): no options, and no error kept anywhere.
        await ctx.LoadDriverOptionsAsync("nosuch");
        Assert.True(ctx.DriverOptionsOf("nosuch")!.IsEmpty);
        Assert.False(ctx.DriverOptionsLoading("nosuch"));
    }

    [Fact]
    public void AccessibleNames()
    {
        Assert.Equal("3D preview kind: DisplayXR Sim Display", OptionViews.AccessibleName("3D preview kind", "DisplayXR Sim Display"));
        Assert.Equal("Use the default for Always 2D: DisplayXR Sim Display", OptionViews.ResetName("Always 2D", "DisplayXR Sim Display"));
        Assert.Equal("More options: DisplayXR Sim Display", OptionViews.MoreName("DisplayXR Sim Display"));
    }
}

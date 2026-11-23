// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.IO;
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
    public void ANonDocument_IsAFailedRead_NotNoOptions(string json)
    {
        Assert.False(DriverOptions.TryParse(json, out _));
        Assert.Null(DriverOptions.From("x", ok: true, json)); // says nothing about the options
    }

    [Fact]
    public void AFailingVerb_IsAFailedRead_OnlyAnEmptyListIsNoOptions()
    {
        // `dp options nosuch --json` exits 1 with a FAIL line; an older CLI has no verb at all.
        var r = new CliResult(1, "FAIL: 'nosuch' is not a registered display processor (see 'dp list') — nothing to show.", "", false, null);
        Assert.Null(DriverOptions.From("nosuch", r.Ok, r.Json));
        var empty = DriverOptions.From("leia-sr", true, """{"id":"leia-sr","settings":[]}""")!;
        Assert.True(empty.IsEmpty);
        Assert.True(OptionViews.Build("leia-sr", empty).IsEmpty);
        // A log line printed around the document does not break it.
        var noisy = new CliResult(0, " WARN [x] loading\n" + TestData.Text("dp-options-leia-sr.json") + "\n", "", false, null);
        Assert.Equal("MULTI_VIEWER", Assert.Single(DriverOptions.From("leia-sr", noisy.Ok, noisy.Json)!.Settings).Key);
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
        // `2d` is not offered: nothing selected, said plainly.
        var flat = OptionViews.View(s with { Value = "2d" }, entry);
        Assert.Null(flat.Selected);
        Assert.Equal("No 3D preview kind is chosen: apps stay in 2D", flat.ValueText);
        Assert.True(flat.NoKind);
        // A value the driver does not know: quoted as stored.
        Assert.Equal("“mosaic”", OptionViews.View(s with { Value = "mosaic" }, entry).ValueText);
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

        // On writes 0; off always writes the driver's default (-1) for this user, never unsets: an unset
        // could leave a machine-wide 0 winning or reveal a machine-wide 1. "Use the default" unsets.
        Assert.Equal("0", OptionViews.PinWrite(off, on: true));
        Assert.Equal("-1", OptionViews.PinWrite(pinned, on: false));
        Assert.Equal("-1", OptionViews.PinWrite(OptionViews.View(s with { Value = "0", Source = SettingSource.Machine }, entry), on: false));
        Assert.Equal("-1", OptionViews.PinWrite(OptionViews.View(s with { Value = "1", Source = SettingSource.Machine }, entry), on: false));
        Assert.True(pinned.CanReset); // the unset is "Use the default"
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
    [InlineData(SettingType.Float, "0,344", null)]
    [InlineData(SettingType.Float, "1,000", null)]
    [InlineData(SettingType.Float, " 0.344 ", "0.344")]
    [InlineData(SettingType.String, "  a  b ", "  a  b ")]
    [InlineData(SettingType.String, "   ", null)]
    [InlineData(SettingType.String, "a\0b", null)]
    [InlineData(SettingType.Int, "4\0", null)]
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
    public void AFloatWithACommaIsRefused_WithTheReason()
    {
        Assert.Null(OptionViews.Validate(SettingType.Float, "1,000", out var error));
        Assert.Equal("Type a number, with a dot for decimals.", error);
        Assert.Null(OptionViews.Validate(SettingType.String, "x\0", out var nul));
        Assert.Equal("That value has a character that can't be stored.", nul);
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

        // A driver the CLI does not know (exit 1): a failed read, with nothing earlier to keep.
        await ctx.LoadDriverOptionsAsync("nosuch");
        Assert.Null(ctx.DriverOptionsOf("nosuch"));
        Assert.True(ctx.DriverOptionsReadFailed("nosuch"));
        Assert.False(ctx.DriverOptionsLoading("nosuch"));
    }

    /// <summary>Review finding 2: a failed read keeps the earlier good read (and the write's reply); the next good read clears the failure.</summary>
    [Fact]
    public async Task AFailedReRead_KeepsTheEarlierOptions_AndTheWritesReply()
    {
        bool fail = false;
        var fx = new FixtureProcessSource(TestData.Path("components.ndjson"), new FakeSource());
        var cli = new FailingReads(fx, () => fail);
        var ctx = new DashboardContext(new StatusFeed(new FakeSource(), new ManualScheduler()), new CliProcessSource("no-such-cli"), cli);
        await ctx.LoadDriverOptionsAsync("sim-display");
        Assert.False(ctx.DriverOptionsReadFailed("sim-display"));

        fail = true; // the write lands, its re-read times out
        await ctx.RunOptionWriteAsync("sim-display", "OUTPUT", OptionCommands.Set("sim-display", "OUTPUT", "sbs"));
        Assert.True(ctx.DriverOptionsReadFailed("sim-display"));
        var kept = ctx.DriverOptionsOf("sim-display")!;
        Assert.False(kept.IsEmpty);                     // the controls stay
        Assert.Equal("quad", kept.Find("OUTPUT")!.Value); // as last read
        Assert.Contains("'sbs'", ctx.LastAction);        // and the write's reply
        Assert.Equal(("sim-display", "OUTPUT"), ctx.LastOptionWrite);

        fail = false; // Try again
        await ctx.LoadDriverOptionsAsync("sim-display", force: true);
        Assert.False(ctx.DriverOptionsReadFailed("sim-display"));
        Assert.Equal("sbs", ctx.DriverOptionsOf("sim-display")!.Find("OUTPUT")!.Value);

        Assert.Equal("Couldn't read its options.", OptionViews.ReadFailedLine);
        Assert.Equal("Try again reading options: DisplayXR Sim Display", OptionViews.RetryName("DisplayXR Sim Display"));
    }

    [Fact]
    public void SecretCommand_PutsNoValueInTheArguments()
    {
        Assert.Equal(new[] { "dp", "set", "sim-display", "LICENCE_KEY", "--stdin" }, OptionCommands.SetStdin("sim-display", "LICENCE_KEY").Args);
        Assert.Equal("Set Licence: Leia SR", OptionViews.SetName("Licence", "Leia SR"));
        Assert.Equal("This version of DisplayXR can only clear this value.", OptionCommands.SecretSetUnsupported);
    }

    [Theory]
    [InlineData("abc-123", "abc-123")]
    [InlineData("  spaced key  ", "  spaced key  ")] // a credential is written as typed
    [InlineData("--looks-like-a-flag", "--looks-like-a-flag")] // fine on stdin: never an argument
    [InlineData("", null)]
    [InlineData("two\nlines", null)]
    [InlineData("cr\r", null)]
    [InlineData("nul\0", null)]
    public void SecretValues_AreCheckedBeforeTheyAreWritten(string text, string? written)
    {
        var v = OptionCommands.ValidateSecret(text, out var error);
        Assert.Equal(written, v);
        Assert.Equal(written is null, error is not null);
    }

    [Fact]
    public void SecretValues_UpToTheStoreLimit()
    {
        Assert.NotNull(OptionCommands.ValidateSecret(new string('x', 2047), out _));
        Assert.Null(OptionCommands.ValidateSecret(new string('x', 2048), out var e));
        Assert.Equal("That value is too long.", e);
        Assert.Null(OptionCommands.ValidateSecret(new string('é', 1024), out _)); // 2048 bytes in UTF-8
    }

    [Fact]
    public void AnOlderCliWithoutStdin_IsToldApart()
    {
        // An older CLI skips the unknown flag, finds two arguments and asks for a value.
        Assert.True(OptionCommands.IsStdinUnsupported(new CliResult(1, "error: 'dp set' needs <id> <KEY> <value>.\n\n  options <id> [--json] ...", "", false, null)));
        // This CLI names --stdin in its own refusals of the verb.
        Assert.False(OptionCommands.IsStdinUnsupported(new CliResult(1, "error: 'dp set' needs <id> <KEY> <value>, or <id> <KEY> --stdin.", "", false, null)));
        Assert.False(OptionCommands.IsStdinUnsupported(new CliResult(1, "error: --stdin read an empty value — nothing written (use 'dp unset' to clear it).", "", false, null)));
        Assert.False(OptionCommands.IsStdinUnsupported(new CliResult(1, "FAIL: 'x' is not a registered display processor (see 'dp list') — nothing written.", "", false, null)));
        Assert.False(OptionCommands.IsStdinUnsupported(new CliResult(0, "sim-display: LICENCE_KEY = <set> for this user (HKCU).", "", false, null)));
    }

    /// <summary>
    /// A secret is set through the CLI's standard input: never an argument, never in the reply the row
    /// shows, never kept; an older CLI is said in words. The fixture source checks the line as the CLI does.
    /// </summary>
    [Fact]
    public async Task Context_SetsASecretThroughStdin()
    {
        const string secret = "LIC-0123-SECRET-VALUE";
        var fx = new FixtureProcessSource(TestData.Path("components.ndjson"), new FakeSource());
        var rec = new RecordingSource(fx);
        var ctx = new DashboardContext(new StatusFeed(new FakeSource(), new ManualScheduler()), new CliProcessSource("no-such-cli"), rec);
        await ctx.LoadDriverOptionsAsync("sim-display");
        await ctx.RunOptionWriteAsync("sim-display", "LICENCE_KEY", OptionCommands.Unset("sim-display", "LICENCE_KEY"));
        Assert.False(ctx.DriverOptionsOf("sim-display")!.Find("LICENCE_KEY")!.IsSet);

        await ctx.RunSecretWriteAsync("sim-display", "LICENCE_KEY", secret);
        Assert.False(ctx.LastActionFailed);
        Assert.Equal(("sim-display", "LICENCE_KEY"), ctx.LastOptionWrite);
        Assert.Equal(OptionCommands.Area, ctx.LastActionArea);
        var row = ctx.DriverOptionsOf("sim-display")!.Find("LICENCE_KEY")!;
        Assert.True(row.IsSet);
        Assert.Null(row.Value);
        Assert.DoesNotContain(secret, ctx.LastAction ?? "");
        Assert.Contains(rec.Runs, r => r.Args.SequenceEqual(new[] { "dp", "set", "sim-display", "LICENCE_KEY", "--stdin" }) && r.Input == secret);
        Assert.DoesNotContain(rec.Runs, r => r.Args.Any(a => a.Contains(secret)));

        // An older CLI: the row says it can only clear the value.
        fx.StdinUnsupported = true;
        await ctx.RunSecretWriteAsync("sim-display", "LICENCE_KEY", secret);
        Assert.True(ctx.LastActionFailed);
        Assert.Equal(OptionCommands.SecretSetUnsupported, ctx.LastAction);
    }

    /// <summary>
    /// A value written through standard input is never echoed back, even for a key the CLI does not think a
    /// secret (its secret test falls back to a name check): LastAction carries the CLI's reply, so the reply
    /// must not hold the value.
    /// </summary>
    [Fact]
    public async Task Context_StdinValueOfANonSecretKey_IsNotEchoed()
    {
        const string value = "1920x1080-stdin-only";
        var fx = new FixtureProcessSource(TestData.Path("components.ndjson"), new FakeSource());
        var ctx = new DashboardContext(new StatusFeed(new FakeSource(), new ManualScheduler()), new CliProcessSource("no-such-cli"), fx);
        await ctx.LoadDriverOptionsAsync("sim-display");
        await ctx.RunSecretWriteAsync("sim-display", "STEREO_CAMERA_SIZE", value);
        Assert.False(ctx.LastActionFailed);
        Assert.DoesNotContain(value, ctx.LastAction ?? "");
        Assert.Contains("<set>", ctx.LastAction ?? "");
    }

    /// <summary>
    /// The real process source writes exactly the value and one "\n" to the child's standard input: UTF-8, no
    /// byte-order mark, nothing else. The child is a tiny script that copies its stdin, byte for byte, to a file.
    /// </summary>
    [Fact]
    public async Task CliProcessSource_WritesValuePlusNewline_NoBom()
    {
        if (!OperatingSystem.IsWindows()) return; // the stand-in CLI is a .cmd script
        string dir = Path.Combine(Path.GetTempPath(), "dxr-dash-stdin-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try
        {
            string outFile = Path.Combine(dir, "stdin.bin");
            string script = Path.Combine(dir, "fake-cli.cmd");
            // [Console]::OpenStandardInput() is the raw byte stream: no decoding, no line handling.
            File.WriteAllText(script,
                "@echo off\r\n" +
                "powershell -NoProfile -NonInteractive -Command \"$i=[Console]::OpenStandardInput(); $o=[IO.File]::Create('"
                + outFile + "'); $i.CopyTo($o); $o.Close()\"\r\n",
                new System.Text.UTF8Encoding(false));
            var src = new CliProcessSource(script);
            const string value = "héllo-✓";
            var r = await src.RunWithInputAsync(new CliArgs("dp", "set", "x", "K", "--stdin"), value, TimeSpan.FromSeconds(60));
            Assert.Equal(0, r.ExitCode);
            byte[] got = File.ReadAllBytes(outFile);
            byte[] want = new System.Text.UTF8Encoding(false).GetBytes(value + "\n");
            Assert.Equal(want, got);
        }
        finally
        {
            try { Directory.Delete(dir, true); } catch { }
        }
    }

    /// <summary>The fixture source, recording each run's arguments and standard input.</summary>
    private sealed class RecordingSource(IProcessSource inner) : IProcessSource
    {
        public readonly List<(string[] Args, string? Input)> Runs = new();

        public IWatchProcess StartWatch(Action<string> onLine, Action<int?> onExit, Action<string> onNote) => inner.StartWatch(onLine, onExit, onNote);

        public Task<CliResult> RunAsync(CliArgs args, TimeSpan timeout, System.Threading.CancellationToken cancel = default)
        {
            Runs.Add((args.Args.ToArray(), null));
            return inner.RunAsync(args, timeout, cancel);
        }

        public Task<CliResult> RunWithInputAsync(CliArgs args, string input, TimeSpan timeout, System.Threading.CancellationToken cancel = default)
        {
            Runs.Add((args.Args.ToArray(), input));
            return inner.RunWithInputAsync(args, input, timeout, cancel);
        }
    }

    private static DriverOptions OptionsJson(string id, string settings)
    {
        Assert.True(DriverOptions.TryParse($$"""{ "id": "{{id}}", "env_prefix": "DXR_DP_X_", "settings": [ {{settings}} ] }""", out var o));
        return o!;
    }

    private static string Row(string key, string type, string value, string? valuesJson) =>
        $$"""{ "key": "{{key}}", "declared": true, "type": "{{type}}", "default": "", "help": "h", "secret": false, "value": {{(value.Length > 0 ? $"\"{value}\"" : "null")}}, "source": "{{(value.Length > 0 ? "user" : "absent")}}"{{(valuesJson is null ? "" : $", \"values\": {valuesJson}")}} }""";

    [Fact]
    public void EnumValues_AreRead_OnlyForAnEnum()
    {
        var o = OptionsJson("x", string.Join(",",
            Row("SCOPE", "enum", "", "[\"canvas\", \"region\", \"Canvas\", \"\", 3]"),
            Row("NOLIST", "enum", "", "null"),
            Row("OLD", "enum", "", null),
            Row("COUNT", "int", "", "[\"1\", \"2\"]")));
        Assert.Equal(new[] { "canvas", "region" }, o.Find("SCOPE")!.Values); // strings only, once each, no empty word
        Assert.Null(o.Find("NOLIST")!.Values);
        Assert.Null(o.Find("OLD")!.Values);   // an older CLI: no key
        Assert.Null(o.Find("COUNT")!.Values); // not an enum: ignored
    }

    [Fact]
    public void AnEnumWithValues_AndNoFriendlyEntry_IsADropDownOfTheRawWords()
    {
        var o = OptionsJson("x", string.Join(",", Row("SCOPE", "enum", "Region", "[\"canvas\", \"region\", \"scanout\"]"),
                                                   Row("PLAIN", "enum", "a", "null")));
        var b = OptionViews.Build("x", o);
        var scope = b.More.Single(v => v.Key == "SCOPE");
        Assert.Equal(OptionControl.Choice, scope.Control);
        Assert.False(scope.Friendly);
        Assert.Equal("SCOPE", scope.Label);
        Assert.Equal(new[] { "canvas", "region", "scanout" }, scope.Choices.Select(c => c.Label));
        Assert.Equal(new[] { "canvas", "region", "scanout" }, scope.Choices.Select(c => c.Value));
        Assert.Equal("region", scope.Selected); // stored values match without regard to case
        Assert.Equal(OptionControl.Text, b.More.Single(v => v.Key == "PLAIN").Control); // no values: a text box, as before

        var odd = OptionViews.Build("x", OptionsJson("x", Row("SCOPE", "enum", "other", "[\"canvas\", \"region\"]"))).More.Single();
        Assert.Null(odd.Selected);
        Assert.Equal("“other”", odd.ValueText); // a stored word the CLI does not list: shown as stored, nothing selected
    }

    [Fact]
    public void AFriendlyDropDown_FollowsTheCliValues()
    {
        var output = FriendlyOptions.Find(FriendlyOptions.SimDisplay, "OUTPUT")!;
        // No values (every current CLI for OUTPUT, whose help is prose): the entry's own list, unchanged.
        Assert.Same(output.Choices, OptionViews.ChoicesFor(output, null));
        // A value the CLI does not list is dropped; one it lists that the entry does not know is appended raw;
        // an alias keeps its value; a no-kind value is never appended.
        var c = OptionViews.ChoicesFor(output, new[] { "sbs", "anaglyph", "squeezed_sbs", "tab", "2d" });
        Assert.Equal(new[] { "anaglyph", "sbs", "squeezed", "tab" }, c.Select(x => x.Value));
        Assert.Equal(new[] { "Red / cyan (anaglyph)", "Side by side", "Side by side, squeezed", "tab" }, c.Select(x => x.Label));

        var o = OptionsJson(FriendlyOptions.SimDisplay, Row("OUTPUT", "enum", "tab", "[\"anaglyph\", \"sbs\", \"tab\"]"));
        var v = OptionViews.Build(FriendlyOptions.SimDisplay, o).Friendly.Single(x => x.Key == "OUTPUT");
        Assert.True(v.Friendly);
        Assert.Equal("3D preview kind", v.Label);
        Assert.Equal(new[] { "anaglyph", "sbs", "tab" }, v.Choices.Select(x => x.Value));
        Assert.Equal("tab", v.Selected);
        Assert.Equal("tab", v.ValueText);

        var mv = FriendlyOptions.Find("leia-sr", "MULTI_VIEWER")!;
        Assert.Equal(new[] { "on", "off", "auto" }, OptionViews.ChoicesFor(mv, new[] { "on", "off", "auto" }).Select(x => x.Value));
    }

    /// <summary>The fixture source, with every <c>dp options</c> read timing out while <c>fail</c> says so.</summary>
    private sealed class FailingReads(IProcessSource inner, Func<bool> fail) : IProcessSource
    {
        public IWatchProcess StartWatch(Action<string> onLine, Action<int?> onExit, Action<string> onNote) => inner.StartWatch(onLine, onExit, onNote);

        public Task<CliResult> RunAsync(CliArgs args, TimeSpan timeout, System.Threading.CancellationToken cancel = default) =>
            fail() && args.Args is ["dp", "options", ..] ? Task.FromResult(new CliResult(-1, "", "", true, null)) : inner.RunAsync(args, timeout, cancel);
    }

    /// <summary>Review finding 4: a submitted edit ends the edit state, so the deferred rebuild (the new value, the reply) lands at once.</summary>
    [Fact]
    public void EditState_SubmitEndsTheEdit_RefuseKeepsTheDraft()
    {
        var e = new OptionEdits();
        const string k = "sim-display/WIDTH_M";
        Assert.False(e.IsEditing);
        e.Focus(k);
        Assert.True(e.IsEditing); // rebuilds wait
        e.Type(k, "0.5");
        Assert.Equal("0.5", e.Draft(k));

        e.Submit(k);              // Enter / Apply with a value that will be written
        Assert.False(e.IsEditing); // the rebuild with the write's result is no longer deferred
        Assert.Null(e.Draft(k));   // the re-read value shows, not the typed text

        e.Focus(k);
        e.Type(k, "1,000");
        e.Refuse(k);              // refused: the message must show at once
        Assert.False(e.IsEditing);
        Assert.Equal("1,000", e.Draft(k)); // kept, to be corrected

        e.Focus(k);
        e.Blur("other/KEY");      // another box's blur does not end this edit
        Assert.True(e.IsEditing);
        e.Blur(k);
        Assert.False(e.IsEditing);
        Assert.Equal("1,000", e.Draft(k)); // leaving the box does not apply or drop the text

        e.Focus(k);
        e.Rebuilt();              // a forced rebuild replaced the box: never stuck deferring
        Assert.False(e.IsEditing);
    }

    /// <summary>Review finding 6: a friendly entry applies only to the type it expects; values match without regard to case.</summary>
    [Fact]
    public void FriendlyEntries_NeedTheirType_AndMatchAnyCase()
    {
        DriverSetting Setting(string key, SettingType type, string? value) =>
            new(key, type, "", "", value, value is not null, value is null ? SettingSource.Absent : SettingSource.User);

        // MULTI_VIEWER declared as a bool or an int on some driver: not the friendly drop-down.
        var asBool = OptionViews.Build("other", new DriverOptions("other", "", new[] { Setting("MULTI_VIEWER", SettingType.Bool, "1") }));
        Assert.Empty(asBool.Friendly);
        Assert.Equal(OptionControl.Switch, Assert.Single(asBool.More).Control);
        var asInt = OptionViews.Build("other", new DriverOptions("other", "", new[] { Setting("MULTI_VIEWER", SettingType.Int, "2") }));
        Assert.Equal(OptionControl.Text, Assert.Single(asInt.More).Control);
        Assert.Equal("MULTI_VIEWER", asInt.More[0].Label);
        Assert.Equal("Several viewers", Assert.Single(OptionViews.Build("other",
            new DriverOptions("other", "", new[] { Setting("MULTI_VIEWER", SettingType.Enum, "ON") })).Friendly).Label);
        // START_3D declared as an enum: not the two-way choice.
        Assert.Null(FriendlyOptions.For("sim-display", Setting("START_3D", SettingType.Enum, "1")));

        // Any case.
        var output = FriendlyOptions.Find("sim-display", "OUTPUT");
        var s = Sim().Find("OUTPUT")!;
        Assert.Equal("sbs", OptionViews.View(s with { Value = "SBS" }, output).Selected);
        var quad = OptionViews.View(s with { Value = "Quad" }, output);
        Assert.Equal("Four views (2x2)", quad.ValueText);
        Assert.Equal(FriendlyOptions.FourViewNote, quad.Note);
        Assert.Equal("squeezed", OptionViews.View(s with { Value = "Squeezed_SBS" }, output).Selected);
        Assert.Equal("On", OptionViews.View(Setting("MULTI_VIEWER", SettingType.Enum, "ON"), FriendlyOptions.Find("x", "MULTI_VIEWER")).ValueText);
    }

    [Fact]
    public void NoKindChosen_SaysSo_AndStartInHasNoEffect()
    {
        var o = Sim();
        var two = o with { Settings = o.Settings.Select(x => x.Key == "OUTPUT" ? x with { Value = "Passthrough" } : x).ToArray() };
        var b = OptionViews.Build("sim-display", two);
        var kind = b.Friendly.Single(v => v.Key == "OUTPUT");
        Assert.Equal("No 3D preview kind is chosen: apps stay in 2D", kind.ValueText);
        Assert.Null(kind.Selected);
        Assert.Equal("Has no effect until a 3D preview kind is chosen.", b.Friendly.Single(v => v.Key == "START_3D").Note);
        // With a kind chosen, no such note.
        Assert.Null(SimBlock().Friendly.Single(v => v.Key == "START_3D").Note);
    }

    [Fact]
    public void AccessibleNames()
    {
        Assert.Equal("3D preview kind: DisplayXR Sim Display", OptionViews.AccessibleName("3D preview kind", "DisplayXR Sim Display"));
        Assert.Equal("Use the default for Always 2D: DisplayXR Sim Display", OptionViews.ResetName("Always 2D", "DisplayXR Sim Display"));
        Assert.Equal("More options: DisplayXR Sim Display", OptionViews.MoreName("DisplayXR Sim Display"));
    }
}

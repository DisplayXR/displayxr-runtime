// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json.Nodes;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>
/// ADR-054 N3e review: the policy rows against the JSON the CLI on this branch prints.
///
/// - <c>dp|input|conv-list-n3e.json</c>: captured verbatim (binary paths neutralised) from
///   <c>displayxr-cli &lt;class&gt; list --json</c> on macOS after <c>dp use sim-display</c> /
///   <c>input use sim-input</c>, with a manifest-disabled second row per class. POSIX
///   sources only: <c>default</c> / <c>manifest</c>, <c>policy.policy_lock</c> false.
/// - <c>dp-list-n3e-win.json</c>, <c>input-list-n3e-locked.json</c>, <c>conv-list-n3e-win.json</c>:
///   the same documents with the values only the Windows CLI emits — <c>enabled_source</c>
///   <c>user</c> (HKCU overlay) / <c>machine</c> (HKLM Enabled = 0), <c>policy_lock</c> true
///   (HKLM Policy\Lock), and the built-in <c>dp-lift</c> conversion row (as the v2.44.6
///   Windows CLI printed it in <c>conv-list-n3.json</c>, plus the new <c>enabled_source</c> /
///   <c>built_in</c> keys).
/// </summary>
public class ComponentPolicyFixtureTests
{
    private static PolicyList Parse(ComponentClass cls, string file)
    {
        Assert.True(PolicyList.TryParse(cls, TestData.Text(file), out var list), file);
        return list!;
    }

    private static string[] V(params string[] a) => a;
    private static string[][] Hints(PolicyList l, string id) =>
        PolicyCommands.MachineHints(l.Row(id)!, l).Select(h => h.Args.ToArray()).ToArray();

    [Fact]
    public void Dp_Captured_PreferredAndManifestDisabled()
    {
        var dp = Parse(ComponentClass.Display, "dp-list-n3e.json");
        Assert.True(dp.Writable);
        Assert.False(dp.PolicyLock);
        Assert.Null(dp.LockText);
        Assert.Equal("sim-display", dp.Preferred);

        var sim = dp.Row("sim-display")!;
        Assert.True(sim.Preferred);
        Assert.True(sim.Active);
        Assert.True(sim.EnabledKnown);
        Assert.Equal("default", sim.EnabledSource);
        Assert.False(sim.BuiltIn);
        Assert.Equal(("unverified", Level.Warn), sim.ProvenanceChip);

        var b = dp.Row("sim-display-b")!;
        Assert.False(b.Enabled);
        Assert.Equal("manifest", b.EnabledSource);
        Assert.Equal(("disabled (manifest)", Level.Plain), b.EnabledChip);

        // Non-elevated: each control's --machine command (Enabled, then Prefer / Reset).
        Assert.Equal(new[] { V("dp", "disable", "sim-display", "--machine"), V("dp", "reset", "--machine") }, Hints(dp, "sim-display"));
        Assert.Equal(new[] { V("dp", "enable", "sim-display-b", "--machine") }, Hints(dp, "sim-display-b")); // disabled: nothing to prefer
    }

    [Fact]
    public void Dp_Windows_UserAndMachineSources()
    {
        var dp = Parse(ComponentClass.Display, "dp-list-n3e-win.json");
        var sim = dp.Row("sim-display")!;
        Assert.False(sim.Enabled);
        Assert.Equal("user", sim.EnabledSource);
        Assert.Equal(("disabled (user)", Level.Plain), sim.EnabledChip);
        var leia = dp.Row("leia-sr")!;
        Assert.Equal("machine", leia.EnabledSource);
        Assert.Equal(("disabled (machine)", Level.Plain), leia.EnabledChip);
        Assert.True(leia.Preferred);
        Assert.Equal(("signed by Leia, Inc.", Level.Ok), leia.ProvenanceChip);
        Assert.Equal(new[] { V("dp", "enable", "leia-sr", "--machine"), V("dp", "reset", "--machine") }, Hints(dp, "leia-sr"));
    }

    [Fact]
    public void Input_Captured_And_Locked()
    {
        var input = Parse(ComponentClass.Input, "input-list-n3e.json");
        Assert.False(input.PolicyLock);
        Assert.Equal("sim-input", input.Preferred);
        Assert.True(input.Row("sim-input")!.Preferred);
        Assert.Equal("manifest", input.Row("sim-input-b")!.EnabledSource);
        Assert.Equal(new[] { V("input", "disable", "sim-input", "--machine"), V("input", "reset", "--machine") }, Hints(input, "sim-input"));

        var locked = Parse(ComponentClass.Input, "input-list-n3e-locked.json");
        Assert.True(locked.PolicyLock);
        Assert.True(locked.LockHidesUserEnabled);
        Assert.False(locked.LockHidesUserPreference); // an input PreferredPlugin is not under the lock
        Assert.Contains("Policy\\Lock", locked.LockText);
        Assert.Contains("--machine", locked.LockText);
        Assert.Equal("machine", locked.Row("sim-input-b")!.EnabledSource);
        Assert.Equal(ComponentClass.Input, PolicyList.ClassOfSection("input_providers"));
        Assert.Equal(ComponentClass.Conversion, PolicyList.ClassOfSection("conversion_providers"));
        Assert.Null(PolicyList.ClassOfSection("present_owners"));
    }

    [Fact]
    public void Conv_Captured_SourcesFromTheCli()
    {
        var conv = Parse(ComponentClass.Conversion, "conv-list-n3e.json");
        Assert.False(conv.PolicyLock);
        Assert.Empty(conv.PreferredByTask);
        var a = conv.Row("sim-conversion")!;
        Assert.True(a.EnabledKnown);
        Assert.False(a.BuiltIn);
        Assert.Equal("default", a.EnabledSource);
        var b = conv.Row("sim-conversion-b")!;
        Assert.False(b.Enabled);
        Assert.Equal("manifest", b.EnabledSource);
        Assert.Equal(("disabled (manifest)", Level.Plain), b.EnabledChip);
    }

    [Fact]
    public void Conv_Windows_BuiltInRowHasNoEnabledAndIsNeverPreferred()
    {
        var conv = Parse(ComponentClass.Conversion, "conv-list-n3e-win.json");
        var lift = conv.Row("dp-lift")!;
        Assert.True(lift.BuiltIn);
        Assert.Equal("default", lift.EnabledSource);
        Assert.False(lift.EnabledKnown); // no Enabled checkbox: `conv disable dp-lift` is refused by the CLI
        Assert.Null(lift.EnabledChip);
        Assert.True(conv.IsPreferredFor("depth-mono", "dp-lift"));
        // Only the Reset of the task it already holds; no Enabled, no Prefer for its other tasks.
        Assert.Equal(new[] { V("conv", "reset", "depth-mono", "--machine") }, Hints(conv, "dp-lift"));

        var user = conv.Row("sim-conversion")!;
        Assert.False(user.Enabled);
        Assert.Equal("user", user.EnabledSource);
        Assert.Equal(("disabled (user)", Level.Plain), user.EnabledChip);
        Assert.True(conv.IsPreferredFor("views-mono", "sim-conversion")); // "sim-conversion/views-a"
        Assert.False(conv.IsPreferredFor("views-mono", "sim"));           // a prefix of the id is not the id
        Assert.Equal(new[] { V("conv", "enable", "sim-conversion", "--machine"), V("conv", "reset", "views-mono", "--machine") },
                     Hints(conv, "sim-conversion"));

        Assert.Equal("machine", conv.Row("sim-conversion-b")!.EnabledSource);
        Assert.Equal(new[] { V("conv", "enable", "sim-conversion-b", "--machine") }, Hints(conv, "sim-conversion-b"));
    }

    [Fact]
    public void Conv_Locked_HidesUserEnabledAndPreference()
    {
        var doc = JsonNode.Parse(TestData.Text("conv-list-n3e-win.json"))!.AsObject();
        doc["policy"]!["policy_lock"] = true;
        Assert.True(PolicyList.TryParse(ComponentClass.Conversion, doc.ToJsonString(), out var conv));
        Assert.True(conv!.PolicyLock);
        Assert.True(conv.LockHidesUserEnabled);
        Assert.True(conv.LockHidesUserPreference);
        Assert.Contains("per-task preferences", conv.LockText);
    }

    [Fact]
    public void Conv_Section_ChipNamesTheSource()
    {
        // The status feed's conversion rows are the same `u_status_conv_to_cjson` objects.
        var list = JsonNode.Parse(TestData.Text("conv-list-n3e-win.json"))!.AsObject();
        var snap = TestData.Parse(new JsonObject
        {
            ["schema"] = 1, ["source"] = "headless",
            ["conv_providers"] = list["providers"]!.DeepClone(), ["conv_tasks"] = list["tasks"]!.DeepClone(),
        }.ToJsonString());
        var policy = Parse(ComponentClass.Conversion, "conv-list-n3e-win.json");
        var s = Components.ConversionProviders(snap, policy);
        var row = s.Items.Single(i => i.Policy?.Id == "sim-conversion");
        Assert.Contains(("disabled (user)", Level.Plain), row.Chips);
        Assert.DoesNotContain(("disabled", Level.Plain), row.Chips);
        var lift = s.Items.Single(i => i.Policy?.Id == "dp-lift");
        Assert.True(lift.Policy!.BuiltIn);
    }

    [Fact]
    public void Notes_EveryNoteAndWarningIsKept()
    {
        var r = new Feed.CliResult(0,
            "sim-input: disabled for this user (HKCU).\nnote: HKLM Policy\\Lock is set, so the per-user value is ignored.\n" +
            "No DisplayXR service reachable -- applies when it next starts.\n",
            "warning: 'x' is not a registered input provider\n", false, null);
        Assert.Equal("sim-input: disabled for this user (HKCU).", r.Summary);
        Assert.Equal(new[] { "note: HKLM Policy\\Lock is set, so the per-user value is ignored.", "warning: 'x' is not a registered input provider" }, r.Notes);
        Assert.Equal("sim-input: disabled for this user (HKCU).\nnote: HKLM Policy\\Lock is set, so the per-user value is ignored.\nwarning: 'x' is not a registered input provider", r.Report);
        // A note that IS the first line is not repeated.
        Assert.Equal("note: a\nnote: b", new Feed.CliResult(0, "note: a\nnote: b\n", "", false, null).Report);
    }
}

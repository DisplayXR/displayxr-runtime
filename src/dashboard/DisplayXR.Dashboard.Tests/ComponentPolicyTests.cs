// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>
/// ADR-054 N3e (#2052): the policy rows behind the Components page's Enabled /
/// Preferred controls — the JSON → row mapping for all three classes, the CLI
/// lines the controls run, and how the rows join the sections. The fixtures are
/// the real `dp|input|conv list --json` output of an N3 CLI (v2.44.6 + #2128).
/// </summary>
public class ComponentPolicyTests
{
    private static PolicyList Parse(ComponentClass cls, string file)
    {
        Assert.True(PolicyList.TryParse(cls, TestData.Text(file), out var list), file);
        return list!;
    }

    [Fact]
    public void DpList_ReadsEnabledSourceAndProvenance()
    {
        var dp = Parse(ComponentClass.Display, "dp-list-n3.json");
        Assert.True(dp.Writable);
        Assert.Null(dp.Preferred);
        Assert.Equal(new[] { "leia-sr", "sim-display" }, dp.Rows.Select(r => r.Id));

        var leia = dp.Row("leia-sr")!;
        Assert.True(leia.EnabledKnown);
        Assert.True(leia.Enabled);
        Assert.Equal("default", leia.EnabledSource);
        Assert.Null(leia.EnabledChip); // the quiet default: no chip
        Assert.True(leia.Active);
        Assert.False(leia.Preferred);
        Assert.Equal(("signed by Leia, Inc.", Level.Ok), leia.ProvenanceChip);

        var sim = dp.Row("sim-display")!;
        Assert.Equal(("allowlisted", Level.Ok), sim.ProvenanceChip);
        Assert.False(sim.Untrusted);
    }

    [Fact]
    public void InputList_UnverifiedReadsAsWarning()
    {
        var input = Parse(ComponentClass.Input, "input-list-n3.json");
        Assert.True(input.Writable);
        Assert.False(input.ForceQwerty);
        Assert.Equal(2, input.Rows.Count);
        Assert.All(input.Rows, r => Assert.Equal(("unverified", Level.Warn), r.ProvenanceChip));
        Assert.All(input.Rows, r => Assert.Contains("allowlisted", r.ProvenanceReason));
        Assert.Equal("DisplayXR-Ultraleap", input.Rows.Single(r => r.Active).Id);
    }

    [Fact]
    public void ConvList_PrefersPerTask()
    {
        var conv = Parse(ComponentClass.Conversion, "conv-list-n3.json");
        Assert.True(conv.Writable);
        var lift = Assert.Single(conv.Rows);
        Assert.Equal("dp-lift", lift.Id);
        Assert.Equal("BUILT_IN", lift.Load);
        Assert.True(lift.Enabled);
        // This CLI (v2.44.6) predates `enabled_source` / `built_in` on conv rows: nothing is
        // invented — the load verdict marks the built-in adapter, which gets no Enabled control.
        Assert.Equal("", lift.EnabledSource);
        Assert.True(lift.BuiltIn);
        Assert.False(lift.EnabledKnown);
        Assert.Null(lift.EnabledChip);
        Assert.Null(lift.ProvenanceChip);            // built-in: no binary, provenance "unknown"
        Assert.Empty(conv.PreferredByTask);
        Assert.Equal(new[] { "depth-mono", "depth-video", "views-mono" }, conv.TasksOf("dp-lift").Select(t => t.Task).Distinct());
        Assert.Contains(conv.TasksOf("dp-lift"), t => t.Selected);
    }

    [Fact]
    public void ConvPolicy_PreferredByTask_IsRead()
    {
        Assert.True(PolicyList.TryParse(ComponentClass.Conversion, """
            { "providers": [ { "id": "acme-conv", "name": "Acme", "enabled": false, "enabled_source": "user", "load": "NEGOTIATED",
                               "built_in": false,
                               "provenance": "unverified", "provenance_reason": "unsigned and not allowlisted (to allow: Trust\\Allowlist\\ab12)" } ],
              "tasks": [ { "task": "depth-mono", "provider": "acme-conv", "selected": false, "verdict": "disabled" } ],
              "policy": { "policy_lock": false, "preferred": { "depth-mono": "acme-conv/depth-a" } } }
            """, out var conv));
        Assert.Equal("acme-conv/depth-a", conv!.PreferredByTask["depth-mono"]);
        var row = Assert.Single(conv.Rows);
        Assert.False(row.Enabled);
        Assert.Equal("user", row.EnabledSource); // the CLI's word, read — never invented
        Assert.Equal(("disabled (user)", Level.Plain), row.EnabledChip);
        Assert.False(conv.PolicyLock);
        Assert.Equal(("unverified", Level.Warn), row.ProvenanceChip);
    }

    [Fact]
    public void OlderCli_HasNoEnabledField_ControlsStayHidden()
    {
        // The pre-N3 fixture: `dp list --json` without enabled / enabled_source / provenance.
        var old = Parse(ComponentClass.Display, "dp-list.json");
        Assert.False(old.Writable);
        Assert.All(old.Rows, r => Assert.False(r.EnabledKnown));
        Assert.All(old.Rows, r => Assert.Null(r.EnabledChip));
        Assert.All(old.Rows, r => Assert.Null(r.ProvenanceChip));

        // Not this verb's output at all.
        Assert.False(PolicyList.TryParse(ComponentClass.Display, "unknown command 'dp'", out _));
        Assert.False(PolicyList.TryParse(ComponentClass.Input, "{\"preferred\":null}", out _));
        Assert.True(PolicyList.TryParse(ComponentClass.Input, "{\"providers\":[]}", out var empty));
        Assert.Empty(empty!.Rows);
        Assert.False(empty.Writable);
    }

    [Fact]
    public void Chips_FollowTheSource()
    {
        PolicyRow R(bool enabled, string src, bool pref = false, string load = "", string prov = "", string? signer = null) =>
            new(ComponentClass.Input, "x", "X", enabled, src, pref, false, load, prov, signer, null);
        Assert.Equal(("disabled (user)", Level.Plain), R(false, "user").EnabledChip);
        Assert.Equal(("disabled (machine)", Level.Plain), R(false, "machine").EnabledChip);
        Assert.Equal(("enabled (user)", Level.Plain), R(true, "user").EnabledChip); // a user re-enable over a machine disable is worth a word
        Assert.Null(R(true, "default").EnabledChip);
        Assert.Null(R(true, "manifest").EnabledChip);
        Assert.True(R(true, "default", load: "UNTRUSTED").Untrusted);
        Assert.Equal(("dev override", Level.Info), R(true, "default", prov: "dev-override").ProvenanceChip);
        Assert.Equal(("platform-trusted", Level.Ok), R(true, "default", prov: "platform-trusted").ProvenanceChip);
        Assert.Equal(("signed", Level.Ok), R(true, "default", prov: "signed").ProvenanceChip);
        Assert.Equal(("quantum", Level.Plain), R(true, "default", prov: "quantum").ProvenanceChip); // unknown verdict: verbatim, neutral

        var chips = new List<(string, Level)>();
        Components.AddPolicyChips(chips, R(false, "user", pref: true, load: "UNTRUSTED", prov: "unverified"));
        Assert.Equal(new[] { ("disabled (user)", Level.Plain), ("preferred", Level.Info), ("refused: provenance", Level.Warn), ("unverified", Level.Warn) }, chips);
    }

    [Fact]
    public void Commands_AreTheCliLines()
    {
        Assert.Equal("dp enable leia-sr", PolicyCommands.Enable(ComponentClass.Display, "leia-sr"));
        Assert.Equal("dp disable leia-sr --machine", PolicyCommands.Disable(ComponentClass.Display, "leia-sr", machine: true));
        Assert.Equal("input use sim-input", PolicyCommands.Prefer(ComponentClass.Input, "sim-input"));
        Assert.Equal("input reset", PolicyCommands.Reset(ComponentClass.Input));
        Assert.Equal("dp reset --machine", PolicyCommands.Reset(ComponentClass.Display, machine: true));
        Assert.Equal("conv enable sim-conversion", PolicyCommands.Enable(ComponentClass.Conversion, "sim-conversion"));
        Assert.Equal("conv use depth-mono acme-conv", PolicyCommands.PreferTask("depth-mono", "acme-conv"));
        Assert.Equal("conv reset depth-mono --machine", PolicyCommands.ResetTask("depth-mono", machine: true));
        Assert.Equal("conv reset all", PolicyCommands.Reset(ComponentClass.Conversion));
        // Ids are quoted the way every other control quotes CLI arguments.
        Assert.Equal("input disable \"odd id\"", PolicyCommands.Disable(ComponentClass.Input, "odd id"));
        Assert.Equal(new[] { "policy-dp", "policy-input", "policy-conv" }, ComponentClasses.All.Select(PolicyCommands.Area));
        Assert.Equal(new[] { "display processor", "input provider", "conversion provider" }, ComponentClasses.All.Select(c => c.Noun()));
    }

    [Fact]
    public void Sections_JoinThePolicyRowsById()
    {
        var snap = TestData.Parse(TestData.Lines("components.ndjson")[0]);
        var policies = new Dictionary<ComponentClass, PolicyList>
        {
            [ComponentClass.Display] = Parse(ComponentClass.Display, "dp-list-n3.json"),
            [ComponentClass.Input] = Parse(ComponentClass.Input, "input-list-n3.json"),
        };
        var all = Components.Build(snap, null, null, policies);

        var dp = all.Single(s => s.Id == "display_processors");
        var leia = dp.Items.Single(i => i.Name == "DisplayXR Leia SR");
        Assert.NotNull(leia.Policy);
        Assert.Equal("leia-sr", leia.Policy!.Id);
        Assert.Contains(("signed by Leia, Inc.", Level.Ok), leia.Chips);
        Assert.DoesNotContain(leia.Chips, c => c.Text.StartsWith("enabled", StringComparison.Ordinal)); // the quiet default

        // No info rows at all: the input section is built from the policy list.
        var input = all.Single(s => s.Id == "input_providers");
        Assert.Equal(2, input.Count);
        var ul = input.Items.Single(i => i.Policy?.Id == "DisplayXR-Ultraleap");
        Assert.Contains(("active", Level.Ok), ul.Chips);
        Assert.Contains(("unverified", Level.Warn), ul.Chips);
        Assert.StartsWith("2 registered", input.ConnectedLine);

        // Without a policy list nothing changes for the older CLI.
        var plain = Components.Build(snap, null);
        Assert.All(plain.SelectMany(s => s.Items), i => Assert.Null(i.Policy));
    }

    [Fact]
    public void Vocabulary_NoPlugInOrDriverInTheRoleText()
    {
        var snap = TestData.Parse(TestData.Lines("components.ndjson")[0]);
        Assert.True(InfoResult.TryParse(TestData.Text("info-components.json"), out var info, out var err), err);
        foreach (var s in Components.Build(snap, info!.Components))
        {
            Assert.DoesNotContain("plug-in", s.Title, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("plug-in", s.Role, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("driver", s.Role, StringComparison.OrdinalIgnoreCase);
        }
        Assert.Equal("Applies to the next stream or job; a live stream never moves.", ComponentClass.Conversion.AppliesNote());
    }
}

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
    internal static PolicyList Parse(ComponentClass cls, string file)
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
        Assert.Equal(("Approved on this PC", Level.Ok), sim.ProvenanceChip);
        Assert.False(sim.Untrusted);
    }

    [Fact]
    public void LicenceObject_ReadsStateSourceAndExpiry()
    {
        // ADR-054 N5: `<class> list --json` rows carry a `licence` object; the fixture is
        // `tracking list --json` with the sim provider's scripted states.
        var l = Parse(ComponentClass.Tracking, "tracking-list-n5-licence.json");
        Assert.Equal(new[] { "acme-eyes", "sim-tracking", "sim-tracking-b", "plain" }, l.Rows.Select(r => r.Id));

        var hw = l.Row("acme-eyes")!;
        Assert.Equal("licensed", hw.Licence);
        Assert.Equal("hardware", hw.LicenceSource);
        Assert.False(hw.LicenceBlocks);
        Assert.Equal(("included with your display", Level.Ok), hw.LicenceChip);

        var trial = l.Row("sim-tracking")!;
        Assert.Equal("trial", trial.Licence);
        Assert.Equal("2100-01-01T00:00:00Z", trial.LicenceExpires);
        Assert.False(trial.LicenceBlocks);
        Assert.Equal(("trial until 2100-01-01", Level.Info), trial.LicenceChip);

        var none = l.Row("sim-tracking-b")!;
        Assert.Equal("unlicensed", none.Licence);
        Assert.True(none.LicenceBlocks);
        Assert.Equal(("unlicensed", Level.Warn), none.LicenceChip);
        Assert.Equal("https://example.invalid/displayxr/sim-licence", none.LicencePurchaseUrl);
        Assert.Equal("No simulated licence", none.LicenceMessage);

        // Nothing to report: no chip, exactly like a row from a CLI that predates the object.
        var plain = l.Row("plain")!;
        Assert.Equal("not_applicable", plain.Licence);
        Assert.Null(plain.LicenceChip);
        Assert.False(plain.LicenceBlocks);
        var old = Parse(ComponentClass.Tracking, "tracking-list-n6.json");
        Assert.All(old.Rows, r => Assert.Null(r.LicenceChip));
        Assert.All(old.Rows, r => Assert.Equal("", r.Licence));

        // The chip rides the row's other policy chips.
        var chips = new ChipList();
        Components.AddPolicyChips(chips, none);
        Assert.Contains(("unlicensed", Level.Warn), chips.Chips);
        Assert.Contains(("Licence", "unlicensed"), chips.Facts); // and in the row's Details, labelled
    }

    [Fact]
    public void InputList_UnverifiedReadsAsWarning()
    {
        var input = Parse(ComponentClass.Input, "input-list-n3.json");
        Assert.True(input.Writable);
        Assert.False(input.ForceQwerty);
        Assert.Equal(2, input.Rows.Count);
        Assert.All(input.Rows, r => Assert.Equal(("Publisher not verified", Level.Warn), r.ProvenanceChip));
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
        Assert.Equal(("Publisher not verified", Level.Warn), row.ProvenanceChip);
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
        Assert.Equal(("Developer override", Level.Info), R(true, "default", prov: "dev-override").ProvenanceChip);
        Assert.Equal(("Trusted by the system", Level.Ok), R(true, "default", prov: "platform-trusted").ProvenanceChip);
        Assert.Equal(("signed", Level.Ok), R(true, "default", prov: "signed").ProvenanceChip);
        Assert.Equal(("quantum", Level.Plain), R(true, "default", prov: "quantum").ProvenanceChip); // unknown verdict: verbatim, neutral

        var chips = new ChipList();
        Components.AddPolicyChips(chips, R(false, "user", pref: true, load: "UNTRUSTED", prov: "unverified"));
        Assert.Equal(new[] { ("disabled (user)", Level.Plain), ("your choice", Level.Info), ("Blocked: publisher not verified", Level.Warn), ("Publisher not verified", Level.Warn) }, chips.Chips);
        Assert.Equal(new[] { ("Enabled setting", "disabled (user)"), ("Preference", "your choice"), ("Publisher check", "Blocked: publisher not verified"), ("Signature", "Publisher not verified") },
                     chips.Facts);
    }

    [Fact]
    public void Commands_AreArgumentVectors()
    {
        static string[] V(params string[] a) => a;
        Assert.Equal(V("dp", "enable", "leia-sr"), PolicyCommands.Enable(ComponentClass.Display, "leia-sr").Args);
        Assert.Equal(V("dp", "disable", "leia-sr", "--machine"), PolicyCommands.Disable(ComponentClass.Display, "leia-sr", machine: true).Args);
        Assert.Equal(V("input", "use", "sim-input"), PolicyCommands.Prefer(ComponentClass.Input, "sim-input").Args);
        Assert.Equal(V("input", "reset"), PolicyCommands.Reset(ComponentClass.Input).Args);
        Assert.Equal(V("dp", "reset", "--machine"), PolicyCommands.Reset(ComponentClass.Display, machine: true).Args);
        Assert.Equal(V("conv", "enable", "sim-conversion"), PolicyCommands.Enable(ComponentClass.Conversion, "sim-conversion").Args);
        Assert.Equal(V("conv", "use", "depth-mono", "acme-conv"), PolicyCommands.PreferTask("depth-mono", "acme-conv").Args);
        Assert.Equal(V("conv", "reset", "depth-mono", "--machine"), PolicyCommands.ResetTask("depth-mono", machine: true).Args);
        Assert.Equal(V("conv", "reset", "all"), PolicyCommands.Reset(ComponentClass.Conversion).Args);
        Assert.Equal(V("input", "disable", "x"), PolicyCommands.Toggle(ComponentClass.Input, "x", enabled: true).Args);
        Assert.Equal(V("input", "enable", "x", "--machine"), PolicyCommands.Toggle(ComponentClass.Input, "x", enabled: false, machine: true).Args);
        // The readable form (log pane, administrator hints).
        Assert.Equal("conv use depth-mono acme-conv/depth-a", PolicyCommands.PreferTask("depth-mono", "acme-conv/depth-a").ToString());
        Assert.Equal(new[] { "policy-dp", "policy-input", "policy-conv", "policy-tracking" }, ComponentClasses.All.Select(PolicyCommands.Area));
        Assert.Equal(new[] { "display driver", "input add-on", "2D-to-3D converter", "camera head tracking add-on" }, ComponentClasses.All.Select(c => c.Noun()));
    }

    [Theory]
    [InlineData("odd id")]
    [InlineData("quote\"in\"id")]
    [InlineData("trailing\\")]
    [InlineData("back\\\"slash")]
    [InlineData("")]
    [InlineData("--machine")]
    public void Ids_TravelAsOneArgument(string id)
    {
        // Whatever a (Linux) manifest put in an id, it is exactly one argument: no quoting
        // rule between the dashboard and the CLI can split or merge it.
        foreach (var c in ComponentClasses.All)
        {
            var cmd = PolicyCommands.Disable(c, id);
            Assert.Equal(3, cmd.Args.Count);
            Assert.Equal(id, cmd.Args[2]);
        }
        Assert.Equal(id, PolicyCommands.PreferTask("depth-mono", id).Args[3]);
    }

    [Theory]
    [InlineData("plain", "plain")]
    [InlineData("odd id", "\"odd id\"")]
    [InlineData("a\"b", "\"a\\\"b\"")]
    [InlineData("C:\\dir\\", "C:\\dir\\")]
    [InlineData("C:\\my dir\\", "\"C:\\my dir\\\\\"")]
    [InlineData("", "\"\"")]
    public void ReadableForm_QuotesTheWayWindowsSplitsBack(string arg, string shown)
    {
        Assert.Equal(shown, CliArgs.Display(arg));
        Assert.Equal(new CliArgs("a", "b"), CliArgs.Words(" a  b "));
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
        // Every chip is kept for the row's Details, labelled; the row's state word is In use.
        Assert.Contains(("Load state", "ACTIVE"), leia.DetailFacts);
        Assert.Contains(leia.DetailFacts, f => f.Label == "Signature" && f.Text.StartsWith("signed", StringComparison.Ordinal));
        Assert.Equal(leia.Chips.Count, leia.Facts!.Count);
        Assert.Contains(leia.DetailFacts, f => f.Label == "Registration" && f.Text.Contains("ProbeOrder"));
        Assert.Equal(PlainState.InUse, leia.State);

        // No info rows at all: the input section is built from the policy list.
        var input = all.Single(s => s.Id == "input_providers");
        Assert.Equal(2, input.Count);
        var ul = input.Items.Single(i => i.Policy?.Id == "DisplayXR-Ultraleap");
        Assert.Contains(("active", Level.Ok), ul.Chips);
        Assert.Contains(("Publisher not verified", Level.Warn), ul.Chips);
        Assert.Contains(("Signature", "Publisher not verified"), ul.DetailFacts);
        Assert.StartsWith("2 installed", input.ConnectedLine);

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
            // The owner's plain word is "display driver"; the architecture's word stays out of the page.
            Assert.DoesNotContain("display processor", s.Role, StringComparison.OrdinalIgnoreCase);
            Assert.DoesNotContain("provider", s.Role, StringComparison.OrdinalIgnoreCase);
        }
        Assert.Equal("Applies to the next photo or video converted; one already playing keeps its converter.", ComponentClass.Conversion.AppliesNote());
    }
}

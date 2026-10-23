// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>
/// ADR-054 N6 Part 2 (#2144): the Components page's tracking card against the JSON the CLI prints.
///
/// - <c>tracking-list-n6.json</c> / <c>tracking-list-n6-preferred.json</c>: captured verbatim (binary
///   paths neutralised) from <c>displayxr-cli tracking list --json</c> on macOS, with sim-tracking
///   (ProbeOrder 200) and a manifest-disabled second row of the same binary (210); the second after
///   <c>tracking use sim-tracking</c>.
/// - <c>status-tracking.json</c>: three headless <c>status --json</c> snapshots over sim-display
///   (viewer policy EXTERNAL_OK): <c>provider</c> (sim-tracking ENABLE=1 → provider:sim-tracking),
///   <c>nosource</c> (ENABLE off → nominal, "no source"), <c>dp</c> (sim-display's own TRACKING dev
///   toggle → the display wins, provider in standby).
/// - <c>info-tracking.json</c>: the <c>info --json</c> <c>tracking_providers</c> block of the same three runs.
/// </summary>
public class TrackingProvidersTests
{
    private static PolicyList List(string file)
    {
        Assert.True(PolicyList.TryParse(ComponentClass.Tracking, TestData.Text(file), out var list), file);
        return list!;
    }

    private static StatusSnapshot Status(string which)
    {
        using var doc = JsonDocument.Parse(TestData.Text("status-tracking.json"));
        return TestData.Parse(doc.RootElement.GetProperty(which).GetRawText());
    }

    private static TrackingSummary? Summary(string which)
    {
        using var doc = JsonDocument.Parse(TestData.Text("info-tracking.json"));
        return TrackingSummary.Read(doc.RootElement.GetProperty(which));
    }

    private static string[] V(params string[] a) => a;

    [Fact]
    public void TrackingList_Captured_RowsAndPolicy()
    {
        var l = List("tracking-list-n6.json");
        Assert.True(l.Writable);
        Assert.False(l.PolicyLock);
        Assert.False(l.DisabledByEnv);
        Assert.Null(l.Preferred);
        Assert.Equal(new[] { "sim-tracking", "sim-tracking-b" }, l.Rows.Select(r => r.Id));

        var a = l.Row("sim-tracking")!;
        Assert.Equal("DisplayXR Sim Tracking", a.Name);
        Assert.Equal("DisplayXR", a.Vendor);
        Assert.Equal("dev", a.Version);
        Assert.Equal(200, a.ProbeOrder);
        Assert.True(a.RankedFirst);
        Assert.True(a.EnabledKnown);
        Assert.Null(a.EnabledChip); // "default": the quiet default
        Assert.Equal(("unverified", Level.Warn), a.ProvenanceChip);
        Assert.False(a.Untrusted); // POSIX audits: loaded anyway

        var b = l.Row("sim-tracking-b")!;
        Assert.False(b.Enabled);
        Assert.False(b.RankedFirst);
        Assert.Equal(("disabled (manifest)", Level.Plain), b.EnabledChip);
    }

    [Fact]
    public void TrackingList_Preferred_AndMachineHints()
    {
        var l = List("tracking-list-n6-preferred.json");
        Assert.Equal("sim-tracking", l.Preferred);
        Assert.True(l.Row("sim-tracking")!.Preferred);
        Assert.Equal(new[] { V("tracking", "disable", "sim-tracking", "--machine"), V("tracking", "reset", "--machine") },
                     PolicyCommands.MachineHints(l.Row("sim-tracking")!, l).Select(h => h.Args.ToArray()));
        // A disabled row offers only its Enabled flip (no Prefer for a row that cannot serve).
        Assert.Equal(new[] { V("tracking", "enable", "sim-tracking-b", "--machine") },
                     PolicyCommands.MachineHints(l.Row("sim-tracking-b")!, l).Select(h => h.Args.ToArray()));
    }

    [Fact]
    public void Commands_UseTheTrackingVerbs()
    {
        Assert.Equal(V("tracking", "enable", "sim-tracking"), PolicyCommands.Enable(ComponentClass.Tracking, "sim-tracking").Args);
        Assert.Equal(V("tracking", "disable", "sim-tracking", "--machine"),
                     PolicyCommands.Disable(ComponentClass.Tracking, "sim-tracking", machine: true).Args);
        Assert.Equal(V("tracking", "use", "sim-tracking"), PolicyCommands.Prefer(ComponentClass.Tracking, "sim-tracking").Args);
        Assert.Equal(V("tracking", "reset"), PolicyCommands.Reset(ComponentClass.Tracking).Args);
        Assert.Equal("policy-tracking", PolicyCommands.Area(ComponentClass.Tracking));
        Assert.Equal(ComponentClass.Tracking, PolicyList.ClassOfSection("tracking_providers"));
        Assert.Contains("next app launch", ComponentClass.Tracking.AppliesNote());
    }

    [Fact]
    public void PolicyLock_HidesThePerUserPreferenceToo()
    {
        // The values only the Windows CLI emits: HKCU / HKLM sources and HKLM Policy\Lock.
        Assert.True(PolicyList.TryParse(ComponentClass.Tracking, """
            { "preferred": "cam-tracker", "policy": { "policy_lock": true }, "disabled_by_env": true,
              "providers": [
                { "id": "cam-tracker", "display_name": "Camera Tracker", "enabled": true, "enabled_source": "machine",
                  "provenance": "signed", "signer": "Example Signer", "preferred": true, "first": true, "probe_order": 50 },
                { "id": "sim-tracking", "display_name": "DisplayXR Sim Tracking", "enabled": false, "enabled_source": "user",
                  "provenance": "allowlisted", "preferred": false, "first": false, "probe_order": 200 } ] }
            """, out var l));
        Assert.True(l!.PolicyLock);
        Assert.True(l.DisabledByEnv);
        Assert.True(l.LockHidesUserEnabled);
        Assert.True(l.LockHidesUserPreference); // tracking: the per-user PreferredPlugin is hidden under the lock
        Assert.Contains("per-user preference", l.LockText);
        Assert.Equal(("enabled (machine)", Level.Plain), l.Row("cam-tracker")!.EnabledChip);
        Assert.Equal(("disabled (user)", Level.Plain), l.Row("sim-tracking")!.EnabledChip);
        Assert.Equal(("signed by Example Signer", Level.Ok), l.Row("cam-tracker")!.ProvenanceChip);

        // Input's PreferredPlugin is not under the lock.
        Assert.True(PolicyList.TryParse(ComponentClass.Input, """{ "providers": [], "policy": { "policy_lock": true } }""", out var input));
        Assert.False(input!.LockHidesUserPreference);
    }

    [Fact]
    public void InfoSummary_ProbeChips()
    {
        var provider = Summary("provider")!;
        Assert.True(provider.Evaluated);
        Assert.Equal(new[] { "sim-tracking" }, provider.Ranked);
        Assert.Equal("sim-tracking", provider.ActiveId);
        Assert.Equal("provider:sim-tracking", provider.ViewerSource);
        Assert.Equal("EXTERNAL_OK", provider.DisplayViewerPolicy);
        Assert.Equal(3, provider.ComposedModes); // MANAGED | MANUAL, composed
        var l = List("tracking-list-n6.json");
        Assert.Equal(("probe: usable source, ranked first", Level.Ok), provider.ProbeChip(l.Row("sim-tracking")!));
        Assert.Null(provider.ProbeChip(l.Row("sim-tracking-b")!)); // disabled: never probed
        Assert.Equal("", provider.ScanLine);

        var none = Summary("nosource")!;
        Assert.True(none.ScanRan);
        Assert.Empty(none.Ranked);
        Assert.Equal("no source", none.ViewerReason);
        Assert.Equal(("probe: no usable source", Level.Plain), none.ProbeChip(l.Row("sim-tracking")!));
        Assert.Equal("1 declined (no source)", none.ScanLine);

        // A summary from a scan that did not run says "not probed", never "no source".
        var idle = new TrackingSummary(1, 1, 0, 0, 0, Array.Empty<string>(), "", true, "nominal", "display is FIXED", "FIXED", 0);
        Assert.False(idle.ScanRan);
        Assert.Equal(("not probed", Level.Plain), idle.ProbeChip(l.Row("sim-tracking")!));

        // An info document without the block (an older CLI).
        using var doc = JsonDocument.Parse("""{ "input_providers": {} }""");
        Assert.Null(TrackingSummary.Read(doc.RootElement));
        Assert.Null(InfoComponents.Read(doc.RootElement).Tracking);
    }

    [Fact]
    public void Screens_ProviderNoSourceAndStandbyAreDistinct()
    {
        string Name(string id) => id == "sim-tracking" ? "DisplayXR Sim Tracking" : id;

        var p = Assert.Single(Tracking.Screens(Status("provider"), null, Name));
        Assert.True(p.Primary);
        Assert.Equal(ViewerSourceKind.Provider, p.Kind);
        Assert.Equal("sim-tracking", p.ProviderId);
        Assert.Equal("Provider: DisplayXR Sim Tracking", p.Source);
        Assert.StartsWith("Selected:", p.Reason); // headless: no live state yet
        Assert.Equal("EXTERNAL_OK", p.Policy);
        Assert.Equal(("policy EXTERNAL_OK", Level.Info), p.PolicyChip);
        Assert.True(p.CanCalibrate);

        var n = Assert.Single(Tracking.Screens(Status("nosource"), null, Name));
        Assert.Equal(ViewerSourceKind.Nominal, n.Kind);
        Assert.Equal("No source: no enabled provider found a usable source.", n.Reason);
        Assert.Equal("no source", n.RawReason);
        Assert.False(n.CanCalibrate);

        var d = Assert.Single(Tracking.Screens(Status("dp"), null, Name));
        Assert.Equal(ViewerSourceKind.Display, d.Kind);
        Assert.Equal("Standby: the display tracks the viewer on this screen.", d.Reason);
        Assert.False(d.CanCalibrate);
        Assert.NotEqual(n.Reason, d.Reason);
    }

    [Fact]
    public void Words_LiveStateAndVerbatimReasons()
    {
        Assert.Equal(("Provider: Cam", "Tracking the viewer.", Level.Ok),
                     Tracking.Words(ViewerSourceKind.Provider, "Cam", null, "TRACKING"));
        var paused = Tracking.Words(ViewerSourceKind.Provider, "Cam", null, "NOT_TRACKING");
        Assert.StartsWith("Source paused:", paused.Reason);
        Assert.Equal(Level.Warn, paused.Level);
        Assert.Equal("Display is FIXED.", Tracking.Words(ViewerSourceKind.Nominal, "", "display is FIXED", "NO_DP").Reason);
        Assert.Equal("Display owns the viewer on this screen (policy OWN).",
                     Tracking.Words(ViewerSourceKind.Display, "", "display owns the viewer on this screen (policy OWN)", "NO_DP").Reason);
        Assert.Equal(ViewerSourceKind.Unknown, Tracking.ParseSource("provider:").Kind);
        Assert.Equal(ViewerSourceKind.Unknown, Tracking.ParseSource("something-new").Kind);
    }

    [Fact]
    public void OlderSnapshot_NotReported_PrimaryFallsBackToInfoPolicy()
    {
        var old = TestData.Parse(TestData.DesignExample); // no viewer_* keys
        var rows = Tracking.Screens(old, null);
        Assert.Equal(2, rows.Count);
        Assert.All(rows, r => Assert.Equal(ViewerSourceKind.Unknown, r.Kind));
        Assert.All(rows, r => Assert.Equal(("policy not reported", Level.Plain), r.PolicyChip));

        // The runtime-default screen takes info's display_viewer_policy; the other screen does not.
        var withInfo = Tracking.Screens(old, Summary("provider"));
        Assert.Equal("EXTERNAL_OK", withInfo.Single(r => r.Primary).Policy);
        Assert.Null(withInfo.Single(r => !r.Primary).Policy);
    }

    [Fact]
    public void Section_JoinsListInfoAndFeed()
    {
        var status = Status("provider");
        var policies = new Dictionary<ComponentClass, PolicyList> { [ComponentClass.Tracking] = List("tracking-list-n6.json") };
        var info = InfoComponents.Empty with { Tracking = Summary("provider") };
        var all = Components.Build(status, info, null, policies);

        var ids = all.Select(s => s.Id).ToList();
        Assert.Equal(ids.IndexOf("input_providers") + 1, ids.IndexOf("tracking_providers")); // beside the input providers
        var tr = all.Single(s => s.Id == "tracking_providers");
        Assert.Equal("Tracking providers", tr.Title);
        Assert.Equal(2, tr.Count);
        Assert.Equal(1, tr.Connected);
        Assert.Equal("2 registered · 1 enabled · serving 1 screen", tr.ConnectedLine);
        var sim = tr.Items.Single(i => i.Policy?.Id == "sim-tracking");
        Assert.Equal(Level.Ok, sim.Level);
        Assert.Contains(("serving", Level.Ok), sim.Chips);
        Assert.Contains(("probe: usable source, ranked first", Level.Ok), sim.Chips);
        Assert.Contains(("unverified", Level.Warn), sim.Chips);
        Assert.StartsWith("DisplayXR · version dev · sim-tracking · ProbeOrder 200", sim.Lines[0]);
        var b = tr.Items.Single(i => i.Policy?.Id == "sim-tracking-b");
        Assert.DoesNotContain(("serving", Level.Ok), b.Chips);
        Assert.Contains(("disabled (manifest)", Level.Plain), b.Chips);

        // Neither `tracking list` nor info's block: an older CLI shows no tracking card.
        Assert.DoesNotContain(Components.Build(status, null), s => s.Id == "tracking_providers");
        // An empty class still shows the card (the class exists; nothing is registered).
        Assert.True(PolicyList.TryParse(ComponentClass.Tracking, """{ "providers": [] }""", out var empty));
        var none = Components.Build(Status("nosource"), null, null, new Dictionary<ComponentClass, PolicyList> { [ComponentClass.Tracking] = empty! })
            .Single(s => s.Id == "tracking_providers");
        Assert.True(none.IsEmpty);
        Assert.Equal("none registered", none.ConnectedLine);
    }
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Linq;
using System.Text.Json.Nodes;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>
/// ADR-034 Amendment 5 (#2181): the Components → Input per-app table — <c>input list --json</c>
/// <c>per_app</c> (stored pins) + <c>holders</c> (live, from the service) → one row per (app, family),
/// the CLI lines Prefer / Reset run, the lock, and the offline fixture's simulation of the verbs.
/// The fixture is <c>fixtures/input-list.json</c> (what the CLI on this branch prints, ids neutral).
/// </summary>
public class PerAppPolicyTests
{
    private static PolicyList Parse(string json)
    {
        Assert.True(PolicyList.TryParse(ComponentClass.Input, json, out var list));
        return list!;
    }

    private static string[] V(params string[] a) => a;

    [Fact]
    public void PerApp_RowsJoinPinsAndLiveHolders()
    {
        var l = Parse(TestData.Text("input-list.json"));
        Assert.True(l.HasPerApp);
        Assert.Equal(2, l.PerApp!.Count);
        Assert.Equal(2, l.Holders!.Count);
        Assert.Equal(0, l.ClientsDropped);

        var rows = l.PerAppRows();
        // Three apps (two pinned, one only running) x three families, apps in name order.
        Assert.Equal(new[] { "Earth.exe", "MediaPlayer.exe", "Viewer.exe" }, rows.Select(r => r.App).Distinct());
        Assert.Equal(9, rows.Count);

        var mp = rows.Where(r => r.App == "MediaPlayer.exe").ToArray();
        Assert.All(mp, r => Assert.Equal("DisplayXR Sim Input", r.Pin)); // the provider's own name
        Assert.Equal("DisplayXR Sim Input", mp.Single(r => r.FamilyKey == "controllers").Holder);
        Assert.Equal("Desk Hand Tracker", mp.Single(r => r.FamilyKey == "hand_tracking").Holder);
        Assert.Equal("none", mp.Single(r => r.FamilyKey == "gamepad").Holder);
        Assert.All(mp, r => Assert.True(r.Running));

        var earth = rows.Where(r => r.App == "Earth.exe").ToArray();
        Assert.All(earth, r => Assert.Equal("default", r.Pin));
        Assert.Null(earth[0].PinId);
        Assert.Equal("Gamepad", earth.Single(r => r.FamilyKey == "gamepad").Holder);

        var viewer = rows.Where(r => r.App == "Viewer.exe").ToArray();
        Assert.All(viewer, r => Assert.Equal("Emulated controllers", r.Pin)); // the runtime's own pair
        Assert.All(viewer, r => Assert.Equal("not running", r.Holder));
        Assert.All(viewer, r => Assert.False(r.Running));

        Assert.Equal(new[] { "hand-tracker", "gamepad", "sim-input", "emulated" }, l.PinChoices());
        Assert.Null(l.PerAppLockText);
    }

    [Fact]
    public void PerApp_CommandsAreArgumentVectors_PerUserOnly()
    {
        Assert.Equal(V("input", "use", "sim-input", "--app", "Media Player.exe"),
                     PolicyCommands.PreferApp("sim-input", "Media Player.exe").Args.ToArray());
        Assert.Equal(V("input", "reset", "--app", "Media Player.exe"), PolicyCommands.ResetApp("Media Player.exe").Args.ToArray());
        // No machine-wide per-app pin exists: the CLI refuses --machine with --app.
        Assert.DoesNotContain("--machine", PolicyCommands.PreferApp("x", "y").Args);
    }

    [Fact]
    public void PerApp_LockedAndServiceAbsent()
    {
        var root = JsonNode.Parse(TestData.Text("input-list.json"))!.AsObject();
        root["policy"]!["policy_lock"] = true;
        root["holders"] = null;           // no service reached
        root["clients_dropped"] = null;
        var l = Parse(root.ToJsonString());
        Assert.True(l.PolicyLock);
        Assert.NotNull(l.PerAppLockText);
        Assert.Null(l.Holders);
        Assert.Null(l.ClientsDropped);
        var rows = l.PerAppRows();
        Assert.Equal(new[] { "MediaPlayer.exe", "Viewer.exe" }, rows.Select(r => r.App).Distinct()); // pins only
        Assert.All(rows, r => Assert.Equal("not running", r.Holder));
    }

    [Fact]
    public void PerApp_AnOlderCliHasNoTable()
    {
        // input-list-n3e.json predates #2181: no per_app key -> no table, nothing invented.
        var l = Parse(TestData.Text("input-list-n3e.json"));
        Assert.False(l.HasPerApp);
        Assert.Empty(l.PerAppRows());
        var dropped = JsonNode.Parse(TestData.Text("input-list.json"))!.AsObject();
        dropped["clients_dropped"] = 2;
        Assert.Equal(2, Parse(dropped.ToJsonString()).ClientsDropped);
    }

    [Fact]
    public void FixtureSource_SimulatesThePerAppVerbs()
    {
        DashboardLog.Disabled = true;
        var fx = new FixtureProcessSource(TestData.Path("components.ndjson"), new FakeSource());
        CliResult Run(params string[] a) => fx.RunAsync(new CliArgs(a), TimeSpan.FromSeconds(1)).Result;

        Assert.True(Run("input", "use", "gamepad", "--app", "Earth.exe").Ok);
        Assert.True(Run("input", "reset", "--app", "Viewer.exe").Ok);
        var l = Parse(Run("input", "list", "--json").Stdout);
        Assert.Equal("gamepad", l.PerApp!.Single(p => p.App == "Earth.exe").Preferred);
        Assert.DoesNotContain(l.PerApp!, p => p.App == "Viewer.exe");
        Assert.Equal("Gamepad", l.PerAppRows().Single(r => r.App == "Earth.exe" && r.FamilyKey == "controllers").Holder);
    }
}

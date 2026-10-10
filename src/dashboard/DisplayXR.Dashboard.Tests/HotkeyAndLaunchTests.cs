// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Linq;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

public class HotkeyAndLaunchTests
{
    [Theory]
    [InlineData("Ctrl+Space", "Ctrl+Space")]
    [InlineData("ctrl+shift+f9", "Ctrl+Shift+F9")]
    [InlineData(" Shift + Ctrl + a ", "Ctrl+Shift+A")]          // canonical modifier order, upper-case letter
    [InlineData("Control+Alt+Return", "Ctrl+Alt+Enter")]         // aliases
    [InlineData("Win+Shift+PgDn", "Shift+Win+PageDown")]
    [InlineData("Meta+F24", "Win+F24")]
    [InlineData("Ctrl+Plus", "Ctrl+Plus")]
    [InlineData("Alt+`", "Alt+`")]
    public void Parse_FormatsCanonically(string text, string canonical)
    {
        Assert.True(Hotkey.TryParse(text, out var hk, out var err), err);
        Assert.True(hk.IsValid);
        Assert.Equal(canonical, hk.ToString());
        Assert.True(Hotkey.TryParse(hk.ToString(), out var again, out _));
        Assert.Equal(hk, again); // round-trips
    }

    [Theory]
    [InlineData("", "empty")]
    [InlineData("Space", "needs at least one")]
    [InlineData("Ctrl+Shift", "no key")]
    [InlineData("Ctrl+A+B", "more than one key")]
    [InlineData("Ctrl+Ctrl+A", "twice")]
    [InlineData("Ctrl+F25", "unknown key")]
    [InlineData("Ctrl++A", "empty part")]
    [InlineData("Hyper+A", "unknown key")]
    public void Parse_RejectsWithAReason(string text, string reason)
    {
        Assert.False(Hotkey.TryParse(text, out _, out var err));
        Assert.Contains(reason, err);
    }

    [Fact]
    public void Validate_NeedsAModifierAndAKey()
    {
        Assert.Null(Hotkey.Validate(HotkeyModifiers.Ctrl, "Space"));
        Assert.NotNull(Hotkey.Validate(HotkeyModifiers.None, "Space"));
        Assert.NotNull(Hotkey.Validate(HotkeyModifiers.Ctrl | HotkeyModifiers.Shift, null));
        Assert.Equal("Ctrl+Shift+Alt+Win+F9",
            new Hotkey(HotkeyModifiers.Win | HotkeyModifiers.Alt | HotkeyModifiers.Shift | HotkeyModifiers.Ctrl, "F9").ToString());
    }

    [Fact]
    public void WorkspaceList_ReadsTheContract()
    {
        Assert.True(WorkspaceList.TryParse(TestData.Text("workspace-list.json"), out var ws));
        Assert.Equal("workspace-host", ws!.ActiveId);
        var host = ws.Controllers[0];
        Assert.Equal(("workspace-host", "Workspace Host", "Example Vendor", "3.1.0"), (host.Id, host.Name, host.Vendor, host.Version));
        Assert.EndsWith("workspace-host.exe", host.Exe);
        Assert.True(host.Connected);
        Assert.Equal(5120, host.Pid);
        Assert.Equal(new LaunchConfig("auto", "Ctrl+Space", "default"), host.Launch);
        var other = ws.Controllers[1];
        Assert.False(other.Connected);
        Assert.Null(other.Pid);
        Assert.Null(other.Launch!.Hotkey);
        Assert.Equal("user", other.Launch.Source);
    }

    [Fact]
    public void WorkspaceList_IsTolerant()
    {
        Assert.False(WorkspaceList.TryParse("unknown command 'workspace'", out _));
        Assert.False(WorkspaceList.TryParse("{\"active_id\":null}", out _)); // no controllers key: not this verb
        Assert.True(WorkspaceList.TryParse("""
            { "active_id": null, "controllers": [ { "id": "c", "connected": null, "pid": "x" },
                                                  { "id": "d", "launch": { "hotkey": null } }, 7 ] }
            """, out var ws));
        Assert.Equal(2, ws!.Controllers.Count);
        Assert.Null(ws.Controllers[0].Connected);  // headless: unknown
        Assert.Null(ws.Controllers[0].Pid);
        Assert.Null(ws.Controllers[0].Launch);     // no launch: the row is hidden
        Assert.Equal(new LaunchConfig("auto", null, "default"), ws.Controllers[1].Launch);
        // info's older shape (name / exe) reads into the same record.
        Assert.True(InfoResult.TryParse(TestData.Text("info-components.json"), out var info, out _));
        Assert.Equal("Workspace Host", info!.Components.WorkspaceControllers[0].Name);
        Assert.Null(info.Components.WorkspaceControllers[0].Launch);
    }

    [Fact]
    public void Components_UseTheWorkspaceList_ConnectedAndNotRunning()
    {
        var snap = TestData.Parse(TestData.Lines("components.ndjson")[0]);
        Assert.True(WorkspaceList.TryParse(TestData.Text("workspace-list.json"), out var ws));
        var wc = Components.Build(snap, InfoComponents.Empty, ws).Single(s => s.Id == "workspace_controller");
        Assert.Equal(2, wc.Items.Count);                         // the connected client is matched, not listed twice
        var host = wc.Items.Single(i => i.Name == "Workspace Host"); // display_name, never a product word of ours
        Assert.Contains(("connected (pid 5120)", Level.Ok), host.Chips);
        Assert.NotNull(host.Controller!.Launch);
        var other = wc.Items.Single(i => i.Name == "Other Controller");
        Assert.Contains(("registered, not running", Level.Plain), other.Chips);
        Assert.Equal(1, wc.Connected);
        Assert.Equal("1 connected · workspace on · 2 registered", wc.ConnectedLine);
    }

    [Fact]
    public void FixtureSource_SimulatesTheWorkspaceVerbs()
    {
        DashboardLog.Disabled = true;
        var fx = new FixtureProcessSource(TestData.Path("components.ndjson"), new FakeSource());
        CliResult Run(string a) => fx.RunAsync(a, TimeSpan.FromSeconds(1)).Result;
        Assert.True(Run("workspace set other-controller --hotkey ctrl+shift+f9").Ok);
        Assert.False(Run("workspace set other-controller --hotkey F9").Ok); // no modifier
        Assert.True(Run("workspace set other-controller --mode disabled").Ok);
        Assert.False(Run("workspace launch other-controller").Ok);         // disabled
        Assert.True(Run("workspace set other-controller --mode auto").Ok);
        Assert.True(Run("workspace launch other-controller").Ok);
        Assert.True(WorkspaceList.TryParse(Run("workspace list --json").Stdout, out var ws));
        var other = ws!.Controllers.Single(c => c.Id == "other-controller");
        Assert.Equal("Ctrl+Shift+F9", other.Launch!.Hotkey);
        Assert.True(other.Connected);
        Assert.True(Run("workspace set other-controller --no-hotkey").Ok);
    }
}

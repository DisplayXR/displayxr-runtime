// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Linq;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

public class ComponentsTests
{
    private static readonly StatusSnapshot Comp = TestData.Parse(TestData.Lines("components.ndjson")[0]);

    private static InfoComponents Info(string file)
    {
        Assert.True(InfoResult.TryParse(TestData.Text(file), out var info, out var err), err);
        return info!.Components;
    }

    private static ComponentSection Section(System.Collections.Generic.IReadOnlyList<ComponentSection> all, string id) =>
        all.Single(s => s.Id == id);

    [Fact]
    public void ClientsAreGroupedByRole_NeverByName()
    {
        var all = Components.Build(Comp, null);
        Assert.Equal(new[] { "display_processors", "workspace_controller", "present_owners", "input_providers", "conversion", "diagnostics" },
                     all.Select(s => s.Id));

        var wc = Section(all, "workspace_controller");
        var item = Assert.Single(wc.Items);
        Assert.Equal("workspace-host.exe", item.Name);
        Assert.Equal(1, wc.Connected);
        Assert.Contains("Workspace controller: workspace-host.exe · connected · workspace on", item.Lines);

        var po = Section(all, "present_owners");
        Assert.Equal("capture_app.exe", Assert.Single(po.Items).Name);
        Assert.Equal("1 connected", po.ConnectedLine);

        var diag = Section(all, "diagnostics");
        Assert.True(diag.Collapsed);
        Assert.Equal("displayxr-cli.exe", Assert.Single(diag.Items).Name);

        // The APP client belongs to no component role.
        Assert.DoesNotContain(all.SelectMany(s => s.Items), i => i.Name == "cube_handle_d3d11_win.exe");
        // No product wording: the role captions never name a product.
        Assert.All(all, s => Assert.DoesNotContain("Shell", s.Role));
    }

    [Fact]
    public void DisplayProcessors_ListClaimsAndStates()
    {
        var dp = Components.DisplayProcessors(Comp);
        Assert.Equal(2, dp.Count);
        Assert.Equal(1, dp.Connected);
        var leia = dp.Items[0];
        Assert.Equal("DisplayXR Leia SR", leia.Name);
        Assert.Contains(("ACTIVE", Level.Ok), leia.Chips);
        Assert.Contains("Claims DISPLAY1 (VERIFIED), DISPLAY5 (VERIFIED)", leia.Lines);
        var sim = dp.Items[1];
        Assert.Contains(("FALLBACK", Level.Plain), sim.Chips);
        Assert.Contains("Claims no screen", sim.Lines);
    }

    [Fact]
    public void RegisteredControllers_MatchConnectedOnesByExe()
    {
        var all = Components.Build(Comp, Info("info-components.json"));
        var wc = Section(all, "workspace_controller");
        Assert.Equal(2, wc.Items.Count); // both registered; the connected one matched, not listed twice
        var host = wc.Items.Single(i => i.Name == "Workspace Host");
        Assert.Contains(("connected (pid 5120)", Level.Ok), host.Chips);
        Assert.Contains("Actions: toggle, launcher", host.Lines);
        var other = wc.Items.Single(i => i.Name == "Other Controller");
        Assert.Contains(("registered, not running", Level.Plain), other.Chips);
        Assert.Contains("not connected", other.Lines[0]);
        Assert.EndsWith("2 registered", wc.ConnectedLine);
    }

    [Fact]
    public void TodaysInfo_HasNoControllerList_AndReadsTheSummaries()
    {
        var info = Info("info.json"); // the real CLI output: no workspace_controllers, summary input_providers
        Assert.False(info.WorkspaceControllersKnown);
        Assert.Empty(info.WorkspaceControllers);
        Assert.NotNull(info.Input);
        Assert.Equal(2, info.Input!.Registered);
        Assert.False(info.Input.HardwarePresent);
        Assert.NotNull(info.Rig);
        Assert.Equal("neurd-directml", info.Lift!.Backend);
        Assert.Null(info.StereoCamera);

        var all = Components.Build(Comp, info);
        Assert.DoesNotContain(all, s => s.Id == "stereo_camera"); // absent block: section omitted
        var input = Section(all, "input_providers");
        Assert.Equal(2, input.Items.Count); // the summary row + the rig role
        Assert.Equal("2 registered · no hardware present", input.ConnectedLine);
        var conv = Section(all, "conversion");
        Assert.Equal("neurd-directml", Assert.Single(conv.Items).Name); // backend verbatim
        Assert.Equal(1, conv.Count);
        Assert.Contains("modes 0x3 · state 1 · 32 streams · up to 8 views", conv.Items[0].Lines);
    }

    [Fact]
    public void ProviderArrays_AndCameraBlock_AreRead()
    {
        var info = Info("info-components.json");
        Assert.Equal(new[] { "Hands X", "Pads Y" }, info.InputProviders.Select(p => p.Name));
        var input = Components.InputProviders(info);
        Assert.Equal(1, input.Connected);
        Assert.Contains(input.Items, i => i.Name == "Rig (navigation) role" && i.Lines.Contains("Device: Nav Puck"));
        var cam = Components.StereoCamera(info)!;
        Assert.Equal(new[] { "present: true", "name: Cam 7", "fps: 60" }, cam.Items[0].Lines); // nested object skipped
    }

    [Fact]
    public void MissingEverything_IsTolerated()
    {
        Assert.True(InfoResult.TryParse("{}", out var bare, out _));
        var all = Components.Build(null, bare!.Components);
        Assert.All(all, s => Assert.Empty(s.Items));
        Assert.Equal("not read yet", Section(all, "input_providers").ConnectedLine);
        Assert.Equal("live clients need the service", Section(all, "present_owners").ConnectedLine);

        Assert.True(InfoResult.TryParse("""
            { "workspace_controllers": "nope", "input_providers": 5, "lift_caps": { "probed": true, "malformed": true },
              "camera": { "x": 1 } }
            """, out var odd, out _));
        Assert.Empty(odd!.Components.WorkspaceControllers);
        Assert.False(odd.Components.WorkspaceControllersKnown);
        Assert.Empty(odd.Components.InputProviders);
        Assert.Null(odd.Components.Input);
        var conv = Components.Conversion(odd.Components);
        Assert.Equal(Level.Critical, conv.Items[0].Level);
        Assert.Equal(0, conv.Count);
        Assert.NotNull(Components.StereoCamera(odd.Components)); // "camera" is accepted as the block name too
    }

    [Fact]
    public void ControllerFromTheSnapshotOnly_WhenNotConnected()
    {
        var s = Comp with { Clients = Comp.Clients.Where(c => c.Class != "CONTROLLER").ToArray(),
                            Workspace = new Workspace(false, "workspace-host") };
        var wc = Components.WorkspaceController(s, InfoComponents.Empty);
        var item = Assert.Single(wc.Items);
        Assert.Equal("Workspace controller: workspace-host · not connected · workspace off", item.Lines[0]);
        Assert.Equal("none connected · workspace off", wc.ConnectedLine);
    }
}

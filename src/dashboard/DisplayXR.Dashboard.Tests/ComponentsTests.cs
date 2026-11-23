// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
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
        Assert.Equal(new[] { "display_processors", "workspace_controller", "input_providers", "conversion" },
                     all.Select(s => s.Id));
        // Running programs are not things you have or can add: they are listed under Troubleshooting.
        var running = Components.RunningPrograms(Comp);
        Assert.Equal(new[] { "present_owners", "diagnostics" }, running.Select(s => s.Id));

        var wc = Section(all, "workspace_controller");
        var item = Assert.Single(wc.Items);
        Assert.Equal("workspace-host.exe", item.Name);
        Assert.Equal(1, wc.Connected);
        Assert.Contains("Workspace controller: workspace-host.exe · connected · workspace on", item.Lines);

        var po = Section(running, "present_owners");
        Assert.Equal("capture_app.exe", Assert.Single(po.Items).Name);
        Assert.Equal("1 running", po.ConnectedLine);

        var diag = Section(running, "diagnostics");
        Assert.True(diag.Collapsed);
        Assert.Equal("displayxr-cli.exe", Assert.Single(diag.Items).Name);

        // The APP client belongs to no component role.
        Assert.DoesNotContain(all.Concat(running).SelectMany(s => s.Items), i => i.Name == "cube_handle_d3d11_win.exe");
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
    public void RowFace_IsOnePlainLine_TheTechnicalLinesAreLabelledInDetails()
    {
        var dp = Components.DisplayProcessors(Comp);
        Assert.Equal("Displays", dp.Title);
        Assert.Equal("2 installed · 1 in use", dp.ConnectedLine);
        var leia = dp.Items[0];
        // Publisher · version · what it drives, in plain words; no id, ProbeOrder or claim confidence.
        Assert.EndsWith("drives 2 displays", leia.Summary);
        Assert.DoesNotContain("ProbeOrder", leia.Summary);
        Assert.DoesNotContain("VERIFIED", leia.Summary);
        Assert.Contains(("Displays", "Claims DISPLAY1 (VERIFIED), DISPLAY5 (VERIFIED)"), leia.DetailFacts);
        Assert.Contains(leia.DetailFacts, f => f.Label == "Registration" && f.Text.Contains("ProbeOrder"));
        // A driver that drives nothing says nothing about it on its face.
        Assert.DoesNotContain("drives", Components.DisplayProcessors(Comp).Items[1].Summary ?? "");
        // Lines without labels are still listed, under "Note".
        var plain = new ComponentItem("x", Level.Plain, Array.Empty<(string, Level)>(), new[] { "hosted in a satellite" });
        Assert.Contains(("Note", "hosted in a satellite"), plain.DetailFacts);
    }

    [Fact]
    public void DrivesLine_SaysWhichDisplayInPlainWords()
    {
        var main = Comp.Screens.First(s => s.Roles.OsMain);
        Assert.Equal("drives your main display", Components.DrivesLine(new[] { main }));
        var other = main with { Roles = main.Roles with { OsMain = false } };
        Assert.Equal($"drives {StatusText.ScreenName(other)}", Components.DrivesLine(new[] { other }));
        Assert.Null(Components.DrivesLine(Array.Empty<Screen>()));
        Assert.Equal("Acme · version 1.2", Components.PublisherLine("Acme", "1.2"));
        Assert.Null(Components.PublisherLine("", ""));
    }

    [Theory]
    [InlineData("ACTIVE", PlainState.InUse)]
    [InlineData("LOADED", PlainState.Ready)]
    [InlineData("NOT_ATTEMPTED", PlainState.Standby)]
    [InlineData("DECLINED", PlainState.Standby)]
    [InlineData("", PlainState.Standby)]
    [InlineData("DISABLED", PlainState.TurnedOff)]
    [InlineData("UNTRUSTED", PlainState.NeedsAttention)]
    [InlineData("LOAD_FAILED", PlainState.NeedsAttention)]
    [InlineData("ABI_MISMATCH", PlainState.NeedsAttention)]
    public void ADisplayProcessorsLoadVerdict_IsOnePlainWord(string load, PlainState state) =>
        Assert.Equal(state, PlainStates.DisplayProcessor(load, null));

    [Fact]
    public void PlainStates_ThePolicyRowDecidesFirst_ThenTheClass()
    {
        PolicyRow R(bool enabled, string load = "ACTIVE") =>
            new(ComponentClass.Display, "x", "x", enabled, "user", false, false, load, "signed", null, null);
        // Turned off by its Enabled value, refused by the provenance gate: whatever the class says.
        Assert.Equal(PlainState.TurnedOff, PlainStates.DisplayProcessor("ACTIVE", R(false)));
        Assert.Equal(PlainState.NeedsAttention, PlainStates.InputProvider(true, true, R(true, "UNTRUSTED")));
        Assert.Equal(PlainState.TurnedOff, PlainStates.TrackingProvider(true, true, R(false)));

        // In use needs its hardware: selected without a device (absent or not known) is Ready.
        Assert.Equal(PlainState.InUse, PlainStates.InputProvider(true, true, null));
        Assert.Equal(PlainState.Ready, PlainStates.InputProvider(true, false, null));
        Assert.Equal(PlainState.Ready, PlainStates.InputProvider(true, null, null));
        Assert.Equal(PlainState.Ready, PlainStates.InputProvider(false, true, null));
        Assert.Equal(PlainState.Standby, PlainStates.InputProvider(false, null, null));
        Assert.Equal(PlainState.Standby, PlainStates.InputProvider(false, false, null));

        Assert.Equal(PlainState.InUse, PlainStates.TrackingProvider(true, false, null));
        Assert.Equal(PlainState.Ready, PlainStates.TrackingProvider(false, true, null));
        Assert.Equal(PlainState.Standby, PlainStates.TrackingProvider(false, false, null));

        Assert.Equal(PlainState.InUse, PlainStates.ConversionProvider("LOADED", true, false, false, true, Level.Ok, null));
        Assert.Equal(PlainState.Ready, PlainStates.ConversionProvider("BUILT_IN", true, false, false, false, Level.Plain, null));
        Assert.Equal(PlainState.Standby, PlainStates.ConversionProvider("LOADED", true, true, false, false, Level.Plain, null)); // superseded
        Assert.Equal(PlainState.TurnedOff, PlainStates.ConversionProvider("NOT_ATTEMPTED", false, false, false, false, Level.Plain, null));
        Assert.Equal(PlainState.NeedsAttention, PlainStates.ConversionProvider("LOADED", true, false, true, false, Level.Plain, null)); // malformed
        Assert.Equal(PlainState.NeedsAttention, PlainStates.ConversionProvider("LOADED", true, false, false, true, Level.Warn, null)); // a failing task

        Assert.Equal(PlainState.InUse, PlainStates.Controller(true));
        Assert.Equal(PlainState.Standby, PlainStates.Controller(false));
        Assert.Equal(PlainState.Ready, PlainStates.ConversionModule(false, true, "ready"));
        Assert.Equal(PlainState.NeedsAttention, PlainStates.ConversionModule(false, true, "unlicensed"));
        Assert.Equal(PlainState.TurnedOff, PlainStates.ConversionModule(false, false, "disabled"));
        Assert.Equal(PlainState.Ready, PlainStates.ModelPack("installed"));
        Assert.Equal(PlainState.Standby, PlainStates.ModelPack("downloading"));

        Assert.Equal(new[] { "In use", "Ready", "Standby", "Turned off", "Needs attention" },
            new[] { PlainState.InUse, PlainState.Ready, PlainState.Standby, PlainState.TurnedOff, PlainState.NeedsAttention }
                .Select(x => PlainStates.Chip(x)!.Value.Text));
        Assert.Null(PlainStates.Chip(PlainState.None));
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
        Assert.EndsWith("2 installed", wc.ConnectedLine);
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
        Assert.Equal("2 installed · no device connected", input.ConnectedLine);
        var conv = Section(all, "conversion");
        Assert.Equal("neurd-directml", Assert.Single(conv.Items).Name); // backend verbatim
        Assert.Equal(1, conv.Count);
        Assert.Contains("modes 0x3 · state 1 · 32 streams · up to 8 views", conv.Items[0].Lines);
    }

    [Fact]
    public void ModuleInfo_NewInfo_ShowsWhichNeurD_OldInfo_ShowsNothingNew()
    {
        // New runtime + plug-in: lift_caps carries module_status / _version / _detail.
        var info = Info("info-lift-module.json");
        Assert.Equal("ready", info.Lift!.ModuleStatus);
        Assert.Equal("0.4.9-v0.4.9-QA+190.20bf39e", info.Lift.ModuleVersion);
        var row = Assert.Single(Components.Conversion(info).Items);
        Assert.Contains(("module ready", Level.Ok), row.Chips);
        Assert.Equal("Module 0.4.9-v0.4.9-QA+190.20bf39e · License: Leia internal use only", row.Lines[0]);

        // Today's output (no module keys): nothing new — no chip, no line, no "unknown".
        var old = Info("info.json");
        Assert.Null(old.Lift!.ModuleStatus);
        var oldRow = Assert.Single(Components.Conversion(old).Items);
        Assert.Single(oldRow.Chips);
        Assert.DoesNotContain(oldRow.Lines, x => x.StartsWith("Module ", StringComparison.Ordinal));
    }

    [Fact]
    public void ModuleInfo_AbsentAndUnknownStatuses()
    {
        Assert.True(InfoResult.TryParse("""
            { "lift_caps": { "probed": true, "malformed": false, "note": "", "modes": 0, "state": 0, "backend": "",
                             "module_status": "absent", "module_version": "",
                             "module_detail": "NeurD is not installed (NeurD.dll not found)" } }
            """, out var absent, out _));
        var row = Assert.Single(Components.Conversion(absent!.Components).Items);
        Assert.Contains(("none", Level.Plain), row.Chips);
        Assert.Contains(("module not installed", Level.Plain), row.Chips);
        Assert.Equal("NeurD is not installed (NeurD.dll not found)", row.Lines[0]); // no version: detail alone

        // A status this dashboard does not know: neutral chip, raw text, never a crash.
        var lift = new LiftModule(true, false, "neurd-directml", 3, 2, 32, 8, "", "quantum", "9.9.9", "from the future");
        Assert.Equal(("quantum", Level.Plain), lift.ModuleChip);
        Assert.Contains(("quantum", Level.Plain),
            Components.Conversion(InfoComponents.Empty with { Lift = lift }).Items[0].Chips);
        Assert.Equal(("module too old", Level.Warn), (lift with { ModuleStatus = "too_old" }).ModuleChip);
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

    /// <summary>
    /// Found on real hardware: the selected input add-on read "In use" while its section said "no
    /// device connected". <c>input list --json</c> <c>active</c> is only the selected one; whether its
    /// device is there is <c>info --json</c> <c>input_providers.hardware_present</c> (+ <c>active_id</c>).
    /// </summary>
    [Fact]
    public void SelectedInputAddOn_WithoutItsDevice_IsReady_NotInUse()
    {
        var policy = ComponentPolicyTests.Parse(ComponentClass.Input, "input-list-n3e.json");
        var selected = policy.Rows.Single(r => r.Active);
        InfoComponents Summary(bool hw, string? activeId) => InfoComponents.Empty with
        {
            Input = new InputSummary(policy.Rows.Count, hw, true, false, activeId, ""),
        };

        // No device: the section says so, and so does the selected row.
        var absent = Components.InputProviders(Summary(false, null), policy);
        Assert.Equal($"{policy.Rows.Count} installed{StatusText.Sep}no device connected", absent.ConnectedLine);
        var row = absent.Items.Single(i => i.Name == (selected.Name.Length > 0 ? selected.Name : selected.Id));
        Assert.Equal(PlainState.Ready, row.State);
        Assert.Equal("No device connected", row.Summary);
        Assert.DoesNotContain(absent.Items, i => i.State == PlainState.InUse);

        // Its device is present: In use, and the face line is the publisher line again.
        var present = Components.InputProviders(Summary(true, selected.Id), policy);
        var inUse = present.Items.Single(i => i.Name == row.Name);
        Assert.Equal(PlainState.InUse, inUse.State);
        Assert.NotEqual("No device connected", inUse.Summary);

        // The summary is about another provider: this one's hardware is not known, so not In use.
        Assert.Null(Components.InputHardware(Summary(true, "someone-else"), selected.Id));
        var unknown = Components.InputProviders(Summary(true, "someone-else"), policy).Items.Single(i => i.Name == row.Name);
        Assert.Equal(PlainState.Ready, unknown.State);
        Assert.NotEqual("No device connected", unknown.Summary); // not known: the face claims nothing
        // Info not read at all: not known either.
        Assert.Null(Components.InputHardware(InfoComponents.Empty, selected.Id));
        Assert.NotEqual("No device connected", Components.InputProviders(InfoComponents.Empty, policy).Items.Single(i => i.Name == row.Name).Summary);
    }

    [Fact]
    public void MissingEverything_IsTolerated()
    {
        Assert.True(InfoResult.TryParse("{}", out var bare, out _));
        var all = Components.Build(null, bare!.Components);
        Assert.All(all, s => Assert.Empty(s.Items));
        Assert.Equal("not read yet", Section(all, "input_providers").ConnectedLine);
        Assert.Equal("shown when DisplayXR runs in the background", Section(Components.RunningPrograms(null), "present_owners").ConnectedLine);

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
        Assert.Equal("not running · turned off", wc.ConnectedLine);
    }
}

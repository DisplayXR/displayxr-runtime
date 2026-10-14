// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Linq;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>ADR-053 (#2006): the snapshot's conversion rows and the Components page section built from them.</summary>
public class ConversionProvidersTests
{
    private const string WithConv = """
    {
      "schema": 1, "source": "service",
      "generation": { "topology": 1, "status": 2, "conversion": 9 },
      "runtime": { "version": "2.40.0", "git_tag": "v2.40.0", "plugin_abi": 5 },
      "plugins": [], "screens": [], "clients": [], "workspace": { "enabled": false, "controller": null, "screens": [] },
      "warnings": [ { "code": "CONV_TASK_UNSERVED", "level": "warn", "text": "No conversion provider can serve splats-mono now." } ],
      "conv_providers": [
        { "id": "dp-lift", "name": "Display plug-in lift slots (legacy adapter)", "vendor": "", "version": "", "load": "BUILT_IN",
          "hosting": "in_service", "status": "READY", "hint": "", "enabled": true, "fallback": false, "hidden": false,
          "licence_exempt": true, "probe_order": 150, "cap_count": 3, "bad_descriptors": 0, "descriptor_error": null,
          "backend": "dp-lift", "licence_class": "UNKNOWN", "host_pid": 0, "restarts": 0 },
        { "id": "sim-conversion", "name": "DisplayXR Sim Conversion", "vendor": "DisplayXR", "version": "ci", "load": "NEGOTIATED",
          "hosting": "in_service", "status": "READY", "hint": "", "enabled": true, "fallback": true, "hidden": false,
          "licence_exempt": false, "probe_order": 250, "cap_count": 1, "bad_descriptors": 1,
          "descriptor_error": "VIEWS_* without EXPLICIT_VIEWPOINTS", "backend": "cpu", "licence_class": "PERMISSIVE",
          "host_pid": 0, "restarts": 0 }
      ],
      "conv_tasks": [
        { "provider_index": 0, "provider": "dp-lift", "task": "views-mono", "cap_id": 3, "composed": false, "state": "WARMING",
          "reason": "none", "progress": 0.5, "selected": true, "order": 0, "verdict": "selected", "licence_class": "UNKNOWN",
          "streams": 1, "jobs_queued": 0, "cpu_staging": false },
        { "provider_index": 1, "provider": "sim-conversion", "task": "views-mono", "cap_id": 1, "composed": true, "state": "FAILED",
          "reason": "crash_loop", "progress": 0, "selected": false, "order": 1, "verdict": "behind", "licence_class": "PERMISSIVE",
          "streams": 0, "jobs_queued": 0, "cpu_staging": true }
      ]
    }
    """;

    [Fact]
    public void ParsesTheConversionRows()
    {
        var s = TestData.Parse(WithConv);
        Assert.True(s.ConvKnown);
        Assert.Equal(9ul, s.Generation.Conversion);
        Assert.Equal(2, s.Providers.Count);
        Assert.Equal("dp-lift", s.Providers[0].Id);
        Assert.True(s.Providers[0].LicenceExempt);
        Assert.Equal(1, s.Providers[1].BadDescriptors);
        Assert.Equal(2, s.Tasks.Count);
        Assert.True(s.Tasks[0].Selected);
        Assert.Equal(0, s.Tasks[0].ProviderIndex);
        Assert.True(s.Tasks[1].CpuStaging);
        Assert.Equal(1, s.Tasks[1].Order);
    }

    [Fact]
    public void AnOlderCliHasNoConversionSection()
    {
        var s = TestData.Parse(TestData.DesignExample);
        Assert.False(s.ConvKnown);
        Assert.Empty(s.Providers);
        Assert.Equal(0ul, s.Generation.Conversion);
        Assert.DoesNotContain(Components.Build(s, null), c => c.Id == "conversion_providers");
    }

    [Fact]
    public void ComponentsListsProvidersWithTaskReadiness()
    {
        var s = TestData.Parse(WithConv);
        var sec = Components.Build(s, null).Single(c => c.Id == "conversion_providers");
        Assert.Equal(2, sec.Count);
        Assert.Equal(0, sec.Connected); // the selected task is WARMING, not READY
        Assert.Contains("1 task served", sec.ConnectedLine);

        var lift = sec.Items[0];
        Assert.Equal("Display plug-in lift slots (legacy adapter)", lift.Name);
        Assert.Contains(lift.Lines, l => l.StartsWith("views-mono: warming 50%") && l.Contains("selected") && l.Contains("1 streams"));
        Assert.Contains(lift.Lines, l => l.Contains("licence: grandfathered"));

        var sim = sec.Items[1];
        Assert.Equal(Level.Critical, sim.Level); // a malformed descriptor
        Assert.Contains(sim.Chips, c => c.Text == "malformed");
        Assert.Contains(sim.Chips, c => c.Text == "FALLBACK");
        Assert.Contains(sim.Lines, l => l.Contains("rejected: VIEWS_* without EXPLICIT_VIEWPOINTS"));
        Assert.Contains(sim.Lines, l => l.Contains("crash loop") && l.Contains("#2 (behind)") && l.Contains("CPU staging"));
    }

    [Fact]
    public void TaskChipsColourByReadiness()
    {
        static ConvTask T(string state, string reason = "none") =>
            new(0, "p", "depth-video", 1, false, state, reason, 0, true, 0, "selected", "PERMISSIVE", 0, 0, false);
        Assert.Equal(Level.Ok, Components.TaskChip(T("READY")).Level);
        Assert.Equal(Level.Info, Components.TaskChip(T("INSTALLED")).Level);
        Assert.Equal(Level.Warn, Components.TaskChip(T("FAILED", "model")).Level);
        Assert.Equal(Level.Critical, Components.TaskChip(T("FAILED", "crash_loop")).Level);
        Assert.Equal(("something_new", Level.Plain), Components.TaskChip(T("SOMETHING_NEW")));
    }
    private const string WithPacks = """
    {
      "schema": 1, "source": "service",
      "generation": { "topology": 1, "status": 3, "conversion": 4 },
      "runtime": { "version": "2.43.0", "git_tag": "v2.43.0", "plugin_abi": 5 },
      "plugins": [], "screens": [], "clients": [], "workspace": { "enabled": false, "controller": null, "screens": [] },
      "warnings": [], "conv_providers": [], "conv_tasks": [],
      "conv_model_store": { "root": "C:\\ProgramData\\DisplayXR\\Models", "source": "%ProgramData% default" },
      "conv_packs": [
        { "pack": "dxr-open-depth", "provider": "open-conversion", "state": "downloading", "models": 4, "models_installed": 2,
          "research_models": 1, "bytes_total": 838860800, "bytes_installed": 115343360, "download_done": 31457280,
          "download_total": 62914560, "progress": 0.5 },
        { "pack": "sim", "provider": "sim-conversion", "state": "evicted", "models": 1, "models_installed": 0,
          "research_models": 0, "bytes_total": 1024, "bytes_installed": 0, "download_done": 0, "download_total": 0,
          "progress": -1 },
        { "pack": "vendor", "provider": "x", "state": "installed", "models": 1, "models_installed": 1,
          "research_models": 0, "bytes_total": 2048, "bytes_installed": 2048, "download_done": 0, "download_total": 0,
          "progress": -1 }
      ]
    }
    """;

    [Fact]
    public void ModelPacksShowInstalledDownloadingEvicted()
    {
        var s = TestData.Parse(WithPacks);
        Assert.Equal(3, s.Packs.Count);
        Assert.Equal(@"C:\ProgramData\DisplayXR\Models", s.ModelStoreRoot);
        var sec = Components.Build(s, null).Single(c => c.Id == "conversion_models");
        Assert.Equal(3, sec.Count);
        Assert.Equal(1, sec.Connected);
        Assert.StartsWith("1 of 3 packs installed", sec.ConnectedLine);

        var open = sec.Items[0];
        Assert.Equal("dxr-open-depth", open.Name);
        Assert.Contains(open.Chips, c => c.Text == "downloading 50%" && c.Level == Level.Info);
        Assert.Contains(open.Lines, l => l.Contains("2 of 4 models installed") && l.Contains("110 MB of 800 MB"));
        Assert.Contains(open.Lines, l => l == "downloading 30 MB of 60 MB");
        Assert.Contains(open.Lines, l => l.Contains("non-commercial"));

        Assert.Equal(Level.Warn, sec.Items[1].Level);
        Assert.Contains(sec.Items[1].Chips, c => c.Text.StartsWith("evicted"));
        Assert.Equal(Level.Ok, sec.Items[2].Level);
    }

    [Fact]
    public void AnOlderCliHasNoModelsSection()
    {
        var s = TestData.Parse(WithConv);
        Assert.Empty(s.Packs);
        Assert.DoesNotContain(Components.Build(s, null), c => c.Id == "conversion_models");
    }
}

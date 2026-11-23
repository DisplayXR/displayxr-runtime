// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Linq;
using System.Text.Json.Nodes;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>
/// ADR-045 Amendment 2026-11-22: a display plug-in's platform remedy (update recommended /
/// required) as the CLI's plug-in rows carry it — flat keys on `status --json` and `info --json`.
/// </summary>
public class PlatformRemedyTests
{
    private static string WithRemedy(string json, int index, bool required, string from, string to, string label, string url)
    {
        var root = JsonNode.Parse(json)!;
        var p = root["plugins"]![index]!;
        p["update_recommended"] = !required;
        p["update_required"] = required;
        p["installed_version"] = from;
        p["required_version"] = to;
        p["action_label"] = label;
        p["action_url"] = url;
        return root.ToJsonString();
    }

    [Fact]
    public void Snapshot_WithoutTheKeys_ReadsNoRemedy()
    {
        var s = TestData.Parse(TestData.DesignExample); // a CLI that predates the amendment
        Assert.All(s.Plugins, p => Assert.Same(PlatformRemedy.None, p.Remedy));
        Assert.Null(StatusText.RemedyChip(s.Plugins[0].Remedy));
        Assert.Null(StatusText.RemedyLine(s.Plugins[0].Remedy));
    }

    [Fact]
    public void Snapshot_ReadsTheRemedyVerbatim()
    {
        var s = TestData.Parse(WithRemedy(TestData.DesignExample, 0, true, "1.2.0", "1.4.0", "Download the update",
                                          "https://example.com/update"));
        var r = s.Plugins[0].Remedy;
        Assert.True(r.Any);
        Assert.True(r.UpdateRequired);
        Assert.False(r.UpdateRecommended);
        Assert.Equal(("1.2.0", "1.4.0", "Download the update"), (r.InstalledVersion, r.RequiredVersion, r.ActionLabel));
        Assert.Equal("https://example.com/update", r.Url);
        Assert.Equal(("Update required", Level.Warn), StatusText.RemedyChip(r));
        Assert.Equal("Version 1.2.0 is installed; 1.4.0 is required.", StatusText.RemedyLine(r));
        Assert.Same(PlatformRemedy.None, s.Plugins[1].Remedy);
        // Advisory: the plug-in's own level is the platform state's, untouched by the remedy.
        Assert.Equal(Level.Ok, StatusText.PluginLevel(s.Plugins[0]));
    }

    [Fact]
    public void Remedy_PlainWords_LeaveOutEmptyParts()
    {
        PlatformRemedy R(string from, string to) => new(true, false, from, to, "", "");
        Assert.Equal(("Update available", Level.Info), StatusText.RemedyChip(R("", "")));
        Assert.Equal("Version 1.2.0 is installed; an update is available.", StatusText.RemedyLine(R("1.2.0", "")));
        Assert.Equal("Version 1.4.0 is available.", StatusText.RemedyLine(R("", "1.4.0")));
        Assert.Equal("An update for the display maker's software is available.", StatusText.RemedyLine(R("", "")));
    }

    [Fact]
    public void Remedy_OpensOnlyHttps()
    {
        Assert.Null(new PlatformRemedy(true, false, "", "", "Get it", "http://example.com/update").Url);
        Assert.Null(new PlatformRemedy(true, false, "", "", "Get it", "file:///C:/setup.exe").Url);
        Assert.Null(new PlatformRemedy(true, false, "", "", "Get it", "").Url);
    }

    [Fact]
    public void Remedy_FlagsOffMeansNone_EvenWithStrings()
    {
        var json = WithRemedy(TestData.DesignExample, 0, false, "1.2.0", "", "", "");
        var root = JsonNode.Parse(json)!;
        root["plugins"]![0]!["update_recommended"] = false;
        Assert.Same(PlatformRemedy.None, TestData.Parse(root.ToJsonString()).Plugins[0].Remedy);
    }

    [Fact]
    public void Info_ReadsTheRemedy()
    {
        var json = WithRemedy(TestData.Text("info.json"), 0, false, "1.2.0", "1.4.0", "Download",
                              "https://example.com/u");
        Assert.True(InfoResult.TryParse(json, out var info, out var err), err);
        var r = info!.Plugins[0].Remedy;
        Assert.True(r.UpdateRecommended);
        Assert.Equal("https://example.com/u", r.Url);
        Assert.True(InfoResult.TryParse(TestData.Text("info.json"), out var old, out _));
        Assert.All(old!.Plugins, p => Assert.False(p.Remedy.Any));
    }

    [Fact]
    public void ComponentsRow_ListsTheUpdate()
    {
        var s = TestData.Parse(WithRemedy(TestData.DesignExample, 0, false, "1.2.0", "1.4.0", "", ""));
        var row = Components.DisplayProcessors(s).Items.First(i => i.Id == s.Plugins[0].Id);
        Assert.Contains("Version 1.2.0 is installed; 1.4.0 is available.", row.Lines);
    }
}

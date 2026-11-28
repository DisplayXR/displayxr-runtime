// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>
/// conversion-provider-discovery.md §5.6.5: the row of a model built on this PC (research preview),
/// read from <c>conv models list --json</c> and driven through the CLI; §5.6.6: a research catalog
/// item is never suggested.
/// </summary>
public class ModelBuildsTests
{
    private const string LicenceText = "Research licence text.\n";
    private static readonly string LicenceSha = Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(LicenceText))).ToLowerInvariant();

    // What `displayxr-cli conv models list --json` prints for one buildable model (cli_conv_models_build.cpp).
    private static string ListJson(bool accepted = false, bool research = false, bool locked = false, bool toolInstalled = true,
                                   bool toolCurrent = true, bool installed = false, long physMb = 32768, long freeMb = 200000,
                                   string textSha = "") => $$"""
    {
      "store_root": "C:\\ProgramData\\DisplayXR\\Models", "source": "%ProgramData%",
      "research": { "allow_research_only": {{(research ? "true" : "false")}}, "locked_off": {{(locked ? "true" : "false")}} },
      "machine": { "phys_mb": {{physMb}}, "free_mb": {{freeMb}}, "os": "windows", "os_version": "10.0.26100" },
      "packs": [ { "pack": "dxr-open-sharp", "provider": "open-conversion", "state": "absent",
        "manifest": "C:\\Program Files\\DisplayXR\\Conversion\\dxr-open-sharp.models.json",
        "models": [
          { "name": "depth-small", "task": "depth-mono", "commercial": true, "prefetch": true, "installed": true, "size": 100, "files": [] },
          { "name": "sharp-int4", "task": "splats-mono", "commercial": false, "prefetch": false, "installed": {{(installed ? "true" : "false")}},
            "size": 734003200, "files": [],
            "local_build": { "recipe": "sharp-onnx", "variant": "int4",
              "tool": { "id": "open-model-builder", "catalog_id": "open-conversion-model-builder", "min_version": "1.0.0" },
              "tool_installed": {{(toolInstalled ? "true" : "false")}}, "tool_version": "1.1.0", "tool_current": {{(toolCurrent ? "true" : "false")}},
              "needs": { "ram_mb": 8192, "disk_mb": 10240, "os": ["windows>=10.0.19041"] },
              "download_size": 2809738232,
              "sources": [ { "role": "checkpoint", "size": 2809738232, "url": "https://example.invalid/c.pt",
                "license": { "id": "apple-amlr", "name": "Apple Machine Learning Research Model licence",
                  "link": "https://example.invalid/LICENSE", "text": "C:\\Program Files\\DisplayXR\\Conversion\\amlr.txt",
                  "text_sha256": "{{(textSha.Length > 0 ? textSha : LicenceSha)}}", "text_verified": true,
                  "accepted": {{(accepted ? "true" : "false")}}{{(accepted ? ", \"accepted_at\": 1764201600" : "")}} } } ] },
            "license_accepted": {{(accepted ? "true" : "false")}} }
        ] } ]
    }
    """;

    private static ModelBuildList Parse(string json)
    {
        Assert.True(ModelBuildList.TryParse(json, out var list));
        return list!;
    }

    private static byte[]? Reader(string path) => Encoding.UTF8.GetBytes(LicenceText);

    private static ModelBuildRow Row(ModelBuildList list, bool ticked = false, bool building = false, Func<string, byte[]?>? read = null) =>
        ModelBuildRow.For(Assert.Single(list.Models), list, ticked, building, read ?? Reader, ModelBuildRow.DriveOf(list.StoreRoot));

    [Fact]
    public void ListParsesOnlyBuildableModels()
    {
        var list = Parse(ListJson());
        var m = Assert.Single(list.Models);
        Assert.Equal("dxr-open-sharp/sharp-int4", m.Key);
        Assert.Equal("open-conversion-model-builder", m.ToolCatalogId);
        Assert.Equal(8192, m.NeedsRamMb);
        Assert.Equal(10240, m.NeedsDiskMb);
        Assert.Equal(2809738232, m.DownloadSize);
        Assert.Equal("apple-amlr", Assert.Single(m.Licences).Id);
        Assert.False(list.AllowResearchOnly);
        Assert.Equal(32768, list.PhysMb);
        Assert.False(ModelBuildList.TryParse("not json", out _));
    }

    [Fact]
    public void RowSaysWhatTheSpecSays()
    {
        var r = Row(Parse(ListJson()));
        Assert.Equal("Research preview (non-commercial)", r.Badge);
        Assert.Equal("sharp-int4", r.Title);
        Assert.Contains("Built on this PC from the official file. DisplayXR does not host this model.", r.Lines);
        Assert.Contains(r.Lines, l => l.StartsWith("Model and licence: Apple Machine Learning Research Model licence, https://", StringComparison.Ordinal));
        Assert.Equal(new[] { "Download", "Space needed while building", "Memory needed to build", "Size when installed", "Licence" },
                     r.Facts.Select(f => f.Label).ToArray());
        Assert.Equal("10 GB", r.Facts.First(f => f.Label == "Space needed while building").Text);
        Assert.Equal("8 GB", r.Facts.First(f => f.Label == "Memory needed to build").Text);
        Assert.Equal(LicenceText, r.LicenceText);
    }

    [Fact]
    public void TickIsNeverPreTickedAndBuildWaitsForIt()
    {
        var list = Parse(ListJson());
        var r = Row(list);
        Assert.True(r.TickShown);
        Assert.Equal("I have read this licence. I will use this model for non-commercial research only, and I allow DisplayXR to use research-only models on this account.", r.TickText);
        Assert.Contains(ModelBuildAction.Build, r.Actions);
        Assert.False(r.BuildEnabled);
        Assert.True(Row(list, ticked: true).BuildEnabled);
    }

    [Fact]
    public void AnAcceptedLicenceReadsAcceptedWithWithdraw()
    {
        var r = Row(Parse(ListJson(accepted: true, research: true)));
        Assert.False(r.TickShown);
        Assert.StartsWith("Licence accepted on ", r.AcceptedLine);
        Assert.Equal("apple-amlr", r.WithdrawLicence?.Id);
        Assert.True(r.BuildEnabled);
        // The gate turned off since (Settings): the tick is needed again to turn it back on.
        var off = Row(Parse(ListJson(accepted: true, research: false)));
        Assert.True(off.TickShown);
        Assert.False(off.BuildEnabled);
    }

    [Fact]
    public void AnUnverifiedLicenceTextHidesBuild()
    {
        var r = Row(Parse(ListJson()), ticked: true, read: _ => Encoding.UTF8.GetBytes("another text"));
        Assert.Null(r.LicenceText);
        Assert.Equal("The licence text could not be verified.", r.LicenceNote);
        Assert.DoesNotContain(ModelBuildAction.Build, r.Actions);
        Assert.False(r.TickShown);
        Assert.Null(Row(Parse(ListJson()), read: _ => null).LicenceText);
    }

    [Fact]
    public void MachineCheckBeforeBuild()
    {
        var mem = Row(Parse(ListJson(physMb: 4096)), ticked: true);
        Assert.False(mem.BuildEnabled);
        Assert.Equal("This PC has 4 GB of memory; building needs 8 GB.", mem.Blocker);
        var disk = Row(Parse(ListJson(freeMb: 2048)), ticked: true);
        Assert.False(disk.BuildEnabled);
        Assert.Equal("Building needs 10 GB free on C:; 2 GB is free.", disk.Blocker);
        var locked = Row(Parse(ListJson(locked: true)), ticked: true);
        Assert.Equal("Your administrator has turned off research-only models.", locked.Blocker);
        Assert.DoesNotContain(ModelBuildAction.Build, locked.Actions);
        Assert.False(locked.TickShown);
    }

    [Fact]
    public void OneActionAtATime()
    {
        Assert.Equal(new[] { ModelBuildAction.GetBuilder }, Row(Parse(ListJson(toolInstalled: false))).Actions);
        Assert.Equal(new[] { ModelBuildAction.UpdateBuilder }, Row(Parse(ListJson(toolCurrent: false))).Actions);
        Assert.Equal(new[] { ModelBuildAction.Cancel }, Row(Parse(ListJson()), building: true).Actions);
        Assert.Equal(new[] { ModelBuildAction.Remove }, Row(Parse(ListJson(installed: true))).Actions);
    }

    [Fact]
    public void CommandsAreTheCliVerbs()
    {
        var m = Parse(ListJson()).Models[0];
        var ticked = ModelBuildRow.BuildCommands(m, ticked: true);
        Assert.Equal(new[] { "conv", "research", "on" }, ticked[0].Args);
        Assert.Equal(new[] { "conv", "models", "fetch", m.Manifest, "--model", "sharp-int4", "--accept-license", "apple-amlr", "--events" },
                     ticked[1].Args);
        var plain = Assert.Single(ModelBuildRow.BuildCommands(m, ticked: false));
        Assert.Equal(new[] { "conv", "models", "fetch", m.Manifest, "--model", "sharp-int4", "--events" }, plain.Args);
        Assert.DoesNotContain("--installer", ticked.SelectMany(c => c.Args));
        Assert.Equal(new[] { "conv", "models", "cancel", m.Manifest, "--model", "sharp-int4" }, ModelBuildCommands.Cancel(m).Args);
        Assert.Equal(new[] { "conv", "models", "remove", m.Manifest, "--model", "sharp-int4", "--json" }, ModelBuildCommands.Remove(m).Args);
        Assert.Equal(new[] { "conv", "models", "license", "withdraw", "apple-amlr", "--json" }, ModelBuildCommands.Withdraw(m.Licences[0]).Args);
        Assert.Equal(new[] { "conv", "models", "list", "--json" }, ModelBuildCommands.List.Args);
    }

    [Fact]
    public void ProgressFollowsTheEventStream()
    {
        var p = BuildProgress.Starting
            .Apply("""{"v":1,"event":"phase","phase":"download-source","variant":null,"step":1,"steps":1}""")
            .Apply("""{"v":1,"event":"progress","phase":"download-source","done":2097152,"total":4194304}""");
        Assert.Equal("Downloading the official file: 2 of 4 MB", p.Text);
        Assert.Equal(0.5, p.Fraction);
        p = p.Apply("""{"v":1,"event":"start","tool":"open-model-builder","version":"1.0.0","recipe":"sharp-onnx","variants":["int4"]}""")
             .Apply("""{"v":1,"event":"phase","phase":"convert","variant":"int4","step":2,"steps":3}""");
        Assert.Equal("Converting on this PC: step 2 of 3 (convert)", p.Text);
        Assert.Null(p.Fraction);
        p = p.Apply("""{"v":1,"event":"progress","phase":"convert","done":0,"total":0}""");
        Assert.Null(p.Fraction); // total 0 = indeterminate
        p = p.Apply("not json").Apply("""{"v":2,"event":"phase","phase":"x"}""");
        Assert.Equal("Converting on this PC: step 2 of 3 (convert)", p.Text);
        p = p.Apply("""{"v":1,"event":"phase","phase":"ingest","variant":null,"step":1,"steps":1}""");
        Assert.Equal("Checking and storing", p.Text);
        p = p.Apply("""{"v":1,"event":"result","models":[{"name":"sharp-int4","result":"build-stalled","hint":"h","build":{"log":"C:\\logs\\b.log","exit_code":-1}}],"failed":1}""");
        Assert.Equal("build-stalled", p.Result);
        Assert.Equal("h", p.Hint);
        Assert.Equal("C:\\logs\\b.log", p.Log);
    }

    [Theory]
    [InlineData("build-license", "The licence was not accepted. Tick the box to build.")]
    [InlineData("build-download", "The official file could not be downloaded. Check the connection and try again.")]
    [InlineData("bad-download", "The official file could not be downloaded. Check the connection and try again.")]
    [InlineData("build-source-mismatch", "The downloaded file is not the expected one, so nothing was built.")]
    [InlineData("build-output-mismatch", "The built model did not match the expected files, so it was not installed. Update the builder and try again.")]
    [InlineData("build-memory", "This PC ran out of memory while building. Close other apps and try again.")]
    [InlineData("build-disk", "Not enough free space on C:. Free 10 GB and try again.")]
    [InlineData("build-unsupported", "This PC cannot build this model.")]
    [InlineData("build-cancelled", "Cancelled. Nothing was installed.")]
    [InlineData("build-tool-missing", "Get the builder first.")]
    [InlineData("build-tool-old", "Update the builder first.")]
    [InlineData("build-tool-untrusted", "The installed builder could not be verified. Reinstall it from the Catalog.")]
    [InlineData("build-tool-mismatch", "The installed builder could not be verified. Reinstall it from the Catalog.")]
    [InlineData("build-stalled", "The build stopped unexpectedly. Try again; the log is in C:\\l.log.")]
    [InlineData("build-failed", "The build stopped unexpectedly. Try again; the log is in C:\\l.log.")]
    [InlineData("build-license-text", "The licence text could not be verified.")]
    [InlineData("policy-locked", "Your administrator has turned off research-only models.")]
    [InlineData("busy", "Another build of this model is running on this PC. Try again when it has finished.")]
    [InlineData("io", "The model could not be stored. Try again; the log is in C:\\l.log.")]
    public void OneMessagePerResult(string result, string text)
    {
        Assert.Equal(text, ModelBuildMessages.For(result, "C:", 10240, "C:\\l.log")?.Text);
    }

    [Fact]
    public void SuccessHasNoFailureMessage()
    {
        Assert.Null(ModelBuildMessages.For("built", "C:", 0, null));
        Assert.Null(ModelBuildMessages.For("ok", "C:", 0, null));
    }

    // §5.6.6: dxr-get's row for the model builder.
    private const string CatalogJson = """
    {"schema":2,"items":[
      {"id":"open-conversion-model-builder","name":"Model builder","class":"model_builder","registered_id":"open-model-builder",
       "publisher":"displayxr","tier":"listed","installed":null,"available":"1.0.0","state":"not_installed","installable":true,
       "receipt":false,"aliases":[],"recommended":true,"research":true,"badge":"Research preview (non-commercial)",
       "detected":"high","evidence":"x","size":3000000,"download":3000000,"compatible":true,"prerequisites":[]}
    ]}
    """;

    [Fact]
    public void ResearchCatalogItemIsNeverSuggested()
    {
        Assert.True(CatalogList.TryParse(CatalogJson, out var list));
        var i = Assert.Single(list!.Items);
        Assert.True(i.Research);
        Assert.False(i.Recommended);  // even when an engine says so
        Assert.False(i.DetectedHere);
        Assert.Null(i.Evidence);
        Assert.Equal("Research preview (non-commercial)", i.ResearchBadge);
        Assert.Equal("Model builders", CatalogClasses.Label("model_builder"));
    }

    [Fact]
    public void TheBuilderSitsWithTheModelsItBuilds()
    {
        Assert.True(CatalogList.TryParse(CatalogJson, out var list));
        var source = new CatalogSource(list, false, true);
        var models = new ComponentSection("conversion_models", "2D-to-3D models", "", 0, 0, "", Array.Empty<ComponentItem>());
        var groups = ComponentRows.Join(new[] { models }, source);
        Assert.Contains(groups.Single(g => g.Section.Id == "conversion_models").Rows, r => r.Catalog?.Id == "open-conversion-model-builder");
        Assert.DoesNotContain(groups, g => g.Section.Id == ComponentRows.AppsGroupId);
        // No models section: listed with the apps, never dropped.
        groups = ComponentRows.Join(Array.Empty<ComponentSection>(), source);
        Assert.Contains(groups.Single(g => g.Section.Id == ComponentRows.AppsGroupId).Rows, r => r.Catalog?.Id == "open-conversion-model-builder");
    }
}

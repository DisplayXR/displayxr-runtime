// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>
/// The 2D-to-3D rows in plain words: the built-in adapter's name and when it is not a row, the
/// conversion module's name, the section line when nothing converts, and the rule that no string
/// the Dashboard or the catalog supplies names the vendor's converter technology.
/// </summary>
public class ConverterWordingTests
{
    /// <summary>The vendor's converter technology name: never in a string the Dashboard or the catalog supplies.</summary>
    private const string Banned = "neurd";

    private const string BuiltInRegisteredName = "Display plug-in lift slots (legacy adapter)";

    private static string Provider(string id, string name, string load, bool enabled = true, bool hidden = false) =>
        $$"""
        { "id": "{{id}}", "name": "{{name}}", "vendor": "", "version": "", "load": "{{load}}", "hosting": "in_service",
          "status": "READY", "hint": "", "enabled": {{(enabled ? "true" : "false")}}, "fallback": false,
          "hidden": {{(hidden ? "true" : "false")}}, "licence_exempt": false, "probe_order": 100, "cap_count": 1,
          "bad_descriptors": 0, "descriptor_error": null, "backend": "x", "licence_class": "COMMERCIAL",
          "host_pid": 0, "restarts": 0 }
        """;

    private static string BuiltIn(bool hidden = false, string name = BuiltInRegisteredName) =>
        Provider("dp-lift", name, "BUILT_IN", hidden: hidden);

    private static string TaskRow(int provider, string id, string task, bool selected, string state = "READY", string verdict = "selected") =>
        $$"""
        { "provider_index": {{provider}}, "provider": "{{id}}", "task": "{{task}}", "cap_id": 1, "composed": false,
          "state": "{{state}}", "reason": "none", "progress": 0, "selected": {{(selected ? "true" : "false")}},
          "order": 0, "verdict": "{{verdict}}", "licence_class": "COMMERCIAL", "streams": 0, "jobs_queued": 0,
          "cpu_staging": false }
        """;

    private static StatusSnapshot Snapshot(IEnumerable<string> providers, IEnumerable<string>? tasks = null, string source = "service") =>
        TestData.Parse($$"""
        { "schema": 1, "source": "{{source}}", "generation": { "topology": 1, "status": 2, "conversion": 3 },
          "runtime": { "version": "2.70.4", "git_tag": "v2.70.4", "plugin_abi": 5 },
          "plugins": [], "screens": [], "clients": [], "workspace": { "enabled": false, "controller": null, "screens": [] },
          "warnings": [],
          "conv_providers": [ {{string.Join(",", providers)}} ],
          "conv_tasks": [ {{string.Join(",", tasks ?? Array.Empty<string>())}} ] }
        """);

    private static ComponentSection Providers(StatusSnapshot s, PolicyList? policy = null) => Components.ConversionProviders(s, policy);

    private static InfoComponents ModuleInfo(string json)
    {
        Assert.True(InfoResult.TryParse(json, out var info, out var err), err);
        return info!.Components;
    }

    /// <summary>A display driver that reports no converter of its own: probed, no modes, no backend.</summary>
    private static readonly InfoComponents NoModule = ModuleInfo("""
        { "lift_caps": { "probed": true, "malformed": false, "note": "", "modes": 0, "state": 0, "backend": "" } }
        """);

    // ── (b) the built-in adapter ────────────────────────────────────────────

    [Fact]
    public void TheBuiltInAdapter_HasAPlainName_OneLine_AndIsPartOfTheRuntime()
    {
        var s = Snapshot(new[] { BuiltIn() }, new[] { TaskRow(0, "dp-lift", "depth-mono", true) });
        var sec = Providers(s);
        var row = Assert.Single(sec.Items);
        Assert.Equal("Converter in the display driver", row.Name);
        Assert.Equal("Built into the runtime for display drivers that carry their own converter.", row.Summary);
        Assert.True(row.BuiltIn);
        Assert.Equal("dp-lift", row.Id);
        // Its registered name and id are not dropped: they are lines of Details.
        Assert.Contains(("Registered name", BuiltInRegisteredName), row.DetailFacts);
        Assert.Contains(row.DetailFacts, f => f.Label == "Registration" && f.Text.StartsWith("dp-lift", StringComparison.Ordinal));

        var joined = Assert.Single(ComponentRows.Join(new[] { sec }, null)[0].Rows);
        Assert.Equal(("Part of the runtime", Level.Plain), joined.StatusChip);
        Assert.Equal("Converter in the display driver", joined.Name);
    }

    [Fact]
    public void TheBuiltInAdapter_IsNamedByItsId_WhenTheCliGivesNoNameOrLoadVerdict()
    {
        // What a test box showed as a bare "dp-lift" row: no name from the CLI.
        var s = Snapshot(new[] { Provider("dp-lift", "", "LOADED") });
        var row = Assert.Single(Providers(s).Items);
        Assert.Equal(Components.BuiltInConverterName, row.Name);
        Assert.Equal(Components.BuiltInConverterLine, row.Summary);
        Assert.True(row.BuiltIn);
        Assert.DoesNotContain(row.DetailFacts, f => f.Label == "Registered name"); // nothing to keep
    }

    [Fact]
    public void AHiddenBuiltInAdapter_IsNotARow_AndIsNotCounted()
    {
        var s = Snapshot(new[] { BuiltIn(hidden: true), Provider("acme", "Acme Depth", "LOADED") },
                         new[] { TaskRow(1, "acme", "depth-mono", true) });
        var sec = Providers(s);
        var row = Assert.Single(sec.Items);
        Assert.Equal("Acme Depth", row.Name); // a provider's own name, verbatim
        Assert.Equal(PlainState.InUse, row.State); // the task still finds its provider by index
        Assert.Equal(1, sec.Count);
        Assert.Equal($"1 installed{StatusText.Sep}in use for 1 task{StatusText.Sep}1 ready", sec.ConnectedLine);
        Assert.DoesNotContain(ComponentRows.Join(new[] { sec }, null)[0].Rows, r => r.Name.Contains("display driver"));

        // Not hidden: it is a row again.
        var shown = Providers(Snapshot(new[] { BuiltIn(), Provider("acme", "Acme Depth", "LOADED") }));
        Assert.Equal(2, shown.Items.Count);
    }

    [Fact]
    public void AHiddenProviderThatIsNotBuiltIn_StaysARow()
    {
        var s = Snapshot(new[] { Provider("acme", "Acme Depth", "LOADED", hidden: true) });
        var row = Assert.Single(Providers(s).Items);
        Assert.Equal(PlainState.Standby, row.State);
    }

    // ── (c) nothing converts: say why ───────────────────────────────────────

    [Fact]
    public void EveryConverterTurnedOff_TheSectionSaysSo_AndNeverPrintsAZero()
    {
        // The test box: both providers turned off, the built-in adapter hidden, no task selected.
        var s = Snapshot(new[]
        {
            BuiltIn(hidden: true),
            Provider("leia-conversion", "Leia conversion", "DISABLED", enabled: false),
            Provider("open-conversion", "Open conversion provider", "DISABLED", enabled: false),
        });
        var sec = Providers(s);
        Assert.Equal("No 2D-to-3D converter is turned on.", sec.ConnectedLine);
        Assert.Equal(2, sec.Count);
        Assert.All(sec.Items, i => Assert.Equal(PlainState.TurnedOff, i.State)); // each row keeps "Turned off"
        Assert.DoesNotContain("0", sec.ConnectedLine);

        // The same from the per-class policy list (`conv list --json`) when the snapshot's flag has not caught up.
        Assert.True(PolicyList.TryParse(ComponentClass.Conversion, """
            { "providers": [ { "id": "acme", "name": "Acme Depth", "enabled": false, "enabled_source": "user", "load": "LOADED",
                               "built_in": false, "provenance": "signed" } ],
              "tasks": [], "policy": { "policy_lock": false, "preferred": {} } }
            """, out var policy));
        Assert.Equal(Components.NoConverterOn,
                     Providers(Snapshot(new[] { Provider("acme", "Acme Depth", "LOADED") }), policy).ConnectedLine);
    }

    [Fact]
    public void NoConverterInstalled_TheSectionSaysSo()
    {
        Assert.Equal("No 2D-to-3D converter is installed.", Providers(Snapshot(Array.Empty<string>())).ConnectedLine);
        // A hidden built-in adapter is not an installed converter.
        var hiddenOnly = Providers(Snapshot(new[] { BuiltIn(hidden: true) }));
        Assert.Equal("No 2D-to-3D converter is installed.", hiddenOnly.ConnectedLine);
        Assert.Empty(hiddenOnly.Items);
        Assert.Equal(0, hiddenOnly.Count);
    }

    [Fact]
    public void AConverterThatIsOn_KeepsTheCountLine_WithNonZeroPartsOnly()
    {
        // One turned off, one on and not selected (its own row says what it waits for): no sentence, no zero.
        var mixed = Providers(Snapshot(new[]
        {
            Provider("a", "A", "DISABLED", enabled: false),
            Provider("b", "B", "LOADED"),
        }));
        Assert.Equal("2 installed", mixed.ConnectedLine);

        // Selected and warming: "in use" is said, "0 ready" is not.
        var warming = Providers(Snapshot(new[] { Provider("b", "B", "LOADED") },
                                         new[] { TaskRow(0, "b", "depth-mono", true, state: "WARMING") }));
        Assert.Equal($"1 installed{StatusText.Sep}in use for 1 task", warming.ConnectedLine);

        // Without the background service the line still says so.
        var headless = Providers(Snapshot(new[] { Provider("b", "B", "LOADED") }, source: "headless"));
        Assert.Equal($"1 installed{StatusText.Sep}DisplayXR isn't running in the background", headless.ConnectedLine);

        // A selected task: never the sentence, whatever is turned off beside it.
        Assert.Null(Components.NoConverterLine(Snapshot(
            new[] { Provider("a", "A", "DISABLED", enabled: false), Provider("b", "B", "LOADED") },
            new[] { TaskRow(1, "b", "depth-mono", true) })));
        // A CLI that predates the conversion rows: nothing is claimed.
        Assert.Null(Components.NoConverterLine(TestData.Parse(TestData.DesignExample)));
        Assert.Null(Components.NoConverterLine(null));
    }

    [Fact]
    public void TheConversionModuleCard_SaysOff_NotStandby_WhenNothingConverts()
    {
        var off = Snapshot(new[] { BuiltIn(hidden: true), Provider("a", "A", "DISABLED", enabled: false) });
        var module = Components.Build(off, NoModule).Single(x => x.Id == "conversion");
        var card = Assert.Single(module.Items);
        Assert.Equal("Converter", card.Name); // one name in every state, a backend named or not
        Assert.Equal(PlainState.Off, card.State);
        Assert.Equal(("Off", Level.Plain), PlainStates.Chip(card.State));
        Assert.Equal("No 2D-to-3D converter is turned on.", card.Summary);
        Assert.Equal("No 2D-to-3D converter is turned on.", module.ConnectedLine);

        var none = Components.Build(Snapshot(Array.Empty<string>()), NoModule).Single(x => x.Id == "conversion");
        Assert.Equal(PlainState.Off, none.Items[0].State);
        Assert.Equal("No 2D-to-3D converter is installed.", none.Items[0].Summary);
        Assert.Equal("No 2D-to-3D converter is installed.", none.ConnectedLine);

        // A converter is selected for a task: the card is as before.
        var on = Snapshot(new[] { Provider("b", "B", "LOADED") }, new[] { TaskRow(0, "b", "depth-mono", true) });
        var asBefore = Components.Build(on, NoModule).Single(x => x.Id == "conversion");
        Assert.Equal("Converter", asBefore.Items[0].Name);
        Assert.Equal(PlainState.Standby, asBefore.Items[0].State);
        Assert.Null(asBefore.Items[0].Summary);
        Assert.Equal("none available", asBefore.ConnectedLine);

        // A CLI that predates the conversion rows: as before too.
        var old = Components.Build(TestData.Parse(TestData.DesignExample), NoModule).Single(x => x.Id == "conversion");
        Assert.Equal(PlainState.Standby, old.Items[0].State);

        // A module the display driver does have keeps its own state, whatever the providers say.
        var present = ModuleInfo("""
            { "lift_caps": { "probed": true, "malformed": false, "note": "", "modes": 3, "state": 2, "backend": "acme-gpu" } }
            """);
        var ready = Components.Build(off, present).Single(x => x.Id == "conversion");
        Assert.Equal(PlainState.Ready, ready.Items[0].State);
        Assert.Equal("1 available", ready.ConnectedLine);

        // The same item has the same name in every state of the card.
        Assert.Equal(new[] { "Converter" }, new[] { card, none.Items[0], asBefore.Items[0], old.Items[0], ready.Items[0] }
            .Select(i => i.Name).Distinct());
    }

    // ── (a) names ───────────────────────────────────────────────────────────

    [Fact]
    public void TheConversionServicesAnswer_IsNotAConverterInTheDisplayDriver()
    {
        // The conversion service answers lift_caps with backend "conversion" when a provider is ready.
        // The display driver has no converter then, and the card must not show one.
        var info = ModuleInfo("""
            { "lift_caps": { "probed": true, "malformed": false, "note": "", "modes": 3, "state": 2, "backend": "conversion" } }
            """);
        var section = Components.Conversion(info);
        Assert.Equal("none available", section.ConnectedLine);
        Assert.NotEqual(PlainState.Ready, Assert.Single(section.Items).State);
    }

    [Fact]
    public void TheConversionModule_IsNamedConverter_ItsBackendIdIsInDetailsOnly()
    {
        var info = ModuleInfo("""
            { "lift_caps": { "probed": true, "malformed": false, "note": "", "modes": 3, "state": 2, "backend": "acme-gpu" } }
            """);
        var row = Assert.Single(Components.Conversion(info).Items);
        Assert.Equal("Converter", row.Name);
        Assert.Null(row.Summary);
        Assert.Contains(("Backend", "acme-gpu"), row.DetailFacts);
    }

    [Fact]
    public void TheCatalogCallsTheVendorConverter_LeiaConversion()
    {
        foreach (string file in new[] { "catalog-list-v2.json", "catalog-list.json" })
        {
            Assert.True(CatalogList.TryParse(TestData.Text(file), out var list), file);
            var item = list!.Find("leia-conversion")!;
            Assert.Equal("Leia conversion", item.Name);
            Assert.Equal("2D-to-3D conversion from the display maker.", item.Summary);
            Assert.Equal("Leia conversion runtime", Assert.Single(item.Prerequisites).Name);
            Assert.Contains("leia_conversion", item.Aliases); // ids and aliases are unchanged
        }
    }

    [Fact]
    public void NoStringTheDashboardOrTheCatalogSupplies_NamesTheVendorConverterTechnology()
    {
        var shown = new List<string>();

        // The catalog's output as the Dashboard shows it: every item's name, summary and prerequisite names.
        foreach (string file in new[] { "catalog-list-v2.json", "catalog-list.json", "catalog-list-v1.json" })
        {
            Assert.True(CatalogList.TryParse(TestData.Text(file), out var list), file);
            foreach (var i in list!.Items)
            {
                shown.Add(i.Name);
                shown.Add(i.Summary);
                shown.Add(CatalogText.About(i));
                shown.AddRange(i.Prerequisites.Select(p => p.Name));
            }
        }

        // The Dashboard's own tables: its constants, the state words, and every section's title, role and line,
        // built with the vendor's provider installed and its module reporting a backend id.
        shown.AddRange(new[]
        {
            Components.BuiltInConverterName, Components.BuiltInConverterLine, Components.NoConverterOn,
            Components.NoConverterInstalled, Components.ModuleName,
        });
        shown.AddRange(Enum.GetValues<PlainState>().Select(x => PlainStates.Chip(x)?.Text ?? ""));
        shown.AddRange(Enum.GetValues<RowStatus>().Select(x => new ComponentRow("k", null, null, x).StatusChip?.Text ?? ""));
        var vendorModule = ModuleInfo(TestData.Text("info-lift-module.json"));
        var snapshots = new[]
        {
            Snapshot(new[] { BuiltIn(), Provider("leia-conversion", "Leia conversion", "LOADED") },
                     new[] { TaskRow(1, "leia-conversion", "depth-mono", true) }),
            Snapshot(new[] { BuiltIn(hidden: true), Provider("leia-conversion", "Leia conversion", "DISABLED", enabled: false) }),
            Snapshot(Array.Empty<string>()),
        };
        foreach (var s in snapshots)
            foreach (var sec in Components.Build(s, vendorModule))
            {
                shown.Add(sec.Title);
                shown.Add(sec.Role);
                shown.Add(sec.ConnectedLine);
                // A row's name and its one line. Its Details pass the component's own words through
                // (the module's backend id, its detail line): those come from the provider, not from here.
                foreach (var i in sec.Items)
                {
                    shown.Add(i.Name);
                    shown.Add(i.Summary ?? "");
                }
            }

        Assert.All(shown, text => Assert.DoesNotContain(Banned, text, StringComparison.OrdinalIgnoreCase));
    }

    // ── a task is named in plain words, never by its id ─────────────────────

    [Theory]
    [InlineData("depth-mono", "depth from a photo")]
    [InlineData("depth-video", "depth from video")]
    [InlineData("disparity-stereo", "depth from a 3D photo or video")]
    [InlineData("views-mono", "3D from 2D video and photos")]
    [InlineData("views-stereo", "more views from a 3D photo or video")]
    [InlineData("views-from-depth", "3D from a picture and its depth")]
    [InlineData("splats-mono", "3D scene from a photo")]
    [InlineData("splats-stereo", "3D scene from a 3D photo")]
    public void ATask_HasAPlainName(string id, string name)
    {
        Assert.Equal(name, StatusText.ConversionTaskName(id));
        Assert.DoesNotContain(id, StatusText.ConversionTaskName(id), StringComparison.Ordinal);
        Assert.DoesNotContain(Banned, name, StringComparison.OrdinalIgnoreCase);
        // The words of the controls are built from it.
        Assert.Equal($"Use this one for {name}", StatusText.PreferTaskButton(id));
        Assert.Equal($"Your choice for {name}", StatusText.TaskChoiceChip(id));
        Assert.Equal($"Choose automatically for {name}", StatusText.ResetTaskButton(id));
    }

    [Fact]
    public void TheButtons_ReadAsASentence()
    {
        Assert.Equal("Use this one for depth from a photo", StatusText.PreferTaskButton("depth-mono"));
        Assert.Equal("Use this one for depth from video", StatusText.PreferTaskButton("depth-video"));
        Assert.Equal("Use this one for 3D from 2D video and photos", StatusText.PreferTaskButton("views-mono"));
        Assert.Equal("Choose automatically for depth from video", StatusText.ResetTaskButton("depth-video"));
        Assert.Equal("Your choice for 3D scene from a photo", StatusText.TaskChoiceChip("splats-mono"));
    }

    [Theory]
    [InlineData("aux:acme-relight")]
    [InlineData("some-new-task")]
    [InlineData("")]
    public void AnUnknownTask_IsShownByItsId(string id)
    {
        Assert.Equal(id, StatusText.ConversionTaskName(id));
        Assert.Equal(id, StatusText.ConversionTaskLabel(id)); // an id is never re-cased
        Assert.Equal($"Use this one for {id}", StatusText.PreferTaskButton(id));
    }

    [Fact]
    public void ARowsDetails_NameTheTaskPlainly_AndKeepItsId()
    {
        var s = Snapshot(new[] { Provider("acme", "Acme Depth", "LOADED") },
                         new[] { TaskRow(0, "acme", "depth-video", true), TaskRow(0, "acme", "aux:acme-relight", false, verdict: "behind") });
        var row = Assert.Single(Providers(s).Items);
        var fact = Assert.Single(row.DetailFacts, f => f.Label == "Depth from video");
        Assert.EndsWith("id depth-video", fact.Text, StringComparison.Ordinal);
        Assert.DoesNotContain(row.DetailFacts, f => f.Label == "depth-video");
        Assert.Contains(row.DetailFacts, f => f.Label == "aux:acme-relight"); // unknown: the id itself
    }
}

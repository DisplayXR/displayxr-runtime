// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Collections.Generic;
using System.Linq;
using System.Text.Json;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>
/// "No 2D-to-3D converter is turned on / installed" is about what can convert on this machine, not
/// about the Enabled values alone (PR #2787, found on a Windows test box: the reference provider
/// ships turned on and its probe declines on real hardware, so turning off both real converters
/// never printed the sentence).
///
/// The <c>conv-list-*-win.json</c> fixtures here are written by hand in the shape of
/// <c>displayxr-cli conv list --json</c> (the row fields of <c>conv-list-n3e-win.json</c>, which
/// was captured); they are not captures themselves. A provider whose probe declined is the row the
/// headless builder writes (target_conv_status.c): <c>load</c> = <c>PROBE_DECLINED</c>,
/// <c>hint</c> = "probe returned -17", <c>status</c> UNKNOWN, <c>enabled</c> true, no capability.
/// The status feed's <c>conv_providers[]</c> / <c>conv_tasks[]</c> are the same rows, so each fixture
/// feeds both the snapshot and the policy list.
/// </summary>
public class ConverterAvailabilityTests
{
    private const string Sim = "sim-conversion";

    private static (StatusSnapshot Snapshot, PolicyList Policy) Captured(string file)
    {
        string json = TestData.Text(file);
        using var doc = JsonDocument.Parse(json);
        string providers = doc.RootElement.GetProperty("providers").GetRawText();
        string tasks = doc.RootElement.GetProperty("tasks").GetRawText();
        var s = TestData.Parse($$"""
            { "schema": 1, "source": "service", "generation": { "topology": 1, "status": 2, "conversion": 3 },
              "runtime": { "version": "2.70.4", "git_tag": "v2.70.4", "plugin_abi": 5 },
              "plugins": [], "screens": [], "clients": [], "workspace": { "enabled": false, "controller": null, "screens": [] },
              "warnings": [],
              "conv_providers": {{providers}},
              "conv_tasks": {{tasks}} }
            """);
        Assert.True(PolicyList.TryParse(ComponentClass.Conversion, json, out var policy), file);
        return (s, policy!);
    }

    /// <summary>A display driver that reports no converter of its own: probed, no modes, no backend.</summary>
    private static InfoComponents NoModule()
    {
        Assert.True(InfoResult.TryParse("""
            { "lift_caps": { "probed": true, "malformed": false, "note": "", "modes": 0, "state": 0, "backend": "" } }
            """, out var info, out var err), err);
        return info!.Components;
    }

    /// <summary>The 2D to 3D section and the display driver's card, with and without the policy list (it arrives later than the snapshot).</summary>
    private static IEnumerable<(ComponentSection Providers, ComponentSection Module)> Built(string file)
    {
        var (s, policy) = Captured(file);
        foreach (var policies in new[] { null, new Dictionary<ComponentClass, PolicyList> { [ComponentClass.Conversion] = policy } })
        {
            var all = Components.Build(s, NoModule(), null, policies);
            yield return (all.Single(x => x.Id == "conversion_providers"), all.Single(x => x.Id == "conversion"));
        }
    }

    [Fact]
    public void BothRealConvertersOff_AndTheReferenceProviderDeclined_SaysNoConverterIsTurnedOn()
    {
        // Case C of the test box. The reference provider is turned on, and it cannot convert here.
        var (s, policy) = Captured("conv-list-off-declined-win.json");
        Assert.True(s.Providers.Single(p => p.Id == Sim).Enabled);
        Assert.Equal("PROBE_DECLINED", s.Providers.Single(p => p.Id == Sim).Load);
        Assert.True(policy.Row(Sim)!.Enabled);
        Assert.Equal(Components.NoConverterOn, Components.NoConverterLine(s));
        Assert.Equal(Components.NoConverterOn, Components.NoConverterLine(s, policy));

        foreach (var (providers, module) in Built("conv-list-off-declined-win.json"))
        {
            Assert.Equal("No 2D-to-3D converter is turned on.", providers.ConnectedLine);
            // The hidden built-in adapter is not a row; the three providers are.
            Assert.Equal(new[] { "Leia conversion", "Open conversion provider", "DisplayXR Sim Conversion" },
                         providers.Items.Select(i => i.Name));
            Assert.Equal(new[] { PlainState.TurnedOff, PlainState.TurnedOff, PlainState.NotAvailable },
                         providers.Items.Select(i => i.State));

            // The display driver's card: one name, the chip "Off", and the section's sentence.
            var card = Assert.Single(module.Items);
            Assert.Equal("Converter", card.Name);
            Assert.Equal(PlainState.Off, card.State);
            Assert.Equal(("Off", Level.Plain), PlainStates.Chip(card.State));
            Assert.Equal(providers.ConnectedLine, card.Summary);
            Assert.Equal(providers.ConnectedLine, module.ConnectedLine);
        }
    }

    [Fact]
    public void ADeclinedProvider_ReadsNotAvailableOnThisPc_NeverStandbyOrNeedsAttention()
    {
        var (s, policy) = Captured("conv-list-off-declined-win.json");
        var row = Components.ConversionProviders(s, policy).Items.Single(i => i.Id == Sim);
        Assert.Equal(PlainState.NotAvailable, row.State);
        Assert.Equal(("Not available on this PC", Level.Plain), PlainStates.Chip(row.State));
        Assert.Equal(Level.Plain, row.Level); // not a failure, and not a working converter
        // What the CLI said stays in Details.
        Assert.Contains(("Load state", "PROBE_DECLINED"), row.DetailFacts);
        Assert.Contains(("Note", "probe returned -17"), row.DetailFacts);

        // Its Enabled value changes nothing: turned on or off, selected or not, it is not available.
        foreach (bool enabled in new[] { true, false })
            Assert.Equal(PlainState.NotAvailable,
                         PlainStates.ConversionProvider("PROBE_DECLINED", enabled, false, false, false, Level.Plain, null));
        // A probe that failed (any other error) is a failure, as before.
        Assert.Equal(PlainState.NeedsAttention,
                     PlainStates.ConversionProvider("PROBE_FAILED", true, false, false, false, Level.Plain, null));
        // NEGOTIATED is the conversion loader's word for "loaded": never a failure.
        Assert.Equal(PlainState.Ready,
                     PlainStates.ConversionProvider("NEGOTIATED", true, false, false, false, Level.Plain, null));
        Assert.Equal(PlainState.InUse,
                     PlainStates.ConversionProvider("NEGOTIATED", true, false, false, true, Level.Ok, null));
    }

    [Fact]
    public void OnlyADeclinedProviderRegistered_SaysNoConverterIsInstalled()
    {
        foreach (var (providers, module) in Built("conv-list-declined-only-win.json"))
        {
            Assert.Equal("No 2D-to-3D converter is installed.", providers.ConnectedLine);
            // The provider is still listed, with its own state.
            Assert.Equal(PlainState.NotAvailable, Assert.Single(providers.Items).State);
            Assert.Equal(PlainState.Off, Assert.Single(module.Items).State);
            Assert.Equal("Converter", module.Items[0].Name);
            Assert.Equal(providers.ConnectedLine, module.Items[0].Summary);
            Assert.Equal(providers.ConnectedLine, module.ConnectedLine);
        }

        // Turning the declined provider off does not make it a converter that could be turned on:
        // the policy list says off while the snapshot still says its probe declined.
        var (s, _) = Captured("conv-list-declined-only-win.json");
        Assert.True(PolicyList.TryParse(ComponentClass.Conversion, """
            { "providers": [ { "id": "sim-conversion", "name": "DisplayXR Sim Conversion", "enabled": false,
                               "enabled_source": "user", "load": "PROBE_DECLINED", "built_in": false, "provenance": "signed" } ],
              "tasks": [], "policy": { "policy_lock": false, "preferred": {} } }
            """, out var off));
        Assert.Equal(Components.NoConverterInstalled, Components.NoConverterLine(s, off));
    }

    [Fact]
    public void AUsableProviderTurnedOnAndWarming_PrintsNeitherSentence()
    {
        var (s, policy) = Captured("conv-list-warming-win.json");
        Assert.DoesNotContain(s.Tasks, t => t.Selected); // nothing is selected yet
        Assert.Null(Components.NoConverterLine(s));
        Assert.Null(Components.NoConverterLine(s, policy));

        foreach (var (providers, module) in Built("conv-list-warming-win.json"))
        {
            // The count line, non-zero parts only; the rows say the rest.
            Assert.Equal("2 installed", providers.ConnectedLine);
            Assert.DoesNotContain("0", providers.ConnectedLine);
            var leia = providers.Items.Single(i => i.Id == "leia-conversion");
            Assert.Equal(PlainState.Ready, leia.State);
            Assert.Contains(leia.DetailFacts, f => f.Label == "depth-mono" && f.Text.StartsWith("warming 40%", System.StringComparison.Ordinal));
            Assert.Equal(PlainState.NotAvailable, providers.Items.Single(i => i.Id == Sim).State);
            // The card does not claim that nothing is turned on.
            Assert.Equal("Converter", module.Items[0].Name);
            Assert.Equal(PlainState.Standby, module.Items[0].State);
            Assert.Null(module.Items[0].Summary);
            Assert.Equal("none available", module.ConnectedLine);
        }
    }

    [Fact]
    public void OneRealProviderOff_TheOtherInUse_PrintsNeitherSentence()
    {
        var (s, policy) = Captured("conv-list-one-off-one-in-use-win.json");
        Assert.Null(Components.NoConverterLine(s));
        Assert.Null(Components.NoConverterLine(s, policy));

        foreach (var (providers, module) in Built("conv-list-one-off-one-in-use-win.json"))
        {
            Assert.Equal($"3 installed{StatusText.Sep}in use for 1 task{StatusText.Sep}1 ready", providers.ConnectedLine);
            Assert.Equal(new[] { PlainState.TurnedOff, PlainState.InUse, PlainState.NotAvailable },
                         providers.Items.Select(i => i.State));
            Assert.Equal("Converter", module.Items[0].Name);
            Assert.Null(module.Items[0].Summary);
        }
    }

    [Fact]
    public void EachProviderIsClassifiedFromWhatTheCliReports()
    {
        var (s, policy) = Captured("conv-list-one-off-one-in-use-win.json");
        Components.ConverterUse Use(string id) => Components.UseOf(s.Providers.Single(p => p.Id == id), policy.Row(id));
        Assert.Equal(Components.ConverterUse.Unavailable, Use("dp-lift")); // the hidden built-in adapter
        Assert.Equal(Components.ConverterUse.TurnedOff, Use("leia-conversion"));
        Assert.Equal(Components.ConverterUse.Usable, Use("open-conversion"));
        Assert.Equal(Components.ConverterUse.Unavailable, Use(Sim)); // turned on, probe declined

        var open = s.Providers.Single(p => p.Id == "open-conversion");
        Assert.Equal(Components.ConverterUse.Broken, Components.UseOf(open with { Load = "LOAD_FAILED" }));
        Assert.Equal(Components.ConverterUse.Broken, Components.UseOf(open with { Load = "PROBE_FAILED" }));
        Assert.Equal(Components.ConverterUse.Broken, Components.UseOf(open with { BadDescriptors = 1 }));
        Assert.Equal(Components.ConverterUse.TurnedOff, Components.UseOf(open with { Enabled = false }));
        Assert.Equal(Components.ConverterUse.BuiltIn,
                     Components.UseOf(s.Providers.Single(p => p.Id == "dp-lift") with { Hidden = false }));
    }

    [Fact]
    public void ABrokenProviderAlone_PrintsNeitherSentence_ItsRowSaysNeedsAttention()
    {
        // Turned on and failed to load: it is installed, and it is not "turned off". The row explains.
        var (s, _) = Captured("conv-list-declined-only-win.json");
        var broken = s with { ConvProviders = new[] { s.Providers[0] with { Load = "LOAD_FAILED" } } };
        Assert.Null(Components.NoConverterLine(broken));
        var sec = Components.ConversionProviders(broken);
        Assert.Equal("1 installed", sec.ConnectedLine);
        Assert.Equal(PlainState.NeedsAttention, Assert.Single(sec.Items).State);

        // Beside a provider that is turned off, the sentence is the one the user can act on.
        var (off, _) = Captured("conv-list-off-declined-win.json");
        var mixed = off with { ConvProviders = off.Providers.Append(s.Providers[0] with { Id = "x", Load = "LOAD_FAILED" }).ToArray() };
        Assert.Equal(Components.NoConverterOn, Components.NoConverterLine(mixed));
    }
}

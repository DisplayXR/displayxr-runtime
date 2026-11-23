// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>
/// The Components page as one list: an installed component joined with its catalog entry, the one
/// status a row has, what a row may offer (never Remove without a receipt), the filters, and what
/// stays when the catalog cannot be read. Pure model (Model/ComponentRows.cs); the sections and
/// the catalog are written by hand here, with made-up ids.
/// </summary>
public class ComponentRowsTests
{
    private static string Item(string id, string cls, string state, bool receipt = false, bool installable = true, string? registered = null,
                               bool? canUninstall = null) =>
        $"{{\"id\":\"{id}\",\"name\":\"{id} name\",\"class\":\"{cls}\",\"publisher\":\"acme-labs\",\"state\":\"{state}\","
        + $"\"installed\":{(state == "not_installed" ? "null" : "\"1.0.0\"")},\"available\":\"1.1.0\","
        + $"\"installable\":{(installable ? "true" : "false")},\"receipt\":{(receipt ? "true" : "false")}"
        + (canUninstall is null && !receipt ? "" : $",\"can_uninstall\":{((canUninstall ?? receipt) ? "true" : "false")}")
        + (registered is null ? "" : $",\"registered_id\":\"{registered}\"") + "}";

    private static CatalogList Catalog(params string[] items)
    {
        Assert.True(CatalogList.TryParse("{\"schema\":2,\"items\":[" + string.Join(",", items) + "]}", out var list));
        return list!;
    }

    private static CatalogSource Live(params string[] items) => new(Catalog(items), false, true);

    private static PolicyRow Policy(ComponentClass cls, string id, bool builtIn = false, string binary = "") =>
        new(cls, id, id, true, "default", false, false, "ACTIVE", "signed", null, null, BuiltIn: builtIn, BinaryPath: binary);

    private static ComponentItem Registered(ComponentClass cls, string id, bool builtIn = false, string binary = "") =>
        new(id, Level.Ok, Array.Empty<(string, Level)>(), Array.Empty<string>(), Policy: Policy(cls, id, builtIn, binary));

    private const string RuntimeDir = @"C:\Program Files\Example\Runtime";

    private static ComponentSection Section(string id, params ComponentItem[] items) =>
        new(id, id, "", items.Length, 0, "", items);

    private static ComponentRow Row(IReadOnlyList<ComponentGroup> groups, string key) =>
        groups.SelectMany(g => g.Rows).Single(r => r.Key == key);

    [Fact]
    public void AnInstalledComponentAndItsCatalogEntry_AreOneRow_WithOneStatus()
    {
        var sections = new[]
        {
            Section("display_processors", Registered(ComponentClass.Display, "panel-a"), Registered(ComponentClass.Display, "panel-b")),
            Section("input_providers"),
        };
        var groups = ComponentRows.Join(sections, Live(
            Item("panel-a-pkg", "display_processor", "current", receipt: true, registered: "panel-a"),
            Item("panel-b", "display_processor", "outdated", receipt: true),
            Item("panel-c", "display_processor", "not_installed"),
            Item("hands", "input_provider", "not_installed")));

        var dp = groups.Single(g => g.Section.Id == "display_processors");
        Assert.Equal(new[] { "display_processor:panel-a", "display_processor:panel-b", "display_processor:panel-c" }, dp.Rows.Select(r => r.Key));

        var a = dp.Rows[0]; // joined by the registered id, not the catalog id
        Assert.NotNull(a.Installed);
        Assert.Equal("panel-a-pkg", a.Catalog!.Id);
        Assert.Equal(RowStatus.Installed, a.Status);
        Assert.Equal(CatalogAction.None, a.Action);
        Assert.True(a.CanUninstall);
        Assert.Null(a.RemoveNote);

        var b = dp.Rows[1];
        Assert.Equal(RowStatus.UpdateAvailable, b.Status);
        Assert.Equal(CatalogAction.Update, b.Action);
        Assert.Equal(("Update available", Level.Info), b.StatusChip);

        var c = dp.Rows[2];
        Assert.Null(c.Installed);
        Assert.Equal(RowStatus.Available, c.Status);
        Assert.Equal(CatalogAction.Install, c.Action);
        Assert.False(c.CanUninstall);
        Assert.Null(c.RemoveNote);

        // A class with nothing installed still lists what can be added.
        Assert.Equal(RowStatus.Available, Assert.Single(groups.Single(g => g.Section.Id == "input_providers").Rows).Status);
    }

    [Fact]
    public void InstalledButNotInTheCatalog_IsShown_ManagedAsUsual_AndNeverOfferedUpdateOrRemove()
    {
        var sections = new[] { Section("tracking_providers", Registered(ComponentClass.Tracking, "dev-tracker")) };
        var row = Assert.Single(ComponentRows.Join(sections, Live(Item("other", "tracking_provider", "not_installed")))[0].Rows.Where(r => r.Installed is not null));

        Assert.Equal(RowStatus.NotFromCatalog, row.Status);
        Assert.Equal(("Installed separately", Level.Plain), row.StatusChip);
        Assert.NotNull(row.Installed!.Policy); // the Enabled / Prefer controls still have their row
        Assert.Equal(CatalogAction.None, row.Action);
        Assert.False(row.CanUninstall);
        Assert.False(row.HasDetail);
        Assert.Equal("Remove with its own uninstaller.", row.RemoveNote);
    }

    [Fact]
    public void WhatTheRuntimeInstallerShips_IsPartOfTheRuntime_WithNoRemoveSentence()
    {
        var sections = new[]
        {
            Section("input_providers",
                Registered(ComponentClass.Input, "pad", binary: RuntimeDir + @"\plugins\Pad.dll"),          // shipped by the runtime
                Registered(ComponentClass.Input, "pen", binary: @"C:\Program Files\Example\Plugins\Pen\Pen.dll"), // its own installer
                Registered(ComponentClass.Input, "dev", binary: @"D:\src\build\plugins\Dev.dll"),          // a dev registration
                Registered(ComponentClass.Input, "old")),                                                 // a CLI that names no binary
            Section("conversion_providers", Registered(ComponentClass.Conversion, "depth", binary: RuntimeDir + @"\plugins\Depth.dll")),
        };
        var catalog = Live(Item("hands", "input_provider", "not_installed"), Item("depth", "conversion_provider", "not_installed"));
        var groups = ComponentRows.Join(sections, catalog, RuntimeDir);

        var pad = Row(groups, "input_provider:pad");
        Assert.Equal(RowStatus.PartOfRuntime, pad.Status);
        Assert.Equal(("Part of the runtime", Level.Plain), pad.StatusChip);
        Assert.Null(pad.RemoveNote);
        Assert.NotNull(pad.Installed!.Policy); // the Enabled / Prefer controls still have their row
        Assert.Equal(CatalogAction.None, pad.Action);
        Assert.False(pad.CanUninstall);
        Assert.False(pad.HasDetail);
        Assert.True(pad.Shows(RowFilter.Installed));
        Assert.False(pad.Shows(RowFilter.Updates));

        // Listed by the catalog, not found by the engine, and shipped by the runtime: still never an Install over it.
        var depth = Row(groups, "conversion_provider:depth");
        Assert.Equal(RowStatus.PartOfRuntime, depth.Status);
        Assert.Equal(CatalogAction.None, depth.Action);
        Assert.Null(depth.RemoveNote);

        foreach (var key in new[] { "input_provider:pen", "input_provider:dev", "input_provider:old" })
        {
            var other = Row(groups, key);
            Assert.Equal(("Installed separately", Level.Plain), other.StatusChip);
            Assert.Equal("Remove with its own uninstaller.", other.RemoveNote);
        }

        // Without the runtime directory nothing is claimed to be part of it.
        Assert.Equal(RowStatus.NotFromCatalog, Row(ComponentRows.Join(sections, catalog), "input_provider:pad").Status);
        // A catalog install in the same folder keeps its catalog status; no catalog read claims nothing.
        var known = ComponentRows.Join(sections, Live(Item("pad", "input_provider", "current", receipt: true)), RuntimeDir);
        Assert.Equal(RowStatus.Installed, Row(known, "input_provider:pad").Status);
        Assert.Equal(RowStatus.Installed, Row(ComponentRows.Join(sections, null, RuntimeDir), "input_provider:pad").Status);
    }

    [Theory]
    [InlineData(@"C:\Program Files\Example\Runtime\plugins\Pad.dll", @"C:\Program Files\Example\Runtime", true)]
    [InlineData(@"c:/program files/example/runtime/PLUGINS/sub/Pad.dll", @"C:\Program Files\Example\Runtime\", true)] // slashes and case
    [InlineData(@"C:\Program Files\Example\Runtime\desk.exe", @"C:\Program Files\Example\Runtime", false)]       // beside the runtime, not its plug-in
    [InlineData(@"C:\Program Files\Example\Runtime\plugins-extra\Pad.dll", @"C:\Program Files\Example\Runtime", false)]
    [InlineData(@"C:\Program Files\Example\Plugins\Pen\Pen.dll", @"C:\Program Files\Example\Runtime", false)]
    [InlineData("", @"C:\Program Files\Example\Runtime", false)]
    [InlineData(@"C:\Program Files\Example\Runtime\plugins\Pad.dll", null, false)]
    [InlineData(@"C:\Program Files\Example\Runtime\plugins\Pad.dll", "", false)]
    public void ShippedByRuntime_IsTheRuntimesPluginsFolder(string binary, string? runtimeDir, bool shipped) =>
        Assert.Equal(shipped, ComponentRows.ShippedByRuntime(binary, runtimeDir));

    [Theory]
    [InlineData(2, 0, "2 installed")]
    [InlineData(0, 1, "1 available to install")]
    [InlineData(2, 1, "2 installed · 1 available to install")]
    public void ASectionCount_SaysOnlyItsNonZeroParts(int installed, int available, string text) =>
        Assert.Equal(text, CatalogText.Counts(installed, available));

    [Fact]
    public void TheDetectedLine_NeverCarriesTheEnginesEvidence()
    {
        Assert.Equal("Works with your display: DisplayXR detected a supported display on this machine.", CatalogText.DetectedLine);
        // The evidence stays in the model (the engine's own words), for diagnostics only.
        var item = Catalog("{\"id\":\"panel-a\",\"name\":\"a\",\"class\":\"display_processor\",\"state\":\"not_installed\",\"installable\":true,"
                           + "\"detected\":\"high\",\"evidence\":\"display EDID 1/2 CODE_NAME (high confidence)\"}").Items[0];
        Assert.True(item.DetectedHere);
        Assert.Contains("CODE_NAME", item.Evidence);
        Assert.DoesNotContain("CODE_NAME", CatalogText.DetectedLine);
    }

    [Fact]
    public void ACopyTheCatalogListsButDidNotFind_IsNotFromCatalog_OneRow_NothingOffered()
    {
        // Registered (a dev build, another installer) under an id the catalog also has, which the engine reads as not installed.
        var sections = new[] { Section("conversion_providers", Registered(ComponentClass.Conversion, "depth")) };
        var rows = ComponentRows.Join(sections, Live(Item("depth", "conversion_provider", "not_installed")))[0].Rows;

        var row = Assert.Single(rows); // not a second "Available" row beside it
        Assert.Equal(RowStatus.NotFromCatalog, row.Status);
        Assert.Equal(CatalogAction.None, row.Action); // never an Install over it
        Assert.False(row.CanUninstall);
        Assert.Equal("Remove with its own uninstaller.", row.RemoveNote);
    }

    [Fact]
    public void Uninstall_NeedsAReceipt()
    {
        var sections = new[] { Section("input_providers", Registered(ComponentClass.Input, "pen"), Registered(ComponentClass.Input, "pad")) };
        var groups = ComponentRows.Join(sections, Live(
            Item("pen", "input_provider", "current", receipt: true),
            Item("pad", "input_provider", "outdated", receipt: false)));

        Assert.True(Row(groups, "input_provider:pen").CanUninstall);
        var pad = Row(groups, "input_provider:pad"); // installed by its own installer: the catalog knows it, has no receipt
        Assert.Equal(RowStatus.UpdateAvailable, pad.Status);
        Assert.False(pad.CanUninstall);
        Assert.Equal(CatalogText.RemoveFromSettings, pad.RemoveNote);
    }

    [Fact]
    public void AReceiptWithoutAnUninstallCommand_IsNotOfferedForRemoval()
    {
        // What the engine answers for an item installed from an entry that names no uninstall command:
        // receipt true, can_uninstall false. `uninstall` would be refused (exit 43), so it is not offered.
        var sections = new[] { Section("display_processors", Registered(ComponentClass.Display, "panel-a"), Registered(ComponentClass.Display, "panel-b")) };
        var groups = ComponentRows.Join(sections, Live(
            Item("panel-a", "display_processor", "current", receipt: true, canUninstall: false),
            "{\"id\":\"panel-b\",\"name\":\"b\",\"class\":\"display_processor\",\"state\":\"current\",\"installed\":\"1.0.0\",\"installable\":true,\"receipt\":true}"));

        var a = Row(groups, "display_processor:panel-a");
        Assert.Equal(RowStatus.Installed, a.Status);
        Assert.True(a.Catalog!.Receipt);
        Assert.False(a.CanUninstall);
        Assert.Equal("Remove it from Windows Settings > Apps.", a.RemoveNote);

        // An engine that predates the field says nothing about it: read as "cannot remove".
        var b = Row(groups, "display_processor:panel-b");
        Assert.False(b.CanUninstall);
        Assert.Equal(CatalogText.RemoveFromSettings, b.RemoveNote);

        // The receipts-only list (offline) carries the same flag.
        Assert.True(CatalogList.TryParseInstalled(
            "{\"installed\":[{\"id\":\"panel-a\",\"class\":\"display_processor\",\"version\":\"1.0.0\",\"can_uninstall\":false,\"uninstall_command\":\"\"}]}", out var receipts));
        var offline = ComponentRows.Join(sections, new CatalogSource(receipts, false, false));
        Assert.False(Row(offline, "display_processor:panel-a").CanUninstall);
    }

    [Fact]
    public void ASuccess_IsOneLine_WithNothingToDismiss()
    {
        var ok = CatalogExit.Describe(CatalogVerb.Update, CatalogExit.Ok, "Panel A");
        Assert.True(ok.Ok);
        Assert.Equal("Panel A is up to date. It is ready to use.", CatalogText.Inline(ok.Title, ok.Text));
        Assert.Equal("Done.", CatalogText.Inline("Done", ""));
    }

    [Fact]
    public void WithNoCatalog_InstalledRowsAreAllThereIs_AndNothingIsClaimedAboutThem()
    {
        var sections = new[]
        {
            Section("display_processors", Registered(ComponentClass.Display, "panel-a")),
            Section("present_owners", new ComponentItem("some_app.exe", Level.Ok, Array.Empty<(string, Level)>(), Array.Empty<string>())),
        };
        foreach (var source in new CatalogSource?[] { null, CatalogSource.None })
        {
            var groups = ComponentRows.Join(sections, source);
            Assert.Equal(new[] { "display_processors", "present_owners" }, groups.Select(g => g.Section.Id)); // no "Apps" group
            var dp = Assert.Single(groups[0].Rows);
            Assert.Equal(RowStatus.Installed, dp.Status);
            Assert.Null(dp.RemoveNote);
            Assert.Equal(CatalogAction.None, dp.Action);
            // A connected client is neither installed nor installable: it has no status at all.
            var client = Assert.Single(groups[1].Rows);
            Assert.Null(client.Status);
            Assert.Null(client.StatusChip);
        }
    }

    [Fact]
    public void TheReceiptsAlone_AreNotACatalog_SoNothingIsCalledNotFromCatalog()
    {
        Assert.True(CatalogList.TryParseInstalled("{\"installed\":[{\"id\":\"pen\",\"name\":\"Pen\",\"class\":\"input_provider\",\"version\":\"1.0.0\",\"can_uninstall\":true}]}", out var receipts));
        var offline = CatalogRead.From(CatalogExit.Offline, "", true, false, null) with { List = receipts };
        var source = CatalogSource.From(offline, null);
        Assert.False(source.Stale);
        Assert.False(source.Complete);

        var sections = new[] { Section("input_providers", Registered(ComponentClass.Input, "pen"), Registered(ComponentClass.Input, "pad")) };
        var groups = ComponentRows.Join(sections, source);
        var pen = Row(groups, "input_provider:pen");
        Assert.Equal(RowStatus.Installed, pen.Status);
        Assert.True(pen.CanUninstall); // offline, what the engine installed can still be removed
        var pad = Row(groups, "input_provider:pad");
        Assert.Equal(RowStatus.Installed, pad.Status);
        Assert.Null(pad.RemoveNote);
    }

    [Fact]
    public void WhenALaterReadFails_TheLastCatalogIsKept_MarkedStale()
    {
        var good = Catalog(Item("panel-a", "display_processor", "current", receipt: true), Item("panel-c", "display_processor", "not_installed"));
        Assert.Equal(new CatalogSource(good, false, true), CatalogSource.From(CatalogRead.Ready(good), null));
        Assert.Equal(CatalogSource.None, CatalogSource.From(null, good)); // nothing was read yet: nothing to show as stale

        var offline = CatalogRead.From(CatalogExit.Offline, "", true, false, null);
        var source = CatalogSource.From(offline, good);
        Assert.True(source.Stale);
        Assert.True(source.Complete);

        var rows = ComponentRows.Join(new[] { Section("display_processors", Registered(ComponentClass.Display, "panel-a")) }, source)[0].Rows;
        Assert.Equal(2, rows.Count); // the catalog-only row is not dropped
        Assert.All(rows, r => Assert.True(r.Stale));
        Assert.Equal(RowStatus.Available, rows[1].Status);
        // Offline, a last-known row offers no Install, and an installed one no Update.
        Assert.Equal(CatalogAction.Install, rows[1].Catalog!.Action);
        Assert.Equal(CatalogAction.None, rows[1].Action);
        var outdated = Catalog(Item("panel-a", "display_processor", "outdated", receipt: true));
        var staleUpdate = Assert.Single(ComponentRows.Join(new[] { Section("display_processors", Registered(ComponentClass.Display, "panel-a")) },
            CatalogSource.From(offline, outdated))[0].Rows);
        Assert.Equal(RowStatus.UpdateAvailable, staleUpdate.Status);
        Assert.Equal(CatalogAction.None, staleUpdate.Action);
        Assert.True(staleUpdate.CanUninstall); // removing needs no catalog

        // No earlier read: only the banner; the installed row stays, with nothing claimed about it.
        var bare = ComponentRows.Join(new[] { Section("display_processors", Registered(ComponentClass.Display, "panel-a")) }, CatalogSource.From(offline, null))[0].Rows;
        Assert.False(Assert.Single(bare).Stale);

        Assert.Contains("09:05", CatalogSource.StaleNote(new DateTime(2026, 1, 2, 9, 5, 0)));
    }

    [Fact]
    public void CatalogItemsThatAreNotComponents_CloseThePageAsApps_AndTheRuntimeOpensIt()
    {
        var sections = new[] { Section("display_processors"), Section("diagnostics") };
        var groups = ComponentRows.Join(sections, Live(
            Item("viewer", "app", "not_installed"),
            Item("browser", "app", "current"),
            Item("runtime", "runtime", "outdated"),
            Item("toolkit", "sdk", "not_installed"),
            Item("panel-c", "display_processor", "not_installed")));

        Assert.Equal(new[] { ComponentRows.RuntimeGroupId, "display_processors", "diagnostics", ComponentRows.AppsGroupId }, groups.Select(g => g.Section.Id));
        var apps = groups[^1];
        Assert.Equal("Apps", apps.Section.Title);
        Assert.Equal("1 installed · 2 available to install", apps.Section.ConnectedLine);
        // Installed first inside a class; apps before the other kinds. The runtime is not an app.
        Assert.Equal(new[] { "browser", "viewer", "toolkit" }, apps.Rows.Select(r => r.Catalog!.Id));
        Assert.DoesNotContain(apps.Rows, r => r.Catalog!.Class == "display_processor");

        // The runtime's own row: a first group of its own, shown, never installed or removed from here.
        Assert.Equal("DisplayXR", groups[0].Section.Title);
        Assert.Equal("1 installed", groups[0].Section.ConnectedLine);
        var runtime = Assert.Single(groups[0].Rows);
        Assert.Equal("runtime", runtime.Catalog!.Id);
        Assert.Equal(RowStatus.UpdateAvailable, runtime.Status);
        Assert.Equal(CatalogAction.None, runtime.Action);
        Assert.False(runtime.CanUninstall);
        Assert.Equal("Remove it from Windows Settings > Apps.", runtime.RemoveNote);

        // Under a filter the Runtime group behaves like Apps: kept while it has a row to show.
        Assert.Equal(ComponentRows.RuntimeGroupId, ComponentRows.Filter(groups, RowFilter.Installed)[0].Section.Id);
        Assert.Equal(ComponentRows.RuntimeGroupId, ComponentRows.Filter(groups, RowFilter.Updates)[0].Section.Id);

        var browser = apps.Rows[0]; // no receipt: its own uninstaller
        Assert.Equal(RowStatus.Installed, browser.Status);
        Assert.NotNull(browser.RemoveNote);
    }

    [Fact]
    public void Filters_All_Installed_Updates()
    {
        var sections = new[]
        {
            Section("display_processors", Registered(ComponentClass.Display, "panel-a"), Registered(ComponentClass.Display, "panel-b")),
            Section("input_providers"),
            Section("present_owners", new ComponentItem("some_app.exe", Level.Ok, Array.Empty<(string, Level)>(), Array.Empty<string>())),
        };
        var all = ComponentRows.Join(sections, Live(
            Item("panel-a", "display_processor", "current", receipt: true),
            Item("panel-b", "display_processor", "outdated", receipt: true),
            Item("panel-c", "display_processor", "not_installed"),
            Item("hands", "input_provider", "not_installed"),
            Item("viewer", "app", "not_installed")));

        Assert.Same(all, ComponentRows.Filter(all, RowFilter.All));
        Assert.Equal(6, all.Sum(g => g.Rows.Count));

        var installed = ComponentRows.Filter(all, RowFilter.Installed);
        Assert.DoesNotContain(installed.SelectMany(g => g.Rows), r => r.Status == RowStatus.Available);
        // The sections stay (an empty one says so), with what is connected; "Apps" goes when none is installed.
        Assert.Equal(new[] { "display_processors", "input_providers", "present_owners" }, installed.Select(g => g.Section.Id));
        Assert.Equal(new[] { 2, 0, 1 }, installed.Select(g => g.Rows.Count));

        var updates = ComponentRows.Filter(all, RowFilter.Updates);
        Assert.Equal("display_processor:panel-b", Assert.Single(Assert.Single(updates).Rows).Key);
    }

    [Fact]
    public void ARowKeepsItsKey_WhenItGoesFromAvailableToInstalled()
    {
        var before = ComponentRows.Join(new[] { Section("conversion_providers") },
            Live(Item("depth-pkg", "conversion_provider", "not_installed", registered: "depth")));
        var after = ComponentRows.Join(new[] { Section("conversion_providers", Registered(ComponentClass.Conversion, "depth")) },
            Live(Item("depth-pkg", "conversion_provider", "current", receipt: true, registered: "depth")));

        string key = Assert.Single(before[0].Rows).Key;
        Assert.Equal(key, ComponentRows.KeyOf(before[0].Rows[0].Catalog!)); // what the page opens after an install
        var row = Assert.Single(after[0].Rows);
        Assert.Equal(key, row.Key);
        Assert.NotNull(row.Installed);
        Assert.Equal(RowStatus.Installed, row.Status);
    }

    [Fact]
    public void ABuiltInRow_IsPartOfTheRuntime_NotSomethingToRemove()
    {
        var sections = new[] { Section("conversion_providers", Registered(ComponentClass.Conversion, "built-in", builtIn: true)) };
        var row = Assert.Single(ComponentRows.Join(sections, Live(Item("depth", "conversion_provider", "not_installed")))[0].Rows.Where(r => r.Installed is not null));
        Assert.Equal(RowStatus.PartOfRuntime, row.Status);
        Assert.Equal(("Part of the runtime", Level.Plain), row.StatusChip);
        Assert.Null(row.RemoveNote);
        Assert.Equal(CatalogAction.None, row.Action);

        // With no catalog read, and before the class's policy list is read (the load verdict alone says it).
        Assert.Equal(RowStatus.PartOfRuntime, Assert.Single(ComponentRows.Join(sections, null)[0].Rows).Status);
        var verdictOnly = new ComponentItem("built-in", Level.Ok, Array.Empty<(string, Level)>(), Array.Empty<string>(), Id: "built-in", BuiltIn: true);
        Assert.Equal(RowStatus.PartOfRuntime,
            Assert.Single(ComponentRows.Join(new[] { Section("conversion_providers", verdictOnly) }, Live())[0].Rows).Status);
    }

    [Fact]
    public void ARow_ShowsItsStatus_OneStateWord_AndWorksWithYourDisplay_EverythingElseIsInDetails()
    {
        var chips = new ChipList();
        chips.Add("Load state", "LOADED", Level.Plain);
        chips.Add("Platform", "READY", Level.Plain);
        chips.Add("Signature", "signed by Example Co", Level.Ok);
        chips.Add("Licence", "trial until 2026-12-21", Level.Info);
        var item = new ComponentItem("panel-a", Level.Ok, chips.Chips, Array.Empty<string>(),
            Policy: Policy(ComponentClass.Display, "panel-a"), Facts: chips.Facts, State: PlainState.Ready);
        string detected = Item("panel-a", "display_processor", "current", receipt: true).Replace("}", ",\"detected\":\"high\",\"recommended\":true,\"size\":971776}");
        var row = Assert.Single(ComponentRows.Join(new[] { Section("display_processors", item) }, Live(detected))[0].Rows);

        var face = row.Face;
        Assert.Equal(("Installed", Level.Ok), face.Status);
        Assert.Equal(("Ready", Level.Plain), face.State);
        Assert.True(face.WorksWithDisplay);
        Assert.Equal(new[]
        {
            ("Load state", "LOADED"), ("Platform", "READY"), ("Signature", "signed by Example Co"),
            ("Licence", "trial until 2026-12-21"), ("Catalog", "Recommended"),
        }, face.Details);

        // Several values of one label are one line.
        var client = new ComponentItem("app.exe", Level.Ok, new[] { ("focused", Level.Ok), ("active", Level.Plain) }, Array.Empty<string>(),
            Facts: new[] { ("Flags", "focused"), ("Flags", "active") });
        var clientFace = Assert.Single(ComponentRows.Join(new[] { Section("present_owners", client) }, null)[0].Rows).Face;
        Assert.Null(clientFace.Status);
        Assert.Null(clientFace.State);
        Assert.Equal(new[] { ("Flags", "focused, active") }, clientFace.Details);

        // A row with no facts of its own lists its chips under "State".
        var bare = new ComponentItem("x", Level.Ok, new[] { ("connected", Level.Ok) }, Array.Empty<string>());
        Assert.Equal(new[] { ("State", "connected") }, bare.DetailFacts);
    }

    [Fact]
    public void ALastKnownRow_SaysSoInItsStatus()
    {
        var good = Catalog(Item("panel-c", "display_processor", "not_installed"));
        var offline = CatalogRead.From(CatalogExit.Offline, "", true, false, null);
        var row = Assert.Single(ComponentRows.Join(new[] { Section("display_processors") }, CatalogSource.From(offline, good))[0].Rows);
        Assert.Equal(("Available (last known)", Level.Plain), row.Face.Status);
        Assert.Equal(("Available", Level.Plain), Assert.Single(ComponentRows.Join(new[] { Section("display_processors") }, Live(
            Item("panel-c", "display_processor", "not_installed")))[0].Rows).Face.Status);
    }

    [Fact]
    public void PolicyNotes_SayTheScope_AndEveryMachineWideCommand_ForTheDetails()
    {
        var row = new PolicyRow(ComponentClass.Display, "panel-a", "panel-a", true, "default", false, false, "ACTIVE", "signed", null, null);
        var list = new PolicyList(ComponentClass.Display, null, new[] { row }, new Dictionary<string, string>(), Array.Empty<PolicyTask>());

        var notes = PolicyCommands.Notes(row, list, machine: false, elevated: false);
        Assert.StartsWith("For you only; for everyone on this PC needs administrator permission.", notes[0].Text);
        Assert.Contains(ComponentClass.Display.AppliesNote(), notes[0].Text);
        Assert.Contains(("Machine-wide, from an administrator prompt:", false), notes);
        Assert.Contains(("displayxr-cli dp disable panel-a --machine", true), notes);
        Assert.Contains(("displayxr-cli dp use panel-a --machine", true), notes);

        // Elevated: the dashboard can write machine-wide itself, so no command to copy.
        var elevated = PolicyCommands.Notes(row, list, machine: true, elevated: true);
        Assert.Equal(new[] { ($"For everyone on this PC. {ComponentClass.Display.AppliesNote()}", false) }, elevated);

        var builtIn = row with { Class = ComponentClass.Conversion, Id = "built-in", EnabledSource = "", BuiltIn = true };
        var conv = new PolicyList(ComponentClass.Conversion, null, new[] { builtIn }, new Dictionary<string, string>(), Array.Empty<PolicyTask>());
        Assert.StartsWith("Built-in adapter:", PolicyCommands.Notes(builtIn, conv, false, false)[0].Text);
    }

    [Fact]
    public void RowsWithoutAnId_AndSectionsTheCatalogHasNoClassFor_AreLeftAsTheyAre()
    {
        var summary = new ComponentItem("Rig (navigation) role", Level.Plain, Array.Empty<(string, Level)>(), Array.Empty<string>());
        var groups = ComponentRows.Join(new[] { Section("input_providers", summary), Section("conversion_models", summary) },
                                        Live(Item("hands", "input_provider", "not_installed")));
        Assert.Null(groups[0].Rows[0].Status);
        Assert.Equal(RowStatus.Available, groups[0].Rows[1].Status);
        Assert.Null(Assert.Single(groups[1].Rows).Status);
        Assert.Null(ComponentRows.CatalogClassOf("conversion_models"));
        Assert.Equal("workspace_controller", ComponentRows.CatalogClassOf("workspace_controller"));
    }

    [Fact]
    public void AWorkspaceController_JoinsByItsRegisteredId()
    {
        var controller = new RegisteredController("desk", "Desk", "1.0.0", "desk.exe", Array.Empty<string>());
        var item = new ComponentItem("Desk", Level.Plain, Array.Empty<(string, Level)>(), Array.Empty<string>(), controller);
        var row = Assert.Single(ComponentRows.Join(new[] { Section("workspace_controller", item) },
            Live(Item("desk", "workspace_controller", "current", receipt: true)))[0].Rows);
        Assert.Equal(RowStatus.Installed, row.Status);
        Assert.Same(controller, row.Installed!.Controller); // its launch controls stay on the row
        Assert.True(row.CanUninstall);
    }

    [Fact]
    public void TheBadge_CountsTheUpdatesTheButtonCanDo()
    {
        Assert.Equal(0, ComponentRows.Updates(null));
        Assert.Equal(1, ComponentRows.Updates(Catalog(
            Item("panel-b", "display_processor", "outdated"),
            Item("runtime", "runtime", "outdated"),                          // updated from the Download page, not from here
            Item("pad", "input_provider", "outdated", installable: false),  // nothing the engine can fetch
            Item("panel-c", "display_processor", "not_installed"))));
    }

    [Theory]
    [InlineData("catalog", "components")]
    [InlineData("components", "components")]
    [InlineData("home", "home")]
    [InlineData("no-such-page", "no-such-page")]
    public void TheCatalogRoute_OpensComponents(string id, string page) => Assert.Equal(page, PageRoutes.Resolve(id));
}

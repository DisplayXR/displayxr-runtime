// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using DisplayXR.Dashboard.Feed;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>
/// ADR-064 step 8: the Catalog tab. The engine's <c>list --json</c> in its schema-2 and
/// schema-1-fallback shapes, what every exit code says, the licence block, the URL rule, and the
/// two things that must never happen: a key on a command line, and a URL through a shell.
/// The JSON fixtures are written by hand from the emitter (get_core.c <c>dxr_list_catalog</c>,
/// see fixtures/gen_catalog_fixtures.py); none was captured from a running engine.
/// </summary>
public class CatalogTests
{
    private static CatalogList Parse(string file)
    {
        Assert.True(CatalogList.TryParse(TestData.Text(file), out var list), file);
        return list!;
    }

    private static PolicyList Policy(ComponentClass cls, string file)
    {
        Assert.True(PolicyList.TryParse(cls, TestData.Text(file), out var list), file);
        return list!;
    }

    // ── list --json ─────────────────────────────────────────────────────────

    [Fact]
    public void Schema2_ReadsEveryFieldThePageShows()
    {
        var list = Parse("catalog-list-v2.json");
        Assert.Equal(2, list.Schema);
        Assert.Equal(new[] { "runtime", "leia-sr", "browser", "open-conversion", "leia-conversion", "ultraleap" },
                     list.Items.Select(i => i.Id));

        var leia = list.Find("leia-sr")!;
        Assert.Equal("Leia SR display processor", leia.Name);
        Assert.Equal("DisplayXR", leia.Publisher); // the catalog's publisher id, shown as a name
        Assert.True(leia.HasUpdate);
        Assert.Equal(("Update available", Level.Info), leia.StateChip);
        Assert.True(leia.DetectedHere);
        Assert.Contains("EDID", leia.Evidence);
        Assert.Equal(CatalogAction.Update, leia.Action);
        Assert.True(leia.CanUninstall);
        Assert.Equal("30 MB", leia.SizeText);
        Assert.Equal("2.22.0 installed · 2.23.0 available", leia.VersionText);
        Assert.Same(leia, list.Find("leia_plugin")); // the schema-1 name still resolves
        Assert.Same(leia, list.FindComponent(ComponentClass.Display, "leia-sr"));

        var open = list.Find("open-conversion")!;
        Assert.True(open.Recommended);
        Assert.False(open.IsInstalled);
        Assert.Null(open.StateChip); // not installed is not a chip: the button says "Get it"
        Assert.Equal(CatalogAction.Install, open.Action);
        Assert.Equal("180 MB", open.SizeText);
        Assert.Equal("2D-to-3D conversion with open models. Needs no vendor software.", CatalogText.About(open));

        var browser = list.Find("browser")!;
        Assert.Equal(("Installed", Level.Ok), browser.StateChip);
        Assert.Equal(CatalogAction.None, browser.Action);
        Assert.False(browser.CanUninstall); // installed by its own installer: no receipt

        var hands = list.Find("ultraleap")!;
        var need = Assert.Single(hands.Unmet);
        Assert.Equal("Ultraleap tracking software", need.Name);
        Assert.Equal("Get it from www.ultraleap.com, then come back.", need.NextStep);
        Assert.Equal("Ready on this machine.", list.Find("leia-conversion")!.Prerequisites[0].NextStep);
    }

    [Fact]
    public void TheRuntimeIsShownButNeverInstalledFromHere()
    {
        var v1 = Parse("catalog-list-v1.json");
        var rt = v1.Find("runtime")!;
        Assert.True(rt.IsRuntime);
        Assert.True(rt.HasUpdate);
        Assert.Equal(CatalogAction.None, rt.Action);
        Assert.False(rt.CanUninstall);
    }

    [Fact]
    public void Schema1Fallback_HasNoSummariesPublishersOrPrerequisites_AndStillReads()
    {
        var list = Parse("catalog-list-v1.json");
        Assert.Equal(1, list.Schema);
        Assert.Equal(3, list.Items.Count);
        Assert.All(list.Items, i =>
        {
            Assert.Equal("", i.Publisher);
            Assert.Equal("", i.Summary);
            Assert.False(i.Recommended);
            Assert.Empty(i.Prerequisites);
            Assert.NotEqual("", CatalogText.About(i)); // a sentence by class stands in
        });
        var leia = list.Find("leia_plugin")!;
        Assert.Equal(CatalogAction.Install, leia.Action);
        Assert.True(leia.DetectedHere);
    }

    [Fact]
    public void Browse_GroupsByClassInPageOrder_DetectedFirst_AndFilters()
    {
        var list = Parse("catalog-list.json"); // the layout fixture
        var groups = list.Browse("", null, false);
        Assert.Equal(new[] { "Displays", "Input", "2D to 3D", "Viewer tracking", "Workspaces", "Apps", "DisplayXR" },
                     groups.Select(g => CatalogClasses.Label(g.Class)));
        Assert.Equal("leia-sr", groups[0].Items[0].Id);               // works with this display: first
        Assert.Equal("open-conversion", groups[2].Items[0].Id);       // recommended: before the rest

        Assert.All(list.Browse("", null, installedOnly: true).SelectMany(g => g.Items), i => Assert.True(i.IsInstalled));
        Assert.Equal(new[] { "conversion_provider" }, list.Browse("", "conversion_provider", false).Select(g => g.Class));
        Assert.Equal(new[] { "ultraleap" }, list.Browse("hand camera", null, false).SelectMany(g => g.Items).Select(i => i.Id));
        Assert.Empty(list.Browse("no such thing", null, false));
    }

    [Fact]
    public void AnIncompatibleItem_HasNoAction_AndACalmReason()
    {
        var item = Parse("catalog-list.json").Find("acme-xr-panel")!;
        Assert.False(item.Compatible);
        Assert.Equal(CatalogAction.None, item.Action);
        Assert.Contains("2.75.0", item.IncompatibleReason);
        Assert.DoesNotContain("error", CatalogText.OtherVersion, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void ALinkOut_OpensItsListing_OnlyWhenItIsHttps()
    {
        var item = Parse("catalog-list.json").Find("acme-anatomy")!;
        Assert.Equal(CatalogAction.OpenListing, item.Action);
        Assert.Equal(CatalogAction.None, (item with { Listing = "http://store.example.com/x" }).Action);
        Assert.Equal(CatalogAction.None, (item with { Listing = "file:///C:/x.exe" }).Action);
    }

    [Theory]
    [InlineData("--default")]
    [InlineData("-x")]
    [InlineData("a b")]
    [InlineData("a;b")]
    [InlineData("")]
    [InlineData("a\"b")]
    public void AnIdThatIsNotOneSafeArgument_IsNeverListedOrRun(string id)
    {
        Assert.False(CatalogList.IdIsSafe(id));
        string json = "{\"schema\":2,\"items\":[{\"id\":" + System.Text.Json.JsonSerializer.Serialize(id) + ",\"name\":\"x\",\"class\":\"app\"}]}";
        Assert.True(CatalogList.TryParse(json, out var list));
        Assert.Empty(list!.Items);
        Assert.Throws<ArgumentException>(() => CatalogCommands.Install(id));
        Assert.Throws<ArgumentException>(() => CatalogCommands.Uninstall(id));
    }

    [Fact]
    public void Commands_AreTheEnginesVerbs_AndNeverMoveTheRuntime()
    {
        Assert.Equal(new[] { "list", "--json", "--no-runtime" }, CatalogCommands.List().Args);
        Assert.Equal(new[] { "install", "fixture.multi-asset", "--yes", "--json", "--no-runtime" }, CatalogCommands.Install("fixture.multi-asset").Args);
        Assert.Equal(CatalogCommands.Install("x").Args, CatalogCommands.For(CatalogVerb.Update, "x").Args);
        Assert.Equal(new[] { "uninstall", "browser", "--json" }, CatalogCommands.Uninstall("browser").Args);
        Assert.Equal(new[] { "list", "--installed", "--json" }, CatalogCommands.ListInstalled().Args);
    }

    // ── Reads that are not a list ───────────────────────────────────────────

    [Fact]
    public void Exit10_IsOffline_Exit11_IsRefused_TheRestSayWhatToDo()
    {
        var offline = CatalogRead.From(10, TestData.Text("catalog-offline.json"), true, false, null);
        Assert.Equal(CatalogStatus.Offline, offline.Status);
        Assert.Equal("You're offline", offline.Title);
        Assert.Contains("keeps working", offline.Text);

        Assert.Equal(CatalogStatus.Refused, CatalogRead.From(11, "", true, false, null).Status);
        Assert.Equal(CatalogStatus.Failed, CatalogRead.From(0, "not json", true, false, null).Status);
        Assert.Equal(CatalogStatus.Failed, CatalogRead.From(2, "", true, false, null).Status);
        Assert.Equal(CatalogStatus.Failed, CatalogRead.From(-1, "", false, false, "no such file").Status);
        Assert.Equal(CatalogStatus.Failed, CatalogRead.From(-1, "", true, true, null).Status);
        Assert.Equal(CatalogStatus.Ready, CatalogRead.From(0, TestData.Text("catalog-list-v2.json"), true, false, null).Status);
        // The offline document is not a list, whatever the exit code says.
        Assert.NotEqual(CatalogStatus.Ready, CatalogRead.From(0, TestData.Text("catalog-offline.json"), true, false, null).Status);
    }

    [Fact]
    public void Receipts_ReadAsInstalledItems_ForTheOfflineView()
    {
        Assert.True(CatalogList.TryParseInstalled(TestData.Text("catalog-installed.json"), out var list));
        Assert.Equal(new[] { "leia-sr", "open-conversion" }, list!.Items.Select(i => i.Id));
        Assert.All(list.Items, i => { Assert.True(i.IsInstalled); Assert.True(i.CanUninstall); Assert.Equal(CatalogAction.None, i.Action); });
        Assert.Equal("2.22.0 installed", list.Items[0].VersionText);
        Assert.False(CatalogList.TryParseInstalled(TestData.Text("catalog-list-v2.json"), out _));
    }

    [Fact]
    public async Task NoEngineOnThisPlatform_AndAMissingEngine_AreDesignedStates_NotErrors()
    {
        var posix = new DxrGetEngine(false, null, "/usr/lib/displayxr/bin/displayxr-cli", "/home/u", _ => false);
        Assert.Equal(EngineAvailability.NoEngineOnThisPlatform, posix.Availability);
        var read = await new CatalogService(posix, new RecordingCli()).ListAsync();
        Assert.Equal(CatalogStatus.NoEngineHere, read.Status);
        Assert.Contains("Download page", read.Title);

        var old = new DxrGetEngine(true, null, @"C:\Program Files\DisplayXR\Runtime\displayxr-cli.exe", @"C:\Program Files", _ => false);
        Assert.Equal(EngineAvailability.Missing, old.Availability);
        Assert.Equal(@"C:\Program Files\DisplayXR\Runtime\dxr-get.exe", old.Path);
        read = await new CatalogService(old, new RecordingCli()).ListAsync();
        Assert.Equal(CatalogStatus.EngineMissing, read.Status);
        Assert.Contains("dxr-get.exe", read.Detail);
    }

    [Fact]
    public void TheEngine_IsResolvedBesideTheCli_ThenTheInstalledRuntime_OverrideFirst()
    {
        string cli = @"D:\dev\_package\bin\displayxr-cli.exe";
        Assert.Equal(@"D:\dev\_package\bin\dxr-get.exe", DxrGetEngine.Resolve(true, null, cli, @"C:\Program Files", p => p.StartsWith(@"D:\")));
        Assert.Equal(@"C:\Program Files\DisplayXR\Runtime\dxr-get.exe", DxrGetEngine.Resolve(true, null, cli, @"C:\Program Files", p => p.StartsWith(@"C:\")));
        Assert.Equal(@"E:\x\dxr-get.exe", DxrGetEngine.Resolve(true, @"E:\x\dxr-get.exe", cli, @"C:\Program Files", _ => true));
    }

    // ── Exit codes ──────────────────────────────────────────────────────────

    public static IEnumerable<object[]> Codes() =>
        from verb in new[] { CatalogVerb.Install, CatalogVerb.Update, CatalogVerb.Uninstall }
        from code in new[] { 0, 2, 10, 11, 20, 21, 30, 40, 41, 42, 43, 50, 77 }
        select new object[] { verb, code };

    [Theory]
    [MemberData(nameof(Codes))]
    public void EveryExitCode_HasATitleAndANextStep(CatalogVerb verb, int code)
    {
        var o = CatalogExit.Describe(verb, code, "Acme Depth Studio");
        Assert.Equal(code == 0, o.Ok);
        Assert.Equal(code, o.ExitCode);
        Assert.False(string.IsNullOrWhiteSpace(o.Title));
        Assert.False(string.IsNullOrWhiteSpace(o.Text));
        // A documented code is never shown as a number; only an unknown one names it.
        Assert.Equal(code == 77, o.Text.Contains("77"));
        Assert.DoesNotContain("exit", o.Title, StringComparison.OrdinalIgnoreCase);
    }

    [Fact]
    public void ExitCodes_SayTheRightThing()
    {
        string n = "Acme Depth Studio";
        Assert.Equal("Acme Depth Studio is installed", CatalogExit.Describe(CatalogVerb.Install, 0, n).Title);
        Assert.Equal("Acme Depth Studio is up to date", CatalogExit.Describe(CatalogVerb.Update, 0, n).Title);
        Assert.Equal("Acme Depth Studio was removed", CatalogExit.Describe(CatalogVerb.Uninstall, 0, n).Title);
        Assert.Equal("You're offline", CatalogExit.Describe(CatalogVerb.Install, 10, n).Title);
        Assert.Contains("verified", CatalogExit.Describe(CatalogVerb.Install, 11, n).Title);
        Assert.Contains("picks up where it stopped", CatalogExit.Describe(CatalogVerb.Install, 20, n).Text);
        Assert.Contains("not installed", CatalogExit.Describe(CatalogVerb.Install, 21, n).Text);
        Assert.Contains("did not finish installing", CatalogExit.Describe(CatalogVerb.Install, 30, n).Title);
        Assert.Contains("could not be removed", CatalogExit.Describe(CatalogVerb.Uninstall, 30, n).Title);
        Assert.Contains("isn't available", CatalogExit.Describe(CatalogVerb.Install, 40, n).Title);
        Assert.Contains("Update DisplayXR", CatalogExit.Describe(CatalogVerb.Install, 41, n).Text);
        Assert.Contains("Windows Settings", CatalogExit.Describe(CatalogVerb.Uninstall, 42, n).Text);
        Assert.Contains("can't be removed from here", CatalogExit.Describe(CatalogVerb.Uninstall, 43, n).Title);
        Assert.Contains("permission", CatalogExit.Describe(CatalogVerb.Install, 50, n).Title);
        // Only a verification or installer problem is a warning; the rest is information.
        Assert.Equal(Level.Info, CatalogExit.Describe(CatalogVerb.Install, 10, n).Level);
        Assert.Equal(Level.Warn, CatalogExit.Describe(CatalogVerb.Install, 21, n).Level);
        Assert.Equal(Level.Info, CatalogExit.Declined(n).Level);
        Assert.Contains("keeps going", CatalogExit.StoppedWaiting(n).Text);
    }

    // ── The operation: unelevated first, then one prompt ────────────────────

    private static CatalogItem Item(string id = "acme-depth") => Parse("catalog-list.json").Find(id)!;

    [Fact]
    public async Task Install_AsksTheEngineUnelevatedFirst_ThenElevatesOnce_OnExit50()
    {
        var engine = new FakeEngine { First = 50, Elevated = new ElevatedResult(ElevatedEnd.Exited, 0) };
        var phases = new List<string>();
        var o = await new CatalogService(engine, new RecordingCli()).RunAsync(CatalogVerb.Install, Item("open-conversion"), phases.Add, default);
        Assert.True(o.Ok);
        Assert.Equal("Open conversion provider is installed", o.Title);
        Assert.Equal(1, engine.Runs);
        Assert.Equal(1, engine.ElevatedRuns);
        Assert.Equal(engine.LastArgs!.Args, engine.LastElevatedArgs!.Args); // the same command, elevated
        Assert.Equal(new[] { "Checking…", "Waiting for your permission…" }, phases.Take(2));
    }

    [Theory]
    [InlineData(0, true)]    // nothing to do
    [InlineData(10, false)]  // offline
    [InlineData(11, false)]
    [InlineData(41, false)]  // not compatible
    [InlineData(42, false)]  // uninstall: no receipt
    [InlineData(43, false)]
    public async Task WhatElevationCannotFix_NeverRaisesAPrompt(int code, bool ok)
    {
        var engine = new FakeEngine { First = code };
        var o = await new CatalogService(engine, new RecordingCli()).RunAsync(CatalogVerb.Install, Item(), _ => { }, default);
        Assert.Equal(ok, o.Ok);
        Assert.Equal(0, engine.ElevatedRuns);
        Assert.Equal(code, o.ExitCode);
    }

    [Fact]
    public async Task EveryWayTheElevatedRunCanEnd_IsAnOutcome_NeverAHang()
    {
        async Task<CatalogOutcome> End(ElevatedResult r) =>
            await new CatalogService(new FakeEngine { First = 50, Elevated = r }, new RecordingCli())
                .RunAsync(CatalogVerb.Install, Item(), _ => { }, default);

        Assert.Equal("Nothing was changed", (await End(new ElevatedResult(ElevatedEnd.Declined))).Title);
        Assert.Equal("Still working in the background", (await End(new ElevatedResult(ElevatedEnd.StoppedWaiting))).Title);
        Assert.Contains("longer than expected", (await End(new ElevatedResult(ElevatedEnd.TimedOut))).Title);
        Assert.Contains("did not start", (await End(new ElevatedResult(ElevatedEnd.StartFailed, Error: "access denied"))).Title);
        Assert.Equal(20, (await End(new ElevatedResult(ElevatedEnd.Exited, 20))).ExitCode);
        Assert.All(new[] { ElevatedEnd.Declined, ElevatedEnd.StoppedWaiting, ElevatedEnd.TimedOut, ElevatedEnd.StartFailed },
                   e => Assert.False(End(new ElevatedResult(e)).Result.Ok));
    }

    [Fact]
    public async Task AnEngineThatDoesNotStart_OrTimesOut_IsSaid()
    {
        var svc = new CatalogService(new FakeEngine { StartError = "not found" }, new RecordingCli());
        Assert.Contains("did not start", (await svc.RunAsync(CatalogVerb.Install, Item(), _ => { }, default)).Title);
        svc = new CatalogService(new FakeEngine { TimesOut = true }, new RecordingCli());
        Assert.Contains("longer than expected", (await svc.RunAsync(CatalogVerb.Install, Item(), _ => { }, default)).Title);
    }

    [Fact]
    public async Task Offline_StillListsWhatTheEngineInstalled()
    {
        var engine = new FakeEngine { First = 10, InstalledJson = TestData.Text("catalog-installed.json") };
        var read = await new CatalogService(engine, new RecordingCli()).ListAsync();
        Assert.Equal(CatalogStatus.Offline, read.Status);
        Assert.Equal(2, read.List!.Items.Count);
    }

    [Fact]
    public void TheElevatedRun_IsTheEngineItself_ThroughRunas_WithAnArgumentVector()
    {
        var psi = DxrGetEngine.ElevatedStartInfo(@"C:\Program Files\DisplayXR\Runtime\dxr-get.exe", CatalogCommands.Install("leia-sr"));
        Assert.True(psi.UseShellExecute);
        Assert.Equal("runas", psi.Verb);
        Assert.Equal(@"C:\Program Files\DisplayXR\Runtime\dxr-get.exe", psi.FileName); // never cmd.exe / powershell
        Assert.Equal(new[] { "install", "leia-sr", "--yes", "--json", "--no-runtime" }, psi.ArgumentList);
        Assert.Equal("", psi.Arguments);
        Assert.Equal(System.Diagnostics.ProcessWindowStyle.Hidden, psi.WindowStyle);
    }

    [Fact]
    public async Task TheFixtureEngine_FollowsTheEnginesOrder_AndMovesTheState()
    {
        var engine = new FixtureCatalogEngine(TestData.Path("catalog-list.json"), TimeSpan.Zero);
        var svc = new CatalogService(engine, new RecordingCli());
        var list = (await svc.ListAsync()).List!;

        Assert.True((await svc.RunAsync(CatalogVerb.Install, list.Find("open-conversion")!, _ => { }, default)).Ok);
        list = (await svc.ListAsync()).List!;
        Assert.Equal("current", list.Find("open-conversion")!.State);
        Assert.True(list.Find("open-conversion")!.CanUninstall);

        Assert.Equal(20, (await svc.RunAsync(CatalogVerb.Install, list.Find("media-player")!, _ => { }, default)).ExitCode);
        Assert.Equal("Nothing was changed", (await svc.RunAsync(CatalogVerb.Install, list.Find("model-viewer")!, _ => { }, default)).Title);
        Assert.Equal(41, (await svc.RunAsync(CatalogVerb.Install, list.Find("acme-xr-panel")!, _ => { }, default)).ExitCode);
        Assert.Equal(42, (await svc.RunAsync(CatalogVerb.Uninstall, list.Find("browser")!, _ => { }, default)).ExitCode);

        Assert.True((await svc.RunAsync(CatalogVerb.Uninstall, list.Find("open-conversion")!, _ => { }, default)).Ok);
        Assert.Equal("not_installed", (await svc.ListAsync()).List!.Find("open-conversion")!.State);
    }

    // ── Licence ─────────────────────────────────────────────────────────────

    [Fact]
    public void Licence_Hardware_IsIncluded_WithNothingToBuyOrPaste()
    {
        var row = Policy(ComponentClass.Conversion, "conv-list.json").Row("leia-conversion")!;
        var v = LicenceView.For(row, "conversion_provider")!;
        Assert.Equal("Included with your display", v.Headline);
        Assert.Equal(Level.Ok, v.Level);
        Assert.False(v.OfferBuy);
        Assert.False(v.OfferKey);
        Assert.Null(v.Reassurance);

        // The zero-click case holds in every state and whatever else the row says.
        foreach (string state in new[] { "licensed", "trial", "expired", "unlicensed" })
        {
            var hw = LicenceView.For(row with { Licence = state, LicencePurchaseUrl = "https://store.example.com/x", LicenceKeyActivation = true }, "conversion_provider")!;
            Assert.False(hw.OfferBuy);
            Assert.False(hw.OfferKey);
        }
        Assert.Equal("Included with this machine", LicenceView.For(row with { LicenceSource = "oem" }, "conversion_provider")!.Headline);
    }

    [Fact]
    public void Licence_Trial_SaysWhenItEnds_AndOffersBuyAndAKey()
    {
        var row = Policy(ComponentClass.Conversion, "conv-list.json").Row("acme-depth")!;
        var v = LicenceView.For(row, "conversion_provider")!;
        Assert.Equal("Trial — ends 21 Dec 2026", v.Headline);
        Assert.Equal("30-day trial, every feature unlocked", v.Message);
        Assert.True(v.OfferBuy);
        Assert.Equal("store.example.com", v.PurchaseUrl!.Host);
        Assert.True(v.OfferKey);
        Assert.Equal("Not licensed? Your other 2D-to-3D converters keep working.", v.Reassurance);
        Assert.True(row.LicenceDeepLink);
    }

    [Fact]
    public void Licence_Unlicensed_And_Expired_AreInvitations_NotAlarms()
    {
        var un = LicenceView.For(Policy(ComponentClass.Conversion, "conv-list.json").Row("northlight-3d")!, "conversion_provider")!;
        Assert.Equal("Ready to activate", un.Headline);
        Assert.Equal(Level.Info, un.Level);
        Assert.True(un.OfferBuy);
        Assert.True(un.OfferKey);

        var ex = LicenceView.For(Policy(ComponentClass.Tracking, "tracking-list.json").Row("open-tracking")!, "tracking_provider")!;
        Assert.Equal("Licence ended 31 Oct 2026", ex.Headline);
        Assert.True(ex.OfferBuy);
        Assert.Contains("viewer-tracking sources keep working", ex.Reassurance);
    }

    [Fact]
    public void Licence_FromTheRealCliFixture_NoKeySlot_NoKeyBox_NoHttpsNoBuy()
    {
        var list = Policy(ComponentClass.Tracking, "tracking-list-n5-licence.json");
        var trial = LicenceView.For(list.Row("sim-tracking")!, "tracking_provider")!;
        Assert.Equal("Trial — ends 1 Jan 2100", trial.Headline);
        Assert.False(trial.OfferBuy);  // no purchase URL
        Assert.False(trial.OfferKey);  // activation.key = false: it takes no key
        var un = LicenceView.For(list.Row("sim-tracking-b")!, "tracking_provider")!;
        Assert.True(un.OfferBuy);
        Assert.True(un.OfferKey);
        Assert.Null(LicenceView.For(list.Row("plain")!, "tracking_provider")); // nothing to report: no block at all

        var row = list.Row("sim-tracking-b")!;
        Assert.False(LicenceView.For(row with { LicencePurchaseUrl = "http://example.com/buy" }, "tracking_provider")!.OfferBuy);
        Assert.False(LicenceView.For(row with { LicencePurchaseUrl = "javascript:alert(1)" }, "tracking_provider")!.OfferBuy);
        // A CLI that predates the activation object: the key box is offered, the CLI will answer.
        Assert.True(LicenceView.For(row with { LicenceKeyActivation = null }, "tracking_provider")!.OfferKey);
    }

    [Fact]
    public void ActivationAnswers_AreWords_AndNeverEchoAKey()
    {
        Assert.True(ActivationOutcome.From(true, "{\"result\":\"ok\",\"accepted\":true,\"error\":\"\"}", "X").Accepted);
        var refused = ActivationOutcome.From(false, "{\"result\":\"refused\",\"accepted\":false,\"error\":\"the key was not recognised\"}", "X");
        Assert.False(refused.Accepted);
        Assert.Equal("X said: the key was not recognised", refused.Text);
        Assert.Contains("doesn't take a key", ActivationOutcome.From(false, "{\"result\":\"unsupported\",\"accepted\":false}", "X").Title);
        Assert.Contains("couldn't be reached", ActivationOutcome.From(false, "{\"result\":\"not-loaded\",\"accepted\":false}", "X").Title);
        Assert.Contains("empty or too long", ActivationOutcome.From(false, "{\"result\":\"bad-token\",\"accepted\":false}", "X").Title);
        Assert.False(ActivationOutcome.From(false, "FAIL: no JSON here", "X").Accepted);
    }

    // ── The key never reaches a command line ────────────────────────────────

    private const string Secret = "S3CR3T-KEY-7f3a-do-not-leak";

    [Fact]
    public async Task APastedKey_TravelsInAnOwnerOnlyFile_NeverInTheArguments_AndTheFileIsDestroyed()
    {
        string dir = Path.Combine(Path.GetTempPath(), "dxr-dash-test-" + Guid.NewGuid().ToString("N"));
        var cli = new RecordingCli();
        string? seenPath = null, seenContent = null;
        UnixFileMode? mode = null;
        cli.OnRun = args =>
        {
            seenPath = args.Args.Single(a => a.StartsWith('@'))[1..];
            seenContent = File.ReadAllText(seenPath);
            if (!OperatingSystem.IsWindows()) mode = File.GetUnixFileMode(seenPath);
            return new CliResult(0, "{\"result\":\"ok\",\"accepted\":true,\"error\":\"\"}", "", false, null);
        };
        try
        {
            var o = await new CatalogService(new FakeEngine(), cli).ActivateAsync(ComponentClass.Conversion, "acme-depth", "Acme Depth Studio", "  " + Secret + " ", dir);
            Assert.True(o.Accepted);

            var args = Assert.Single(cli.Argv);
            Assert.Equal(new[] { "conv", "activate", "acme-depth" }, args.Args.Take(3));
            Assert.Equal("--json", args.Args[^1]);
            Assert.All(args.Args, a => Assert.DoesNotContain(Secret, a));
            Assert.DoesNotContain(Secret, args.ToString()); // the readable form the log would carry
            Assert.DoesNotContain(Secret, o.Title + o.Text);

            // What the real process source would hand to the OS for that vector.
            var psi = new CliProcessSource("displayxr-cli").Info(args, stdin: true);
            Assert.All(psi.ArgumentList, a => Assert.DoesNotContain(Secret, a));
            Assert.Equal("", psi.Arguments);
            Assert.False(psi.UseShellExecute);
            Assert.DoesNotContain(psi.Environment, kv => (kv.Value ?? "").Contains(Secret));

            Assert.Equal(Secret + "\n", seenContent); // the CLI reads the first line
            Assert.StartsWith(dir, seenPath);
            if (mode is { } m) Assert.Equal(UnixFileMode.UserRead | UnixFileMode.UserWrite, m);
            Assert.False(File.Exists(seenPath));      // gone when the call returns
            Assert.Empty(Directory.GetFiles(dir));
        }
        finally
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
        }
    }

    [Fact]
    public async Task TheKeyFile_IsDestroyed_WhenTheCliFailsOrThrows()
    {
        string dir = Path.Combine(Path.GetTempPath(), "dxr-dash-test-" + Guid.NewGuid().ToString("N"));
        try
        {
            var cli = new RecordingCli { OnRun = _ => new CliResult(1, "{\"result\":\"refused\",\"accepted\":false,\"error\":\"no\"}", "", false, null) };
            Assert.False((await new CatalogService(new FakeEngine(), cli).ActivateAsync(ComponentClass.Input, "x", "X", Secret, dir)).Accepted);
            Assert.Empty(Directory.GetFiles(dir));

            cli.OnRun = _ => throw new IOException("pipe broke");
            var o = await new CatalogService(new FakeEngine(), cli).ActivateAsync(ComponentClass.Input, "x", "X", Secret, dir);
            Assert.False(o.Accepted);
            Assert.DoesNotContain(Secret, o.Title + o.Text);
            Assert.Empty(Directory.GetFiles(dir));
        }
        finally
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
        }
    }

    [Theory]
    [InlineData("")]
    [InlineData("   ")]
    [InlineData("line1\nline2")]
    public async Task AnEmptyOrMultiLineKey_NeverReachesTheCli(string key)
    {
        var cli = new RecordingCli();
        Assert.False((await new CatalogService(new FakeEngine(), cli).ActivateAsync(ComponentClass.Conversion, "x", "X", key, Path.GetTempPath())).Accepted);
        Assert.Empty(cli.Argv);
    }

    [Fact]
    public void KeyFile_OnWindows_IsReadableByItsOwnerOnly()
    {
        string dir = Path.Combine(Path.GetTempPath(), "dxr-dash-test-" + Guid.NewGuid().ToString("N"));
        try
        {
            string path = KeyFile.Write(Secret, dir);
            Assert.Equal(Secret + "\n", File.ReadAllText(path));
            if (OperatingSystem.IsWindows())
            {
                var acl = new FileInfo(path).GetAccessControl();
                Assert.True(acl.AreAccessRulesProtected); // nothing inherited from the folder
                var rules = acl.GetAccessRules(true, true, typeof(System.Security.Principal.SecurityIdentifier))
                    .Cast<System.Security.AccessControl.FileSystemAccessRule>().ToArray();
                using var me = System.Security.Principal.WindowsIdentity.GetCurrent();
                var rule = Assert.Single(rules);
                Assert.Equal(me.User, rule.IdentityReference);
            }
            else Assert.Equal(UnixFileMode.UserRead | UnixFileMode.UserWrite, File.GetUnixFileMode(path));
            KeyFile.Destroy(path);
            Assert.False(File.Exists(path));
            KeyFile.Destroy(path); // already gone: fine
        }
        finally
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
        }
    }

    [Fact]
    public async Task TheFixtureCli_AcceptsOnlyTheTestKey_AndTheLicenceThenReadsLicensed()
    {
        var src = new FixtureProcessSource(TestData.Path("components.ndjson"), new RecordingCli());
        var svc = new CatalogService(new FakeEngine(), src);
        string dir = Path.Combine(Path.GetTempPath(), "dxr-dash-test-" + Guid.NewGuid().ToString("N"));
        try
        {
            var no = await svc.ActivateAsync(ComponentClass.Conversion, "northlight-3d", "Northlight Live 3D", "wrong", dir);
            Assert.Equal("That key wasn't accepted", no.Title);
            var yes = await svc.ActivateAsync(ComponentClass.Conversion, "northlight-3d", "Northlight Live 3D", FixtureProcessSource.TestKey, dir);
            Assert.True(yes.Accepted);
            var r = await src.RunAsync(new CliArgs("conv", "list", "--json"), TimeSpan.FromSeconds(5));
            Assert.True(PolicyList.TryParse(ComponentClass.Conversion, r.Json, out var list));
            var v = LicenceView.For(list!.Row("northlight-3d")!, "conversion_provider")!;
            Assert.Equal("Licensed", v.Headline);
            Assert.False(v.OfferKey);
        }
        finally
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
        }
    }

    // ── URLs ────────────────────────────────────────────────────────────────

    [Theory]
    [InlineData("https://store.example.com/depth-studio", true)]
    [InlineData("https://example.com/buy?item=a&ref=dxr", true)]
    [InlineData("HTTPS://EXAMPLE.COM/x", true)]
    [InlineData("http://example.com/buy", false)]
    [InlineData("file:///C:/Windows/System32/calc.exe", false)]
    [InlineData("displayxr://activate?item=x&token=y", false)]
    [InlineData("javascript:alert(1)", false)]
    [InlineData("ms-settings:privacy", false)]
    [InlineData("https://user:pw@example.com/", false)]
    [InlineData("https://", false)]
    [InlineData("https:///nohost", false)]
    [InlineData("https://example.com/a b", false)]
    [InlineData("https://example.com/\" & calc.exe", false)]
    [InlineData("https://example.com/\r\nx", false)]
    [InlineData("C:\\Windows\\System32\\calc.exe", false)]
    [InlineData("\\\\server\\share\\x.exe", false)]
    [InlineData("", false)]
    [InlineData(null, false)]
    public void OnlyAPlainHttpsUrl_IsEverOpened(string? text, bool ok)
    {
        Assert.Equal(ok, SafeUrl.TryHttps(text, out var url));
        Assert.Equal(ok, url is not null);
        Assert.False(UrlLauncher.Open(ok ? null : text)); // a refused URL starts nothing
    }

    [Fact]
    public void AnOverlongUrl_IsRefused()
    {
        Assert.False(SafeUrl.TryHttps("https://example.com/" + new string('a', 2100), out _));
    }

    [Fact]
    public void OpeningAUrl_NeverGoesThroughACommandInterpreter()
    {
        var url = new Uri("https://store.example.com/buy?item=a&x=1");
        var win = UrlLauncher.StartInfo(url, windows: true, macos: false);
        Assert.True(win.UseShellExecute);                 // ShellExecute "open" on the URL: the default browser
        Assert.Equal(url.AbsoluteUri, win.FileName);
        Assert.Empty(win.ArgumentList);
        Assert.Equal("", win.Arguments);

        var mac = UrlLauncher.StartInfo(url, windows: false, macos: true);
        Assert.Equal("open", mac.FileName);
        Assert.False(mac.UseShellExecute);
        Assert.Equal(new[] { url.AbsoluteUri }, mac.ArgumentList); // one argument, however many '&' it has

        var linux = UrlLauncher.StartInfo(url, windows: false, macos: false);
        Assert.Equal("xdg-open", linux.FileName);
        Assert.Equal(new[] { url.AbsoluteUri }, linux.ArgumentList);

        Assert.Throws<ArgumentException>(() => UrlLauncher.StartInfo(new Uri("http://example.com/"), true, false));
        Assert.Throws<ArgumentException>(() => UrlLauncher.StartInfo(new Uri("file:///C:/x.exe"), true, false));
    }

    // ── Buy: activate-begin when the CLI has it, the purchase URL as it is otherwise ──

    [Fact]
    public async Task Buy_FallsBackToThePurchaseUrl_WhenTheCliHasNoActivateBegin()
    {
        var purchase = new Uri("https://store.example.com/depth-studio");
        var cli = new RecordingCli { OnRun = _ => new CliResult(2, "", "error: unknown command 'activate-begin'", false, null) };
        var svc = new CatalogService(new FakeEngine(), cli);
        Assert.Equal(purchase, await svc.PurchaseUrlAsync(ComponentClass.Conversion, "acme-depth", purchase, deepLink: true));
        Assert.Equal(new[] { "conv", "activate-begin", "acme-depth", "--json" }, Assert.Single(cli.Argv).Args);

        // A component that takes no deep-link token: the verb is not even asked.
        cli.Argv.Clear();
        Assert.Equal(purchase, await svc.PurchaseUrlAsync(ComponentClass.Conversion, "acme-depth", purchase, deepLink: false));
        Assert.Empty(cli.Argv);
    }

    [Fact]
    public async Task Buy_CarriesTheNonce_WhenActivateBeginAnswers()
    {
        var purchase = new Uri("https://store.example.com/depth-studio?ref=dxr");
        var cli = new RecordingCli
        {
            OnRun = _ => new CliResult(0, "{\"nonce\":\"n0nce-123_abc\",\"return_url\":\"displayxr://activate\"}", "", false, null),
        };
        var url = await new CatalogService(new FakeEngine(), cli).PurchaseUrlAsync(ComponentClass.Conversion, "acme-depth", purchase, true);
        Assert.Equal("https://store.example.com/depth-studio?ref=dxr&item=acme-depth&nonce=n0nce-123_abc&return=displayxr%3A%2F%2Factivate", url.AbsoluteUri);
        Assert.True(SafeUrl.TryHttps(url.AbsoluteUri, out _));
    }

    [Theory]
    [InlineData("{\"url\":\"https://store.example.com/checkout?n=1\"}", "https://store.example.com/checkout?n=1")]
    [InlineData("{\"url\":\"https://evil.example.net/checkout\"}", null)]       // not the vendor's host
    [InlineData("{\"url\":\"http://store.example.com/checkout\"}", null)]       // not https
    [InlineData("{\"nonce\":\"bad nonce&x=1\"}", null)]
    [InlineData("{\"nonce\":\"abc\",\"return_url\":\"https://evil.example.net/\"}", "https://store.example.com/p?item=x&nonce=abc")]
    [InlineData("{}", null)]
    [InlineData("not json", null)]
    public void ActivateBegin_IsUsedOnlyWhenItIsWellFormed(string json, string? expected)
    {
        var url = PurchaseLink.FromActivateBegin(new Uri("https://store.example.com/p"), "x", json);
        Assert.Equal(expected, url?.AbsoluteUri);
    }

    // ── Fakes ───────────────────────────────────────────────────────────────

    private sealed class RecordingCli : IProcessSource
    {
        public readonly List<CliArgs> Argv = new();
        public Func<CliArgs, CliResult> OnRun = _ => new CliResult(0, "", "", false, null);

        public IWatchProcess StartWatch(Action<string> onLine, Action<int?> onExit, Action<string> onNote) => throw new NotSupportedException();

        public Task<CliResult> RunAsync(CliArgs args, TimeSpan timeout, CancellationToken cancel = default)
        {
            Argv.Add(args);
            return Task.FromResult(OnRun(args));
        }
    }

    private sealed class FakeEngine : ICatalogEngine
    {
        public int First = 0;
        public string? StartError;
        public bool TimesOut;
        public string InstalledJson = "{\"installed\":[]}";
        public ElevatedResult Elevated = new(ElevatedEnd.Exited, 0);
        public int Runs, ElevatedRuns;
        public CliArgs? LastArgs, LastElevatedArgs;

        public EngineAvailability Availability => EngineAvailability.Ready;
        public string Path => "dxr-get.exe";
        public bool AlreadyElevated => false;

        public Task<CliResult> RunAsync(CliArgs args, TimeSpan timeout, CancellationToken cancel)
        {
            if (args.Args.Contains("--installed")) return Task.FromResult(new CliResult(0, InstalledJson, "", false, null));
            Runs++;
            LastArgs = args;
            return Task.FromResult(new CliResult(StartError is null ? First : -1, "", "", TimesOut, StartError));
        }

        public Task<ElevatedResult> RunElevatedAsync(CliArgs args, TimeSpan timeout, CancellationToken stopWaiting)
        {
            ElevatedRuns++;
            LastElevatedArgs = args;
            return Task.FromResult(Elevated);
        }
    }
}

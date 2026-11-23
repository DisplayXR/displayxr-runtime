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
/// ADR-064 step 8: the catalog (on the Components page). The engine's <c>list --json</c> in its schema-2 and
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
        Assert.All(list.Items, i => { Assert.True(i.IsInstalled); Assert.True(i.Receipt); Assert.Equal(CatalogAction.None, i.Action); });
        // A receipt is not enough: only the one that recorded an uninstall command can be removed from here.
        Assert.Equal(new[] { true, false }, list.Items.Select(i => i.CanUninstall));
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
        // F4: "installing" is said only once the process is known to be running (the prompt was answered).
        Assert.Equal(new[] { "Checking…", "Waiting for your permission…", "Downloading, verifying and installing…" }, phases);
    }

    [Fact]
    public async Task WhileThePermissionPromptIsUnanswered_ThePhaseNeverClaimsToBeInstalling()
    {
        var engine = new FakeEngine { First = 50, PromptAnswered = false };
        var phases = new List<string>();
        var o = await new CatalogService(engine, new RecordingCli()).RunAsync(CatalogVerb.Install, Item("open-conversion"), phases.Add, default);
        Assert.Equal("Nothing was changed", o.Title);
        Assert.Equal(new[] { "Checking…", "Waiting for your permission…" }, phases);
    }

    // F1: an already elevated Dashboard. The run that changes the machine must never be the killable
    // job child, and "Stop waiting" must leave it running.

    [Fact]
    public async Task AlreadyElevated_TheInstallNeverGoesThroughTheKillableRun()
    {
        var engine = new FakeEngine { AlreadyElevated = true, Elevated = new ElevatedResult(ElevatedEnd.Exited, 0) };
        using var stop = new CancellationTokenSource();
        var phases = new List<string>();
        var o = await new CatalogService(engine, new RecordingCli()).RunAsync(CatalogVerb.Install, Item("open-conversion"), phases.Add, stop.Token);
        Assert.True(o.Ok);
        Assert.Equal(0, engine.Runs);                    // no captured, job-child, kill-on-cancel run at all
        Assert.False(engine.KillableRunGotAStopToken);
        Assert.Equal(1, engine.ElevatedRuns);            // the detached work run, once
        Assert.Equal(new[] { "Starting…", "Downloading, verifying and installing…" }, phases); // no prompt is claimed
        // The work run still answers what the unelevated ask would have: nothing to do, offline, not compatible.
        foreach (int code in new[] { 10, 41, 42, 43 })
        {
            engine.Elevated = new ElevatedResult(ElevatedEnd.Exited, code);
            Assert.Equal(code, (await new CatalogService(engine, new RecordingCli()).RunAsync(CatalogVerb.Uninstall, Item(), _ => { }, default)).ExitCode);
        }
        Assert.Equal(0, engine.Runs);
    }

    [Theory]
    [InlineData(true)]
    [InlineData(false)]
    public async Task StopWaiting_LeavesTheEngineRunning_AndSaysSo(bool alreadyElevated)
    {
        var engine = new FakeEngine { AlreadyElevated = alreadyElevated, First = 50, Work = new TaskCompletionSource<int>() };
        using var stop = new CancellationTokenSource();
        var run = new CatalogService(engine, new RecordingCli()).RunAsync(CatalogVerb.Install, Item("open-conversion"), _ => { }, stop.Token);
        Assert.False(run.IsCompleted);
        stop.Cancel();
        var o = await run;
        Assert.Equal("Still working in the background", o.Title);
        Assert.Contains("keeps going", o.Text);
        Assert.NotNull(o.StillRunning);
        Assert.False(o.StillRunning!.IsCompleted);       // the work was not ended by the stop
        engine.Work.SetResult(0);
        Assert.Equal(0, await o.StillRunning);
    }

    [Fact]
    public void TheWorkRun_IsNeverAJobChild_AndFromAnElevatedDashboardRaisesNoPrompt()
    {
        var psi = DxrGetEngine.ElevatedStartInfo(@"C:\Program Files\DisplayXR\Runtime\dxr-get.exe", CatalogCommands.Install("leia-sr"), alreadyElevated: true);
        Assert.True(psi.UseShellExecute);                // started by the shell: no pipes, not ours to kill
        Assert.Equal("", psi.Verb);                      // inherits the elevation; "runas" is for the non-elevated case
        Assert.False(psi.RedirectStandardOutput);
        Assert.False(psi.RedirectStandardInput);
        Assert.Equal(new[] { "install", "leia-sr", "--yes", "--json", "--no-runtime" }, psi.ArgumentList);
    }

    // F2: while the engine may still be running, nothing else starts.

    [Fact]
    public async Task AfterStopWaiting_ActionsStayBusy_UntilTheEngineExits_OrRefresh()
    {
        var engine = new FakeEngine { First = 50, Work = new TaskCompletionSource<int>() };
        DashboardLog.Disabled = true;
        var ctx = new DashboardContext(new StatusFeed(new FakeSource(), new ManualScheduler()), new CliProcessSource("no-such-cli"), new RecordingCli(), engine);
        var item = Item("open-conversion");
        var run = ctx.RunCatalogAsync(CatalogVerb.Install, item);
        for (int i = 0; i < 200 && engine.ElevatedRuns == 0; i++) await Task.Delay(10);
        Assert.NotNull(ctx.CatalogOp);
        ctx.StopWaitingForCatalog();
        await run;
        Assert.Null(ctx.CatalogOp);
        Assert.NotNull(ctx.CatalogBackground);
        Assert.True(ctx.CatalogBusy);
        Assert.Contains("still being installed", ctx.CatalogBackground!.Line);

        // A second verb is refused while the first engine may still be at work.
        await ctx.RunCatalogAsync(CatalogVerb.Uninstall, Item("acme-depth"));
        Assert.Equal(1, engine.ElevatedRuns);

        engine.Work.SetResult(0); // the engine exits
        for (int i = 0; i < 200 && ctx.CatalogBusy; i++) await Task.Delay(10);
        Assert.False(ctx.CatalogBusy);
        Assert.True(ctx.CatalogResult!.Value.Outcome.Ok);
        Assert.Equal("Open conversion provider is installed", ctx.CatalogResult.Value.Outcome.Title);

        // Refresh also clears it (the list the user then sees is the answer).
        engine.Work = new TaskCompletionSource<int>();
        run = ctx.RunCatalogAsync(CatalogVerb.Install, item);
        for (int i = 0; i < 200 && engine.ElevatedRuns < 2; i++) await Task.Delay(10);
        ctx.StopWaitingForCatalog();
        await run;
        Assert.True(ctx.CatalogBusy);
        ctx.ForgetCatalogBackground();
        Assert.False(ctx.CatalogBusy);
    }

    // F7: the registered id is an argument of the CLI too.

    [Theory]
    [InlineData("--machine")]
    [InlineData("-x")]
    [InlineData("a b")]
    [InlineData("@C:\\secrets.txt")]
    [InlineData("a\"b")]
    public async Task ARegisteredIdThatIsNotOneSafeArgument_NeverReachesTheCli(string registered)
    {
        string json = "{\"schema\":2,\"items\":[{\"id\":\"ok-item\",\"name\":\"X\",\"class\":\"conversion_provider\",\"registered_id\":"
                      + System.Text.Json.JsonSerializer.Serialize(registered) + ",\"state\":\"current\"}]}";
        Assert.True(CatalogList.TryParse(json, out var list));
        var item = Assert.Single(list!.Items);
        Assert.Null(item.RegisteredId);                  // dropped when the list is read
        Assert.Equal("ok-item", item.ComponentId);
        Assert.True(CatalogList.IdIsSafe(item.ComponentId));

        Assert.Throws<ArgumentException>(() => CatalogService.ActivateCommand(ComponentClass.Conversion, registered, "f"));
        Assert.Throws<ArgumentException>(() => CatalogService.ActivateBeginCommand(ComponentClass.Conversion, registered));

        var cli = new RecordingCli();
        var svc = new CatalogService(new FakeEngine(), cli);
        string dir = Path.Combine(Path.GetTempPath(), "dxr-dash-test-" + Guid.NewGuid().ToString("N"));
        try
        {
            Assert.False((await svc.ActivateAsync(ComponentClass.Conversion, registered, "X", Secret, dir)).Accepted);
            Assert.False(Directory.Exists(dir) && Directory.GetFiles(dir).Length > 0); // no key file was even written
            var plan = await svc.BeginBuyAsync(ComponentClass.Conversion, registered, "X", Purchase, true);
            Assert.Equal(BuyKind.OpenPurchasePage, plan.Kind);
            Assert.Empty(cli.Argv);
        }
        finally
        {
            if (Directory.Exists(dir)) Directory.Delete(dir, true);
        }
    }

    [Fact]
    public void ASafeRegisteredId_IsKept()
    {
        Assert.Equal("leia-sr", Parse("catalog-list-v2.json").Find("leia-sr")!.RegisteredId);
        Assert.Equal(new[] { "conv", "activate-begin", "acme.depth_2", "--json" }, CatalogService.ActivateBeginCommand(ComponentClass.Conversion, "acme.depth_2").Args);
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
        // The unelevated ask timing out changed nothing, and says exactly that.
        svc = new CatalogService(new FakeEngine { TimesOut = true }, new RecordingCli());
        var slow = await svc.RunAsync(CatalogVerb.Install, Item(), _ => { }, default);
        Assert.Contains("taking too long", slow.Title);
        Assert.Contains("Nothing was changed", slow.Text);
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

    // ── Buy: activate-begin (activation-deep-link.md §3) ─────────────────────

    private static readonly Uri Purchase = new("https://store.example.com/depth-studio");
    private static readonly DateTimeOffset Now = DateTimeOffset.FromUnixTimeSeconds(1795262606);

    private static BuyPlan Begin(string file, bool exitOk, Uri? purchase = null) =>
        BuyPlan.FromActivateBegin(exitOk, TestData.Text(file), "Acme Depth Studio", purchase ?? Purchase, Now);

    private static string Refusal(string result) =>
        "{\"class\":\"conv\",\"id\":\"acme-depth\",\"result\":\"" + result + "\",\"url\":\"\",\"return_url\":\"displayxr://activate\"," +
        "\"nonce_id\":\"\",\"ttl_s\":900,\"expires_unix\":null,\"expires\":null,\"licence\":{\"state\":\"unlicensed\"}}";

    [Fact]
    public void ActivateBegin_Ok_OpensItsUrlExactlyAsGiven_AndIsPendingUntilItExpires()
    {
        var plan = Begin("activate-begin-ok.json", exitOk: true);
        Assert.Equal(BuyKind.OpenWithReturn, plan.Kind);
        Assert.True(plan.Opens);
        // The CLI's url is complete: byte for byte what is opened, nothing appended, nothing rebuilt.
        using var doc = System.Text.Json.JsonDocument.Parse(TestData.Text("activate-begin-ok.json"));
        string url = doc.RootElement.GetProperty("url").GetString()!;
        Assert.Equal(url, plan.Url!.OriginalString);
        Assert.Contains("displayxr_nonce=", url);
        Assert.Contains("displayxr_item=conv%2Facme-depth", url);
        Assert.Contains("displayxr_return=displayxr%3A%2F%2Factivate", url);
        Assert.Equal(DateTimeOffset.FromUnixTimeSeconds(1795263506), plan.Expires);
        Assert.Equal("790fa3af", plan.NonceId);                 // the loggable id, not the nonce
        Assert.DoesNotContain(plan.NonceId, url);
        Assert.Contains("one-time code", plan.Text);
        // What is handed to the OS for it: the URL alone, no shell.
        var psi = UrlLauncher.StartInfo(plan.Url, windows: true, macos: false);
        Assert.Equal(url, psi.FileName);
        Assert.Empty(psi.ArgumentList);
    }

    [Fact]
    public void ActivateBegin_NoDeepLink_FallsBackToThePurchasePage_HttpsOnly()
    {
        var plan = BuyPlan.FromActivateBegin(false, Refusal("no-deep-link"), "X", Purchase, Now);
        Assert.Equal(BuyKind.OpenPurchasePage, plan.Kind);
        Assert.Equal(Purchase.AbsoluteUri, plan.Url!.AbsoluteUri);
        Assert.Null(plan.Expires);
        Assert.Contains("Paste a key", plan.Text);

        Assert.Equal(BuyKind.NoPurchasePage, BuyPlan.FromActivateBegin(false, Refusal("no-deep-link"), "X", new Uri("http://store.example.com/x"), Now).Kind);
        Assert.Equal(BuyKind.NoPurchasePage, BuyPlan.FromActivateBegin(false, Refusal("no-deep-link"), "X", null, Now).Kind);
    }

    [Fact]
    public void ActivateBegin_NoPurchaseUrl_MeansNoBuy()
    {
        var plan = BuyPlan.FromActivateBegin(false, Refusal("no-purchase-url"), "X", Purchase, Now);
        Assert.Equal(BuyKind.NoPurchasePage, plan.Kind);
        Assert.Null(plan.Url);            // even though the list's purchase_url looked fine: the CLI's check decides
        Assert.False(plan.Opens);
    }

    [Theory]
    [InlineData("too-many", "still waiting")]
    [InlineData("store-error", "couldn't be started")]
    [InlineData("not-registered", "isn't loaded")]
    [InlineData("a-result-from-the-future", "couldn't be started")]
    public void ActivateBegin_NotNow_IsACalmSentence_AndTheKeyBoxStays(string result, string title)
    {
        var plan = BuyPlan.FromActivateBegin(false, Refusal(result), "X", Purchase, Now);
        Assert.Equal(BuyKind.NotNow, plan.Kind);
        Assert.Null(plan.Url);
        Assert.Contains(title, plan.Title);
        Assert.Contains("paste a key", plan.Text, StringComparison.OrdinalIgnoreCase);
        Assert.DoesNotContain("error", plan.Title, StringComparison.OrdinalIgnoreCase);
    }

    [Theory]
    [InlineData("")]                                                    // an unknown verb prints no JSON
    [InlineData("error: unknown command 'activate-begin'")]
    [InlineData("Usage: displayxr-cli conv <list|use|reset|enable|disable|options|set|activate>")]
    [InlineData("{\"providers\":[]}")]                                  // JSON, but not this verb's
    public void ARuntimeWithoutTheVerb_IsNotAvailable_AndBuyOpensThePurchasePage(string stdout)
    {
        var plan = BuyPlan.FromActivateBegin(false, stdout, "X", Purchase, Now);
        Assert.Equal(BuyKind.OpenPurchasePage, plan.Kind);
        Assert.Equal(Purchase.AbsoluteUri, plan.Url!.AbsoluteUri);
    }

    [Theory]
    [InlineData("http://store.example.com/buy?displayxr_nonce=x")]
    [InlineData("displayxr://activate?nonce=x&token=y")]
    [InlineData("file:///C:/x.exe")]
    [InlineData("")]
    public void AnOk_WithAUrlThatIsNotHttps_OpensNothing(string url)
    {
        string json = "{\"result\":\"ok\",\"url\":" + System.Text.Json.JsonSerializer.Serialize(url) + ",\"nonce_id\":\"aa\",\"expires_unix\":1795263506}";
        var plan = BuyPlan.FromActivateBegin(true, json, "X", Purchase, Now);
        Assert.Equal(BuyKind.NotNow, plan.Kind);
        Assert.False(plan.Opens);
    }

    [Fact]
    public void APendingActivation_NeverOutlivesItsFifteenMinutesByMuch()
    {
        string json = "{\"result\":\"ok\",\"url\":\"https://store.example.com/buy?displayxr_nonce=x\",\"nonce_id\":\"aa\",\"expires_unix\":4102444800}";
        Assert.Equal(Now.AddMinutes(20), BuyPlan.FromActivateBegin(true, json, "X", Purchase, Now).Expires);
        json = "{\"result\":\"ok\",\"url\":\"https://store.example.com/buy?displayxr_nonce=x\",\"nonce_id\":\"aa\"}";
        Assert.Equal(Now.AddMinutes(15), BuyPlan.FromActivateBegin(true, json, "X", Purchase, Now).Expires);
    }

    [Fact]
    public async Task Buy_AsksActivateBegin_UnlessTheComponentTakesNoDeepLink()
    {
        var cli = new RecordingCli { OnRun = _ => new CliResult(0, TestData.Text("activate-begin-ok.json"), "", false, null) };
        var svc = new CatalogService(new FakeEngine(), cli);
        var plan = await svc.BeginBuyAsync(ComponentClass.Conversion, "acme-depth", "Acme Depth Studio", Purchase, deepLink: true);
        Assert.Equal(BuyKind.OpenWithReturn, plan.Kind);
        Assert.Equal(new[] { "conv", "activate-begin", "acme-depth", "--json" }, Assert.Single(cli.Argv).Args);

        // activation.deep_link = false: the verb is not even run; the purchase page as it is.
        cli.Argv.Clear();
        plan = await svc.BeginBuyAsync(ComponentClass.Conversion, "acme-depth", "Acme Depth Studio", Purchase, deepLink: false);
        Assert.Equal(BuyKind.OpenPurchasePage, plan.Kind);
        Assert.Empty(cli.Argv);

        // A CLI that predates the activation object (null): ask; an old runtime answers with a usage error.
        cli.OnRun = _ => new CliResult(2, "", "error: unknown command 'activate-begin'", false, null);
        plan = await svc.BeginBuyAsync(ComponentClass.Conversion, "acme-depth", "Acme Depth Studio", Purchase, deepLink: null);
        Assert.Equal(BuyKind.OpenPurchasePage, plan.Kind);
        Assert.Single(cli.Argv);

        // The CLI missing or hanging is the same fallback, never an exception.
        cli.OnRun = _ => new CliResult(-1, "", "", true, null);
        Assert.Equal(BuyKind.OpenPurchasePage, (await svc.BeginBuyAsync(ComponentClass.Conversion, "x", "X", Purchase, true)).Kind);
        cli.OnRun = _ => throw new InvalidOperationException("boom");
        Assert.Equal(BuyKind.OpenPurchasePage, (await svc.BeginBuyAsync(ComponentClass.Conversion, "x", "X", Purchase, true)).Kind);
    }

    [Fact]
    public async Task TheFixtureCli_AnswersActivateBegin_InTheRealShape()
    {
        var src = new FixtureProcessSource(TestData.Path("components.ndjson"), new RecordingCli());
        var svc = new CatalogService(new FakeEngine(), src);
        var ok = await svc.BeginBuyAsync(ComponentClass.Conversion, "acme-depth", "Acme Depth Studio", Purchase, true);
        Assert.Equal(BuyKind.OpenWithReturn, ok.Kind);
        Assert.StartsWith("https://store.example.com/depth-studio?displayxr_nonce=", ok.Url!.OriginalString);
        Assert.EndsWith("&displayxr_item=conv%2Facme-depth&displayxr_return=displayxr%3A%2F%2Factivate", ok.Url.OriginalString);

        var noLink = await svc.BeginBuyAsync(ComponentClass.Conversion, "northlight-3d", "Northlight Live 3D", new Uri("https://northlight.example.com/buy"), null);
        Assert.Equal(BuyKind.OpenPurchasePage, noLink.Kind);
        Assert.Equal("no-deep-link", noLink.Result);
        Assert.Equal(BuyKind.NotNow, (await svc.BeginBuyAsync(ComponentClass.Conversion, "nobody", "Nobody", Purchase, true)).Kind);
    }

    // ── After Buy: the licence changes underneath the Dashboard ─────────────

    private static (DashboardContext Ctx, RecordingCli Cli, List<string> Opened) Context(Func<CliArgs, CliResult> run)
    {
        DashboardLog.Disabled = true;
        var cli = new RecordingCli { OnRun = run };
        var ctx = new DashboardContext(new StatusFeed(new FakeSource(), new ManualScheduler()), new CliProcessSource("no-such-cli"), cli, new FakeEngine())
        {
            BuyPollInterval = TimeSpan.FromMilliseconds(20),
        };
        var opened = new List<string>();
        ctx.OpenUrl = u => { opened.Add(u); return true; };
        return (ctx, cli, opened);
    }

    [Fact]
    public async Task AfterBuy_TheLicenceIsPolled_UntilItChanges_ThenThePollStops()
    {
        string trial = TestData.Text("conv-list.json");
        string licensed = trial.Replace("\"state\": \"trial\"", "\"state\": \"licensed\"").Replace("\"source\": \"trial\"", "\"source\": \"key\"");
        Assert.NotEqual(trial, licensed);
        bool bought = false;
        var (ctx, cli, opened) = Context(a => a.Args[1] switch
        {
            // The fixture's expiry is a fixed moment; a pending activation is one that has not ended yet.
            "activate-begin" => new CliResult(0, TestData.Text("activate-begin-ok.json")
                .Replace("1795263506", DateTimeOffset.UtcNow.AddMinutes(15).ToUnixTimeSeconds().ToString()), "", false, null),
            _ => new CliResult(0, bought ? licensed : trial, "", false, null),
        });
        await ctx.LoadPolicyAsync(ComponentClass.Conversion);
        var item = Item("acme-depth");
        var row = ctx.Policies[ComponentClass.Conversion].Row("acme-depth")!;

        await ctx.BuyAsync(item, ComponentClass.Conversion, LicenceView.For(row, item.Class)!.PurchaseUrl, row.LicenceDeepLink);
        Assert.StartsWith("https://store.example.com/depth-studio?displayxr_nonce=", Assert.Single(opened));
        Assert.EndsWith("&displayxr_return=displayxr%3A%2F%2Factivate", opened[0]); // as given: nothing after it
        Assert.NotNull(ctx.BuyPending);
        Assert.Equal(BuyKind.OpenWithReturn, ctx.BuyNote!.Value.Plan.Kind);

        bought = true; // the handler delivered the code; the component now reports licensed
        for (int i = 0; i < 200 && ctx.BuyPending is not null; i++) await Task.Delay(20);
        Assert.Null(ctx.BuyPending);
        Assert.True(ctx.ActivationResult!.Value.Outcome.Accepted);
        Assert.Equal("Acme Depth Studio is activated", ctx.ActivationResult.Value.Outcome.Title);

        int reads = cli.Argv.Count;
        await Task.Delay(150);
        Assert.Equal(reads, cli.Argv.Count); // the poll has stopped
    }

    [Fact]
    public async Task APendingBuy_EndsWithItsActivation_EvenIfNothingChanges()
    {
        string ended = TestData.Text("activate-begin-ok.json").Replace("1795263506", DateTimeOffset.UtcNow.AddSeconds(2).ToUnixTimeSeconds().ToString());
        var (ctx, cli, _) = Context(a => a.Args[1] == "activate-begin"
            ? new CliResult(0, ended, "", false, null)
            : new CliResult(0, TestData.Text("conv-list.json"), "", false, null));
        await ctx.LoadPolicyAsync(ComponentClass.Conversion);
        await ctx.BuyAsync(Item("acme-depth"), ComponentClass.Conversion, Purchase, true);
        Assert.NotNull(ctx.BuyPending);
        for (int i = 0; i < 400 && ctx.BuyPending is not null; i++) await Task.Delay(20);
        Assert.Null(ctx.BuyPending);
        Assert.Null(ctx.ActivationResult);
        int reads = cli.Argv.Count;
        await Task.Delay(150);
        Assert.Equal(reads, cli.Argv.Count);
    }

    [Fact]
    public async Task Buy_ThatCannotStart_OpensNothing_AndNoPurchasePageRemovesTheButton()
    {
        string result = "too-many";
        var (ctx, _, opened) = Context(a => a.Args[1] == "activate-begin"
            ? new CliResult(1, Refusal(result), "", false, null)
            : new CliResult(0, TestData.Text("conv-list.json"), "", false, null));
        var item = Item("acme-depth");
        await ctx.BuyAsync(item, ComponentClass.Conversion, Purchase, true);
        Assert.Empty(opened);
        Assert.Null(ctx.BuyPending);
        Assert.Equal(BuyKind.NotNow, ctx.BuyNote!.Value.Plan.Kind);
        Assert.False(ctx.NoBuy(item.Id));

        result = "no-purchase-url";
        await ctx.BuyAsync(item, ComponentClass.Conversion, Purchase, true);
        Assert.Empty(opened);
        Assert.True(ctx.NoBuy(item.Id));

        // no-deep-link: the purchase page, pending for the focus refresh only (no poll, no nonce).
        result = "no-deep-link";
        await ctx.BuyAsync(Item("northlight-3d"), ComponentClass.Conversion, Purchase, null);
        Assert.Equal(Purchase.AbsoluteUri, Assert.Single(opened));
        Assert.Equal(BuyKind.OpenPurchasePage, ctx.BuyPending!.Plan.Kind);
    }

    [Fact]
    public void AReadInProgress_IsSaid_AndNothingWhenNoneIs()
    {
        Assert.Null(CatalogText.Checking(false));
        var line = CatalogText.Checking(true);
        Assert.NotNull(line);
        Assert.Equal("Checking the catalog", line!.Value.Title);
        Assert.Contains("20 seconds", line.Value.Text);
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
        public bool AlreadyElevated { get; set; }
        /// <summary>The killable, job-child path received a token that can be cancelled (the Stop waiting token).</summary>
        public bool KillableRunGotAStopToken;
        /// <summary>When set, the work run does not end until this completes (and reports StoppedWaiting if the wait is stopped first).</summary>
        public TaskCompletionSource<int>? Work;
        public bool PromptAnswered = true;

        public Task<CliResult> RunAsync(CliArgs args, TimeSpan timeout, CancellationToken cancel)
        {
            if (args.Args.Contains("--installed")) return Task.FromResult(new CliResult(0, InstalledJson, "", false, null));
            Runs++;
            LastArgs = args;
            KillableRunGotAStopToken |= cancel.CanBeCanceled;
            return Task.FromResult(new CliResult(StartError is null ? First : -1, "", "", TimesOut, StartError));
        }

        public async Task<ElevatedResult> RunElevatedAsync(CliArgs args, TimeSpan timeout, CancellationToken stopWaiting, Action? started = null)
        {
            ElevatedRuns++;
            LastElevatedArgs = args;
            if (!PromptAnswered) return new ElevatedResult(ElevatedEnd.Declined);
            started?.Invoke();
            if (Work is null) return Elevated;
            try { return new ElevatedResult(ElevatedEnd.Exited, await Work.Task.WaitAsync(stopWaiting)); }
            catch (OperationCanceledException) { return new ElevatedResult(ElevatedEnd.StoppedWaiting, StillRunning: Work.Task); }
        }
    }
}

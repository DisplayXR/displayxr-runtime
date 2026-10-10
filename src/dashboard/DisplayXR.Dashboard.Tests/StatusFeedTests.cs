// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using DisplayXR.Dashboard.Feed;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>A clock and a post queue the test drives by hand.</summary>
internal sealed class ManualScheduler : IFeedScheduler
{
    private readonly Queue<Action> _posted = new();
    private readonly List<(DateTime At, Action Action, Handle H)> _timers = new();
    public DateTime Now { get; private set; } = new(2026, 10, 9, 12, 0, 0, DateTimeKind.Utc);

    private sealed class Handle : IDisposable
    {
        public bool Cancelled;
        public void Dispose() => Cancelled = true;
    }

    public void Post(Action action)
    {
        lock (_posted) _posted.Enqueue(action);
    }

    public IDisposable Schedule(TimeSpan delay, Action action)
    {
        var h = new Handle();
        _timers.Add((Now + delay, action, h));
        return h;
    }

    public void Drain()
    {
        while (true)
        {
            Action? a;
            lock (_posted) { if (!_posted.TryDequeue(out a)) return; }
            a();
        }
    }

    public void Advance(TimeSpan by)
    {
        var end = Now + by;
        while (true)
        {
            Drain();
            var due = _timers.Where(t => !t.H.Cancelled && t.At <= end).OrderBy(t => t.At).FirstOrDefault();
            if (due.Action is null) break;
            _timers.Remove(due);
            Now = due.At;
            due.Action();
        }
        Now = end;
        Drain();
    }

    public int PendingTimers => _timers.Count(t => !t.H.Cancelled);
}

internal sealed class FakeSource : IProcessSource
{
    public sealed class Child : IWatchProcess
    {
        public Action<string> OnLine = _ => { };
        public Action<int?> OnExit = _ => { };
        public bool Stopped;
        public void Stop() => Stopped = true;
        public void Line(string s) => OnLine(s);
        public void Exit(int? code = 1) => OnExit(code);
    }

    public readonly List<Child> Children = new();
    public readonly List<string> Runs = new();
    public bool FailStart;
    public Func<string, CliResult> Run = _ => new CliResult(0, "", "", false, null);

    public Child Last => Children[^1];

    public IWatchProcess StartWatch(Action<string> onLine, Action<int?> onExit)
    {
        if (FailStart) throw new System.IO.FileNotFoundException("displayxr-cli.exe not found");
        var c = new Child { OnLine = onLine, OnExit = onExit };
        Children.Add(c);
        return c;
    }

    public Task<CliResult> RunAsync(string arguments, TimeSpan timeout, CancellationToken cancel = default)
    {
        Runs.Add(arguments);
        return Task.FromResult(Run(arguments));
    }
}

public class StatusFeedTests
{
    private static readonly string Service = TestData.Lines("two-panels.ndjson")[0];
    private static readonly string Truncated = TestData.Lines("two-panels.ndjson")[2];
    private static readonly string Headless = TestData.Lines("empty-headless.ndjson")[0];

    private static (StatusFeed Feed, FakeSource Src, ManualScheduler Clock) Make()
    {
        DashboardLog.Disabled = true;
        var src = new FakeSource();
        var clock = new ManualScheduler();
        return (new StatusFeed(src, clock), src, clock);
    }

    [Fact]
    public void Hold_StartsOneChild_ReleaseStopsItAndDropsTheSnapshot()
    {
        var (feed, src, clock) = Make();
        Assert.Equal(FeedMode.Idle, feed.Mode);
        feed.Hold();
        feed.Hold(); // a second status page: still one child
        Assert.Single(src.Children);
        Assert.Equal(FeedMode.Watching, feed.Mode);

        src.Last.Line(Service);
        clock.Drain();
        Assert.NotNull(feed.Snapshot);
        Assert.True(feed.Snapshot!.IsService);

        feed.Release();
        Assert.False(src.Last.Stopped);
        feed.Release();
        Assert.False(src.Last.Stopped); // lingers for a quick hop back
        Assert.True(feed.IsLingering);
        clock.Advance(StatusFeed.ReleaseLinger);
        Assert.True(src.Last.Stopped);
        Assert.Equal(FeedMode.Idle, feed.Mode);
        Assert.Null(feed.Snapshot); // D5.4: no stale badge after release
    }

    [Fact]
    public void QuickHopBack_KeepsTheSameChild()
    {
        var (feed, src, clock) = Make();
        feed.Hold();
        feed.Release();                 // Home -> Performance
        clock.Advance(TimeSpan.FromSeconds(5));
        feed.Hold();                    // -> back to Displays
        clock.Advance(TimeSpan.FromSeconds(30));
        Assert.Single(src.Children);
        Assert.False(src.Last.Stopped);
        Assert.False(feed.IsLingering);
    }

    [Fact]
    public void MinimiseWhileLingering_StopsAtOnce()
    {
        var (feed, src, _) = Make();
        feed.Hold();
        feed.Release();
        feed.SetPaused(true);
        Assert.True(src.Last.Stopped);
        Assert.False(feed.IsLingering);
    }

    [Fact]
    public void Minimise_PausesTheChild()
    {
        var (feed, src, _) = Make();
        feed.Hold();
        feed.SetPaused(true);
        Assert.True(src.Children[0].Stopped);
        Assert.Equal(FeedMode.Idle, feed.Mode);
        feed.SetPaused(false);
        Assert.Equal(2, src.Children.Count);
        Assert.Equal(FeedMode.Watching, feed.Mode);
    }

    [Fact]
    public void TruncatedLine_KeepsTheLastGoodSnapshot()
    {
        var (feed, src, clock) = Make();
        feed.Hold();
        src.Last.Line(Service);
        src.Last.Line(Truncated);
        src.Last.Line("");
        clock.Drain();
        Assert.NotNull(feed.Snapshot);
        Assert.Equal(2, feed.Snapshot!.Screens.Count);
        Assert.True(feed.LastReadFailed);
        src.Last.Line(Service);
        clock.Drain();
        Assert.False(feed.LastReadFailed);
    }

    [Fact]
    public void HealthyChildExit_IsRestartedWithTheSnapshotKept()
    {
        var (feed, src, clock) = Make();
        feed.Hold();
        src.Last.Line(Service);
        clock.Advance(TimeSpan.FromSeconds(60));
        src.Last.Exit(0); // e.g. someone killed it
        clock.Drain();
        Assert.Equal(2, src.Children.Count);
        Assert.Equal(FeedMode.Watching, feed.Mode);
        Assert.NotNull(feed.Snapshot);
        Assert.Empty(src.Runs);
    }

    [Fact]
    public void YoungExits_FallBackToPolling_EveryThirtySecondsNeverFaster()
    {
        var (feed, src, clock) = Make();
        src.Run = _ => new CliResult(0, Headless, "", false, null);
        feed.Hold();
        for (int i = 0; i <= StatusFeed.MaxQuickRestarts; i++)
        {
            clock.Advance(TimeSpan.FromSeconds(1));
            src.Last.Exit(2); // "unknown verb": a CLI without --watch
            clock.Drain();
        }
        Assert.Equal(StatusFeed.MaxQuickRestarts + 1, src.Children.Count);
        Assert.Equal(FeedMode.Polling, feed.Mode);
        Assert.Equal(new[] { "status --json" }, src.Runs); // one poll right away
        Assert.Equal("headless", feed.Snapshot!.Source);
        Assert.Contains("exited", feed.FallbackReason);

        clock.Advance(TimeSpan.FromSeconds(29));
        Assert.Single(src.Runs);
        clock.Advance(TimeSpan.FromSeconds(1));
        Assert.Equal(2, src.Runs.Count);
        clock.Advance(TimeSpan.FromSeconds(30));
        Assert.Equal(3, src.Runs.Count);
    }

    [Fact]
    public void Polling_RecoversToWatchingWhenThePollSeesTheService()
    {
        var (feed, src, clock) = Make();
        string answer = Headless;
        src.Run = _ => new CliResult(0, answer, "", false, null);
        feed.Hold();
        for (int i = 0; i <= StatusFeed.MaxQuickRestarts; i++) { src.Last.Exit(2); clock.Drain(); }
        Assert.Equal(FeedMode.Polling, feed.Mode);
        int children = src.Children.Count;

        answer = Service; // the service came back
        clock.Advance(StatusFeed.PollInterval);
        Assert.Equal(FeedMode.Watching, feed.Mode);
        Assert.Equal(children + 1, src.Children.Count);
        src.Last.Line(Service);
        clock.Drain();
        Assert.True(feed.Snapshot!.IsService);
    }

    [Fact]
    public void FailedRecovery_WaitsForTheNextSlot()
    {
        var (feed, src, clock) = Make();
        src.Run = _ => new CliResult(0, Service, "", false, null); // polls always see the service...
        feed.Hold();
        for (int i = 0; i <= StatusFeed.MaxQuickRestarts; i++) { src.Last.Exit(2); clock.Drain(); }
        // ...but the watch child keeps dying: at most one poll and one watch attempt per 30 s.
        Assert.Equal(FeedMode.Watching, feed.Mode); // the first poll already tried to recover
        for (int i = 0; i < 4; i++)
        {
            src.Last.Exit(2);
            clock.Drain();
            Assert.Equal(FeedMode.Polling, feed.Mode);
            clock.Advance(StatusFeed.PollInterval);
        }
        Assert.InRange(src.Runs.Count, 4, 5);
        Assert.InRange(src.Children.Count, StatusFeed.MaxQuickRestarts + 5, StatusFeed.MaxQuickRestarts + 6);
    }

    [Fact]
    public void Polling_RetriesWatchPeriodicallyEvenWithoutTheService()
    {
        var (feed, src, clock) = Make();
        src.Run = _ => new CliResult(0, Headless, "", false, null);
        feed.Hold();
        for (int i = 0; i <= StatusFeed.MaxQuickRestarts; i++) { src.Last.Exit(2); clock.Drain(); }
        int children = src.Children.Count;
        clock.Advance(TimeSpan.FromSeconds(30 * (StatusFeed.RetryWatchEveryPolls - 1)));
        Assert.Equal(children + 1, src.Children.Count);
        Assert.Equal(FeedMode.Watching, feed.Mode);
    }

    [Fact]
    public void MissingCli_GoesStraightToPolling_AndReportsWhy()
    {
        var (feed, src, clock) = Make();
        src.FailStart = true;
        src.Run = _ => new CliResult(-1, "", "", false, "displayxr-cli.exe not found");
        feed.Hold();
        clock.Drain();
        Assert.Equal(FeedMode.Polling, feed.Mode);
        Assert.Null(feed.Snapshot);
        Assert.True(feed.LastReadFailed);
        Assert.Contains("not found", feed.FallbackReason);
    }

    [Fact]
    public void Refresh_ReplacesTheChild_AndIgnoresTheOldOnesLateLines()
    {
        var (feed, src, clock) = Make();
        feed.Hold();
        var old = src.Last;
        old.Line(Service);
        clock.Drain();
        feed.Refresh();
        Assert.True(old.Stopped);
        Assert.Equal(2, src.Children.Count);
        Assert.NotNull(feed.Snapshot); // kept through the restart

        old.Line(Headless); // a late line from the replaced child
        old.Exit(0);
        clock.Drain();
        Assert.True(feed.Snapshot!.IsService);
        Assert.Equal(2, src.Children.Count);
        Assert.Equal(FeedMode.Watching, feed.Mode);
    }

    [Fact]
    public void Dispose_StopsEverything()
    {
        var (feed, src, clock) = Make();
        feed.Hold();
        feed.Dispose();
        Assert.True(src.Last.Stopped);
        src.Last.Line(Service);
        clock.Drain();
        Assert.Null(feed.Snapshot);
        feed.Hold(); // after dispose: nothing starts
        Assert.Single(src.Children);
    }

    [Fact]
    public void FixtureSource_SimulatesThePerScreenVerbs()
    {
        DashboardLog.Disabled = true;
        var fx = new FixtureProcessSource(TestData.Path("two-panels.ndjson"), new FakeSource());
        string key = "edid:ACR-0001-QI012321D10117";
        var list0 = fx.RunAsync("dp list --json", TimeSpan.FromSeconds(1)).Result;
        Assert.True(Model.DpList.TryParse(list0.Stdout, out var dp0));
        Assert.False(dp0!.Screen(key)!.Forced);
        Assert.True(fx.RunAsync($"dp use sim-display --screen {key}", TimeSpan.FromSeconds(1)).Result.Ok);
        var snap = TestData.Parse(fx.RunAsync("status --json", TimeSpan.FromSeconds(1)).Result.Stdout);
        var ds1 = snap.Screens.Single(s => s.Key == key);
        Assert.True(ds1.Claim.Forced);
        Assert.Equal("sim-display", ds1.Claim.PreferredPlugin);
        Assert.True(fx.RunAsync("dp reset --screen all", TimeSpan.FromSeconds(1)).Result.Ok);
        Assert.True(Model.DpList.TryParse(fx.RunAsync("dp list --json", TimeSpan.FromSeconds(1)).Result.Stdout, out var dp1));
        Assert.Null(dp1!.Screen(key)!.PreferredPlugin);
    }
}

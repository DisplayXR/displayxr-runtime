// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using DisplayXR.Dashboard.Model;

namespace DisplayXR.Dashboard.Feed;

public enum FeedMode
{
    /// <summary>Nobody holds the feed (no status page visible, or the window is minimised): no child runs.</summary>
    Idle,
    /// <summary>One long-lived <c>status --watch --json</c> child, one snapshot per NDJSON line.</summary>
    Watching,
    /// <summary>The watch child would not stay up: <c>status --json</c> once every 30 s.</summary>
    Polling,
}

/// <summary>
/// The status feed (ADR-051 D5, design §5 / §8): page-scoped, one DIAG
/// consumer, the last good snapshot kept through a child restart.
///
/// <para>Holders (Hold / Release, ref-counted) are the pages that show status;
/// <see cref="SetPaused"/> is the window being minimised. While anybody holds
/// it and the window is up, ONE <c>status --watch --json</c> child runs. If
/// that child exits after running a while it is restarted (twice in a row at
/// most); if it exits young — no service build of the CLI, an elevated token,
/// a CLI too old for <c>--watch</c> — the feed falls back to <c>status
/// --json</c> every 30 s, never faster (each headless run creates a vendor
/// instance), and goes back to watching when a poll finds the service.</para>
///
/// <para>Single-threaded: every state change runs on the scheduler's thread
/// (the UI thread in the app). Process callbacks are posted onto it and tagged
/// with the child they came from, so a late line from a child that was
/// already replaced is dropped.</para>
/// </summary>
public sealed class StatusFeed : IDisposable
{
    public static readonly TimeSpan PollInterval = TimeSpan.FromSeconds(30);
    /// <summary>A watch child that ran at least this long counts as healthy and is simply restarted.</summary>
    public static readonly TimeSpan HealthyRun = TimeSpan.FromSeconds(10);
    /// <summary>Consecutive quick restarts allowed before falling back to polling.</summary>
    public const int MaxQuickRestarts = 2;
    /// <summary>While polling, retry the watch child every this many polls even if no poll saw the service.</summary>
    public const int RetryWatchEveryPolls = 4;
    public static readonly TimeSpan PollTimeout = TimeSpan.FromSeconds(60);

    private readonly IProcessSource _source;
    private readonly IFeedScheduler _sched;

    private int _holders;
    private bool _paused;
    private bool _disposed;

    private IWatchProcess? _child;
    private int _childId;
    private DateTime _childStarted;
    private int _quickRestarts;

    private IDisposable? _pollTimer;
    private int _pollGen;
    private bool _pollInFlight;
    private int _pollsSinceWatch;
    private bool _recovering;

    public StatusFeed(IProcessSource source, IFeedScheduler scheduler)
    {
        _source = source;
        _sched = scheduler;
    }

    /// <summary>The last good snapshot; null before the first one and after the feed is released.</summary>
    public StatusSnapshot? Snapshot { get; private set; }
    public FeedMode Mode { get; private set; } = FeedMode.Idle;
    /// <summary>The newest line or poll could not be parsed; the last good snapshot is still shown.</summary>
    public bool LastReadFailed { get; private set; }
    public string? LastReadError { get; private set; }
    /// <summary>Why the feed is polling (or the last child failure), for the banner.</summary>
    public string? FallbackReason { get; private set; }
    public DateTime? LastUpdate { get; private set; }
    public int ChildStarts { get; private set; }
    public int Holders => _holders;
    public bool IsRunning => Mode != FeedMode.Idle;

    /// <summary>Raised on the scheduler's thread after any visible state change.</summary>
    public event Action? Changed;

    public void Hold()
    {
        _holders++;
        Sync();
    }

    public void Release()
    {
        _holders = Math.Max(0, _holders - 1);
        Sync();
    }

    public void SetPaused(bool paused)
    {
        if (_paused == paused) return;
        _paused = paused;
        Sync();
    }

    /// <summary>The manual kick: drop the child (or the polling) and start a fresh watch.</summary>
    public void Refresh()
    {
        if (!Wanted) return;
        StopAll(keepSnapshot: true);
        _quickRestarts = 0;
        _recovering = false;
        StartWatch();
        RaiseChanged();
    }

    private bool Wanted => !_disposed && _holders > 0 && !_paused;

    private void Sync()
    {
        if (Wanted && Mode == FeedMode.Idle)
        {
            _quickRestarts = 0;
            _recovering = false;
            StartWatch();
            RaiseChanged();
        }
        else if (!Wanted && Mode != FeedMode.Idle)
        {
            // Released: nothing runs for nobody, and the snapshot is dropped so a
            // badge never shows stale data (ADR-051 D5.4).
            StopAll(keepSnapshot: false);
            RaiseChanged();
        }
    }

    private void StartWatch()
    {
        CancelPolling();
        int id = ++_childId;
        Mode = FeedMode.Watching;
        _childStarted = _sched.Now;
        ChildStarts++;
        try
        {
            _child = _source.StartWatch(
                line => _sched.Post(() => OnLine(id, line)),
                code => _sched.Post(() => OnExit(id, code)));
        }
        catch (Exception ex)
        {
            _child = null;
            DashboardLog.Warn($"status --watch could not start: {ex.Message}");
            _sched.Post(() => OnExit(id, null));
        }
    }

    private void OnLine(int id, string line)
    {
        if (id != _childId || Mode != FeedMode.Watching || _disposed) return;
        line = line.Trim();
        if (line.Length == 0) return;
        _recovering = false;
        Accept(line);
    }

    private void Accept(string json)
    {
        if (StatusSnapshotParser.TryParse(json, out var snap, out var error))
        {
            Snapshot = snap;
            LastReadFailed = false;
            LastReadError = null;
            LastUpdate = _sched.Now;
        }
        else
        {
            LastReadFailed = true;
            LastReadError = error;
            DashboardLog.Warn($"status read failed, keeping the last snapshot: {error}");
        }
        RaiseChanged();
    }

    private void OnExit(int id, int? code)
    {
        if (id != _childId || _disposed) return;
        _child = null;
        if (!Wanted || Mode != FeedMode.Watching) return;

        var ran = _sched.Now - _childStarted;
        string why = code is null ? "displayxr-cli could not be started" : $"status --watch exited (code {code})";
        DashboardLog.Warn($"{why} after {ran.TotalSeconds:0.0} s");
        if (ran >= HealthyRun)
        {
            _quickRestarts = 0;
            _recovering = false;
            StartWatch();
        }
        else if (_quickRestarts < MaxQuickRestarts && code is not null)
        {
            _quickRestarts++;
            StartWatch();
        }
        else
        {
            FallbackReason = why;
            // A failed recovery attempt waits for the next poll slot: one poll and
            // one watch attempt per 30 s at most, never a tight loop.
            EnterPolling(pollNow: !_recovering);
        }
        RaiseChanged();
    }

    private void EnterPolling(bool pollNow)
    {
        Mode = FeedMode.Polling;
        _pollsSinceWatch = 0;
        if (pollNow) PollNow();
        else SchedulePoll();
    }

    private void SchedulePoll()
    {
        _pollTimer?.Dispose();
        int gen = _pollGen;
        _pollTimer = _sched.Schedule(PollInterval, () =>
        {
            if (gen == _pollGen && Mode == FeedMode.Polling && Wanted) PollNow();
        });
    }

    private async void PollNow()
    {
        if (_pollInFlight) return;
        _pollInFlight = true;
        int gen = _pollGen;
        CliResult result;
        try
        {
            result = await _source.RunAsync("status --json", PollTimeout).ConfigureAwait(false);
        }
        catch (Exception ex)
        {
            result = new CliResult(-1, "", "", false, ex.Message);
        }
        _sched.Post(() => OnPollResult(gen, result));
    }

    private void OnPollResult(int gen, CliResult result)
    {
        _pollInFlight = false;
        if (gen != _pollGen || Mode != FeedMode.Polling || !Wanted) return;
        _pollsSinceWatch++;
        if (result.Started && !result.TimedOut && StatusSnapshotParser.TryParse(result.Stdout, out _, out _))
        {
            Accept(result.Stdout);
        }
        else
        {
            LastReadFailed = true;
            LastReadError = result.Summary;
            FallbackReason = result.Started ? FallbackReason : result.Summary;
            RaiseChanged();
        }

        // Recovery: the service answered a poll, or it is time for another try.
        if ((Snapshot?.IsService ?? false) || _pollsSinceWatch >= RetryWatchEveryPolls)
        {
            _quickRestarts = MaxQuickRestarts; // a recovery attempt that dies young goes straight back to polling
            _recovering = true;
            StartWatch();
            RaiseChanged();
            return;
        }
        SchedulePoll();
    }

    private void CancelPolling()
    {
        _pollGen++;
        _pollTimer?.Dispose();
        _pollTimer = null;
        _pollInFlight = false;
    }

    private void StopAll(bool keepSnapshot)
    {
        CancelPolling();
        _childId++; // anything still in flight from the old child is now stale
        var child = _child;
        _child = null;
        Mode = FeedMode.Idle;
        if (child is not null)
        {
            try { child.Stop(); }
            catch (Exception ex) { DashboardLog.Warn($"stopping status --watch: {ex.Message}"); }
        }
        if (!keepSnapshot)
        {
            Snapshot = null;
            LastReadFailed = false;
            LastReadError = null;
            FallbackReason = null;
            LastUpdate = null;
        }
    }

    private void RaiseChanged()
    {
        try { Changed?.Invoke(); }
        catch (Exception ex) { DashboardLog.Error("feed listener", ex); }
    }

    public void Dispose()
    {
        if (_disposed) return;
        StopAll(keepSnapshot: false);
        _disposed = true;
    }
}

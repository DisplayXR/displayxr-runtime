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
    /// <summary>
    /// After the last page releases the feed, the child lingers this long before
    /// it is stopped: a hop Home -> Performance -> Home must not restart it,
    /// because a child that cannot reach the service builds a headless snapshot,
    /// and each one creates a vendor instance. Minimising stops it at once.
    /// </summary>
    public static readonly TimeSpan ReleaseLinger = TimeSpan.FromSeconds(15);
    /// <summary>
    /// A watch child that is up but has printed no line this long is reported
    /// (log WARN + <see cref="Silent"/>): the CLI prints a snapshot at once, from
    /// the service or headless, so silence means it is stuck — e.g. a service that
    /// admits it but refuses the status read because it was demoted from DIAG.
    /// </summary>
    public static readonly TimeSpan SilenceWarn = TimeSpan.FromSeconds(10);

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
    private IDisposable? _linger;

    // Diagnostics, one-shot per child: the first line, the first drop, the silence watchdog.
    private bool _childGotLine;
    private bool _childDropLogged;
    private bool _childFirstSnapshotLogged;
    private IDisposable? _silenceTimer;

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
    /// <summary>The current watch child has printed nothing for <see cref="SilenceWarn"/> (cleared by its first line).</summary>
    public bool Silent { get; private set; }
    /// <summary>The newest note the watch child printed on stderr (<c>displayxr-cli status: …</c>), for the pill's tooltip.</summary>
    public string? ChildNote { get; private set; }
    public bool IsRunning => Mode != FeedMode.Idle;

    /// <summary>Raised on the scheduler's thread after any visible state change.</summary>
    public event Action? Changed;

    public void Hold()
    {
        _holders++;
        CancelLinger();
        Sync();
    }

    public void Release()
    {
        _holders = Math.Max(0, _holders - 1);
        if (_holders == 0 && !_paused && !_disposed && Mode != FeedMode.Idle && _linger is null)
        {
            _linger = _sched.Schedule(ReleaseLinger, () =>
            {
                _linger = null;
                Sync();
            });
            return;
        }
        Sync();
    }

    public void SetPaused(bool paused)
    {
        if (_paused == paused) return;
        _paused = paused;
        CancelLinger();
        Sync();
    }

    /// <summary>The child is still running for a page that was just left (see <see cref="ReleaseLinger"/>).</summary>
    public bool IsLingering => _linger is not null;

    private void CancelLinger()
    {
        _linger?.Dispose();
        _linger = null;
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
        else if (!Wanted && Mode != FeedMode.Idle && _linger is null)
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
        _childGotLine = false;
        _childDropLogged = false;
        _childFirstSnapshotLogged = false;
        Silent = false;
        ChildNote = null;
        _silenceTimer?.Dispose();
        _silenceTimer = _sched.Schedule(SilenceWarn, () => OnSilence(id));
        try
        {
            _child = _source.StartWatch(
                line => _sched.Post(() => OnLine(id, line)),
                code => _sched.Post(() => OnExit(id, code)),
                note => _sched.Post(() => OnNote(id, note)));
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
        if (id != _childId || Mode != FeedMode.Watching || _disposed)
        {
            // Expected for a child that was just replaced; logged once per current child
            // so a feed that drops EVERY line (the "connecting… forever" class) says so.
            if (!_childDropLogged && !_disposed)
            {
                _childDropLogged = true;
                DashboardLog.Info($"status --watch: dropped a {line.Length}-byte line (from child #{id}; current #{_childId}, mode {Mode})");
            }
            return;
        }
        line = line.Trim();
        if (line.Length == 0) return;
        _recovering = false;
        if (!_childGotLine)
        {
            _childGotLine = true;
            _silenceTimer?.Dispose();
            _silenceTimer = null;
            if (Silent)
            {
                Silent = false;
                DashboardLog.Info($"status --watch: first line {(_sched.Now - _childStarted).TotalSeconds:0.0} s after start");
            }
        }
        Accept(line);
    }

    private void OnNote(int id, string note)
    {
        if (id != _childId || _disposed || note == ChildNote) return;
        ChildNote = note;
        DashboardLog.Info($"status --watch says: {note}");
        RaiseChanged();
    }

    private void OnSilence(int id)
    {
        _silenceTimer = null;
        if (id != _childId || Mode != FeedMode.Watching || _disposed || _childGotLine) return;
        Silent = true;
        DashboardLog.Warn($"status --watch: no line {SilenceWarn.TotalSeconds:0} s after start (child #{id}: {_child?.State ?? "gone"})" +
                          (ChildNote is null ? "; the CLI printed no note" : $"; last CLI note: {ChildNote.TrimEnd('.')}") +
                          ". Compare: displayxr-cli status --watch --json by hand, and the service log's [CLASS] line for this pid.");
        RaiseChanged();
    }

    private void Accept(string json)
    {
        if (StatusSnapshotParser.TryParse(json, out var snap, out var error))
        {
            if (Mode == FeedMode.Watching && !_childFirstSnapshotLogged)
            {
                _childFirstSnapshotLogged = true;
                DashboardLog.Info($"first snapshot: source={snap!.Source} gen={snap.Generation.Topology}/{snap.Generation.Status}/{snap.Generation.Conversion}");
            }
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
            result = await _source.RunAsync(CliArgs.Words("status --json"), PollTimeout).ConfigureAwait(false);
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
        if (result.Started && !result.TimedOut && StatusSnapshotParser.TryParse(result.Json, out _, out _))
        {
            Accept(result.Json);
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
        _silenceTimer?.Dispose();
        _silenceTimer = null;
        Silent = false;
        ChildNote = null;
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
        CancelLinger();
        StopAll(keepSnapshot: false);
        _disposed = true;
    }
}

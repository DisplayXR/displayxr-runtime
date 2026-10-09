// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Threading;
using System.Threading.Tasks;

namespace DisplayXR.Dashboard.Feed;

/// <summary>The result of one finished <c>displayxr-cli</c> run.</summary>
public sealed record CliResult(int ExitCode, string Stdout, string Stderr, bool TimedOut, string? StartError)
{
    public bool Started => StartError is null;
    public bool Ok => Started && !TimedOut && ExitCode == 0;

    /// <summary>The first non-empty line a person should read: stdout, else stderr, else the failure.</summary>
    public string Summary
    {
        get
        {
            if (StartError is not null) return StartError;
            if (TimedOut) return "displayxr-cli did not answer in time.";
            foreach (var text in new[] { Stdout, Stderr })
                foreach (var line in text.Split('\n'))
                    if (line.Trim().Length > 0 && !line.TrimStart().StartsWith(" WARN", StringComparison.Ordinal))
                        return line.Trim();
            return ExitCode == 0 ? "(done)" : $"displayxr-cli exited with code {ExitCode}.";
        }
    }
}

/// <summary>A running <c>status --watch</c> child.</summary>
public interface IWatchProcess
{
    /// <summary>End the child: close its stdin (its documented stop signal), then kill it if it lingers.</summary>
    void Stop();
}

/// <summary>Where CLI processes come from. The app spawns <c>displayxr-cli</c>; the tests fake it.</summary>
public interface IProcessSource
{
    /// <summary>
    /// Start <c>status --watch --json</c>. <paramref name="onLine"/> gets each
    /// stdout line and <paramref name="onExit"/> fires once when the child is
    /// gone (exit code, or null when it could not start); both on any thread.
    /// </summary>
    IWatchProcess StartWatch(Action<string> onLine, Action<int?> onExit);

    /// <summary>Run one CLI verb to completion.</summary>
    Task<CliResult> RunAsync(string arguments, TimeSpan timeout, CancellationToken cancel = default);
}

/// <summary>The feed's clock and thread. The app posts to the UI thread; the tests drive time by hand.</summary>
public interface IFeedScheduler
{
    DateTime Now { get; }

    /// <summary>Run <paramref name="action"/> on the feed's thread.</summary>
    void Post(Action action);

    /// <summary>Run <paramref name="action"/> on the feed's thread after <paramref name="delay"/>; dispose to cancel.</summary>
    IDisposable Schedule(TimeSpan delay, Action action);
}

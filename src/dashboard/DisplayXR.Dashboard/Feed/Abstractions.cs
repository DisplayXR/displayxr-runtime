// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using System.Threading;
using System.Threading.Tasks;
using DisplayXR.Dashboard.Model;

namespace DisplayXR.Dashboard.Feed;

/// <summary>The result of one finished <c>displayxr-cli</c> run.</summary>
public sealed record CliResult(int ExitCode, string Stdout, string Stderr, bool TimedOut, string? StartError)
{
    public bool Started => StartError is null;

    /// <summary>The runtime's log lines (U_LOG: " WARN [fn] ...", "ERROR [fn] ...") are never a result.</summary>
    private static bool IsLogLine(string l) =>
        l.StartsWith("WARN ", StringComparison.Ordinal) || l.StartsWith("ERROR ", StringComparison.Ordinal) ||
        l.StartsWith("INFO ", StringComparison.Ordinal) || l.StartsWith("DEBUG ", StringComparison.Ordinal) ||
        l.StartsWith("TRACE ", StringComparison.Ordinal);

    /// <summary>
    /// The JSON document in stdout: from the first '{' to the last '}', so a
    /// log line a plug-in printed to stdout around it cannot break the parse
    /// (the same extraction the CI gate uses).
    /// </summary>
    public string Json
    {
        get
        {
            int a = Stdout.IndexOf('{'), b = Stdout.LastIndexOf('}');
            return a >= 0 && b > a ? Stdout[a..(b + 1)] : Stdout;
        }
    }
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
                    if (line.Trim().Length > 0 && !IsLogLine(line.TrimStart()))
                        return line.Trim();
            return ExitCode == 0 ? "(done)" : $"displayxr-cli exited with code {ExitCode} without a result.";
        }
    }

    /// <summary>
    /// Every <c>note:</c> / <c>warning:</c> line the CLI printed (stdout, then stderr), in order:
    /// a write's caveats — "HKLM Policy\Lock is set, so the per-user value is ignored", an
    /// unregistered id — which are rarely the first line, so <see cref="Summary"/> alone drops them.
    /// </summary>
    public IReadOnlyList<string> Notes =>
        new[] { Stdout, Stderr }.SelectMany(t => t.Split('\n')).Select(l => l.Trim())
            .Where(l => l.StartsWith("note:", StringComparison.OrdinalIgnoreCase) ||
                        l.StartsWith("warning:", StringComparison.OrdinalIgnoreCase))
            .ToArray();

    /// <summary>What a person reads after a write: <see cref="Summary"/>, then every note it is not.</summary>
    public string Report => Compose(Summary, Notes);

    /// <summary><paramref name="summary"/> and then each distinct note that is not the summary, one per line.</summary>
    public static string Compose(string summary, IEnumerable<string> notes) =>
        string.Join("\n", new[] { summary }.Concat(notes.Where(n => n != summary).Distinct()));
}

/// <summary>A running <c>status --watch</c> child.</summary>
public interface IWatchProcess
{
    /// <summary>End the child: close its stdin (its documented stop signal), then kill it if it lingers.</summary>
    void Stop();

    /// <summary>One line on the child for the log ("pid 1234, running"); empty when unknown.</summary>
    string State => "";
}

/// <summary>Where CLI processes come from. The app spawns <c>displayxr-cli</c>; the tests fake it.</summary>
public interface IProcessSource
{
    /// <summary>
    /// Start <c>status --watch --json</c>. <paramref name="onLine"/> gets each
    /// stdout line and <paramref name="onExit"/> fires once when the child is
    /// gone (exit code, or null when it could not start); <paramref name="onNote"/>
    /// gets the CLI's own stderr notes (<c>displayxr-cli status: …</c> — a
    /// refused status read, "no service reached"), not the plug-in log lines.
    /// All on any thread.
    /// </summary>
    IWatchProcess StartWatch(Action<string> onLine, Action<int?> onExit, Action<string> onNote);

    /// <summary>Run one CLI verb to completion; <paramref name="args"/> is the argument vector, never a parsed string.</summary>
    Task<CliResult> RunAsync(CliArgs args, TimeSpan timeout, CancellationToken cancel = default);
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

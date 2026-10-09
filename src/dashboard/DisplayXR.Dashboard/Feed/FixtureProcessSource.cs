// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text.Json;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;

namespace DisplayXR.Dashboard.Feed;

/// <summary>
/// <c>--fixture file.ndjson</c>: a development aid. The status feed replays
/// the file's lines (one snapshot per line, every 2 s, looping) instead of
/// running <c>status --watch</c>, so the live-service pages (clients,
/// segments, bound DPs, vendor cells, warnings) can be laid out and soaked on
/// a box whose service cannot answer.
///
/// <para>When the fixture's screens carry a <c>key</c>, the per-screen
/// display-processor verbs (<c>dp list --json</c>, <c>dp use &lt;id&gt; --screen
/// &lt;key&gt;</c>, <c>dp reset --screen &lt;key|all&gt;</c>) are simulated in
/// memory, and the replayed snapshots carry the resulting
/// <c>claim.preferred_plugin</c> / <c>forced</c>. Every other verb goes to the
/// real CLI.</para>
/// </summary>
public sealed class FixtureProcessSource : IProcessSource
{
    private readonly string[] _lines;
    private readonly IProcessSource _real;
    private readonly object _gate = new();
    private readonly Dictionary<string, string> _preferred = new();
    private readonly List<(string Key, string Device, string Name, string? Plugin)> _screens = new();
    private readonly List<(string Id, string Name)> _plugins = new();
    public static readonly TimeSpan Interval = TimeSpan.FromSeconds(2);

    public FixtureProcessSource(string path, IProcessSource real)
    {
        _lines = File.ReadAllLines(path).Where(l => l.Trim().Length > 0).ToArray();
        _real = real;
        try
        {
            if (_lines.Length > 0 && JsonNode.Parse(_lines[0]) is JsonObject root)
            {
                foreach (var s in root["screens"]?.AsArray() ?? new JsonArray())
                    if (s?["key"]?.GetValue<string>() is { } key)
                        _screens.Add((key, s["device_name"]?.GetValue<string>() ?? "", s["friendly_name"]?.GetValue<string>() ?? "",
                                      s["claim"]?["plugin_id"]?.GetValue<string?>()));
                foreach (var p in root["plugins"]?.AsArray() ?? new JsonArray())
                    if (p?["id"]?.GetValue<string>() is { } id) _plugins.Add((id, p["name"]?.GetValue<string>() ?? ""));
            }
        }
        catch (Exception ex) { DashboardLog.Warn($"fixture: {ex.Message}"); }
        DashboardLog.Info($"fixture mode: {_lines.Length} line(s) from {path}; {_screens.Count} keyed screen(s)");
    }

    public IWatchProcess StartWatch(Action<string> onLine, Action<int?> onExit)
    {
        var w = new Replay();
        _ = Task.Run(async () =>
        {
            int i = 0;
            while (!w.Token.IsCancellationRequested && _lines.Length > 0)
            {
                onLine(Apply(_lines[i++ % _lines.Length]));
                try { await Task.Delay(Interval, w.Token).ConfigureAwait(false); } catch (OperationCanceledException) { }
            }
            onExit(0);
        });
        return w;
    }

    /// <summary>Overlay the simulated per-screen overrides onto a replayed line.</summary>
    private string Apply(string line)
    {
        if (_screens.Count == 0) return line;
        try
        {
            if (JsonNode.Parse(line) is not JsonObject root) return line;
            lock (_gate)
            {
                foreach (var s in root["screens"]?.AsArray() ?? new JsonArray())
                {
                    if (s?["key"]?.GetValue<string>() is not { } key || s["claim"] is not JsonObject claim) continue;
                    bool forced = _preferred.TryGetValue(key, out var pref);
                    claim["forced"] = forced;
                    claim["preferred_plugin"] = forced ? pref : null;
                    claim["preferred_source"] = forced ? "user" : "none";
                    claim["apply"] = "live";
                    if (forced) claim["plugin_id"] = pref;
                }
            }
            return root.ToJsonString();
        }
        catch (JsonException) { return line; } // a deliberately truncated line stays truncated
    }

    public Task<CliResult> RunAsync(string arguments, TimeSpan timeout, CancellationToken cancel = default)
    {
        if (arguments == "status --json" && _lines.Length > 0)
            return Task.FromResult(new CliResult(0, Apply(_lines[0]), "", false, null));
        if (_screens.Count > 0 && arguments.StartsWith("dp ", StringComparison.Ordinal))
            return Task.FromResult(Dp(arguments.Split(' ', StringSplitOptions.RemoveEmptyEntries)));
        return _real.RunAsync(arguments, timeout, cancel);
    }

    private CliResult Dp(string[] a)
    {
        lock (_gate)
        {
            if (a is [_, "list", "--json"])
            {
                var o = new JsonObject
                {
                    ["preferred"] = null,
                    ["plugins"] = new JsonArray(_plugins.Select(p => (JsonNode)new JsonObject
                    {
                        ["id"] = p.Id, ["display_name"] = p.Name, ["vendor"] = "", ["version"] = "",
                        ["probe_order"] = 0, ["active"] = false, ["preferred"] = false,
                    }).ToArray()),
                    ["screens"] = new JsonArray(_screens.Select(s =>
                    {
                        bool forced = _preferred.TryGetValue(s.Key, out var pref);
                        return (JsonNode)new JsonObject
                        {
                            ["key"] = s.Key, ["device_name"] = s.Device, ["friendly_name"] = s.Name,
                            ["effective_plugin"] = forced ? pref : s.Plugin, ["preferred_plugin"] = forced ? pref : null,
                            ["preferred_source"] = forced ? "user" : null, ["forced"] = forced, ["apply"] = "live",
                        };
                    }).ToArray()),
                };
                return new CliResult(0, o.ToJsonString(), "", false, null);
            }
            if (a is [_, "use", var id, "--screen", var key])
            {
                if (!_screens.Any(s => s.Key == key)) return new CliResult(1, "", $"dp use: no screen '{key}'", false, null);
                _preferred[key] = id;
                return new CliResult(0, $"Screen {key}: display processor '{id}' (user override, applies live)", "", false, null);
            }
            if (a is [_, "reset", "--screen", var which])
            {
                if (which == "all") _preferred.Clear();
                else _preferred.Remove(which);
                return new CliResult(0, $"Screen {which}: automatic display-processor selection", "", false, null);
            }
            return new CliResult(2, "", $"fixture: unsupported '{string.Join(' ', a)}'", false, null);
        }
    }

    private sealed class Replay : IWatchProcess
    {
        private readonly CancellationTokenSource _cts = new();
        public CancellationToken Token => _cts.Token;
        public void Stop() => _cts.Cancel();
    }
}

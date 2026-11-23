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
using DisplayXR.Dashboard.Model;

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
/// <c>claim.preferred_plugin</c> / <c>forced</c>. A screen's <c>candidates</c>
/// are its claiming plug-in plus every <c>fallback</c> plug-in (sim-display
/// claims every monitor). A <c>dp-machine-override.txt</c> beside the fixture
/// (one plug-in id) simulates a machine-wide PreferredPlugin that a global
/// <c>dp reset</c> clears. Every other verb goes to the real CLI.</para>
/// </summary>
public sealed class FixtureProcessSource : IProcessSource
{
    private readonly string[] _lines;
    private readonly IProcessSource _real;
    private readonly object _gate = new();
    private readonly Dictionary<string, string> _preferred = new();
    private readonly List<(string Key, string Device, string Name, string? Plugin)> _screens = new();
    private readonly List<(string Id, string Name, bool Fallback)> _plugins = new();
    private string? _machinePreferred;
    // workspace-list.json beside the fixture: the phase-8 `workspace` verbs, simulated.
    private readonly JsonObject? _workspace;
    // input-list.json beside the fixture (#2181): `input list --json` and the per-app pin verbs, simulated.
    private readonly JsonObject? _input;
    // <verb>-list.json beside the fixture (conv, tracking; input-list.json above): `<verb> list --json`
    // and `<verb> activate <id> @file --json`, simulated, so the licence block of a Components row can be tried.
    private readonly Dictionary<string, JsonObject> _classLists = new();
    // dp-options-<id>.json beside the fixture: `dp options <id> --json`, `dp set|unset <id> <KEY>`, simulated
    // (a driver with no file answers like the CLI for an unregistered id), so a fixture run never writes a setting.
    private readonly string _dir;
    private readonly Dictionary<string, JsonObject?> _options = new();
    /// <summary>The key the simulated <c>activate</c> accepts (the sim providers' own test key).</summary>
    public const string TestKey = "DXR-SIM-TEST-KEY";
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
                    if (p?["id"]?.GetValue<string>() is { } id)
                        _plugins.Add((id, p["name"]?.GetValue<string>() ?? "", p["fallback"]?.GetValue<bool>() ?? false));
            }
        }
        catch (Exception ex) { DashboardLog.Warn($"fixture: {ex.Message}"); }
        string dir = Path.GetDirectoryName(Path.GetFullPath(path)) ?? ".";
        _dir = dir;
        try
        {
            string mo = Path.Combine(dir, "dp-machine-override.txt");
            if (File.Exists(mo) && File.ReadAllText(mo).Trim() is { Length: > 0 } id) _machinePreferred = id;
        }
        catch (Exception ex) { DashboardLog.Warn($"fixture machine override: {ex.Message}"); }
        string ws = Path.Combine(dir, "workspace-list.json");
        try { if (File.Exists(ws)) _workspace = JsonNode.Parse(File.ReadAllText(ws)) as JsonObject; }
        catch (Exception ex) { DashboardLog.Warn($"fixture workspace: {ex.Message}"); }
        string il = Path.Combine(dir, "input-list.json");
        try { if (File.Exists(il)) _input = JsonNode.Parse(File.ReadAllText(il)) as JsonObject; }
        catch (Exception ex) { DashboardLog.Warn($"fixture input: {ex.Message}"); }
        if (_input is not null) _classLists["input"] = _input;
        foreach (string verb in new[] { "conv", "tracking" })
        {
            try
            {
                string f = Path.Combine(dir, verb + "-list.json");
                if (File.Exists(f) && JsonNode.Parse(File.ReadAllText(f)) is JsonObject o) _classLists[verb] = o;
            }
            catch (Exception ex) { DashboardLog.Warn($"fixture {verb} list: {ex.Message}"); }
        }
        DashboardLog.Info($"fixture mode: {_lines.Length} line(s) from {path}; {_screens.Count} keyed screen(s); workspace {(_workspace is null ? "absent" : "simulated")}");
    }

    public IWatchProcess StartWatch(Action<string> onLine, Action<int?> onExit, Action<string> onNote)
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

    /// <summary>Every other stream (the calibration watch) goes to the real CLI.</summary>
    public IWatchProcess StartStream(CliArgs args, Action<string> onLine, Action<int?> onExit, Action<string> onNote) =>
        _real.StartStream(args, onLine, onExit, onNote);

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

    /// <summary>
    /// <c>dp set &lt;id&gt; &lt;KEY&gt; --stdin</c> with the value on standard input, as the CLI takes it; the
    /// fixture checks the line as the CLI does (one line, not empty) and never keeps or prints it.
    /// <see cref="StdinUnsupported"/>: answer as a CLI older than <c>--stdin</c> does.
    /// </summary>
    public Task<CliResult> RunWithInputAsync(CliArgs args, string input, TimeSpan timeout, CancellationToken cancel = default)
    {
        var a = args.Args.ToArray();
        if (a is ["dp", "set", var id, var key, "--stdin"])
        {
            if (StdinUnsupported)
                return Task.FromResult(new CliResult(1, $"error: 'dp set' needs <id> <KEY> <value>.\n\n  options <id> [--json] ...", "", false, null));
            string line = (input ?? "").Split('\n')[0].TrimEnd('\r');
            if (line.Length == 0)
                return Task.FromResult(new CliResult(1, "error: --stdin read an empty value — nothing written (use 'dp unset' to clear it).", "", false, null));
            return Task.FromResult(Options(new[] { "dp", "set", id, key, line }, fromStdin: true));
        }
        return _real.RunWithInputAsync(args, input ?? "", timeout, cancel);
    }

    /// <summary>Answer <c>set ... --stdin</c> as a CLI that predates it (tests of the Dashboard's fallback).</summary>
    public bool StdinUnsupported { get; set; }

    public Task<CliResult> RunAsync(CliArgs args, TimeSpan timeout, CancellationToken cancel = default)
    {
        var a = args.Args.ToArray();
        if (a is ["status", "--json"] && _lines.Length > 0)
            return Task.FromResult(new CliResult(0, Apply(_lines[0]), "", false, null));
        if (_workspace is not null && a is ["workspace", ..])
            return Task.FromResult(Workspace(a));
        if (a is [var cv, "activate" or "activate-begin", ..] && _classLists.ContainsKey(cv))
            return Task.FromResult(Activate(a));
        if (a is [var lv, "list", "--json"] && lv != "input" && _classLists.TryGetValue(lv, out var cl))
            lock (_gate) return Task.FromResult(new CliResult(0, cl.ToJsonString(), "", false, null));
        if (_input is not null && (a is ["input", "list", "--json"] || a is ["input", "use" or "reset", .., "--app", _]))
            return Task.FromResult(Input(a));
        if (a is ["dp", "options", _, "--json"] or ["dp", "set", _, _, _] or ["dp", "unset", _, _])
            return Task.FromResult(Options(a));
        if (_screens.Count > 0 && a is ["dp", ..])
            return Task.FromResult(Dp(a));
        return _real.RunAsync(args, timeout, cancel);
    }

    /// <summary>A driver's settings from <c>dp-options-&lt;id&gt;.json</c>, read once and changed in memory by set / unset.</summary>
    private JsonObject? OptionsOf(string id)
    {
        if (_options.TryGetValue(id, out var o)) return o;
        try
        {
            string f = Path.Combine(_dir, $"dp-options-{id}.json");
            o = File.Exists(f) ? JsonNode.Parse(File.ReadAllText(f)) as JsonObject : null;
        }
        catch (Exception ex) { DashboardLog.Warn($"fixture dp options: {ex.Message}"); o = null; }
        _options[id] = o;
        return o;
    }

    private CliResult Options(string[] a, bool fromStdin = false)
    {
        lock (_gate)
        {
            string id = a[2];
            if (OptionsOf(id) is not { } doc)
                return new CliResult(1, $"FAIL: '{id}' is not a registered display processor (see 'dp list') — nothing to show.", "", false, null);
            if (a[1] == "options") return new CliResult(0, doc.ToJsonString(), "", false, null);
            string key = a[3].ToUpperInvariant();
            var row = (doc["settings"] as JsonArray)?.OfType<JsonObject>().FirstOrDefault(r => r["key"]?.GetValue<string>() == key);
            if (row is null) return new CliResult(1, "", $"fixture: '{key}' is not a setting of '{id}'", false, null);
            if (row["source"]?.GetValue<string>() == "environment")
                return new CliResult(0, $"{id}: {key} written per user.\nnote: DXR_DP_{id.ToUpperInvariant().Replace('-', '_')}_{key} is set in this process's environment and wins.", "", false, null);
            bool secret = row["secret"]?.GetValue<bool>() == true;
            if (a[1] == "set")
            {
                row["source"] = "user";
                row["value"] = secret ? null : a[4];
                if (secret) row["set"] = true;
                // As the CLI: a secret, or any value that came through --stdin, is never echoed.
                return new CliResult(0, secret ? $"{id}: {key} = <set> per user (fixture). The value is a secret: it is never shown again."
                                       : fromStdin ? $"{id}: {key} = <set> per user (fixture). Read from standard input: not echoed."
                                       : $"{id}: {key} = '{a[4]}' per user (fixture).", "", false, null);
            }
            row["source"] = "absent";
            row["value"] = null;
            if (secret) row["set"] = false;
            return new CliResult(0, $"{id}: {key} cleared per user (fixture). Effective now: {key} unset — the display processor keeps its compiled default.", "", false, null);
        }
    }

    private CliResult Dp(string[] a)
    {
        lock (_gate)
        {
            if (a is [_, "list", "--json"])
            {
                var o = new JsonObject
                {
                    ["preferred"] = _machinePreferred,
                    ["plugins"] = new JsonArray(_plugins.Select(p => (JsonNode)new JsonObject
                    {
                        ["id"] = p.Id, ["display_name"] = p.Name, ["vendor"] = "", ["version"] = "",
                        ["probe_order"] = 0, ["active"] = false, ["preferred"] = p.Id == _machinePreferred,
                    }).ToArray()),
                    ["screens"] = new JsonArray(_screens.Select(s =>
                    {
                        bool forced = _preferred.TryGetValue(s.Key, out var pref);
                        return (JsonNode)new JsonObject
                        {
                            ["key"] = s.Key, ["device_name"] = s.Device, ["friendly_name"] = s.Name,
                            ["effective_plugin"] = forced ? pref : s.Plugin, ["preferred_plugin"] = forced ? pref : null,
                            ["preferred_source"] = forced ? "user" : null, ["forced"] = forced, ["apply"] = "live",
                            ["candidates"] = new JsonArray(_plugins
                                .Where(p => p.Id == s.Plugin || p.Fallback)
                                .Select(p => (JsonNode)new JsonObject { ["plugin_id"] = p.Id, ["confidence"] = p.Fallback ? 10 : 100 })
                                .ToArray()),
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
            if (a is [_, "reset"])
            {
                _machinePreferred = null;
                return new CliResult(0, "PreferredPlugin override cleared — normal ProbeOrder discovery restored.", "", false, null);
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

    /// <summary>
    /// #2181: <c>input list --json</c> from <c>input-list.json</c>, and <c>input use &lt;id&gt; --app &lt;exe&gt;</c> /
    /// <c>input reset --app &lt;exe&gt;</c> against its <c>per_app</c> rows (a live holder follows a pin to a
    /// provider that supplies the family, as the runtime's walk would, only roughly: controllers).
    /// Every other <c>input</c> verb goes to the real CLI.
    /// </summary>
    private CliResult Input(string[] a)
    {
        lock (_gate)
        {
            var perApp = _input!["per_app"] as JsonArray ?? new JsonArray();
            _input["per_app"] = perApp;
            JsonObject? Pin(string exe) => perApp.OfType<JsonObject>().FirstOrDefault(p => p["app"]?.GetValue<string>() == exe);
            if (a is [_, "list", "--json"]) return new CliResult(0, _input.ToJsonString(), "", false, null);
            if (a is [_, "use", var id, "--app", var exe])
            {
                bool registered = id == PolicyList.EmulatedId
                    || (_input["providers"] as JsonArray)?.OfType<JsonObject>().Any(p => p["id"]?.GetValue<string>() == id) == true;
                if (Pin(exe) is { } row) row["preferred"] = id;
                else perApp.Add(new JsonObject { ["app"] = exe, ["preferred"] = id, ["registered"] = registered });
                foreach (var h in (_input["holders"] as JsonArray ?? new JsonArray()).OfType<JsonObject>())
                    if (h["app"]?.GetValue<string>() == exe && h["holders"] is JsonObject held && h["preferred"] is JsonObject pref)
                    {
                        held["controllers"] = id;
                        pref["controllers"] = id;
                    }
                return new CliResult(0, $"Per-app pin: {exe} -> '{id}'", "", false, null);
            }
            if (a is [_, "reset", "--app", var rexe])
            {
                if (Pin(rexe) is { } row) perApp.Remove(row);
                return new CliResult(0, $"Per-app pin for {rexe} cleared", "", false, null);
            }
            return new CliResult(2, "", $"fixture: unsupported '{string.Join(' ', a)}'", false, null);
        }
    }

    /// <summary>
    /// <c>&lt;verb&gt; activate &lt;id&gt; @file --json</c> against a simulated class list: the file's first
    /// line is the key; <see cref="TestKey"/> makes the provider <c>licensed</c> (source <c>key</c>),
    /// anything else is refused. <c>&lt;verb&gt; activate-begin &lt;id&gt; --json</c> answers in the CLI's shape
    /// (activation-deep-link.md §3): <c>ok</c> with a complete URL for a provider whose licence says
    /// <c>activation.deep_link</c>, else <c>no-deep-link</c> / <c>no-purchase-url</c> / <c>not-registered</c>.
    /// The nonce in that URL is made up: nothing is pending anywhere.
    /// </summary>
    private CliResult Activate(string[] a)
    {
        if (a is [var bverb, "activate-begin", var bid, ..])
        {
            lock (_gate)
            {
                var list = _classLists[bverb];
                var row = ((list["providers"] ?? list["plugins"]) as JsonArray)?.OfType<JsonObject>()
                    .FirstOrDefault(p => p["id"]?.GetValue<string>() == bid);
                var lic = row?["licence"] as JsonObject;
                string purchase = lic?["purchase_url"]?.GetValue<string>() ?? "";
                string result = row is null ? "not-registered"
                    : lic?["activation"]?["deep_link"]?.GetValue<bool>() != true ? "no-deep-link"
                    : !purchase.StartsWith("https://", StringComparison.Ordinal) ? "no-purchase-url" : "ok";
                bool ok = result == "ok";
                long expires = DateTimeOffset.UtcNow.AddSeconds(900).ToUnixTimeSeconds();
                var o = new JsonObject
                {
                    ["class"] = bverb, ["id"] = bid, ["result"] = result,
                    ["url"] = ok ? purchase + (purchase.Contains('?') ? "&" : "?")
                                   + "displayxr_nonce=FIXTUREFIXTUREFIXTUREFIXTUREFIXTUREFIXTURE0"
                                   + "&displayxr_item=" + bverb + "%2F" + Uri.EscapeDataString(bid)
                                   + "&displayxr_return=displayxr%3A%2F%2Factivate" : "",
                    ["return_url"] = "displayxr://activate", ["nonce_id"] = ok ? "f1x7ure0" : "", ["ttl_s"] = 900,
                    ["expires_unix"] = ok ? expires : null,
                    ["expires"] = ok ? DateTimeOffset.FromUnixTimeSeconds(expires).ToString("yyyy-MM-ddTHH:mm:ssZ") : null,
                    ["licence"] = lic?.DeepClone(),
                };
                return new CliResult(ok ? 0 : 1, o.ToJsonString(), "", false, null);
            }
        }
        if (a is not [var verb, "activate", var id, var arg, ..] || !arg.StartsWith('@'))
            return new CliResult(2, "", $"fixture: unsupported '{string.Join(' ', a)}'", false, null);
        string key;
        try { key = File.ReadLines(arg[1..]).FirstOrDefault()?.Trim() ?? ""; }
        catch (Exception) { return new CliResult(1, "", "error: cannot read the token file.", false, null); }
        lock (_gate)
        {
            var list = _classLists[verb];
            var row = ((list["providers"] ?? list["plugins"]) as JsonArray)?.OfType<JsonObject>()
                .FirstOrDefault(p => p["id"]?.GetValue<string>() == id);
            if (row is null)
                return new CliResult(1, new JsonObject { ["id"] = id, ["result"] = "not-loaded", ["accepted"] = false, ["error"] = "" }.ToJsonString(), "", false, null);
            bool ok = key == TestKey;
            if (ok)
                row["licence"] = new JsonObject
                {
                    ["state"] = "licensed", ["source"] = "key", ["expires"] = null, ["message"] = "Activated with your key",
                    ["purchase_url"] = "", ["activation"] = new JsonObject { ["key"] = true, ["deep_link"] = false }, ["blocks"] = false,
                };
            var o = new JsonObject
            {
                ["id"] = id, ["class"] = verb, ["result"] = ok ? "ok" : "refused", ["accepted"] = ok,
                ["error"] = ok ? "" : "the key was not recognised",
            };
            return new CliResult(ok ? 0 : 1, o.ToJsonString(), "", false, null);
        }
    }

    private CliResult Workspace(string[] a)
    {
        lock (_gate)
        {
            if (a is [_, "list", "--json"]) return new CliResult(0, _workspace!.ToJsonString(), "", false, null);
            if (a is [_, "hotkey-suspend", "on" or "off"]) return new CliResult(0, $"Workspace hotkey {(a[2] == "on" ? "suspended" : "resumed")}.", "", false, null);
            JsonObject? Find(string id) => _workspace!["controllers"]?.AsArray()
                .OfType<JsonObject>().FirstOrDefault(c => c["id"]?.GetValue<string>() == id.Trim('"'));
            if (a is [_, "set", var sid, .. var rest] && Find(sid) is { } c && c["launch"] is JsonObject l)
            {
                if (rest is ["--hotkey", var combo])
                {
                    if (!Model.Hotkey.TryParse(combo.Trim('"'), out var hk, out var err))
                        return new CliResult(2, "", $"workspace set: bad hotkey: {err}", false, null);
                    l["hotkey"] = hk.ToString();
                }
                else if (rest is ["--no-hotkey"]) l["hotkey"] = null;
                else if (rest is ["--mode", "auto" or "disabled"]) l["mode"] = rest[1];
                else return new CliResult(2, "", "workspace set: bad arguments", false, null);
                l["source"] = "user";
                return new CliResult(0, $"Workspace controller '{sid}' updated (live).", "", false, null);
            }
            if (a is [_, "reset", var rid] && Find(rid) is { } rc && rc["launch"] is JsonObject rl)
            {
                rl["mode"] = "auto";
                rl["hotkey"] = Model.Hotkey.Default;
                rl["source"] = "default";
                return new CliResult(0, $"Workspace controller '{rid}': launch settings back to the defaults (mode=auto hotkey={Model.Hotkey.Default}).", "", false, null);
            }
            if (a is [_, "launch", var lid] && Find(lid) is { } lc)
            {
                if (lc["launch"]?["mode"]?.GetValue<string>() == "disabled")
                    return new CliResult(1, "", "workspace launch: launching is disabled", false, null);
                lc["connected"] = true;
                lc["pid"] = 9999;
                return new CliResult(0, $"Launched '{lid}'.", "", false, null);
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

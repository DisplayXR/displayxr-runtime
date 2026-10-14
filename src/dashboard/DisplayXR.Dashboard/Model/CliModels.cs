// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;

namespace DisplayXR.Dashboard.Model;

// The one-shot CLI verbs the dashboard reads besides `status`: `info --json`
// (runtime / plug-in / GPU topology, read once per Home show and on Refresh),
// `perf list --json`, `dp list --json` and `selftest --json`. Same rules as the
// snapshot: tolerant, immutable, enum-like values verbatim.

public sealed record GpuAdapter(string Name, string Luid, long VramMb);

public sealed record GpuEndpoint(bool Resolved, string Name, string Luid);

public sealed record GpuTopology(
    bool Probed, string Note, string Verdict, bool SplitApplies, IReadOnlyList<GpuAdapter> Adapters,
    GpuEndpoint Scanout, GpuEndpoint Render, GpuEndpoint Ingest, string IngestProvenance,
    bool WeaveOnScanoutSet, string? WeaveOnScanout, string WeaveOnScanoutSource, string Ingress, string ServiceSplit);

public sealed record PerfLever(string Name, string? Value, string Source)
{
    /// <summary>Set by anything other than the runtime's own default.</summary>
    public bool IsSet => !string.IsNullOrEmpty(Value);
}

public sealed record PerfState(string UserFile, string UserWritten, IReadOnlyList<PerfLever> Levers)
{
    public static readonly PerfState Empty = new("", "", Array.Empty<PerfLever>());

    public PerfLever? Lever(string name) => Levers.FirstOrDefault(l => l.Name == name);
    public string Value(string name) => Lever(name)?.Value ?? "";
    public string SourceOf(string name) => Lever(name)?.Source ?? "";
    public bool AnyNonDefault => Levers.Any(l => l.IsSet);

    /// <summary>
    /// Compatibility mode is derived from the resolved levers, never stored:
    /// the UI can then never claim a mode the runtime is not in.
    /// </summary>
    public bool CompatibilityMode => Value("DXR_WEAVE_ON_SCANOUT").StartsWith('0') && Value("DXR_WEAVE_REPAINT").StartsWith('0');

    public bool DiagnosticsOn => Value("DXR_FRAME_WITNESS").Length > 0 || Value("DXR_FRAME_STAGE_TIMING").Length > 0;

    public string TargetGpu => Value("DXR_D3D_FORCE_GPU");

    public static PerfState Read(JsonElement e) => new(
        e.Str("user_file"), e.Str("user_written"),
        e.Arr("levers").Where(l => l.ValueKind == JsonValueKind.Object)
         .Select(l => new PerfLever(l.Str("name"), l.StrOrNull("value"), l.Str("source", "default"))).ToArray());
}

public sealed record InfoPlugin(string Id, string Name, string Version, string LoadResult, string PlatformState,
                                string Hint, string Reason, bool Fallback, int ProbeOrder);

public sealed record InfoResult(
    string Description, string GitTag, int PluginAbi, bool ActiveRuntimeQueried, bool ActiveRuntimeSet,
    string ActiveRuntimeValue, string? ActivePluginId, string ActivePluginName, string ActivePluginVendor,
    string ActivePluginVersion, string Device, IReadOnlyList<InfoPlugin> Plugins, GpuTopology? Gpu, PerfState Perf)
{
    /// <summary>The Components page's blocks (workspace controllers, input, conversion, camera).</summary>
    public InfoComponents Components { get; init; } = InfoComponents.Empty;

    public bool ActiveRuntimeIsDisplayXR =>
        ActiveRuntimeSet && ActiveRuntimeValue.Contains("DisplayXR", StringComparison.OrdinalIgnoreCase);

    public static bool TryParse(string? json, out InfoResult? info, out string? error)
    {
        info = null;
        error = null;
        if (string.IsNullOrWhiteSpace(json)) { error = "displayxr-cli printed nothing"; return false; }
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) { error = "not a JSON object"; return false; }
            var rt = root.Obj("runtime");
            var ar = root.Obj("active_openxr_runtime");
            var pl = root.Obj("plugin");
            var g = root.Obj("gpu");
            GpuTopology? gpu = null;
            if (g is { } gg)
            {
                GpuEndpoint Ep(string key)
                {
                    var o = gg.Obj(key);
                    return new GpuEndpoint(o.Bool("resolved"), o.Str("name"), o.Str("luid"));
                }
                var ingest = gg.Obj("service_ingest");
                var split = gg.Obj("split");
                gpu = new GpuTopology(
                    gg.Bool("probed"), gg.Str("note"), gg.Str("verdict"), gg.Bool("split_applies"),
                    gg.Arr("adapters").Where(a => a.ValueKind == JsonValueKind.Object)
                      .Select(a => new GpuAdapter(a.Str("name"), a.Str("luid"), a.Long("dedicated_vram_mb"))).ToArray(),
                    Ep("scanout"), Ep("render"), Ep("service_ingest"), ingest.Str("provenance"),
                    split.Bool("weave_on_scanout_set"), split.StrOrNull("weave_on_scanout"),
                    split.Str("weave_on_scanout_source"), split.Str("ingress"), split.Str("service_split"));
            }
            var perf = root.Obj("performance") is { } pf ? PerfState.Read(pf) : PerfState.Empty;
            info = new InfoResult(
                rt.Str("description"), rt.Str("git_tag"), rt.Int("plugin_abi_version"),
                ar is not null, ar.Bool("set"), ar.Str("value"),
                pl.StrOrNull("id"), pl.Str("display_name"), pl.Str("vendor"), pl.Str("version"),
                root.Str("device"),
                root.Arr("plugins").Where(p => p.ValueKind == JsonValueKind.Object).Select(p => new InfoPlugin(
                    p.Str("id"), p.Str("display_name"), p.Str("version"), p.Str("load_result"),
                    p.Str("platform_state", "UNKNOWN"), p.Str("hint"), p.Str("reason"), p.Bool("fallback"),
                    p.Int("probe_order"))).ToArray(),
                gpu, perf) { Components = InfoComponents.Read(root) };
            return true;
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException or FormatException)
        {
            error = ex.Message;
            return false;
        }
    }
}

public sealed record DpRow(string Id, string Name, string Vendor, string Version, int ProbeOrder, bool Active, bool Preferred);

/// <summary>A plug-in that claimed a screen (at any confidence): one that can drive it.</summary>
public sealed record DpCandidate(string PluginId, int Confidence);

/// <summary>
/// One screen of <c>dp list --json</c> <c>screens[]</c> (phase 7, per-screen override).
/// <see cref="Candidates"/> is null when the CLI does not list them (older CLI, or a
/// resolve that asked only the active plug-in): the selector then offers every
/// registered plug-in.
/// </summary>
public sealed record DpScreen(string Key, string DeviceName, string FriendlyName, string? EffectivePlugin,
                              string? PreferredPlugin, string? PreferredSource, bool Forced, string? Apply,
                              IReadOnlyList<DpCandidate>? Candidates = null);

public sealed record DpList(string? Preferred, IReadOnlyList<DpRow> Plugins, IReadOnlyList<DpScreen>? Screens = null)
{
    public DpScreen? Screen(string? key) => key is null ? null : Screens?.FirstOrDefault(s => s.Key == key);

    public static bool TryParse(string? json, out DpList? list)
    {
        list = null;
        if (string.IsNullOrWhiteSpace(json)) return false;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return false;
            // plugins[] entries are objects today; a bare id string is accepted too.
            var plugins = root.Arr("plugins").Select(p => p.ValueKind switch
            {
                JsonValueKind.Object => new DpRow(p.Str("id"), p.Str("display_name"), p.Str("vendor"), p.Str("version"),
                                                  p.Int("probe_order"), p.Bool("active"), p.Bool("preferred")),
                JsonValueKind.String => new DpRow(p.GetString() ?? "", "", "", "", 0, false, false),
                _ => null,
            }).Where(r => r is not null && r.Id.Length > 0).Select(r => r!).ToArray();
            IReadOnlyList<DpScreen>? screens = root.Has("screens")
                ? root.Arr("screens").Where(s => s.ValueKind == JsonValueKind.Object && s.Str("key").Length > 0)
                      .Select(s => new DpScreen(s.Str("key"), s.Str("device_name"), s.Str("friendly_name"),
                          s.StrOrNull("effective_plugin"), s.StrOrNull("preferred_plugin"), s.StrOrNull("preferred_source"),
                          s.Bool("forced"), s.StrOrNull("apply"),
                          s.TryGetProperty("candidates", out var ca) && ca.ValueKind == JsonValueKind.Array
                              ? ca.EnumerateArray().Where(c => c.ValueKind == JsonValueKind.Object && c.Str("plugin_id").Length > 0)
                                 .Select(c => new DpCandidate(c.Str("plugin_id"), c.Int("confidence"))).ToArray()
                              : null)).ToArray()
                : null;
            list = new DpList(root.StrOrNull("preferred"), plugins, screens);
            return true;
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException) { return false; }
    }
}

public sealed record SelftestCheck(string Name, bool Ok, string Detail);

public sealed record SelftestResult(string Verdict, int ResultCode, IReadOnlyList<SelftestCheck> Checks, DateTime When)
{
    public bool Passed => ResultCode == 0;

    public static bool TryParse(string? json, DateTime when, out SelftestResult? result)
    {
        result = null;
        if (string.IsNullOrWhiteSpace(json)) return false;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return false;
            result = new SelftestResult(root.Str("verdict", "?"), root.Int("result_code", -1),
                root.Arr("checks").Where(c => c.ValueKind == JsonValueKind.Object)
                    .Select(c => new SelftestCheck(c.Str("name"), c.Bool("ok"), c.Str("detail"))).ToArray(), when);
            return true;
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException) { return false; }
    }
}

public static class PerfStateParser
{
    public static bool TryParse(string? json, out PerfState? state)
    {
        state = null;
        if (string.IsNullOrWhiteSpace(json)) return false;
        try
        {
            using var doc = JsonDocument.Parse(json);
            if (doc.RootElement.ValueKind != JsonValueKind.Object) return false;
            state = PerfState.Read(doc.RootElement);
            return true;
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException) { return false; }
    }
}

/// <summary>Which process a Developer-settings row is filtered to.</summary>
public enum ProcFilter { All = 0, App = 1, Service = 2 }

/// <summary>
/// One row of <c>perf list --all --json</c> (perf-settings Phase 2A): a Tier 1
/// or Tier 2 lever from the census. <see cref="Managed"/> levers resolve from
/// the per-user / machine stores and can be reset; every other lever is read by
/// its own <c>getenv</c> and is environment-only, so the list offers no write
/// for it (a stored value would do nothing).
/// </summary>
public sealed record DevLever(string Name, int Tier, string Proc, string Default, string Description,
                              bool Managed, string? Value, string Source)
{
    /// <summary>Resolved from anything but the compiled default (env, user or machine).</summary>
    public bool NonDefault => Source.Length > 0 && Source != "default";

    /// <summary>Read in the app's process (the runtime DLL), per the census Proc column.</summary>
    public bool ForApp => Proc.Contains("App", StringComparison.OrdinalIgnoreCase) || Proc.Contains("Both", StringComparison.OrdinalIgnoreCase);

    /// <summary>Read in displayxr-service.exe, per the census Proc column.</summary>
    public bool ForService => Proc.Contains("Svc", StringComparison.OrdinalIgnoreCase) ||
                              Proc.Contains("Service", StringComparison.OrdinalIgnoreCase) ||
                              Proc.Contains("Both", StringComparison.OrdinalIgnoreCase);

    /// <summary>The per-user store holds it: the one source <c>perf reset NAME</c> can clear.</summary>
    public bool Resettable => Managed && Source == "user";

    /// <summary>Search (name / description substring, case-insensitive), tier (0 = all) and process filters.</summary>
    public bool Matches(string? search, int tier, ProcFilter proc)
    {
        if (tier != 0 && Tier != tier) return false;
        if (proc == ProcFilter.App && !ForApp) return false;
        if (proc == ProcFilter.Service && !ForService) return false;
        string q = search?.Trim() ?? "";
        return q.Length == 0 || Name.Contains(q, StringComparison.OrdinalIgnoreCase) ||
               Description.Contains(q, StringComparison.OrdinalIgnoreCase);
    }
}

/// <summary><c>perf list --all --json</c>: the Developer page's read-only lever list.</summary>
public sealed record DevLeverList(string UserFile, string UserWritten, string ResolvedFor, IReadOnlyList<DevLever> Levers)
{
    public int NonDefaultCount => Levers.Count(l => l.NonDefault);

    /// <summary>
    /// False when the CLI ignored <c>--all</c> (it predates Phase 2A) and printed
    /// the plain allow-listed list: no row then carries a tier.
    /// </summary>
    public bool IsCatalog => ResolvedFor.Length > 0 || Levers.Any(l => l.Tier > 0);

    public DevLever? Lever(string name) => Levers.FirstOrDefault(l => l.Name == name);

    public static DevLeverList Read(JsonElement e) => new(
        e.Str("user_file"), e.Str("user_written"), e.Str("resolved_for"),
        e.Arr("levers").Where(l => l.ValueKind == JsonValueKind.Object && l.Str("name").Length > 0)
         .Select(l => new DevLever(l.Str("name"), l.Int("tier"), l.Str("proc"), l.Str("default"), l.Str("description"),
                                   l.Bool("managed"), l.StrOrNull("value"), l.Str("source", "default"))).ToArray());

    public static bool TryParse(string? json, out DevLeverList? list)
    {
        list = null;
        if (string.IsNullOrWhiteSpace(json)) return false;
        try
        {
            using var doc = JsonDocument.Parse(json);
            if (doc.RootElement.ValueKind != JsonValueKind.Object) return false;
            list = Read(doc.RootElement);
            return true;
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException) { return false; }
    }
}

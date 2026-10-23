// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;

namespace DisplayXR.Dashboard.Model;

// ADR-054 N6 Part 2 (#2144): viewer-tracking providers on the Components page.
// Three CLI sources, all read tolerantly (every key optional; an older CLI that
// has none of them simply shows no tracking card):
//   - `tracking list --json`: the rows and their policy (PolicyList, class Tracking);
//   - `info --json` `tracking_providers`: what the headless probe found (ranked,
//     scan counts) and the primary screen's arbitration (TrackingSummary);
//   - the status feed's per-screen `eye_tracking.viewer_source` / `viewer_reason` /
//     `viewer_policy` (TrackingScreen).
// Contract: docs/specs/runtime/tracking-provider-discovery.md.

/// <summary>
/// <c>info --json</c> (or <c>selftest --json</c>) <c>tracking_providers</c>: the class as the headless
/// CLI evaluated it. <see cref="Ranked"/> = the providers whose probe claimed a usable source, best
/// first. <see cref="ViewerSource"/> / <see cref="ViewerReason"/> / <see cref="DisplayViewerPolicy"/>
/// are the primary screen's arbitration ("" when not evaluated).
/// </summary>
public sealed record TrackingSummary(int Registered, int Enabled, int ScanDeclined, int ScanFailed, int ScanUntrusted,
                                     IReadOnlyList<string> Ranked, string ActiveId, bool Evaluated, string ViewerSource,
                                     string ViewerReason, string DisplayViewerPolicy, int ComposedModes)
{
    /// <summary>Read <c>tracking_providers</c> from the root of <c>info --json</c>; null when the CLI predates it.</summary>
    public static TrackingSummary? Read(JsonElement root)
    {
        if (root.Obj("tracking_providers") is not { ValueKind: JsonValueKind.Object } tp) return null;
        return new TrackingSummary(tp.Int("registered"), tp.Int("enabled"), tp.Int("scan_declined"), tp.Int("scan_failed"),
            tp.Int("scan_untrusted"),
            tp.Arr("ranked").Where(e => e.ValueKind == JsonValueKind.String).Select(e => e.GetString() ?? "").Where(s => s.Length > 0).ToArray(),
            tp.Str("active_id"), tp.Bool("evaluated"), tp.Str("viewer_source"), tp.Str("viewer_reason"),
            tp.Str("display_viewer_policy"), tp.Int("composed_eye_tracking_modes"));
    }

    /// <summary>
    /// Whether the lazy provider scan ran in the CLI that built this summary: it runs only where a
    /// screen could use a provider (a display that does not track and declares EXTERNAL_OK), so
    /// when it did not run, no row was probed.
    /// </summary>
    public bool ScanRan => Evaluated && Ranked.Count + ScanDeclined + ScanFailed + ScanUntrusted > 0;

    /// <summary>
    /// The probe chip of one row: "usable source" when its probe claimed one (and "ranked first"
    /// for the best), "no usable source" when the scan ran and it is not resident (the scan does
    /// not say per row whether it declined, failed to load or was refused — the counts do), "not
    /// probed" when the scan did not run. Null for a disabled row (never probed) or before info.
    /// </summary>
    public (string Text, Level Level)? ProbeChip(PolicyRow r)
    {
        if (!Evaluated || !r.Enabled) return null;
        int i = Ranked.ToList().IndexOf(r.Id);
        if (i == 0) return ("probe: usable source, ranked first", Level.Ok);
        if (i > 0) return ($"probe: usable source, #{i + 1}", Level.Ok);
        return ScanRan ? ("probe: no usable source", Level.Plain) : ("not probed", Level.Plain);
    }

    /// <summary>The scan's counts as one line ("" when nothing was declined, failed or refused).</summary>
    public string ScanLine => !ScanRan ? ""
        : string.Join(StatusText.Sep, new[]
        {
            ScanDeclined > 0 ? StatusText.Plural(ScanDeclined, "declined (no source)", "declined (no source)") : "",
            ScanFailed > 0 ? StatusText.Plural(ScanFailed, "failed to load", "failed to load") : "",
            ScanUntrusted > 0 ? StatusText.Plural(ScanUntrusted, "refused (provenance)", "refused (provenance)") : "",
        }.Where(x => x.Length > 0));
}

/// <summary>What the views on one screen follow (the arbitration's outcome), as the page words it.</summary>
public enum ViewerSourceKind
{
    /// <summary>Not evaluated in the process that built the snapshot, or a CLI that predates the field.</summary>
    Unknown,
    /// <summary>The display's own eyes (it tracks, or owns the viewer by policy).</summary>
    Display,
    /// <summary>A viewer-tracking provider.</summary>
    Provider,
    /// <summary>The nominal viewer.</summary>
    Nominal,
}

/// <summary>
/// One screen of the tracking card: whose eyes its views follow, why, and the display's viewer
/// policy. <see cref="Reason"/> is the page's sentence; <see cref="RawReason"/> is the CLI's.
/// </summary>
public sealed record TrackingScreen(string ScreenId, string Name, bool Primary, ViewerSourceKind Kind, string? ProviderId,
                                    string Source, string Reason, string? RawReason, string? Policy, Level Level)
{
    /// <summary>Camera-pose calibration is per provider-served screen (tracking-provider-discovery §6).</summary>
    public bool CanCalibrate => Kind == ViewerSourceKind.Provider;

    /// <summary>The viewer-policy chip ("policy not reported" when this screen was not evaluated).</summary>
    public (string Text, Level Level) PolicyChip => Policy switch
    {
        null or "" => ("policy not reported", Level.Plain),
        "EXTERNAL_OK" => ("policy EXTERNAL_OK", Level.Info),
        var p => ($"policy {p}", Level.Plain),
    };
}

public static class Tracking
{
    /// <summary>The CLI's standby reason (tracking-provider-discovery §4, target_viewer_arbitrate).</summary>
    public const string StandbyReason = "display tracks the viewer on this screen";
    public const string NoSourceReason = "no source";

    /// <summary>The <c>viewer_source</c> string split: kind + provider id.</summary>
    public static (ViewerSourceKind Kind, string? ProviderId) ParseSource(string? source) => source switch
    {
        null or "" => (ViewerSourceKind.Unknown, null),
        "dp" => (ViewerSourceKind.Display, null),
        "nominal" => (ViewerSourceKind.Nominal, null),
        var s when s.StartsWith("provider:", StringComparison.Ordinal) && s.Length > "provider:".Length
            => (ViewerSourceKind.Provider, s["provider:".Length..]),
        _ => (ViewerSourceKind.Unknown, null),
    };

    /// <summary>
    /// One row per screen of the snapshot. The primary (runtime-default) screen falls back to
    /// <paramref name="summary"/>'s policy when the feed does not carry one (an older service).
    /// <paramref name="nameOf"/> maps a provider id to its display name (the policy rows').
    /// </summary>
    public static IReadOnlyList<TrackingScreen> Screens(StatusSnapshot? s, TrackingSummary? summary, Func<string, string>? nameOf = null)
    {
        var rows = new List<TrackingScreen>();
        foreach (var sc in s?.Screens ?? Array.Empty<Screen>())
        {
            var et = sc.EyeTracking;
            bool primary = sc.Roles.RuntimeDefault;
            string? policy = et.ViewerPolicy is { Length: > 0 } p ? p
                           : primary && summary is { DisplayViewerPolicy.Length: > 0 } sm ? sm.DisplayViewerPolicy : null;
            var (kind, id) = ParseSource(et.ViewerSource);
            string provider = id is null ? "" : nameOf?.Invoke(id) ?? id;
            var (source, reason, level) = Words(kind, provider, et.ViewerReason, et.State);
            rows.Add(new TrackingScreen(sc.Id, StatusText.ScreenName(sc), primary, kind, id, source, reason, et.ViewerReason,
                                        policy, level));
        }
        return rows;
    }

    /// <summary>
    /// The page's words for a screen's viewer source. The two "why no provider" cases are kept
    /// distinct on purpose: <b>standby</b> (the display tracks the viewer itself, so a provider is
    /// outranked and opens nothing) is not a fault; <b>no source</b> (EXTERNAL_OK, but no enabled
    /// provider found a usable source) is what a user fixes. A provider that is selected but has
    /// no lock right now (live <c>state</c> NOT_TRACKING from the service) reads "source paused".
    /// Any other reason is the CLI's, verbatim.
    /// </summary>
    public static (string Source, string Reason, Level Level) Words(ViewerSourceKind kind, string provider, string? reason, string state)
    {
        switch (kind)
        {
        case ViewerSourceKind.Provider:
            return state switch
            {
                "TRACKING" => ($"Provider: {provider}", "Tracking the viewer.", Level.Ok),
                "NOT_TRACKING" => ($"Provider: {provider}",
                    "Source paused: no viewer lock right now (no face, camera unplugged or provider stopped); the views hold the last known viewer.",
                    Level.Warn),
                _ => ($"Provider: {provider}", "Selected: starts when an app needs a viewer on this screen.", Level.Info),
            };
        case ViewerSourceKind.Display:
            return reason == StandbyReason
                ? ("Display", "Standby: the display tracks the viewer on this screen.", Level.Plain)
                : ("Display", Sentence(reason) ?? "The display owns the viewer on this screen.", Level.Plain);
        case ViewerSourceKind.Nominal:
            return reason == NoSourceReason
                ? ("Nominal viewer", "No source: no enabled provider found a usable source.", Level.Info)
                : ("Nominal viewer", Sentence(reason) ?? "The views stay on the nominal viewer.", Level.Plain);
        default:
            return ("Not reported", "Not evaluated: the viewer source is reported by a current CLI for the runtime's default screen.", Level.Plain);
        }
    }

    private static string? Sentence(string? s) =>
        s is not { Length: > 0 } ? null : char.ToUpperInvariant(s[0]) + s[1..] + (s.EndsWith('.') ? "" : ".");

    /// <summary>
    /// The tracking card's section: one row per registered provider (from <c>tracking list --json</c>),
    /// with its Enabled / Preferred / provenance chips and the probe result from <c>info --json</c>.
    /// Null when the CLI reports neither (it predates the class).
    /// </summary>
    public static ComponentSection? Section(StatusSnapshot? s, PolicyList? policy, TrackingSummary? summary)
    {
        if (policy is null && summary is null) return null;
        var items = new List<ComponentItem>();
        var screens = Screens(s, summary, id => policy?.Row(id)?.Name is { Length: > 0 } n ? n : id);
        foreach (var r in policy?.Rows ?? Array.Empty<PolicyRow>())
        {
            var chips = new List<(string, Level)>();
            bool isServing = screens.Any(x => x.Kind == ViewerSourceKind.Provider && x.ProviderId == r.Id);
            if (isServing) chips.Add(("serving", Level.Ok));
            if (summary?.ProbeChip(r) is { } pc) chips.Add(pc);
            else if (r.RankedFirst && r.Enabled) chips.Add(("ranked first", Level.Plain));
            Components.AddPolicyChips(chips, r);
            var lines = new List<string>
            {
                string.Join(StatusText.Sep, new[]
                {
                    r.Vendor, r.Version.Length > 0 ? "version " + r.Version : "", r.Id,
                    r.ProbeOrder is { } po ? $"ProbeOrder {po}" : "",
                }.Where(x => x.Length > 0)),
            };
            if (r.Provenance == "unverified" && r.ProvenanceReason is { Length: > 0 } why) lines.Add(why);
            items.Add(new ComponentItem(r.Name.Length > 0 ? r.Name : r.Id,
                isServing ? Level.Ok : r.Untrusted ? Level.Warn : Level.Plain, chips, lines, Policy: r));
        }
        int registered = policy?.Rows.Count ?? summary?.Registered ?? 0;
        int enabled = policy?.Rows.Count(r => r.Enabled) ?? summary?.Enabled ?? 0;
        int serving = screens.Count(x => x.Kind == ViewerSourceKind.Provider);
        string line = registered == 0 ? "none registered"
            : $"{StatusText.Plural(registered, "registered", "registered")}{StatusText.Sep}{enabled} enabled{StatusText.Sep}"
              + (serving > 0 ? $"serving {StatusText.Plural(serving, "screen", "screens")}" : "serving no screen");
        if (policy is { DisabledByEnv: true }) line += $"{StatusText.Sep}DXR_TRACKING_PROVIDERS=0 in the CLI's environment";
        return new ComponentSection(ComponentClass.Tracking.SectionId(), "Tracking providers",
            "A viewer-tracking provider measures where the viewer is from a sensor the display does not own (a webcam, a depth camera); "
            + "it serves only a screen whose display does not track the viewer itself and allows an external viewer.",
            registered, serving, line, items);
    }
}

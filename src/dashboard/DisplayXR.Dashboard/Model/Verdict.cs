// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;

namespace DisplayXR.Dashboard.Model;

// Home's verdict: one headline that answers "is my 3D display working?", one row per display with up
// to three facts, and a "Needs attention" list of plain sentences. Pure, from what the Dashboard
// already reads (the status snapshot, `info --json`, `selftest --json`); no check of its own.
//
// Honesty rule: a display has live state (3D on / off, sees you) only while a 3D app draws on it
// through the running service (`screens[].dps` non-empty). With no app running the snapshot carries
// none, so the two facts are "not known yet" and the headline never claims
// that 3D is on or that the display sees the viewer.

/// <summary>One fact's state: known yes, known no, or not known yet (no live state).</summary>
public enum Tick { Yes, No, NotKnownYet }

/// <summary>One of a display row's facts, in the words shown.</summary>
public sealed record VerdictFact(string Text, Tick Tick);

/// <summary>
/// One display on Home. <see cref="Is3D"/> false is an ordinary 2D display: no facts.
/// <see cref="Driver"/> is the display driver's own name (the id when it has none), <see cref="DriverId"/> its id.
/// </summary>
public sealed record VerdictRow(string ScreenId, string Name, string? Role, bool Is3D, string? Driver, string? DriverId,
                                string? Forced, IReadOnlyList<VerdictFact> Facts);

/// <summary>What a "Needs attention" sentence offers beside it. Only actions the Dashboard already has.</summary>
public enum AttentionAction { None, OpenDisplays, OpenComponents, ActivateRuntime, ShowChecks }

/// <summary>One plain sentence of "Needs attention"; <see cref="Technical"/> is the warning's code and text (tooltip).</summary>
public sealed record AttentionItem(string Text, Level Level, string? Technical = null, AttentionAction Action = AttentionAction.None);

public enum VerdictKind
{
    /// <summary>No snapshot yet.</summary>
    Checking,
    /// <summary>The snapshot lists no display at all.</summary>
    NoDisplay,
    /// <summary>Displays, none recognised as a 3D display.</summary>
    No3DDisplay,
    /// <summary>Something blocks 3D and no display is known to show 3D.</summary>
    Off,
    /// <summary>At least one 3D display is showing 3D now.</summary>
    Working,
    /// <summary>A 3D display is recognised, the service runs, no app shows 3D right now.</summary>
    Ready,
    /// <summary>A 3D display is recognised, the background service is not reached (no live state).</summary>
    NotRunning,
}

/// <param name="SimulatedDriverId">Set when the only display driver in use is the simulated one: its id, for the
/// "Try a 3D preview" link to its Details on Components. Null otherwise.</param>
public sealed record Verdict(VerdictKind Kind, string Headline, string Subline, Level Level,
                             IReadOnlyList<VerdictRow> Rows, IReadOnlyList<AttentionItem> Attention,
                             string? SimulatedDriverId = null);

public static class Verdicts
{
    private static string Sep => StatusText.Sep;

    /// <summary>"1 thing needs attention" / "2 things need attention" (a count of zero is never printed).</summary>
    public static string Things(int n, string verbOne, string verbMany) =>
        $"{StatusText.Plural(n, "thing", "things")} {(n == 1 ? verbOne : verbMany)}";

    /// <summary>
    /// The active OpenXR runtime as Home knows it: <c>info --json</c>'s resolved key when read, else
    /// the snapshot's. <c>Known</c> false when neither said (then nothing is flagged).
    /// </summary>
    public static (bool Known, string? Value) ActiveRuntime(InfoResult? info, StatusSnapshot? s) =>
        info is { ActiveRuntimeQueried: true } ? (true, info.ActiveRuntimeSet ? info.ActiveRuntimeValue : null)
        : s?.Runtime.ActiveOpenXrRuntime is { Length: > 0 } v ? (true, v)
        : (false, null);

    /// <summary>
    /// Whether display driver <paramref name="pluginId"/> is the simulated one: the snapshot marks it as the
    /// fallback (<c>plugins[].fallback</c>: the runtime's vendor-neutral simulated display), or, when the
    /// snapshot does not list it, its id is the simulated display's.
    /// </summary>
    public static bool IsSimulated(StatusSnapshot s, string? pluginId) =>
        pluginId is { Length: > 0 } id
        && (s.Plugins.FirstOrDefault(p => p.Id == id) is { } p ? p.Fallback : id == FriendlyOptions.SimDisplay);

    /// <summary>
    /// A display recognised as a 3D display: claimed by a display driver that is not the simulated one.
    /// The simulated driver claims every display (at the lowest confidence) so that apps run anywhere;
    /// that is not a 3D display. A display the simulated driver was CHOSEN for (a per-display override,
    /// <c>claim.forced</c>) still counts: someone picked it for a display DisplayXR found.
    /// </summary>
    public static bool IsReal3D(StatusSnapshot s, Screen x) =>
        x.Claim.IsClaimed && (x.Claim.Forced == true || !IsSimulated(s, x.Claim.PluginId));

    /// <summary>
    /// The simulated display driver's id when it is the only display driver in use (it claims a display or
    /// is the active one, and no other driver claims a display or is active); null otherwise.
    /// </summary>
    public static string? OnlySimulated(StatusSnapshot s)
    {
        if (s.Screens.Any(x => IsReal3D(s, x))) return null;
        if (s.Plugins.Any(p => p.Load == "ACTIVE" && !IsSimulated(s, p.Id))) return null;
        return s.Screens.Where(x => x.Claim.IsClaimed).Select(x => x.Claim.PluginId).FirstOrDefault()
               ?? s.Plugins.FirstOrDefault(p => p.Load == "ACTIVE")?.Id;
    }

    /// <summary>The line under "No 3D display found" on a PC whose only display driver is the simulated one.</summary>
    public const string SimulatedOnlyLine = "DisplayXR apps show in normal 2D on this PC. Nothing is wrong.";

    /// <summary>A display has live state only while an app draws on it through the running service.</summary>
    public static bool IsLive(StatusSnapshot s, Screen x) => s.IsService && x.Dps.Count > 0;

    public static Tick ThreeD(StatusSnapshot s, Screen x) =>
        !IsLive(s, x) ? Tick.NotKnownYet
        : StatusText.Is3DOn(x) switch { true => Tick.Yes, false => Tick.No, null => Tick.NotKnownYet };

    public static Tick SeesYou(StatusSnapshot s, Screen x) =>
        !IsLive(s, x) ? Tick.NotKnownYet
        : x.EyeTracking.State switch { "TRACKING" => Tick.Yes, "NOT_TRACKING" => Tick.No, _ => Tick.NotKnownYet };

    /// <summary>The three facts of a 3D display, in the words shown.</summary>
    public static IReadOnlyList<VerdictFact> Facts(StatusSnapshot s, Screen x) => new[]
    {
        new VerdictFact("3D display recognised", Tick.Yes),
        ThreeD(s, x) switch
        {
            Tick.Yes => new VerdictFact("3D on", Tick.Yes),
            Tick.No => new VerdictFact("3D off", Tick.No),
            _ => new VerdictFact("3D on: not known yet", Tick.NotKnownYet),
        },
        SeesYou(s, x) switch
        {
            Tick.Yes => new VerdictFact("Sees you", Tick.Yes),
            Tick.No => new VerdictFact("Doesn't see you right now", Tick.No),
            _ => new VerdictFact("Sees you: not known yet", Tick.NotKnownYet),
        },
    };

    /// <summary>The words Home uses for an ordinary display (not recognised as a 3D display).</summary>
    public const string Plain2D = "Ordinary 2D display";

    public static IReadOnlyList<VerdictRow> Rows(StatusSnapshot s, DpList? dp = null)
    {
        var rows = new List<VerdictRow>();
        foreach (var x in s.Screens)
        {
            string? role = x.Roles.OsMain ? "Main display" : null;
            string? id = dp?.Screen(x.Key)?.EffectivePlugin ?? (x.Claim.IsClaimed ? x.Claim.PluginId : null);
            string? name = id is null ? null : s.Plugins.FirstOrDefault(p => p.Id == id)?.Name is { Length: > 0 } n ? n : id;
            bool is3D = IsReal3D(s, x);
            rows.Add(new VerdictRow(x.Id, StatusText.ScreenName(x), role, is3D, is3D ? name : null, is3D ? id : null,
                                    is3D ? StatusText.ForcedChip(x, dp) : null,
                                    is3D ? Facts(s, x) : Array.Empty<VerdictFact>()));
        }
        return rows;
    }

    /// <summary>
    /// A runtime warning in plain words, naming the display it is about. The codes are the runtime's own
    /// (u_status_snapshot.h); any other code from the runtime gets a general sentence, and a display
    /// maker's own warning is quoted as theirs. The technical code and text stay in the tooltip.
    /// </summary>
    public static string PlainWarning(StatusWarning w, string? screen, bool fromDisplayMaker = false)
    {
        string on = screen is null ? "" : $" on {screen}";
        string it = screen ?? "A display";
        if (fromDisplayMaker)
            return w.Text.Length > 0 ? $"The display maker's software reports a problem{on}: {w.Text}"
                                     : $"The display maker's software reports a problem{on}.";
        return w.Code switch
        {
            "NOT_NATIVE" => $"{it} is not at its full resolution, so 3D can't be shown correctly. Set it to its recommended resolution in your system's display settings.",
            "PLUGIN_NOT_READY" => $"The display driver for {screen ?? "your 3D display"} isn't ready. It may need the display maker's software installed or running.",
            "CLAIM_FALLBACK" => $"{it} is shown with a generic display driver. Its maker's own driver may show 3D better; Components lists what you can add.",
            "NO_PHYSICAL_SIZE" => $"DisplayXR doesn't know the size of {screen ?? "a display"}, so depth in 3D may look wrong.",
            "SEGMENT_FLAT_2D" => $"Part of an app's window is{on}, which has no display driver, so that part shows flat 2D.",
            "TRACKER_DOWN" => $"The head tracking of {screen ?? "a display"} has stopped, so the picture can't follow you.",
            "DP_DEGRADED" => $"3D{on} is not working as well as it should.",
            "DP_STALE" => $"3D{on} has stopped updating.",
            "CONV_CRASH_LOOP" => "A 2D-to-3D converter keeps stopping. Components shows which one.",
            "CONV_TASK_UNSERVED" => "No 2D-to-3D converter can do one of its jobs right now. Components shows which one.",
            "CONV_RESEARCH_MODEL_ENABLED" => "A 2D-to-3D converter is using a model that is for research only.",
            _ => screen is null ? "DisplayXR reports a problem. Troubleshooting has the details." : $"DisplayXR reports a problem with {screen}. Displays has the details.",
        };
    }

    private static AttentionAction ActionOf(string code, bool onScreen) => code switch
    {
        "CLAIM_FALLBACK" or "CONV_CRASH_LOOP" or "CONV_TASK_UNSERVED" or "CONV_RESEARCH_MODEL_ENABLED" => AttentionAction.OpenComponents,
        _ => onScreen ? AttentionAction.OpenDisplays : AttentionAction.None,
    };

    /// <summary>
    /// "Needs attention": every warn / critical warning the snapshot carries (per display, the display
    /// maker's too, then system-wide), the background service when it is not reached, a failed
    /// "Check my setup", and another program set as the OpenXR runtime apps use. Nothing else.
    /// </summary>
    public static IReadOnlyList<AttentionItem> Attention(StatusSnapshot s, SelftestResult? selftest = null,
                                                         (bool Known, string? Value) activeRuntime = default)
    {
        var items = new List<AttentionItem>();
        foreach (var x in s.Screens)
        {
            string name = StatusText.ScreenName(x);
            foreach (var w in StatusText.ScreenWarnings(x).Where(w => w.Level != WarningLevel.Info))
            {
                bool vendor = ReferenceEquals(w, x.Vendor.WorstWarning);
                items.Add(new AttentionItem(PlainWarning(w, name, vendor), StatusText.ToLevel(w.Level),
                                            StatusText.WarningLine(w), ActionOf(w.Code, true)));
            }
        }
        foreach (var w in s.Warnings.Where(w => w.Level != WarningLevel.Info))
            items.Add(new AttentionItem(PlainWarning(w, null), StatusText.ToLevel(w.Level), StatusText.WarningLine(w), ActionOf(w.Code, false)));
        if (!s.IsService)
            items.Add(new AttentionItem(NotRunningSentence, Level.Info, StatusText.SourceLine(s)));
        if (activeRuntime.Known && !(activeRuntime.Value?.Contains("DisplayXR", StringComparison.OrdinalIgnoreCase) ?? false))
            items.Add(new AttentionItem(activeRuntime.Value is null ? RuntimeUnsetSentence : RuntimeOtherSentence, Level.Critical,
                                        activeRuntime.Value ?? "<unset>", AttentionAction.ActivateRuntime));
        if (selftest is { } st && st.Checks.Count(c => !c.Ok) is > 0 and var failed)
            items.Add(new AttentionItem($"Check my setup found {(failed == 1 ? "a problem" : $"{failed} problems")}. The list is under Check my setup.",
                                        Level.Critical, st.Verdict, AttentionAction.ShowChecks));
        return items;
    }

    public const string NotRunningSentence =
        "DisplayXR isn't running in the background, so live details are missing. It normally starts by itself when you sign in.";

    public const string RuntimeOtherSentence =
        "Another program is set as the 3D system that apps use, so 3D apps won't use DisplayXR.";

    public const string RuntimeUnsetSentence =
        "No 3D system is set for apps to use, so 3D apps won't find DisplayXR.";

    /// <summary>The whole verdict. <paramref name="s"/> null: still checking.</summary>
    public static Verdict From(StatusSnapshot? s, SelftestResult? selftest = null,
                               (bool Known, string? Value) activeRuntime = default, DpList? dp = null)
    {
        if (s is null)
            return new Verdict(VerdictKind.Checking, "Checking your 3D setup…", "This takes a moment.", Level.Info,
                               Array.Empty<VerdictRow>(), Array.Empty<AttentionItem>());
        var rows = Rows(s, dp);
        var attention = Attention(s, selftest, activeRuntime);
        int n = attention.Count;
        string more = n > 0 ? $"{Things(n, "needs", "need")} attention." : "";
        Level withMore(Level calm) => n == 0 ? calm : attention.Any(a => a.Level == Level.Critical) ? Level.Critical : Level.Warn;

        if (s.Screens.Count == 0)
            return new Verdict(VerdictKind.NoDisplay, "No display found",
                               Join("Check that your display is connected and awake.", more), Level.Warn, rows, attention);
        int claimed = s.Screens.Count(x => IsReal3D(s, x));
        if (claimed == 0)
        {
            // Only the simulated display driver: an ordinary PC, where apps show in 2D. Nothing to fix.
            if (OnlySimulated(s) is { } sim)
                return new Verdict(VerdictKind.No3DDisplay, "No 3D display found", Join(SimulatedOnlyLine, more),
                                   withMore(Level.Info), rows, attention, sim);
            return new Verdict(VerdictKind.No3DDisplay, "No 3D display found",
                               Join("None of your displays is recognised as a 3D display. Displays shows what DisplayXR found.", more),
                               Level.Warn, rows, attention);
        }

        int working = s.Screens.Count(x => IsReal3D(s, x) && ThreeD(s, x) == Tick.Yes);
        bool blocking = attention.Any(a => a.Level == Level.Critical);
        if (working > 0)
            return new Verdict(VerdictKind.Working, $"3D is working on {StatusText.Plural(working, "display", "displays")}", more,
                               withMore(Level.Ok), rows, attention);
        if (blocking)
            return new Verdict(VerdictKind.Off, $"3D is off: {Things(n, "needs", "need")} fixing",
                               "Each one is listed under Needs attention.", Level.Critical, rows, attention);
        if (!s.IsService)
            return new Verdict(VerdictKind.NotRunning, claimed == 1 ? "Your 3D display is recognised" : "Your 3D displays are recognised",
                               "Whether 3D is on is not known yet: DisplayXR isn't running in the background.", withMore(Level.Info), rows, attention);
        return new Verdict(VerdictKind.Ready, claimed == 1 ? "Your 3D display is ready" : "Your 3D displays are ready",
                           Join(StatusText.IdleLine, more), withMore(Level.Ok), rows, attention);
    }

    private static string Join(string a, string b) => b.Length == 0 ? a : $"{a} {b}";

    /// <summary>"Last checked: not yet" / "Last checked: 14:05" (this session only; nothing is saved).</summary>
    public static string LastChecked(SelftestResult? st, bool running) =>
        running ? "Checking now…" : st is null ? "Last checked: not yet" : $"Last checked: {st.When.ToString("HH:mm", CultureInfo.InvariantCulture)}";
}

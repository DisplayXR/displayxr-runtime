// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text;

namespace DisplayXR.Dashboard.Model;

/// <summary>A state, as every dot, chip and badge colours it.</summary>
public enum Level { Plain = 0, Ok = 1, Info = 2, Warn = 3, Critical = 4 }

/// <summary>A nav-rail badge: a count and the colour of its worst member.</summary>
public sealed record NavBadge(int Count, Level Level);

/// <summary>
/// The words of every status page, pure so the tests pin what the pages show;
/// the pages only lay these strings out (design §8).
/// </summary>
public static class StatusText
{
    public const string Dash = "—";
    public const string Times = "×";
    public const string Sep = " · ";

    private static string N(double v, string fmt = "0.##") => v.ToString(fmt, CultureInfo.InvariantCulture);

    public static Level ToLevel(WarningLevel w) => w switch
    {
        WarningLevel.Critical => Level.Critical,
        WarningLevel.Warn => Level.Warn,
        _ => Level.Info,
    };

    public static Level Max(Level a, Level b) => (Level)Math.Max((int)a, (int)b);

    // ── 2D-to-3D jobs: the one place a task id becomes words ────────────────

    /// <summary>
    /// The plain name of a conversion task, for every place a task is shown to a person. The feed
    /// carries only the id (<c>depth-video</c>, the word <c>displayxr-cli conv use</c> takes), so the
    /// id stays what commands and Details print. An id this list does not know is shown as it is.
    /// </summary>
    public static string ConversionTaskName(string taskId) => taskId switch
    {
        "depth-mono" => "depth from a photo",
        "depth-video" => "depth from video",
        "disparity-stereo" => "depth from a 3D photo or video",
        "views-mono" => "3D from 2D video and photos",
        "views-stereo" => "more views from a 3D photo or video",
        "views-from-depth" => "3D from a picture and its depth",
        "splats-mono" => "3D scene from a photo",
        "splats-stereo" => "3D scene from a 3D photo",
        _ => taskId,
    };

    /// <summary><see cref="ConversionTaskName"/> as the start of a line: a label in a row's Details.</summary>
    public static string ConversionTaskLabel(string taskId)
    {
        string name = ConversionTaskName(taskId);
        return name == taskId || name.Length == 0 ? name : char.ToUpperInvariant(name[0]) + name[1..];
    }

    /// <summary>The button that makes a converter the first choice for one task.</summary>
    public static string PreferTaskButton(string taskId) => $"Use this one for {ConversionTaskName(taskId)}";

    /// <summary>The chip on the converter the user chose for one task.</summary>
    public static string TaskChoiceChip(string taskId) => $"Your choice for {ConversionTaskName(taskId)}";

    /// <summary>The button that hands one task's choice back to DisplayXR.</summary>
    public static string ResetTaskButton(string taskId) => $"Choose automatically for {ConversionTaskName(taskId)}";

    // ── Screens ─────────────────────────────────────────────────────────────

    /// <summary>
    /// Every warning a screen row shows: the runtime's own, then the vendor's
    /// worst one (passed through verbatim under <c>vendor</c>, design §4),
    /// unless the runtime already carries the same code.
    /// </summary>
    public static IReadOnlyList<StatusWarning> ScreenWarnings(Screen s)
    {
        if (s.Vendor.WorstWarning is not { } vw || s.Warnings.Any(w => w.Code == vw.Code)) return s.Warnings;
        return s.Warnings.Append(vw).ToArray();
    }

    // ── Is this display a real 3D display? The one rule, shared by Home and Displays ──

    /// <summary>
    /// Whether display driver <paramref name="pluginId"/> is the simulated one: the snapshot marks it as the
    /// fallback (<c>plugins[].fallback</c>: the runtime's vendor-neutral simulated display), or, when the
    /// snapshot does not list it (or there is no snapshot), its id is the simulated display's.
    /// </summary>
    public static bool IsSimulatedDriver(StatusSnapshot? snap, string? pluginId) =>
        pluginId is { Length: > 0 } id
        && (snap?.Plugins.FirstOrDefault(p => p.Id == id) is { } p ? p.Fallback : id == FriendlyOptions.SimDisplay);

    /// <summary>The runtime's warnings that say a display got only the fallback driver because a vendor driver is unhealthy.</summary>
    public static bool IsFallbackWarning(string code) => code is "CLAIM_FALLBACK" or "PLUGIN_NOT_READY";

    /// <summary>
    /// A registered display driver that is not the simulated one failed to load or reports a platform state
    /// other than ready: the runtime's own proxy (u_status_snapshot.c <c>vendor_plugin_unhealthy</c>) for "a
    /// 3D display got only the fallback driver".
    /// </summary>
    public static bool VendorDriverUnhealthy(StatusSnapshot? snap) =>
        snap is not null && snap.Plugins.Any(p => !p.Fallback
            && (LoadIsFailure(p.Load) || p.PlatformState is not ("" or "UNKNOWN" or "READY")));

    // u_status_snapshot.c load_is_failure: every verdict but these is a failure.
    private static bool LoadIsFailure(string load) =>
        load is not ("" or "UNKNOWN" or "ACTIVE" or "LOADED" or "DECLINED" or "NOT_ATTEMPTED" or "NOT_LOADED" or "DISABLED");

    /// <summary>
    /// A display the simulated driver claims only because a vendor driver is unhealthy: the display (or the
    /// snapshot) carries CLAIM_FALLBACK / PLUGIN_NOT_READY, or a vendor driver failed / is not ready.
    /// </summary>
    public static bool FellBack(StatusSnapshot? snap, Screen s) =>
        s.Warnings.Any(w => IsFallbackWarning(w.Code))
        || (snap is not null && snap.Warnings.Any(w => IsFallbackWarning(w.Code)))
        || VendorDriverUnhealthy(snap);

    /// <summary>
    /// A display recognised as a 3D display: claimed by a display driver that is not the simulated one.
    /// The simulated driver claims every display (at the lowest confidence) so that apps run anywhere;
    /// that is not a 3D display. Two exceptions count as a 3D display: the simulated driver was CHOSEN
    /// for it (a per-display preference: <c>claim.forced</c>, or <c>claim.preferred_plugin</c> naming the
    /// simulated driver from any source, the environment included), and it holds the display only because
    /// a vendor driver is unhealthy (<see cref="FellBack"/>: a 3D display that needs attention, as before).
    /// A GLOBAL preference (<c>dp use</c> without <c>--screen</c>, or its environment pin: the snapshot's
    /// <c>dp_preference</c>) naming the simulated driver counts too, on a PC where a real display driver is
    /// installed and usable (<see cref="ChoseSimulatedGlobally"/>). A runtime without that key behaves as before.
    /// </summary>
    public static bool IsReal3D(StatusSnapshot? snap, Screen s) =>
        s.Claim.IsClaimed && (s.Claim.Forced == true || ChoseSimulated(snap, s)
                              || !IsSimulatedDriver(snap, s.Claim.PluginId) || FellBack(snap, s));

    /// <summary>
    /// The simulated driver was chosen for this display: a per-display preference names it (whatever its
    /// source), or the global preference does (<see cref="ChoseSimulatedGlobally"/>).
    /// </summary>
    public static bool ChoseSimulated(StatusSnapshot? snap, Screen s) =>
        (s.Claim.PreferredPlugin is { Length: > 0 } pp && IsSimulatedDriver(snap, pp)) || ChoseSimulatedGlobally(snap, s);

    /// <summary>
    /// The global preference (<c>dp_preference</c>, any source) names the simulated driver, the simulated
    /// driver holds this display, no per-display preference names another driver for it, and a real display
    /// driver is installed AND usable (<see cref="IsUsableRealDriver"/>: enabled, not failed or declined, its
    /// platform READY): a 3D display showing the simulated preview by the user's choice. A real driver that
    /// is disabled, or installed on a PC with no panel attached (NO_DISPLAY / PLATFORM_ABSENT / ...), or that
    /// never reported a platform state (UNKNOWN), does not count, so a 2D-only PC never reads "chosen by you".
    /// <para>
    /// PER PC, not per display: the status snapshot does not say which display a real driver would claim (its
    /// claim names only the winner; the per-display <c>candidates</c> are in <c>dp list --json</c>, not in the
    /// snapshot every caller shares). So on a PC with a usable real driver, every display the simulated driver
    /// holds under this preference counts, a second, ordinary monitor included.
    /// </para>
    /// </summary>
    public static bool ChoseSimulatedGlobally(StatusSnapshot? snap, Screen s) =>
        snap?.DpPreference is { IsSet: true } g && IsSimulatedDriver(snap, g.PluginId)
        && IsSimulatedDriver(snap, s.Claim.PluginId)
        && (s.Claim.PreferredPlugin is not { Length: > 0 } pp || IsSimulatedDriver(snap, pp))
        && snap.Plugins.Any(p => IsUsableRealDriver(snap, p));

    /// <summary>
    /// A display driver that is not the simulated one and could drive a panel now: enabled, its load not a
    /// failure, not DISABLED and not DECLINED ("not my platform"), and its platform state READY.
    /// </summary>
    public static bool IsUsableRealDriver(StatusSnapshot? snap, PluginInfo p) =>
        !p.Fallback && !IsSimulatedDriver(snap, p.Id) && p.Enabled
        && p.Load is not ("DISABLED" or "DECLINED") && !LoadIsFailure(p.Load)
        && p.PlatformState == "READY";

    /// <summary>Where the choice of the simulated driver is stored: the per-display one, else the global one.</summary>
    public static string? ChosenSource(StatusSnapshot? snap, Screen s) =>
        s.Claim.Forced == true || s.Claim.PreferredPlugin is { Length: > 0 } ? s.Claim.PreferredSource
        : ChoseSimulatedGlobally(snap, s) ? snap!.DpPreference!.Source : s.Claim.PreferredSource;

    /// <summary>Claimed only by the simulated display driver: an ordinary display that driver gives a 3D preview.</summary>
    public static bool IsSimulatedPreview(StatusSnapshot? snap, Screen s) => s.Claim.IsClaimed && !IsReal3D(snap, s);

    /// <summary>The state line of a display the simulated display driver gives a 3D preview (no live state).</summary>
    public const string SimulatedPreviewLine = "The simulated display driver gives it a 3D preview.";

    /// <summary>The row's status dot: the worst warn / critical, else ok when it is a real 3D display, else plain.</summary>
    public static Level ScreenLevel(Screen s, StatusSnapshot? snap = null)
    {
        var worst = ScreenWarnings(s).Select(w => w.Level).DefaultIfEmpty(WarningLevel.Info).Max();
        if (worst == WarningLevel.Critical) return Level.Critical;
        if (worst == WarningLevel.Warn) return Level.Warn;
        return IsReal3D(snap, s) ? Level.Ok : Level.Plain;
    }

    public static string ShortDevice(string device)
    {
        if (string.IsNullOrEmpty(device)) return "";
        int i = device.LastIndexOf('\\');
        return i >= 0 && i < device.Length - 1 ? device[(i + 1)..] : device;
    }

    public static string ScreenName(Screen s) =>
        s.FriendlyName.Trim().Length > 0 ? s.FriendlyName.Trim()
        : ShortDevice(s.DeviceName).Length > 0 ? ShortDevice(s.DeviceName)
        : s.Id.Length > 0 ? s.Id : $"Display {s.Index}";

    public static IEnumerable<string> Chips(Screen s)
    {
        if (s.Roles.OsMain) yield return "OS main";
        if (s.Roles.RuntimeDefault) yield return "runtime default";
        if (s.Roles.VendorPrimary) yield return "vendor primary";
    }

    /// <summary>The claim-confidence badge and its colour.</summary>
    public static (string Text, Level Level) Badge(Screen s)
    {
        if (!s.Claim.IsClaimed) return ("UNCLAIMED", Level.Plain);
        return s.Claim.Confidence switch
        {
            "VERIFIED" => ("VERIFIED", Level.Ok),
            "EDID" => ("EDID", Level.Info),
            "FALLBACK" => ("FALLBACK", Level.Warn),
            var other => (other, Level.Plain),
        };
    }

    // ── Plain words for a display card (the technical lines above stay for Copy and tooltips) ──

    /// <summary>The role chips in plain words (<see cref="Chips"/> keeps the technical ones for Copy).</summary>
    public static IEnumerable<string> PlainChips(Screen s)
    {
        if (s.Roles.OsMain) yield return "Main display";
        if (s.Roles.RuntimeDefault) yield return "Default for 3D apps";
        if (s.Roles.VendorPrimary) yield return "Main 3D display";
    }

    /// <summary>The badge in plain words: is this a 3D display, and how sure is DisplayXR.</summary>
    public static (string Text, Level Level) PlainBadge(Screen s, StatusSnapshot? snap = null)
    {
        if (!s.Claim.IsClaimed) return ("Not a 3D display", Level.Plain);
        if (IsSimulatedPreview(snap, s)) return (Verdicts.Plain2D, Level.Plain);
        return s.Claim.Confidence switch
        {
            "VERIFIED" => ("Confirmed", Level.Ok),
            "EDID" => ("Recognised by model", Level.Info),
            "FALLBACK" => ("Generic driver", Level.Warn),
            _ => ("Recognised", Level.Plain),
        };
    }

    /// <summary><c>3840×2160 at 60 Hz · 344×194 mm · scaling 250%</c>: the identity line without device names or positions.</summary>
    public static string PlainIdentity(Screen s)
    {
        var parts = new List<string>();
        var d = s.Desktop.Rect;
        string hz = Hz(s.Native.RefreshMhz);
        if (d.IsEmpty) parts.Add("not part of the desktop");
        else parts.Add(hz.Length > 0 ? $"{d.Width}{Times}{d.Height} at {hz}" : $"{d.Width}{Times}{d.Height}");
        if (s.PhysicalMm.Width > 0 && s.PhysicalMm.Height > 0)
            parts.Add($"{N(s.PhysicalMm.Width, "0")}{Times}{N(s.PhysicalMm.Height, "0")} mm");
        if (s.Desktop.Scale > 0 && Math.Abs(s.Desktop.Scale - 1) > 0.001)
            parts.Add($"scaling {N(s.Desktop.Scale * 100, "0")}%");
        return string.Join(Sep, parts);
    }

    /// <summary>
    /// <c>Recognised as a 3D display · display driver: Leia SR</c>. <paramref name="nameOf"/> maps a driver id to
    /// its own name (the snapshot's plug-in list); the id is used when it has none.
    /// </summary>
    public static string PlainClaimLine(Screen s, Func<string, string?>? nameOf = null, StatusSnapshot? snap = null)
    {
        if (!s.Claim.IsClaimed) return "Not recognised as a 3D display. No display driver shows 3D on it.";
        string id = s.Claim.PluginId!;
        string name = nameOf?.Invoke(id) is { Length: > 0 } n ? n : id;
        if (IsSimulatedPreview(snap, s)) return $"Not recognised as a 3D display{Sep}display driver: {name}, which gives it a 3D preview";
        string line = $"Recognised as a 3D display{Sep}display driver: {name}";
        if (s.Claim.Forced == true || ChoseSimulated(snap, s)) line += $"{Sep}{ChosenWord(ChosenSource(snap, s))}";
        return line;
    }

    /// <summary>A forced driver choice in plain words, by where the choice is stored.</summary>
    public static string ChosenWord(string? source) => source switch
    {
        "user" => "chosen by you",
        "machine" => "chosen for everyone on this PC",
        _ => "chosen by hand",
    };

    /// <summary>The tracking state in plain words; null when there is nothing to say (no driver, not reported).</summary>
    public static string? PlainTracking(string state) => state switch
    {
        "TRACKING" => "Sees you",
        "NOT_TRACKING" => "Doesn't see you right now",
        _ => null,
    };

    /// <summary>Whether 3D is on, from the display maker's lens state, else from the mode; null when neither says.</summary>
    public static bool? Is3DOn(Screen s) =>
        s.Vendor.Present && s.Vendor.Lens is "3D" ? true
        : s.Vendor.Present && s.Vendor.Lens is "2D" ? false
        : s.Mode?.Is3D;

    /// <summary><c>Sees you · 3D on · used by 1 app</c>: the state line in plain words (<see cref="StateLine"/> is the technical one).</summary>
    public static string PlainStateLine(Screen s, StatusSnapshot? snap = null)
    {
        var parts = new List<string>();
        if (PlainTracking(s.EyeTracking.State) is { } t) parts.Add(t);
        if (Is3DOn(s) is { } on) parts.Add(on ? "3D on" : "3D off");
        if (s.Dps.Count > 0) parts.Add($"used by {Plural(s.Dps.Count, "app", "apps")}");
        if (parts.Count > 0) return string.Join(Sep, parts);
        // No live state (no app draws on it): the same plain words as Home.
        return IsSimulatedPreview(snap, s) ? SimulatedPreviewLine : s.Claim.IsClaimed ? IdleLine : Verdicts.Plain2D;
    }

    /// <summary>A 3D display with no live state, on Home and on Displays.</summary>
    public const string IdleLine = "3D turns on when a 3D app runs.";

    /// <summary>The display maker's own status in plain words, or null when its software reports none.</summary>
    public static string? PlainVendorLine(Screen s)
    {
        var v = s.Vendor;
        if (!v.Present) return null;
        return "Display maker's software: " + string.Join(Sep, new[]
        {
            v.Ready ? "ready" : "not ready",
            v.Calibrated ? "calibrated" : "not calibrated",
        });
    }

    /// <summary>
    /// The Displays caption. Never a zero count; one display is said in words; whether a display sees you
    /// is "not known yet" while no app draws on it (no live state, <see cref="Verdicts.SeesYou"/>):
    /// <c>One display, recognised as 3D · whether it sees you is not known yet</c>,
    /// <c>2 displays · 2 recognised as 3D · 2 see you</c>.
    /// </summary>
    public static string DisplaysSummary(StatusSnapshot s)
    {
        var claimed = s.Screens.Where(x => IsReal3D(s, x)).ToList();
        if (s.Screens.Count == 0) return "No display found";
        if (s.Screens.Count == 1)
        {
            var one = s.Screens[0];
            if (!IsReal3D(s, one)) return "One display, not a 3D display";
            return "One display, recognised as 3D" + Sep + Verdicts.SeesYou(s, one) switch
            {
                Tick.Yes => "it sees you",
                Tick.No => "it doesn't see you right now",
                _ => "whether it sees you is not known yet",
            };
        }
        var parts = new List<string> { Plural(s.Screens.Count, "display", "displays") };
        if (claimed.Count == 0) { parts.Add("no 3D display recognised"); return string.Join(Sep, parts); }
        parts.Add($"{claimed.Count} recognised as 3D");
        int seeing = claimed.Count(x => Verdicts.SeesYou(s, x) == Tick.Yes);
        if (seeing > 0) parts.Add($"{seeing} {(seeing == 1 ? "sees" : "see")} you");
        else if (!claimed.Any(x => Verdicts.IsLive(s, x))) parts.Add($"whether {(claimed.Count == 1 ? "it sees" : "they see")} you is not known yet");
        return string.Join(Sep, parts);
    }

    public static string Scale(double scale) => $"{Times}{N(scale <= 0 ? 1 : scale)}";

    public static string Hz(long refreshMhz) => refreshMhz > 0 ? $"{N(refreshMhz / 1000.0, "0.##")} Hz" : "";

    /// <summary><c>\\.\DISPLAY1 · 3840×2160 @ 60 Hz · (0,0) · ×2.5 · 344×194 mm</c></summary>
    public static string Identity(Screen s)
    {
        var parts = new List<string>();
        if (s.DeviceName.Length > 0) parts.Add(s.DeviceName);
        var d = s.Desktop.Rect;
        string res = d.IsEmpty ? "not on the desktop" : $"{d.Width}{Times}{d.Height}";
        string hz = Hz(s.Native.RefreshMhz);
        parts.Add(hz.Length > 0 && !d.IsEmpty ? $"{res} @ {hz}" : res);
        if (!d.IsEmpty) parts.Add($"({d.Left},{d.Top})");
        parts.Add(Scale(s.Desktop.Scale));
        parts.Add(s.PhysicalMm.Width > 0 && s.PhysicalMm.Height > 0
            ? $"{N(s.PhysicalMm.Width, "0")}{Times}{N(s.PhysicalMm.Height, "0")} mm"
            : "size unknown");
        return string.Join(Sep, parts);
    }

    /// <summary><c>leia-sr · VERIFIED · QALA2137AL0011 · d3d11 d3d12 vk gl</c></summary>
    public static string ClaimLine(Screen s)
    {
        if (!s.Claim.IsClaimed) return "Not claimed by any display-processor plug-in";
        var parts = new List<string> { s.Claim.PluginId!, s.Claim.Confidence };
        if (s.Claim.Serial.Length > 0) parts.Add(s.Claim.Serial);
        parts.Add(s.Claim.Apis.Count > 0 ? string.Join(" ", s.Claim.Apis) : "no APIs");
        if (s.Claim.Forced == true) parts.Add($"forced{(s.Claim.PreferredSource is { Length: > 0 } src && src != "none" ? " by " + src : "")}");
        return string.Join(Sep, parts);
    }

    public static string TrackingWord(string state) => state switch
    {
        "TRACKING" => "tracking",
        "NOT_TRACKING" => "not tracking",
        "NO_DP" => "no DP bound",
        "UNKNOWN" => "tracking unknown",
        var other => other.ToLowerInvariant(),
    };

    public static string DpText(ScreenDp dp) => $"client {dp.ClientId} {dp.Api} {dp.Kind} {dp.Backend}";

    /// <summary><c>tracking · lens 3D · mode LeiaSR (2 views) · DP: client 3 d3d11 primary OK</c></summary>
    public static string StateLine(Screen s)
    {
        var parts = new List<string> { TrackingWord(s.EyeTracking.State) };
        if (s.Vendor.Present && s.Vendor.Lens != "UNKNOWN") parts.Add($"lens {s.Vendor.Lens}");
        if (s.Mode is { } m) parts.Add($"mode {(m.Name.Length > 0 ? m.Name : "#" + m.Index)} ({m.Views} view{(m.Views == 1 ? "" : "s")}{(m.Is3D ? ", 3D" : ", 2D")})");
        if (s.Dps.Count > 0) parts.Add((s.Dps.Count == 1 ? "DP: " : "DPs: ") + string.Join(", ", s.Dps.Select(DpText)));
        else if (s.EyeTracking.State != "NO_DP") parts.Add("no DP bound");
        if (s.EyeTracking.Supported.Count > 0) parts.Add($"eye tracking {string.Join("/", s.EyeTracking.Supported)}");
        return string.Join(Sep, parts);
    }

    /// <summary>The vendor cell, or null when the plug-in reports no vendor status for the screen.</summary>
    public static string? VendorLine(Screen s)
    {
        var v = s.Vendor;
        if (!v.Present) return null;
        var parts = new List<string>
        {
            v.Ready ? "ready" : "not ready",
            v.Verified ? "verified" : "not verified",
            v.Calibrated ? "calibrated" : "not calibrated",
            $"tracker {v.Tracker}",
        };
        if (v.Model.Length > 0) parts.Add($"model {v.Model}");
        if (v.Serial.Length > 0 && v.Serial != s.Claim.Serial) parts.Add(v.Serial);
        return "Vendor: " + string.Join(Sep, parts);
    }

    // ── Per-screen display-processor override (phase 7) ───────────────────

    public sealed record DpChoice(string? PluginId, string Label);

    /// <summary>
    /// The selector on a screen card: "Auto (effective)" then the plug-ins that
    /// CLAIMED that screen (<c>screens[].candidates</c>, at any confidence: the
    /// ones that can drive it), or every registered plug-in when the CLI does not
    /// list candidates (older CLI). The selection is the screen's preferred
    /// plug-in, else Auto; a preference outside the list is appended and named.
    /// Null — no row at all — when the CLI predates per-screen overrides: no
    /// <c>key</c> on the screen, or a <c>dp list --json</c> without
    /// <c>screens[]</c>. The second check is a safety interlock, not a nicety:
    /// a CLI that predates <c>--screen</c> ignores the flag and writes the
    /// GLOBAL PreferredPlugin override instead (seen on a phase-2 CLI).
    /// </summary>
    public static (IReadOnlyList<DpChoice> Items, int Selected)? DpSelector(Screen s, DpList? dp)
    {
        if (s.Key is null || dp is null || dp.Plugins.Count == 0 || dp.Screens is null) return null;
        var ds = dp.Screen(s.Key);
        string? effective = ds?.EffectivePlugin ?? s.Claim.PluginId;
        string? preferred = ds is not null ? ds.PreferredPlugin : s.Claim.PreferredPlugin;
        var items = new List<DpChoice> { new(null, $"Automatic ({effective ?? "none"})") };
        foreach (var id in DpOptionIds(ds, dp))
        {
            var row = dp.Plugins.FirstOrDefault(p => string.Equals(p.Id, id, StringComparison.OrdinalIgnoreCase));
            items.Add(new DpChoice(id, row is { Name.Length: > 0 } ? $"{row.Name} ({id})" : id));
        }
        int selected = 0;
        if (preferred is not null)
        {
            selected = items.FindIndex(i => string.Equals(i.PluginId, preferred, StringComparison.OrdinalIgnoreCase));
            if (selected < 0)
            {
                bool registered = dp.Plugins.Any(p => string.Equals(p.Id, preferred, StringComparison.OrdinalIgnoreCase));
                items.Add(new DpChoice(preferred, $"{preferred} ({(registered ? "does not drive this display" : "not installed")})"));
                selected = items.Count - 1;
            }
        }
        return (items, selected);
    }

    /// <summary>
    /// The plug-in ids a screen's selector offers: its claimants in source
    /// (ProbeOrder) order, or every registered plug-in without a candidate list.
    /// </summary>
    public static IReadOnlyList<string> DpOptionIds(DpScreen? ds, DpList dp) =>
        ds?.Candidates is { } cands
            ? cands.Select(c => c.PluginId).Distinct(StringComparer.OrdinalIgnoreCase).ToList()
            : dp.Plugins.Select(p => p.Id).ToList();

    /// <summary>
    /// The Developer page's machine-wide override notice: only when a global
    /// PreferredPlugin is set (<c>dp list --json</c> <c>preferred</c>); null otherwise.
    /// </summary>
    public static (string Title, string Text)? MachineOverrideNotice(DpList? dp) => dp?.Preferred is { Length: > 0 } id
        ? ($"One display driver is set for every display on this PC: {id}.",
           "You can choose a driver for each display on Displays. Clearing this setting needs administrator permission. The amber mark next to Home stands for this setting.")
        : null;

    /// <summary>The tooltip and accessible name of the amber mark on the Home rail entry (a machine-wide driver choice).</summary>
    public const string HomeBadgeTip = "One display driver is set for every display on this PC. Home says which, and how to clear it.";

    /// <summary>
    /// Whether a display card shows its driver selector on its face rather than behind Details: when
    /// more than one display driver can drive this display (its candidates in <c>dp list --json</c>),
    /// or when more than one 3D display is present. The generic fallback driver (the one "used when no
    /// other driver fits", <c>plugins[].fallback</c> in the snapshot) is not a real choice and is not
    /// counted. Otherwise there is nothing to choose between.
    /// </summary>
    public static bool DriverChoiceOnFace(Screen s, StatusSnapshot snap, DpList? dp)
    {
        if (Real3D(snap) > 1) return true;
        var fallback = snap.Plugins.Where(p => p.Fallback).Select(p => p.Id).ToHashSet(StringComparer.OrdinalIgnoreCase);
        int real = dp?.Screen(s.Key)?.Candidates?.Select(c => c.PluginId).Where(id => !fallback.Contains(id))
                     .Distinct(StringComparer.OrdinalIgnoreCase).Count() ?? 0;
        return real > 1;
    }

    /// <summary>ADR-066 Part B: the three-way viewer-tracking selector, in order, with the CLI value each writes.</summary>
    public static readonly IReadOnlyList<(string Label, string Value)> TrackingChoices = new[]
    {
        ("Off", "off"),
        ("In 3D only", "3d"),
        ("Always (2D and 3D)", "always"),
    };

    /// <summary>The selector index a screen's setting shows: what it resolved to ("default" reads as its effect).</summary>
    public static int TrackingSelected(ViewerTracking vt)
    {
        string v = vt.Setting is "off" or "3d" or "always" ? vt.Setting : vt.Effective;
        for (int i = 0; i < TrackingChoices.Count; i++)
            if (TrackingChoices[i].Value == v) return i;
        return 0;
    }

    /// <summary>The CLI verb a viewer-tracking choice runs (per user, as every per-screen choice here).</summary>
    public static CliArgs TrackingVerb(string key, int index) =>
        new("tracking", "set", "--screen", key, TrackingChoices[index].Value);

    /// <summary>The line under the selector: why it is greyed out, or what it tracks from.</summary>
    public static string? TrackingNote(ViewerTracking vt) => vt.Available
        ? (vt.Detail is { Length: > 0 } d ? $"Uses {d}." : null)
        : (vt.Reason is { Length: > 0 } r ? $"Not available: {r}." : "Not available on this display.");

    /// <summary>The CLI verb a selector choice runs.</summary>
    public static CliArgs DpVerb(string key, DpChoice choice) => choice.PluginId is null
        ? new CliArgs("dp", "reset", "--screen", key)
        : new CliArgs("dp", "use", choice.PluginId, "--screen", key);

    /// <summary>
    /// "Chosen by you" when an override forces this screen's claim, or when the global preference chose the
    /// simulated driver for it (<paramref name="snap"/> given; <see cref="ChoseSimulatedGlobally"/>).
    /// </summary>
    public static string? ForcedChip(Screen s, DpList? dp, StatusSnapshot? snap = null)
    {
        var ds = dp?.Screen(s.Key);
        bool forced = ds?.Forced ?? s.Claim.Forced ?? false;
        bool global = !forced && ChoseSimulatedGlobally(snap, s);
        if (!forced && !global) return null;
        string? src = global ? snap!.DpPreference!.Source : ds?.PreferredSource ?? s.Claim.PreferredSource;
        return src is { Length: > 0 } && src != "none" ? char.ToUpperInvariant(ChosenWord(src)[0]) + ChosenWord(src)[1..] : "Chosen by hand";
    }

    public static string? ApplyLabel(Screen s, DpList? dp) => (dp?.Screen(s.Key)?.Apply ?? s.Claim.Apply) switch
    {
        "live" => "applies now",
        "next-session" or "next_session" => "applies when 3D apps next start",
        null => null,
        var other => other,
    };

    // ── Per-screen 3D mode ────────────────────────────────────────────────

    /// <summary>What the 3D mode picker does, in plain words.</summary>
    public const string Mode3DExplanation =
        "How this display shows a 3D app. Most changes apply right away, also to apps that are already running. " +
        "Some apply the next time an app starts.";

    /// <summary>
    /// Whether a display card shows the 3D mode picker: only when the feed reports the screen's 3D modes
    /// and there are at least two (one mode, or a feed that says <c>null</c>, is nothing to choose).
    /// </summary>
    public static bool Mode3DPickerShown(Screen s) => s.Key is not null && s.Mode3D is { Choices.Count: >= 2 };

    /// <summary>
    /// A stored choice on a display whose driver reports no 3D modes now: no picker, but the row stays,
    /// with the stored choice and the reason, so the choice can be seen and cleared.
    /// </summary>
    public static bool Mode3DStoredOnly(Screen s) =>
        s.Key is not null && s.Mode3D is { Choices.Count: 0, Setting: { Length: > 0 } };

    /// <summary>Whether a display card shows the 3D mode row at all (the picker, or a stored choice alone).</summary>
    public static bool Mode3DRowShown(Screen s) => Mode3DPickerShown(s) || Mode3DStoredOnly(s);

    /// <summary>
    /// The entries the picker lists, in the driver's order and by the driver's labels. A mode this display
    /// cannot show is left out, so it cannot be picked (<see cref="Mode3DNotes"/> names it and says why),
    /// unless it is the one drawn now. A mode that applies at the next app start is listed and can be picked.
    /// </summary>
    public static IReadOnlyList<Mode3DChoice> Mode3DItems(Mode3D m) =>
        m.Choices.Where(c => !c.NotSupported || c.Name == m.Active).ToList();

    /// <summary>
    /// What the picker shows for an entry: the driver's label, and for a mode that waits for an app start
    /// (not the one drawn now) the plain fact that it does.
    /// </summary>
    public static string Mode3DItemText(Mode3D m, Mode3DChoice c) =>
        c.NeedsRestart && c.Name != m.Active ? $"{c.Label} (applies the next time an app starts)" : c.Label;

    /// <summary>
    /// The picker's selected entry: the stored choice when it is listed (it is what was picked, also while
    /// it waits for the next app start), else the mode drawn now, else none (-1).
    /// </summary>
    public static int Mode3DSelected(Mode3D m)
    {
        var items = Mode3DItems(m);
        foreach (var name in new[] { m.Setting, m.Active })
            for (int i = 0; name is not null && i < items.Count; i++)
                if (items[i].Name == name) return i;
        return -1;
    }

    /// <summary>The CLI verb a picker choice runs (per user, as every per-screen choice here).</summary>
    public static CliArgs Mode3DVerb(string key, Mode3DChoice choice) => new("dp", "mode", "set", choice.Name, "--screen", key);

    /// <summary>The CLI verb "Reset to default" runs: clears this user's choice for the screen.</summary>
    public static CliArgs Mode3DResetVerb(string key) => new("dp", "mode", "reset", "--screen", key);

    /// <summary>
    /// Whether "Reset to default" is offered: only for this user's own choice. A choice made for everyone
    /// on the PC is not cleared by the per-user verb, so no reset is offered for it.
    /// </summary>
    public static bool Mode3DCanReset(Mode3D m) => m.Source == "user";

    /// <summary>Next to the picker: who chose the mode; null when it is a default.</summary>
    public static string? Mode3DSourceText(Mode3D m) => m.Source switch
    {
        "user" => "Chosen by you",
        "machine" when m.Choices.Count < 2 => "Chosen for everyone on this PC.",
        "machine" => "Chosen for everyone on this PC. Choosing a mode here is for you only, and yours is used.",
        _ => null,
    };

    /// <summary>The stored choice, as shown when there is no picker: the driver's label when known, else the stored name.</summary>
    public static string Mode3DStoredText(Mode3D m) => m.Find(m.Setting)?.Label ?? m.Setting ?? "";

    /// <summary>
    /// The lines under the row: a stored choice that waits for the next app start; the note of the mode
    /// drawn now; a stored choice that is not used; the modes this display cannot show, each with its
    /// reason; and, with no modes at all, why there are none.
    /// </summary>
    public static IReadOnlyList<string> Mode3DNotes(Mode3D m)
    {
        var notes = new List<string>();
        var stored = m.Find(m.Setting);
        var active = m.Find(m.Active);
        bool waiting = stored is not null && stored.Name != m.Active;
        if (waiting && stored!.NeedsRestart)
            notes.Add(active is null
                ? $"{stored.Label} is stored. It applies the next time an app starts."
                : $"{stored.Label} is stored. It applies the next time an app starts. Apps that are running keep {active.Label}.");
        else if (waiting && stored!.NotSupported)
            notes.Add(stored.Reason is { Length: > 0 } why
                ? $"{stored.Label} is stored, but this display cannot show it: {why}."
                : $"{stored.Label} is stored, but this display cannot show it.");
        else if (!waiting && active is { Reason: { Length: > 0 } note })
            notes.Add(Sentence(note));
        if (m.Setting is { Length: > 0 } name && stored is null && m.Choices.Count > 0)
            notes.Add($"The stored choice \"{name}\" is not one of this display's 3D modes and is not used.");
        var missing = m.Choices.Where(c => c.NotSupported && c.Name != m.Active && c.Name != m.Setting)
            .Select(c => c.Reason is { Length: > 0 } r ? $"{c.Label} ({r})" : c.Label).ToList();
        if (missing.Count > 0) notes.Add($"Not available on this display: {string.Join("; ", missing)}.");
        if (m.Choices.Count == 0 && m.Reason is { Length: > 0 } none) notes.Add(Sentence(none));
        return notes;
    }

    /// <summary>Whether a display driver drives a display that shows the 3D mode picker.</summary>
    public static bool DriverHasMode3DPicker(StatusSnapshot? snap, string driverId) =>
        snap is not null && snap.Screens.Any(s => Mode3DPickerShown(s) &&
                                                 string.Equals(s.Claim.PluginId, driverId, StringComparison.OrdinalIgnoreCase));

    /// <summary>What a display card's rebuild key reads of a screen's 3D mode.</summary>
    public static string Mode3DKey(Mode3D? m) => m is null
        ? "-"
        : $"{m.Active}/{m.Setting}/{m.Source}/{m.Reason}/{string.Join(",", m.Choices.Select(c => $"{c.Name}:{c.Label}:{c.Applies}:{c.Reason}"))}";

    private static string Sentence(string text)
    {
        string t = text.Trim();
        if (t.Length == 0) return t;
        t = char.ToUpperInvariant(t[0]) + t[1..];
        return t[^1] is '.' or '!' or '?' ? t : t + ".";
    }

    public static string WarningLine(StatusWarning w) => w.Text.Length > 0 ? $"{w.Code}: {w.Text}" : w.Code;

    public static int Claimed(StatusSnapshot s) => s.Screens.Count(x => x.Claim.IsClaimed);
    /// <summary>How many displays are real 3D displays (<see cref="IsReal3D"/>).</summary>
    public static int Real3D(StatusSnapshot s) => s.Screens.Count(x => IsReal3D(s, x));
    public static int Verified(StatusSnapshot s) => s.Screens.Count(x => x.Claim.IsClaimed && x.Claim.Confidence == "VERIFIED");
    public static int Tracking(StatusSnapshot s) => s.Screens.Count(x => x.EyeTracking.State == "TRACKING");

    public static string Plural(int n, string one, string many) => $"{n} {(n == 1 ? one : many)}";

    /// <summary><c>2 monitors · 2 claimed (2 verified) · 2 tracking</c></summary>
    public static string DisplaysHeader(StatusSnapshot s) =>
        $"{Plural(s.Screens.Count, "monitor", "monitors")}{Sep}{Claimed(s)} claimed ({Verified(s)} verified){Sep}{Tracking(s)} tracking";

    /// <summary>Every warn / critical the snapshot carries: per screen (vendor included) and system-wide.</summary>
    public static IEnumerable<StatusWarning> ActionableWarnings(StatusSnapshot s) =>
        s.Screens.SelectMany(ScreenWarnings).Concat(s.Warnings).Where(w => w.Level != WarningLevel.Info);

    /// <summary>The Displays nav badge: warn / critical only (design §8 feed discipline).</summary>
    public static NavBadge? DisplaysBadge(StatusSnapshot? s)
    {
        if (s is null) return null;
        var list = ActionableWarnings(s).ToList();
        if (list.Count == 0) return null;
        return new NavBadge(list.Count, list.Any(w => w.Level == WarningLevel.Critical) ? Level.Critical : Level.Warn);
    }

    /// <summary>The Windows nav badge: how many client apps (diagnostics clients excluded).</summary>
    public static NavBadge? WindowsBadge(StatusSnapshot? s)
    {
        if (s is null) return null;
        var apps = s.Clients.Where(c => !c.IsDiag).ToList();
        if (apps.Count == 0) return null;
        bool flat = apps.Any(c => c.Segments.Items.Any(i => !i.HasDp));
        return new NavBadge(apps.Count, flat ? Level.Warn : Level.Plain);
    }

    /// <summary>
    /// <c>Display driver for every display: sim-display (user)</c> when a global preference is set
    /// (<c>dp_preference</c>); null when none is, or the runtime predates the key.
    /// </summary>
    public static string? GlobalPreferenceLine(StatusSnapshot s) => s.DpPreference is { IsSet: true } g
        ? $"Display driver for every display: {g.PluginId}{(g.Source is { Length: > 0 } src ? $" ({src})" : "")}"
        : null;

    public static string Copy(StatusSnapshot s, bool includeSource = true)
    {
        var sb = new StringBuilder();
        sb.Append("Displays: ").Append(DisplaysHeader(s)).Append('\n');
        if (includeSource) sb.Append("Source: ").Append(SourceLine(s)).Append('\n');
        if (GlobalPreferenceLine(s) is { } gp) sb.Append(gp).Append('\n');
        for (int i = 0; i < s.Screens.Count; i++)
        {
            var x = s.Screens[i];
            string chips = string.Join(", ", Chips(x));
            sb.Append($"{i + 1}. {ScreenName(x)}{(chips.Length > 0 ? $" [{chips}]" : "")} - {Badge(x).Text}\n");
            sb.Append("   ").Append(Identity(x)).Append('\n');
            sb.Append("   ").Append(ClaimLine(x)).Append('\n');
            sb.Append("   ").Append(StateLine(x)).Append('\n');
            if (VendorLine(x) is { } v) sb.Append("   ").Append(v).Append('\n');
            foreach (var w in ScreenWarnings(x))
                sb.Append($"   ({w.Level.ToString().ToLowerInvariant()}) {WarningLine(w)}\n");
        }
        foreach (var w in s.Warnings)
            sb.Append($"({w.Level.ToString().ToLowerInvariant()}) {WarningLine(w)}\n");
        return sb.ToString();
    }

    /// <summary>The banner shown while the background service cannot be reached (a headless snapshot).</summary>
    public const string NotRunningTitle = "DisplayXR isn't running in the background";

    /// <summary>
    /// What the not-running banner can honestly say: what the pages show and what is missing. It does not
    /// promise that 3D apps work meanwhile (the Dashboard cannot know that).
    /// </summary>
    public const string NotRunningText =
        "The pages show what a 3D app starting now would find. Live details, such as running apps and whether a display sees you, appear when it runs. The Dashboard keeps trying to reach it.";

    /// <summary>"Check my setup"'s verdict in plain words (the CLI's own word is the badge's tooltip).</summary>
    public static string SelftestVerdict(int checks, int failed) =>
        failed == 0 ? "Everything checked is fine"
        : $"{failed} of {Plural(checks, "check", "checks")} found a problem";

    /// <summary>A display driver's platform state in plain words; null when it is ready (nothing to say).</summary>
    public static string? PlainPlatform(string state) => state switch
    {
        "READY" or "UNKNOWN" or "" => null,
        "PLATFORM_ABSENT" => "Needs the display maker's software",
        "NO_DISPLAY" => "Its display is not connected",
        var other => $"Not ready ({other})",
    };

    /// <summary>The connection pill in plain words; <see cref="SourceLine"/> is the technical one (tooltip, Copy).</summary>
    public static string PlainSourceLine(StatusSnapshot s) =>
        s.IsService ? "DisplayXR is running" : "Not running in the background";

    public static string SourceLine(StatusSnapshot s) =>
        s.IsService ? $"source: service{Sep}gen {s.Generation.Topology}/{s.Generation.Status}" : $"source: {s.Source}";

    // ── Plug-ins ────────────────────────────────────────────────────────────

    /// <summary>
    /// A platform remedy's chip (ADR-045 Amendment 2026-11-22): "Update required" / "Update
    /// available"; null when there is none. Advisory, so it never changes the plug-in's own level.
    /// </summary>
    public static (string Text, Level Level)? RemedyChip(PlatformRemedy r) =>
        !r.Any ? null : r.UpdateRequired ? ("Update required", Level.Warn) : ("Update available", Level.Info);

    /// <summary>
    /// The remedy in plain words, the plug-in's version strings verbatim, empty parts left out:
    /// "Version 1.2.0 is installed; 1.4.0 is required." Null when there is no remedy.
    /// </summary>
    public static string? RemedyLine(PlatformRemedy r)
    {
        if (!r.Any) return null;
        var verb = r.UpdateRequired ? "is required" : "is available";
        var from = r.InstalledVersion.Length > 0 ? $"Version {r.InstalledVersion} is installed" : null;
        var to = r.RequiredVersion.Length > 0 ? $"{r.RequiredVersion} {verb}" : null;
        return (from, to) switch
        {
            ({ } f, { } t) => $"{f}; {t}.",
            ({ } f, null) => $"{f}; an update {verb}.",
            (null, { } t) => $"Version {t}.",
            _ => $"An update for the display maker's software {verb}.",
        };
    }

    public static Level PluginLevel(PluginInfo p) => p.PlatformState switch
    {
        "READY" => p.Load == "ACTIVE" ? Level.Ok : Level.Plain,
        "UNKNOWN" => Level.Plain,
        "PLATFORM_ABSENT" or "NO_DISPLAY" => Level.Warn,
        _ => Level.Critical,
    };

    // ── Clients ─────────────────────────────────────────────────────────────

    public static string ClientName(Client c) => c.Name.Length > 0 ? c.Name : $"client {c.Id}";

    public static IEnumerable<string> ClientFlags(Client c)
    {
        if (c.Flags.Focused) yield return "focused";
        if (c.Flags.Active) yield return "active";
        if (c.Flags.Visible) yield return "visible";
        if (c.Flags.Overlay) yield return "overlay";
    }

    public static string WindowText(PixelRect? w) =>
        w is { } r ? $"{r.Left},{r.Top} {r.Width}{Times}{r.Height}" : "no window";

    public static string ScreenRef(StatusSnapshot s, string? id)
    {
        if (id is null) return Dash;
        var sc = s.ScreenById(id);
        if (sc is null) return id;
        string dev = ShortDevice(sc.DeviceName);
        string name = ScreenName(sc);
        return dev.Length > 0 && dev != name ? $"{dev} ({name})" : name;
    }

    public static string CanvasText(CanvasRect c) => $"{c.X},{c.Y} {c.W}{Times}{c.H}";

    /// <summary>The #1248 integrity triple, never a rate without it.</summary>
    public static string IntegrityLine(Client c) =>
        $"paint {c.Integrity.Paint}{Sep}present {c.Integrity.Present}{Sep}skip {c.Integrity.Skip}{Sep}weave {c.Integrity.WeavePlacement ?? Dash}";

    public static string ViewsLine(Client c) =>
        $"views {c.Views.Active}/{c.Views.Capacity} (reported {c.Views.Reported})";

    /// <summary>
    /// The text a flat-2D segment shows: the screen row's own
    /// <c>SEGMENT_FLAT_2D</c> warning when it carries one, so both pages say
    /// the same thing (design §8 Windows).
    /// </summary>
    public static string FlatSegmentText(StatusSnapshot s, Segment seg)
    {
        var w = s.ScreenById(seg.Screen)?.Warnings.FirstOrDefault(x => x.Code == "SEGMENT_FLAT_2D");
        return w is not null
            ? WarningLine(w)
            : "SEGMENT_FLAT_2D: this part of the window is on a display with no display driver, so it shows flat 2D.";
    }

    // ── Developer settings (perf-settings Phase 2A) ─────────────────────────

    public const string DevSettingsTitle = "Developer settings";

    public const string DevSettingsCollapsed =
        "Every Tier 1 and Tier 2 runtime lever with its current value, where it came from and its compiled default. Read-only; collapsed every time the dashboard starts.";

    public const string DevSettingsIntro =
        "Values are what a process starting now would resolve (this dashboard's CLI environment, then the per-user file, then the machine default). " +
        "Tier 1 levers apply the next time an app (or the service) starts; Tier 2 levers are read by the service when it starts. " +
        "Tier 4 debug and test switches are intentionally not listed.";

    public const string DevSettingsEnvOnlyNote =
        "Most levers are environment-only: the runtime reads them with its own getenv, so a stored value would do nothing. Only the levers marked “settable” resolve from the settings file.";

    /// <summary>The section's header caption: how many levers are not on their compiled default.</summary>
    public static string DevSettingsCount(DevLeverList? list)
    {
        if (list is null) return "";
        int n = list.NonDefaultCount;
        return n == 0 ? $"{list.Levers.Count} levers, all on their defaults"
                      : $"{n} of {list.Levers.Count} levers non-default";
    }

    public static string DevShownText(int shown, int total) =>
        shown == total ? $"{total} levers" : $"{shown} of {total} levers shown";

    public const string DevResetAllNote =
        "Reset all clears every lever in the per-user settings file, including the Performance page's. Environment variables and the machine default are untouched.";

    /// <summary>
    /// The Settings page's name for a setting it writes (<c>perf</c> levers), so its "changed from the
    /// defaults" notice names no variable; null for a lever Settings does not show.
    /// </summary>
    public static string? PlainSettingName(string lever) => lever switch
    {
        "DXR_D3D_FORCE_GPU" or "DXR_VK_FORCE_GPU" => "Graphics card for 3D apps",
        "DXR_WEAVE_ON_SCANOUT" or "DXR_WEAVE_REPAINT" => "Compatibility mode",
        "DXR_FRAME_WITNESS" or "DXR_FRAME_STAGE_TIMING" => "Detailed logging",
        _ => null,
    };

    /// <summary>The changed settings in plain words, once each, in the order Settings shows them ("other settings" for the rest).</summary>
    public static IReadOnlyList<string> ChangedSettings(PerfState perf)
    {
        var names = perf.Levers.Where(l => l.IsSet).Select(l => PlainSettingName(l.Name) ?? "Other settings").Distinct().ToList();
        string[] order = { "Graphics card for 3D apps", "Compatibility mode", "Detailed logging", "Other settings" };
        return names.OrderBy(n => Array.IndexOf(order, n)).ToList();
    }

    /// <summary>The chip colour of a value's source.</summary>
    public static Level DevSourceLevel(string source) => source switch
    {
        "env" => Level.Warn,
        "user" or "machine" => Level.Info,
        _ => Level.Plain,
    };

    public static string DevTierLabel(int tier) => tier switch
    {
        1 => "Tier 1 · next launch",
        2 => "Tier 2 · service start",
        _ => "Tier ?",
    };

    /// <summary>What a non-managed row says instead of a Reset button.</summary>
    public static string DevEnvOnlyHint(DevLever l)
    {
        string who = l.ForService && !l.ForApp ? "the service" : "the app";
        return $"environment only — set {l.Name} before launching {who}";
    }

    /// <summary>The switch button's label: what it does to the lever.</summary>
    public static string DevSwitchLabel(DevLever l) => l.SwitchValue == "0" ? "Turn off" : "Turn on";

    /// <summary>When a switched lever takes effect, by the process that reads it.</summary>
    public static string DevSwitchNote(DevLever l) => l.ForService
        ? "applies to apps started next; restart the service for workspace apps"
        : "applies to apps started next";

    /// <summary>Why a managed row's Reset is unavailable, or null when it can reset.</summary>
    public static string? DevResetBlocked(DevLever l) => l.Source switch
    {
        "user" => null,
        "env" => "set by an environment variable, which outranks the settings file",
        "machine" => @"machine-wide default (HKLM\Software\DisplayXR\Settings), set by an administrator or installer",
        _ => "on its default",
    };
}

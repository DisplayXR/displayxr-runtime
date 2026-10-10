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

    /// <summary>The row's status dot: the worst warn / critical, else ok when claimed, else plain.</summary>
    public static Level ScreenLevel(Screen s)
    {
        var worst = ScreenWarnings(s).Select(w => w.Level).DefaultIfEmpty(WarningLevel.Info).Max();
        if (worst == WarningLevel.Critical) return Level.Critical;
        if (worst == WarningLevel.Warn) return Level.Warn;
        return s.Claim.IsClaimed ? Level.Ok : Level.Plain;
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
        : s.Id.Length > 0 ? s.Id : $"Screen {s.Index}";

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
        var items = new List<DpChoice> { new(null, $"Auto ({effective ?? "none"})") };
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
                items.Add(new DpChoice(preferred, $"{preferred} ({(registered ? "no claim on this screen" : "not registered")})"));
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
        ? ($"A machine-wide display-processor override is set: {id} (applies to every screen).",
           "Choose per screen on Displays. Clearing it needs administrator rights.")
        : null;

    public sealed record HomeScreenDp(string Name, string? Plugin, string? Forced);

    /// <summary>
    /// Home's per-screen summary: each screen's effective display processor
    /// (dp list's resolve when read, else the snapshot's claim) and its forced chip.
    /// </summary>
    public static IReadOnlyList<HomeScreenDp> HomeScreenDps(StatusSnapshot s, DpList? dp) =>
        s.Screens.Select(x => new HomeScreenDp(ScreenName(x),
            dp?.Screen(x.Key)?.EffectivePlugin ?? (x.Claim.IsClaimed ? x.Claim.PluginId : null),
            ForcedChip(x, dp))).ToList();

    public static string Quote(string arg) => arg.Length == 0 || arg.Any(c => char.IsWhiteSpace(c) || c == '"')
        ? "\"" + arg.Replace("\"", "\\\"") + "\"" : arg;

    /// <summary>The CLI verb a selector choice runs.</summary>
    public static string DpVerb(string key, DpChoice choice) => choice.PluginId is null
        ? $"dp reset --screen {Quote(key)}"
        : $"dp use {Quote(choice.PluginId)} --screen {Quote(key)}";

    /// <summary>"forced (user)" when an override forces this screen's claim.</summary>
    public static string? ForcedChip(Screen s, DpList? dp)
    {
        var ds = dp?.Screen(s.Key);
        bool forced = ds?.Forced ?? s.Claim.Forced ?? false;
        if (!forced) return null;
        string? src = ds?.PreferredSource ?? s.Claim.PreferredSource;
        return src is { Length: > 0 } && src != "none" ? $"forced ({src})" : "forced";
    }

    public static string? ApplyLabel(Screen s, DpList? dp) => (dp?.Screen(s.Key)?.Apply ?? s.Claim.Apply) switch
    {
        "live" => "applies live",
        "next-session" or "next_session" => "applies to new sessions (service restart / app relaunch)",
        null => null,
        var other => other,
    };

    public static string WarningLine(StatusWarning w) => w.Text.Length > 0 ? $"{w.Code}: {w.Text}" : w.Code;

    public static int Claimed(StatusSnapshot s) => s.Screens.Count(x => x.Claim.IsClaimed);
    public static int Verified(StatusSnapshot s) => s.Screens.Count(x => x.Claim.IsClaimed && x.Claim.Confidence == "VERIFIED");
    public static int Tracking(StatusSnapshot s) => s.Screens.Count(x => x.EyeTracking.State == "TRACKING");

    public static string Plural(int n, string one, string many) => $"{n} {(n == 1 ? one : many)}";

    /// <summary><c>2 monitors · 2 claimed (2 verified) · 2 tracking</c></summary>
    public static string DisplaysHeader(StatusSnapshot s) =>
        $"{Plural(s.Screens.Count, "monitor", "monitors")}{Sep}{Claimed(s)} claimed ({Verified(s)} verified){Sep}{Tracking(s)} tracking";

    /// <summary>Every warn / critical the snapshot carries: per screen (vendor included) and system-wide.</summary>
    public static IEnumerable<StatusWarning> ActionableWarnings(StatusSnapshot s) =>
        s.Screens.SelectMany(ScreenWarnings).Concat(s.Warnings).Where(w => w.Level != WarningLevel.Info);

    /// <summary>
    /// Home's line (design §8, the vendor dashboard's Home rule): only with
    /// more than one claimed screen or a warn / critical warning.
    /// </summary>
    public static string? HomeLine(StatusSnapshot s)
    {
        int claimed = Claimed(s);
        int warnings = ActionableWarnings(s).Count();
        if (claimed <= 1 && warnings == 0) return null;
        return $"{Plural(claimed, "screen", "screens")}{Sep}{Tracking(s)} tracking{Sep}{Plural(warnings, "warning", "warnings")}";
    }

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

    public static string Copy(StatusSnapshot s, bool includeSource = true)
    {
        var sb = new StringBuilder();
        sb.Append("Displays: ").Append(DisplaysHeader(s)).Append('\n');
        if (includeSource) sb.Append("Source: ").Append(SourceLine(s)).Append('\n');
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

    public static string SourceLine(StatusSnapshot s) =>
        s.IsService ? $"source: service{Sep}gen {s.Generation.Topology}/{s.Generation.Status}" : $"source: {s.Source}";

    // ── Plug-ins ────────────────────────────────────────────────────────────

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
            : "SEGMENT_FLAT_2D: this segment has no display processor; that part of the window is flat 2D.";
    }
}

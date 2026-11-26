// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;

namespace DisplayXR.Dashboard.Model;

// The display status snapshot, `displayxr-cli status --json` (ADR-051,
// docs/roadmap/display-dashboard.md §3). Immutable; every field tolerant.
// Enum-like values stay strings: an older or newer CLI may say something this
// build has never heard of, and it is shown verbatim rather than dropped.

public enum WarningLevel { Info = 0, Warn = 1, Critical = 2 }

public sealed record StatusWarning(string Code, WarningLevel Level, string Text)
{
    public static WarningLevel ParseLevel(string s) => s switch
    {
        "critical" => WarningLevel.Critical,
        "warn" => WarningLevel.Warn,
        _ => WarningLevel.Info,
    };

    internal static StatusWarning From(JsonElement e) =>
        new(e.Str("code", "?"), ParseLevel(e.Str("level", "info")), e.Str("text"));
}

/// <summary>The snapshot's counters; <c>Conversion</c> (ADR-053) is 0 from a CLI that predates it.</summary>
public sealed record Generation(ulong Topology, ulong Status, ulong Conversion = 0);

/// <summary>
/// One conversion provider (ADR-053, <c>conv_providers[]</c>): a 2D→3D provider plug-in or
/// the built-in adapter over a display plug-in's lift slots. Names are shown verbatim.
/// </summary>
public sealed record ConvProvider(string Id, string Name, string Vendor, string Version, string Load, string Hosting,
                                  string Status, string Hint, bool Enabled, bool Fallback, bool Hidden, bool LicenceExempt,
                                  int ProbeOrder, int CapCount, int BadDescriptors, string? DescriptorError,
                                  string Backend, string LicenceClass, long HostPid, int Restarts);

/// <summary>One per-task row (ADR-053, <c>conv_tasks[]</c>), in the task's selection order.</summary>
public sealed record ConvTask(int? ProviderIndex, string? Provider, string Task, int CapId, bool Composed, string State,
                              string Reason, double Progress, bool Selected, int? Order, string Verdict, string LicenceClass,
                              int Streams, int JobsQueued, bool CpuStaging);

/// <summary>
/// One model pack in the runtime-owned model store (ADR-053 P4, <c>conv_packs[]</c>), read
/// from disk by the CLI: <c>State</c> is installed / downloading / partial / evicted / absent.
/// </summary>
public sealed record ConvPack(string Pack, string Provider, string State, int Models, int ModelsInstalled,
                              int ResearchModels, long BytesTotal, long BytesInstalled, long DownloadDone,
                              long DownloadTotal, double Progress);

public sealed record RuntimeInfo(string Version, string GitTag, int PluginAbi, string? ActiveOpenXrRuntime);

public sealed record PluginInfo(string Id, string Name, string Vendor, string Version, string Load,
                                string PlatformState, string Hint, bool Fallback, int ProbeOrder)
{
    /// <summary>The plug-in's platform update notice (ADR-045 Amendment 2026-11-22); <see cref="PlatformRemedy.None"/> when it reports none.</summary>
    public PlatformRemedy Remedy { get; init; } = PlatformRemedy.None;

    /// <summary>
    /// Registered with <c>Enabled</c> = 1 (<c>dp enable|disable</c>); true from a runtime that predates the key.
    /// A disabled plug-in is listed but never tried by any scan.
    /// </summary>
    public bool Enabled { get; init; } = true;
}

/// <summary>
/// A display plug-in's platform remedy (ADR-045 Amendment 2026-11-22): the vendor platform has an
/// update, recommended or required. Advisory: the runtime changes nothing because of it, the
/// Dashboard only shows it. The strings are the plug-in's own, verbatim; "" = absent. The CLI
/// already dropped an <c>action_url</c> that is not https; <see cref="Url"/> checks again.
/// </summary>
public sealed record PlatformRemedy(bool UpdateRecommended, bool UpdateRequired, string InstalledVersion,
                                    string RequiredVersion, string ActionLabel, string ActionUrl)
{
    public static readonly PlatformRemedy None = new(false, false, "", "", "", "");

    /// <summary>A notice to show (either bit; REQUIRED wins when both are set).</summary>
    public bool Any => UpdateRecommended || UpdateRequired;

    /// <summary>The page to open, or null when there is none or it is not plain https.</summary>
    public string? Url => SafeUrl.TryHttps(ActionUrl, out var u) ? u!.AbsoluteUri : null;

    /// <summary>Flat keys on a plug-in row (<c>status --json</c>, <c>info --json</c>); absent keys read as no remedy.</summary>
    internal static PlatformRemedy Read(JsonElement p)
    {
        var r = new PlatformRemedy(p.Bool("update_recommended"), p.Bool("update_required"), p.Str("installed_version"),
                                   p.Str("required_version"), p.Str("action_label"), p.Str("action_url"));
        return r.Any ? r : None;
    }
}

public readonly record struct PixelRect(long Left, long Top, long Width, long Height)
{
    public long Right => Left + Width;
    public long Bottom => Top + Height;
    public bool IsEmpty => Width <= 0 || Height <= 0;
}

public sealed record EdidInfo(string Manufacturer, string Product, long Serial);

public sealed record DesktopInfo(PixelRect Rect, double Scale);

public sealed record NativeMode(long Width, long Height, long RefreshMhz, bool IsNative);

public sealed record PhysicalMm(double Width, double Height, string Source);

public sealed record Roles(bool OsMain, bool RuntimeDefault, bool VendorPrimary);

/// <summary>
/// A screen's claim. <c>Forced</c> / <c>PreferredPlugin</c> / <c>PreferredSource</c> /
/// <c>Apply</c> are the per-screen display-processor override (phase 7); they are
/// null when the CLI predates it, and the selector is then not shown.
/// </summary>
public sealed record Claim(string? PluginId, string Confidence, int ConfidenceValue, string Serial, IReadOnlyList<string> Apis,
                           bool? Forced = null, string? PreferredPlugin = null, string? PreferredSource = null, string? Apply = null)
{
    public bool IsClaimed => !string.IsNullOrEmpty(PluginId) && Confidence != "NONE";
}

public sealed record NominalViewer(double X, double Y, double Z);

public sealed record ScreenLayout(double WidthM, double HeightM, NominalViewer Viewer, string Source);

/// <summary>
/// A screen's eye tracking. ADR-054 N6 Part 2: <see cref="ViewerSource"/> is whose eyes the views
/// follow (<c>dp</c> / <c>provider:&lt;id&gt;</c> / <c>nominal</c>), <see cref="ViewerReason"/> why,
/// <see cref="ViewerPolicy"/> the display's viewer policy (OWN / EXTERNAL_OK / FIXED) where the
/// snapshot's builder evaluated it. All null from a CLI that predates them, or where not evaluated.
/// </summary>
public sealed record EyeTracking(IReadOnlyList<string> Supported, string? Default, string State,
                                 string? ViewerSource = null, string? ViewerReason = null, string? ViewerPolicy = null);

/// <summary>
/// ADR-066 Part B: a screen's per-screen viewer-tracking setting (<c>screens[i].viewer_tracking</c>).
/// <see cref="Setting"/> is what is stored ("off" / "3d" / "always" / "default"), <see cref="Effective"/>
/// what it resolved to, <see cref="Available"/> whether it is offered here (else <see cref="Reason"/>
/// says why); <see cref="Detail"/> what the provider tracks from when it is.
/// </summary>
public sealed record ViewerTracking(string Setting, string SettingSource, string Effective, bool Tracked2D,
                                    bool Tracked3D, string? CurrentMode, bool? Active, string? Provider,
                                    bool Available, string? Reason, string? Detail);

public sealed record ScreenMode(int Index, string Name, int Views, bool Is3D);

/// <summary>
/// One 3D mode a screen's display driver offers (<c>screens[i].mode_3d.choices[j]</c>). <see cref="Name"/>
/// is what <c>dp mode set</c> takes, <see cref="Label"/> the driver's own words (shown as given).
/// <see cref="Applies"/> is "now" (a live change) / "restart_app" (the choice is stored and used the next
/// time an app starts) / "not_supported" (it cannot be chosen). <see cref="Reason"/> is the runtime's
/// note: why, when it is not "now"; a "now" mode can carry one too ("4 views: stereo apps show 2D").
/// </summary>
public sealed record Mode3DChoice(string Name, string Label, int Views, bool Tracked, bool Passthrough,
                                  string Applies, string? Reason)
{
    public bool NotSupported => Applies == "not_supported";
    public bool NeedsRestart => Applies == "restart_app";
}

/// <summary>
/// A screen's 3D mode (<c>screens[i].mode_3d</c>): which of its driver's 3D modes is drawn now
/// (<see cref="Active"/>), what is stored for this screen (<see cref="Setting"/>, null = nothing), and
/// what chose it (<see cref="Source"/>: "driver_default" / "runtime_default" / "user" / "machine").
/// The two differ while a stored choice waits for the next app start. A stored choice on a screen whose
/// driver reports no 3D modes reads <see cref="Active"/> null, no <see cref="Choices"/> and a
/// <see cref="Reason"/>. The whole record is null when the feed says <c>null</c> (nothing known and
/// nothing stored) or predates it.
/// </summary>
public sealed record Mode3D(string? Active, string? Setting, string Source, IReadOnlyList<Mode3DChoice> Choices)
{
    /// <summary>Why there are no choices (the feed's top-level <c>reason</c>); null otherwise.</summary>
    public string? Reason { get; init; }

    public Mode3DChoice? Find(string? name) => name is null ? null : Choices.FirstOrDefault(c => c.Name == name);
}

public sealed record ScreenDp(long ClientId, string Api, string Kind, string Backend);

public sealed record VendorCell(bool Present, bool Ready, bool Verified, bool Calibrated, string Tracker, string Lens,
                                string Model, string Serial, StatusWarning? WorstWarning, string? DashboardCommand);

public sealed partial record Screen(
    string Id, string? Key, int Index, string DeviceName, string FriendlyName, EdidInfo Edid, DesktopInfo Desktop, NativeMode Native,
    PhysicalMm PhysicalMm, Roles Roles, Claim Claim, ScreenLayout Layout, EyeTracking EyeTracking, ScreenMode? Mode,
    IReadOnlyList<ScreenDp> Dps, VendorCell Vendor, IReadOnlyList<StatusWarning> Warnings,
    ViewerTracking? ViewerTracking = null);

public sealed partial record Screen
{
    /// <summary>The screen's 3D mode and the modes its driver offers; null when there is nothing to choose.</summary>
    public Mode3D? Mode3D { get; init; }
}

public sealed record ClientFlags(bool Active, bool Visible, bool Focused, bool Overlay);

public sealed record CanvasRect(long X, long Y, long W, long H);

public sealed record Segment(string? Screen, CanvasRect Canvas, bool HasDp, bool Woven, string EyeSource);

public sealed record Segments(ulong Generation, bool Split, IReadOnlyList<Segment> Items);

public sealed record Views(int Capacity, int Active, int Reported);

public sealed record Integrity(ulong Paint, ulong Present, ulong Skip, string? WeavePlacement);

public sealed record Client(
    long Id, long Pid, string Class, string Name, ClientFlags Flags, string Presenter, string Lease, PixelRect? Window,
    string? OwnerScreen, Segments Segments, Views Views, Integrity Integrity)
{
    public bool IsDiag => Class == "DIAG";
}

public sealed record Workspace(bool Enabled, string? Controller);

/// <summary>
/// The GLOBAL display-driver preference (<c>dp_preference</c>: <c>dp use &lt;id&gt;</c> without <c>--screen</c>,
/// per user or for the whole PC, or an environment pin). <see cref="PluginId"/> null = none set;
/// <see cref="Source"/> is "user" / "machine" / "env". The whole record is null from a runtime that predates it.
/// </summary>
public sealed record DpPreference(string? PluginId, string? Source)
{
    public bool IsSet => !string.IsNullOrEmpty(PluginId);
}

public sealed record StatusSnapshot(
    int Schema, string Source, Generation Generation, RuntimeInfo Runtime, IReadOnlyList<PluginInfo> Plugins,
    IReadOnlyList<Screen> Screens, IReadOnlyList<Client> Clients, Workspace Workspace,
    IReadOnlyList<StatusWarning> Warnings, string RawJson,
    IReadOnlyList<ConvProvider>? ConvProviders = null, IReadOnlyList<ConvTask>? ConvTasks = null, bool ConvKnown = false,
    IReadOnlyList<ConvPack>? ConvPacks = null, string? ModelStoreRoot = null, DpPreference? DpPreference = null)
{
    /// <summary>Model packs in the runtime-owned store (ADR-053 P4); empty from a CLI that predates the store.</summary>
    public IReadOnlyList<ConvPack> Packs => ConvPacks ?? Array.Empty<ConvPack>();

    /// <summary>Conversion providers (ADR-053); empty from a CLI that predates them (<see cref="ConvKnown"/> false).</summary>
    public IReadOnlyList<ConvProvider> Providers => ConvProviders ?? Array.Empty<ConvProvider>();

    /// <summary>Per-task conversion rows (ADR-053).</summary>
    public IReadOnlyList<ConvTask> Tasks => ConvTasks ?? Array.Empty<ConvTask>();

    public bool IsService => Source == "service";

    public Screen? ScreenById(string? id) =>
        id is null ? null : Screens.FirstOrDefault(s => string.Equals(s.Id, id, StringComparison.OrdinalIgnoreCase));
}

public static class StatusSnapshotParser
{
    /// <summary>
    /// Parse one snapshot (pretty JSON or one NDJSON line). Fails, without
    /// throwing, on anything that is not a JSON object carrying the snapshot's
    /// <c>schema</c> key: a truncated line, a stray log line, an empty string.
    /// </summary>
    public static bool TryParse(string? json, out StatusSnapshot? snapshot, out string? error)
    {
        snapshot = null;
        error = null;
        if (string.IsNullOrWhiteSpace(json)) { error = "empty"; return false; }
        try
        {
            using var doc = JsonDocument.Parse(json, new JsonDocumentOptions { MaxDepth = 64 });
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) { error = "not a JSON object"; return false; }
            if (!root.Has("schema")) { error = "no 'schema' key: not a status snapshot"; return false; }
            snapshot = Read(root, json);
            return true;
        }
        catch (JsonException ex) { error = ex.Message; return false; }
        catch (Exception ex) when (ex is InvalidOperationException or FormatException or ArgumentException or OverflowException)
        {
            error = ex.Message;
            return false;
        }
    }

    private static StatusSnapshot Read(JsonElement root, string raw)
    {
        var gen = root.Obj("generation");
        var rt = root.Obj("runtime");
        var ws = root.Obj("workspace");
        return new StatusSnapshot(
            root.Int("schema"),
            root.Str("source", "headless"),
            new Generation((ulong)Math.Max(0, gen.Long("topology")), (ulong)Math.Max(0, gen.Long("status")),
                           (ulong)Math.Max(0, gen.Long("conversion"))),
            new RuntimeInfo(rt.Str("version"), rt.Str("git_tag"), rt.Int("plugin_abi"), rt.StrOrNull("active_openxr_runtime")),
            root.Arr("plugins").Where(p => p.ValueKind == JsonValueKind.Object).Select(ReadPlugin).ToArray(),
            root.Arr("screens").Where(s => s.ValueKind == JsonValueKind.Object)
                .Select(s => ReadScreen(s) with { Mode3D = ReadMode3D(s.Obj("mode_3d")) }).ToArray(),
            root.Arr("clients").Where(c => c.ValueKind == JsonValueKind.Object).Select(ReadClient).ToArray(),
            new Workspace(ws.Bool("enabled"), ws.StrOrNull("controller")),
            ReadWarnings(root),
            raw,
            root.Arr("conv_providers").Where(p => p.ValueKind == JsonValueKind.Object).Select(ReadConvProvider).ToArray(),
            root.Arr("conv_tasks").Where(t => t.ValueKind == JsonValueKind.Object).Select(ReadConvTask).ToArray(),
            root.Has("conv_providers"),
            root.Arr("conv_packs").Where(p => p.ValueKind == JsonValueKind.Object).Select(ReadConvPack).ToArray(),
            root.Obj("conv_model_store").StrOrNull("root"),
            root.Obj("dp_preference") is { } pref && pref.ValueKind == JsonValueKind.Object
                ? new DpPreference(pref.StrOrNull("plugin_id"), pref.StrOrNull("source"))
                : null);
    }

    private static ConvPack ReadConvPack(JsonElement p) => new(
        p.Str("pack"), p.Str("provider"), p.Str("state", "absent"), p.Int("models"), p.Int("models_installed"),
        p.Int("research_models"), p.Long("bytes_total"), p.Long("bytes_installed"), p.Long("download_done"),
        p.Long("download_total"), p.Num("progress", -1));

    private static ConvProvider ReadConvProvider(JsonElement p) => new(
        p.Str("id"), p.Str("name"), p.Str("vendor"), p.Str("version"), p.Str("load"), p.Str("hosting", "in_service"),
        p.Str("status", "UNKNOWN"), p.Str("hint"), p.Bool("enabled", true), p.Bool("fallback"), p.Bool("hidden"),
        p.Bool("licence_exempt"), p.Int("probe_order"), p.Int("cap_count"), p.Int("bad_descriptors"),
        p.StrOrNull("descriptor_error"), p.Str("backend"), p.Str("licence_class", "UNKNOWN"), p.Long("host_pid"),
        p.Int("restarts"));

    private static ConvTask ReadConvTask(JsonElement t) => new(
        t.Has("provider_index") && t.GetProperty("provider_index").ValueKind == JsonValueKind.Number ? t.Int("provider_index") : null,
        t.StrOrNull("provider"), t.Str("task"), t.Int("cap_id"), t.Bool("composed"), t.Str("state", "UNKNOWN"),
        t.Str("reason", "none"), t.Num("progress"), t.Bool("selected"),
        t.Has("order") && t.GetProperty("order").ValueKind == JsonValueKind.Number ? t.Int("order") : null,
        t.Str("verdict"), t.Str("licence_class", "UNKNOWN"), t.Int("streams"), t.Int("jobs_queued"), t.Bool("cpu_staging"));

    private static IReadOnlyList<StatusWarning> ReadWarnings(JsonElement e) =>
        e.Arr("warnings").Where(w => w.ValueKind == JsonValueKind.Object).Select(StatusWarning.From).ToArray();

    private static IReadOnlyList<string> Strings(JsonElement? e, string key) =>
        e.Arr(key).Where(a => a.ValueKind == JsonValueKind.String).Select(a => a.GetString() ?? "").Where(a => a.Length > 0).ToArray();

    private static PluginInfo ReadPlugin(JsonElement p) => new(
        p.Str("id"), p.Str("name"), p.Str("vendor"), p.Str("version"), p.Str("load"),
        p.Str("platform_state", "UNKNOWN"), p.Str("hint"), p.Bool("fallback"), p.Int("probe_order"))
    { Remedy = PlatformRemedy.Read(p), Enabled = p.Bool("enabled", true) };

    /// <summary>
    /// A screen's <c>mode_3d</c>: null for a JSON <c>null</c> or a missing key. A choice without a name
    /// cannot be set and is dropped; one without a label shows its name; a missing <c>applies</c> reads "now".
    /// </summary>
    private static Mode3D? ReadMode3D(JsonElement? m3)
    {
        if (m3 is not { } o) return null;
        var choices = o.Arr("choices").Where(c => c.ValueKind == JsonValueKind.Object && c.Str("name").Length > 0)
            .Select(c => new Mode3DChoice(c.Str("name"), c.Str("label", c.Str("name")), c.Int("views"), c.Bool("tracked"),
                                          c.Bool("passthrough"), c.Str("applies", "now"), c.StrOrNull("reason")))
            .ToArray();
        return new Mode3D(o.StrOrNull("active"), o.StrOrNull("setting"), o.Str("source", "runtime_default"), choices)
        { Reason = o.StrOrNull("reason") is { Length: > 0 } why ? why : null };
    }

    private static PixelRect ReadRect(JsonElement? e) =>
        new(e.Long("left"), e.Long("top"), e.Long("width"), e.Long("height"));

    private static Screen ReadScreen(JsonElement s)
    {
        var edid = s.Obj("edid");
        var desk = s.Obj("desktop");
        var nat = s.Obj("native");
        var mm = s.Obj("physical_mm");
        var roles = s.Obj("roles");
        var claim = s.Obj("claim");
        var lay = s.Obj("layout");
        var viewer = lay.Obj("nominal_viewer_m");
        var et = s.Obj("eye_tracking");
        var mode = s.Obj("mode");
        var v = s.Obj("vendor");
        var ww = v.Obj("worst_warning");

        return new Screen(
            s.Str("id"),
            s.StrOrNull("key"),
            s.Int("index"),
            s.Str("device_name"),
            s.Str("friendly_name"),
            new EdidInfo(edid.Str("manufacturer"), edid.Str("product"), edid.Long("serial")),
            new DesktopInfo(ReadRect(desk), desk.Num("scale", 1)),
            new NativeMode(nat.Long("width"), nat.Long("height"), nat.Long("refresh_mhz"), nat.Bool("is_native", true)),
            new PhysicalMm(mm.Num("width"), mm.Num("height"), mm.Str("source", "none")),
            new Roles(roles.Bool("os_main"), roles.Bool("runtime_default"), roles.Bool("vendor_primary")),
            new Claim(claim.StrOrNull("plugin_id"), claim.Str("confidence", "NONE"), claim.Int("confidence_value"),
                      claim.Str("serial"), Strings(claim, "apis"),
                      claim is { } cl && cl.Has("forced") ? cl.Bool("forced") : null,
                      claim.StrOrNull("preferred_plugin"), claim.StrOrNull("preferred_source"), claim.StrOrNull("apply")),
            new ScreenLayout(lay.Num("width_m"), lay.Num("height_m"),
                             new NominalViewer(viewer.Num("x"), viewer.Num("y"), viewer.Num("z")), lay.Str("source", "none")),
            new EyeTracking(Strings(et, "supported"), et.StrOrNull("default"), et.Str("state", "UNKNOWN"),
                            et.StrOrNull("viewer_source"), et.StrOrNull("viewer_reason"), et.StrOrNull("viewer_policy")),
            mode is { } m ? new ScreenMode(m.Int("index"), m.Str("name"), m.Int("views"), m.Bool("is_3d")) : null,
            s.Arr("dps").Where(d => d.ValueKind == JsonValueKind.Object)
             .Select(d => new ScreenDp(d.Long("client_id"), d.Str("api", "?"), d.Str("kind", "primary"), d.Str("backend", "OK")))
             .ToArray(),
            new VendorCell(v.Bool("present"), v.Bool("ready"), v.Bool("verified"), v.Bool("calibrated"),
                           v.Str("tracker", "UNKNOWN"), v.Str("lens", "UNKNOWN"), v.Str("model"), v.Str("serial"),
                           ww is { } w ? StatusWarning.From(w) : null, v.StrOrNull("dashboard_command")),
            ReadWarnings(s),
            ReadViewerTracking(s.Obj("viewer_tracking")));
    }

    /// <summary>ADR-066 Part B: null from a CLI that predates it, or where the builder did not evaluate it.</summary>
    private static ViewerTracking? ReadViewerTracking(JsonElement? vt)
    {
        if (vt is not { } o) return null;
        var tracked = o.Obj("tracked");
        bool? active = o.TryGetProperty("active", out var a) && a.ValueKind is JsonValueKind.True or JsonValueKind.False
            ? a.GetBoolean()
            : null;
        return new ViewerTracking(o.Str("setting", "default"), o.Str("setting_source", "default"),
                                  o.Str("effective", "off"), tracked.Bool("2d"), tracked.Bool("3d"),
                                  o.StrOrNull("current_mode"), active, o.StrOrNull("provider"), o.Bool("available"),
                                  o.StrOrNull("reason"), o.StrOrNull("detail"));
    }

    private static Client ReadClient(JsonElement c)
    {
        var fl = c.Obj("flags");
        var win = c.Obj("window");
        var sg = c.Obj("segments");
        var vw = c.Obj("views");
        var ig = c.Obj("integrity");
        return new Client(
            c.Long("id"), c.Long("pid"), c.Str("class", "?"), c.Str("name"),
            new ClientFlags(fl.Bool("active"), fl.Bool("visible"), fl.Bool("focused"), fl.Bool("overlay")),
            c.Str("presenter", "NONE"), c.Str("lease", "none"),
            win is { } w ? ReadRect(w) : null,
            c.StrOrNull("owner_screen"),
            new Segments((ulong)Math.Max(0, sg.Long("generation")), sg.Bool("split"),
                sg.Arr("items").Where(i => i.ValueKind == JsonValueKind.Object).Select(i =>
                {
                    var cv = i.Obj("canvas");
                    return new Segment(i.StrOrNull("screen"), new CanvasRect(cv.Long("x"), cv.Long("y"), cv.Long("w"), cv.Long("h")),
                                       i.Bool("has_dp"), i.Bool("woven"), i.Str("eye_source", "NONE"));
                }).ToArray()),
            new Views(vw.Int("capacity"), vw.Int("active"), vw.Int("reported")),
            new Integrity((ulong)Math.Max(0, ig.Long("paint")), (ulong)Math.Max(0, ig.Long("present")),
                          (ulong)Math.Max(0, ig.Long("skip")), ig.StrOrNull("weave_placement")));
    }
}

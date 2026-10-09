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

public sealed record Generation(ulong Topology, ulong Status);

public sealed record RuntimeInfo(string Version, string GitTag, int PluginAbi, string? ActiveOpenXrRuntime);

public sealed record PluginInfo(string Id, string Name, string Vendor, string Version, string Load,
                                string PlatformState, string Hint, bool Fallback, int ProbeOrder);

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

public sealed record EyeTracking(IReadOnlyList<string> Supported, string? Default, string State);

public sealed record ScreenMode(int Index, string Name, int Views, bool Is3D);

public sealed record ScreenDp(long ClientId, string Api, string Kind, string Backend);

public sealed record VendorCell(bool Present, bool Ready, bool Verified, bool Calibrated, string Tracker, string Lens,
                                string Model, string Serial, StatusWarning? WorstWarning, string? DashboardCommand);

public sealed record Screen(
    string Id, string? Key, int Index, string DeviceName, string FriendlyName, EdidInfo Edid, DesktopInfo Desktop, NativeMode Native,
    PhysicalMm PhysicalMm, Roles Roles, Claim Claim, ScreenLayout Layout, EyeTracking EyeTracking, ScreenMode? Mode,
    IReadOnlyList<ScreenDp> Dps, VendorCell Vendor, IReadOnlyList<StatusWarning> Warnings);

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

public sealed record StatusSnapshot(
    int Schema, string Source, Generation Generation, RuntimeInfo Runtime, IReadOnlyList<PluginInfo> Plugins,
    IReadOnlyList<Screen> Screens, IReadOnlyList<Client> Clients, Workspace Workspace,
    IReadOnlyList<StatusWarning> Warnings, string RawJson)
{
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
            new Generation((ulong)Math.Max(0, gen.Long("topology")), (ulong)Math.Max(0, gen.Long("status"))),
            new RuntimeInfo(rt.Str("version"), rt.Str("git_tag"), rt.Int("plugin_abi"), rt.StrOrNull("active_openxr_runtime")),
            root.Arr("plugins").Where(p => p.ValueKind == JsonValueKind.Object).Select(ReadPlugin).ToArray(),
            root.Arr("screens").Where(s => s.ValueKind == JsonValueKind.Object).Select(ReadScreen).ToArray(),
            root.Arr("clients").Where(c => c.ValueKind == JsonValueKind.Object).Select(ReadClient).ToArray(),
            new Workspace(ws.Bool("enabled"), ws.StrOrNull("controller")),
            ReadWarnings(root),
            raw);
    }

    private static IReadOnlyList<StatusWarning> ReadWarnings(JsonElement e) =>
        e.Arr("warnings").Where(w => w.ValueKind == JsonValueKind.Object).Select(StatusWarning.From).ToArray();

    private static IReadOnlyList<string> Strings(JsonElement? e, string key) =>
        e.Arr(key).Where(a => a.ValueKind == JsonValueKind.String).Select(a => a.GetString() ?? "").Where(a => a.Length > 0).ToArray();

    private static PluginInfo ReadPlugin(JsonElement p) => new(
        p.Str("id"), p.Str("name"), p.Str("vendor"), p.Str("version"), p.Str("load"),
        p.Str("platform_state", "UNKNOWN"), p.Str("hint"), p.Bool("fallback"), p.Int("probe_order"));

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
            new EyeTracking(Strings(et, "supported"), et.StrOrNull("default"), et.Str("state", "UNKNOWN")),
            mode is { } m ? new ScreenMode(m.Int("index"), m.Str("name"), m.Int("views"), m.Bool("is_3d")) : null,
            s.Arr("dps").Where(d => d.ValueKind == JsonValueKind.Object)
             .Select(d => new ScreenDp(d.Long("client_id"), d.Str("api", "?"), d.Str("kind", "primary"), d.Str("backend", "OK")))
             .ToArray(),
            new VendorCell(v.Bool("present"), v.Bool("ready"), v.Bool("verified"), v.Bool("calibrated"),
                           v.Str("tracker", "UNKNOWN"), v.Str("lens", "UNKNOWN"), v.Str("model"), v.Str("serial"),
                           ww is { } w ? StatusWarning.From(w) : null, v.StrOrNull("dashboard_command")),
            ReadWarnings(s));
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

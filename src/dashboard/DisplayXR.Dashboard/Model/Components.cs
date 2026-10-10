// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;

namespace DisplayXR.Dashboard.Model;

// "What is plugged into the runtime", by ROLE, never by product: the runtime
// knows that a workspace controller can connect to host 3D windows, not which
// product that is. Names, vendors and backends are shown verbatim as the
// components report them; the dashboard adds no product wording of its own.

/// <summary>The extra <c>info --json</c> blocks the Components page reads (all optional).</summary>
public sealed record InfoComponents(
    IReadOnlyList<RegisteredController> WorkspaceControllers, bool WorkspaceControllersKnown,
    IReadOnlyList<InputProviderRow> InputProviders, InputSummary? Input, RigRole? Rig,
    LiftModule? Lift, IReadOnlyList<(string Key, string Value)>? StereoCamera)
{
    public static readonly InfoComponents Empty = new(Array.Empty<RegisteredController>(), false,
        Array.Empty<InputProviderRow>(), null, null, null, null);

    /// <summary>Read from the root of <c>info --json</c>; every block may be missing or shaped differently.</summary>
    public static InfoComponents Read(JsonElement root)
    {
        bool wcKnown = root.ValueKind == JsonValueKind.Object && root.TryGetProperty("workspace_controllers", out var wcArr)
                       && wcArr.ValueKind == JsonValueKind.Array;
        var controllers = root.Arr("workspace_controllers").Where(e => e.ValueKind == JsonValueKind.Object)
            .Select(e => new RegisteredController(e.Str("id"), e.Str("name"), e.Str("version"), e.Str("exe"),
                e.Arr("actions").Select(a => a.ValueKind == JsonValueKind.String ? a.GetString() ?? ""
                                           : a.ValueKind == JsonValueKind.Object ? a.Str("name", a.Str("id")) : "")
                 .Where(a => a.Length > 0).ToArray()))
            .ToArray();

        // input_providers: today one summary object; a per-provider array is accepted too
        // (either top-level, or as "providers" inside the summary).
        var providers = new List<InputProviderRow>();
        InputSummary? input = null;
        if (root.ValueKind == JsonValueKind.Object && root.TryGetProperty("input_providers", out var ip))
        {
            IEnumerable<JsonElement> rows = Array.Empty<JsonElement>();
            if (ip.ValueKind == JsonValueKind.Array) rows = ip.EnumerateArray();
            else if (ip.ValueKind == JsonValueKind.Object)
            {
                input = new InputSummary(ip.Int("registered"), ip.Bool("hardware_present"), ip.Bool("evaluated"),
                                         ip.Bool("force_qwerty"), ip.StrOrNull("active_id"), ip.Str("note"));
                rows = ip.Arr("providers");
            }
            providers.AddRange(rows.Where(r => r.ValueKind == JsonValueKind.Object).Select(r => new InputProviderRow(
                r.Str("name", r.Str("display_name", r.Str("id"))), r.Str("id"), r.Bool("hardware_present"),
                r.Bool("active"), r.Str("note"))));
        }

        RigRole? rig = root.Obj("rig_role") is { } rg
            ? new RigRole(rg.Bool("evaluated"), rg.StrOrNull("device"), rg.Str("note")) : null;

        LiftModule? lift = root.Obj("lift_caps") is { } lc
            ? new LiftModule(lc.Bool("probed"), lc.Bool("malformed"), lc.StrOrNull("backend"), lc.Long("modes"),
                             lc.Has("state") ? lc.Long("state") : null, lc.Int("max_streams"), lc.Int("max_views"), lc.Str("note"))
            : null;

        var cam = root.Obj("stereo_camera") ?? root.Obj("camera");
        IReadOnlyList<(string, string)>? camera = cam is { } c
            ? c.EnumerateObject().Where(p => p.Value.ValueKind is JsonValueKind.String or JsonValueKind.Number
                                                 or JsonValueKind.True or JsonValueKind.False)
               .Select(p => (p.Name, p.Value.ValueKind == JsonValueKind.String ? p.Value.GetString() ?? "" : p.Value.GetRawText()))
               .ToArray()
            : null;

        return new InfoComponents(controllers, wcKnown, providers, input, rig, lift, camera);
    }
}

public sealed record RegisteredController(string Id, string Name, string Version, string Exe, IReadOnlyList<string> Actions);
public sealed record InputProviderRow(string Name, string Id, bool HardwarePresent, bool Active, string Note);
public sealed record InputSummary(int Registered, bool HardwarePresent, bool Evaluated, bool ForceQwerty, string? ActiveId, string Note);
public sealed record RigRole(bool Evaluated, string? Device, string Note);
public sealed record LiftModule(bool Probed, bool Malformed, string? Backend, long Modes, long? State, int MaxStreams, int MaxViews, string Note);

/// <summary>One row of a Components section.</summary>
public sealed record ComponentItem(string Name, Level Level, IReadOnlyList<(string Text, Level Level)> Chips, IReadOnlyList<string> Lines);

/// <summary>One role: what it is, how many are registered / connected, and its rows.</summary>
public sealed record ComponentSection(string Id, string Title, string Role, int Count, int Connected,
                                      string ConnectedLine, IReadOnlyList<ComponentItem> Items, bool Collapsed = false)
{
    public bool IsEmpty => Items.Count == 0;
}

public static class Components
{
    public const string ControllerClass = "CONTROLLER";
    public const string PresentOwnerClass = "PRESENT_OWNER";
    public const string DiagClass = "DIAG";

    private static string Sep => StatusText.Sep;

    private static string Plural(int n, string one, string many) => StatusText.Plural(n, one, many);

    /// <summary>Every section, in page order. Sections whose source is absent are omitted (stereo camera).</summary>
    public static IReadOnlyList<ComponentSection> Build(StatusSnapshot? s, InfoComponents? info)
    {
        info ??= InfoComponents.Empty;
        var list = new List<ComponentSection>
        {
            DisplayProcessors(s),
            WorkspaceController(s, info),
            ByClass(s, "present_owners", "Present owners", PresentOwnerClass,
                "A present owner presents its own window (a browser, a capture app) while the runtime weaves into it."),
            InputProviders(info),
            Conversion(info),
        };
        if (StereoCamera(info) is { } cam) list.Add(cam);
        list.Add(ByClass(s, "diagnostics", "Diagnostics clients", DiagClass,
            "Read-only status readers, such as this dashboard's own status feed and the CLI.", collapsed: true));
        return list;
    }

    public static ComponentSection DisplayProcessors(StatusSnapshot? s)
    {
        var items = new List<ComponentItem>();
        int active = 0;
        foreach (var p in s?.Plugins ?? Array.Empty<PluginInfo>())
        {
            bool isActive = p.Load == "ACTIVE";
            if (isActive) active++;
            var chips = new List<(string, Level)>
            {
                (p.Load.Length > 0 ? p.Load : "UNKNOWN", isActive ? Level.Ok : Level.Plain),
                (p.PlatformState, StatusText.PluginLevel(p) is Level.Ok or Level.Plain ? Level.Plain : StatusText.PluginLevel(p)),
            };
            if (p.Fallback) chips.Add(("FALLBACK", Level.Plain));
            var claims = s!.Screens.Where(x => x.Claim.PluginId == p.Id).Select(x =>
                $"{(StatusText.ShortDevice(x.DeviceName) is { Length: > 0 } d ? d : StatusText.ScreenName(x))} ({x.Claim.Confidence})").ToList();
            var lines = new List<string>
            {
                string.Join(Sep, new[] { p.Vendor, p.Version.Length > 0 ? "version " + p.Version : "", $"{p.Id}", $"ProbeOrder {p.ProbeOrder}" }
                    .Where(x => x.Length > 0)),
                claims.Count > 0 ? "Claims " + string.Join(", ", claims) : "Claims no screen",
            };
            if (p.Hint.Length > 0) lines.Add(p.Hint);
            items.Add(new ComponentItem(p.Name.Length > 0 ? p.Name : p.Id, StatusText.PluginLevel(p), chips, lines));
        }
        return new ComponentSection("display_processors", "Display processors",
            "A display processor turns the views an app renders into what one kind of 3D display shows; each is a plug-in that claims the screens it drives.",
            items.Count, active, $"{Plural(items.Count, "registered", "registered")}{Sep}{active} active", items);
    }

    public static ComponentSection WorkspaceController(StatusSnapshot? s, InfoComponents info)
    {
        var connected = (s?.Clients ?? Array.Empty<Client>()).Where(c => c.Class == ControllerClass).ToList();
        bool workspaceOn = s?.Workspace.Enabled ?? false;
        string? snapName = s?.Workspace.Controller;
        var items = new List<ComponentItem>();
        var matched = new HashSet<long>();

        foreach (var reg in info.WorkspaceControllers)
        {
            var client = connected.FirstOrDefault(c => !matched.Contains(c.Id) && Matches(reg, c));
            if (client is not null) matched.Add(client.Id);
            string name = reg.Name.Length > 0 ? reg.Name : reg.Id;
            var lines = new List<string> { Line(name, client is not null, workspaceOn) };
            var detail = new[] { reg.Id.Length > 0 && reg.Id != name ? reg.Id : "", reg.Version.Length > 0 ? "version " + reg.Version : "", reg.Exe }
                .Where(x => x.Length > 0).ToArray();
            if (detail.Length > 0) lines.Add(string.Join(Sep, detail));
            if (client is not null) lines.Add(ClientLine(client));
            if (reg.Actions.Count > 0) lines.Add("Actions: " + string.Join(", ", reg.Actions));
            items.Add(new ComponentItem(name, client is not null ? Level.Ok : Level.Plain,
                new[] { (client is not null ? "connected" : "registered, not running", client is not null ? Level.Ok : Level.Plain) }, lines));
        }
        foreach (var c in connected.Where(c => !matched.Contains(c.Id)))
        {
            string name = c.Name.Length > 0 ? c.Name : snapName ?? $"client {c.Id}";
            items.Add(new ComponentItem(name, Level.Ok, new[] { ("connected", Level.Ok) },
                new[] { Line(name, true, workspaceOn), ClientLine(c) }));
        }
        if (items.Count == 0 && snapName is not null)
            items.Add(new ComponentItem(snapName, workspaceOn ? Level.Info : Level.Plain, new[] { ("not connected", Level.Plain) },
                new[] { Line(snapName, false, workspaceOn) }));

        string summary = connected.Count > 0 ? $"{connected.Count} connected" : "none connected";
        return new ComponentSection("workspace_controller", "Workspace controller",
            "A workspace controller hosts other apps' 3D windows; the runtime treats it as a client class, not a product.",
            items.Count, connected.Count, $"{summary}{Sep}workspace {(workspaceOn ? "on" : "off")}"
                + (info.WorkspaceControllersKnown ? $"{Sep}{Plural(info.WorkspaceControllers.Count, "registered", "registered")}" : ""),
            items);
    }

    private static string Line(string name, bool connected, bool workspaceOn) =>
        $"Workspace controller: {name}{Sep}{(connected ? "connected" : "not connected")}{Sep}workspace {(workspaceOn ? "on" : "off")}";

    private static bool Matches(RegisteredController reg, Client c)
    {
        static string Stem(string p)
        {
            string f = p.Replace('/', '\\');
            int i = f.LastIndexOf('\\');
            f = i >= 0 ? f[(i + 1)..] : f;
            return f.EndsWith(".exe", StringComparison.OrdinalIgnoreCase) ? f[..^4] : f;
        }
        string cn = Stem(c.Name);
        return (reg.Exe.Length > 0 && string.Equals(Stem(reg.Exe), cn, StringComparison.OrdinalIgnoreCase))
            || (reg.Name.Length > 0 && string.Equals(reg.Name, c.Name, StringComparison.OrdinalIgnoreCase))
            || (reg.Id.Length > 0 && string.Equals(reg.Id, cn, StringComparison.OrdinalIgnoreCase));
    }

    private static string ClientLine(Client c)
    {
        var parts = new List<string> { $"pid {c.Pid}", $"id {c.Id}" };
        var flags = StatusText.ClientFlags(c).ToList();
        if (flags.Count > 0) parts.Add(string.Join(" ", flags));
        if (c.Window is { } w) parts.Add("window " + StatusText.WindowText(w));
        return string.Join(Sep, parts);
    }

    public static ComponentSection ByClass(StatusSnapshot? s, string id, string title, string cls, string role, bool collapsed = false)
    {
        var clients = (s?.Clients ?? Array.Empty<Client>()).Where(c => c.Class == cls).ToList();
        var items = clients.Select(c => new ComponentItem(StatusText.ClientName(c), Level.Ok,
            StatusText.ClientFlags(c).Select(f => (f, f == "focused" ? Level.Ok : Level.Plain)).ToArray(),
            new[] { ClientLine(c) })).ToArray();
        return new ComponentSection(id, title, role, items.Length, items.Length,
            items.Length == 0 ? (s is { IsService: true } ? "none connected" : "live clients need the service") : $"{items.Length} connected",
            items, collapsed);
    }

    public static ComponentSection InputProviders(InfoComponents info)
    {
        var items = new List<ComponentItem>();
        foreach (var p in info.InputProviders)
            items.Add(new ComponentItem(p.Name.Length > 0 ? p.Name : p.Id, p.Active ? Level.Ok : Level.Plain,
                new[] { (p.HardwarePresent ? "hardware present" : "hardware absent", p.HardwarePresent ? Level.Ok : Level.Plain) }
                    .Concat(p.Active ? new[] { ("active", Level.Ok) } : Array.Empty<(string, Level)>()).ToArray(),
                p.Note.Length > 0 ? new[] { p.Note } : Array.Empty<string>()));
        if (items.Count == 0 && info.Input is { } sum)
        {
            var lines = new List<string> { sum.Note };
            if (sum.ActiveId is { Length: > 0 } a) lines.Insert(0, "Active: " + a);
            if (sum.ForceQwerty) lines.Add("Keyboard / mouse forced");
            items.Add(new ComponentItem($"{Plural(sum.Registered, "provider", "providers")} registered",
                sum.Evaluated ? Level.Ok : Level.Plain,
                new[]
                {
                    (sum.HardwarePresent ? "hardware present" : "hardware absent", sum.HardwarePresent ? Level.Ok : Level.Plain),
                    (sum.Evaluated ? "evaluated" : "not evaluated", Level.Plain),
                }, lines));
        }
        if (info.Rig is { } rig)
            items.Add(new ComponentItem("Rig (navigation) role", rig.Evaluated ? Level.Ok : Level.Plain,
                new[] { (rig.Evaluated ? "evaluated" : "not evaluated", Level.Plain) },
                new[] { rig.Device is { Length: > 0 } d ? $"Device: {d}" : "", rig.Note }.Where(x => x.Length > 0).ToArray()));
        int registered = info.InputProviders.Count > 0 ? info.InputProviders.Count : info.Input?.Registered ?? 0;
        int connected = info.InputProviders.Count > 0 ? info.InputProviders.Count(p => p.HardwarePresent)
                      : info.Input is { HardwarePresent: true } ? 1 : 0;
        return new ComponentSection("input_providers", "Input providers",
            "An input provider feeds controllers, hands or a navigation device into the runtime; without hardware, keyboard and mouse hold those roles.",
            registered, connected,
            info.Input is null && info.InputProviders.Count == 0 ? "not read yet"
                : $"{Plural(registered, "registered", "registered")}{Sep}{(connected > 0 ? "hardware present" : "no hardware present")}",
            items);
    }

    public static ComponentSection Conversion(InfoComponents info)
    {
        var items = new List<ComponentItem>();
        var l = info.Lift;
        bool present = l is { Probed: true, Malformed: false } && (l.Modes != 0 || l.Backend is { Length: > 0 });
        if (l is not null && (l.Probed || l.Note.Length > 0))
        {
            var lines = new List<string>();
            if (l.Probed)
                lines.Add(string.Join(Sep, new[]
                {
                    $"modes 0x{l.Modes.ToString("x", CultureInfo.InvariantCulture)}",
                    l.State is { } st ? $"state {st}" : "",
                    l.MaxStreams > 0 ? $"{l.MaxStreams} streams" : "",
                    l.MaxViews > 0 ? $"up to {l.MaxViews} views" : "",
                }.Where(x => x.Length > 0)));
            if (l.Note.Length > 0) lines.Add(l.Note);
            items.Add(new ComponentItem(l.Backend is { Length: > 0 } b ? b : "Conversion module",
                l.Malformed ? Level.Critical : present ? Level.Ok : Level.Plain,
                new[] { (l.Malformed ? "malformed" : present ? "available" : l.Probed ? "none" : "not probed",
                         l.Malformed ? Level.Critical : present ? Level.Ok : Level.Plain) }, lines));
        }
        return new ComponentSection("conversion", "Conversion module",
            "A conversion module turns 2D content into 3D (depth, side-by-side, multi-view) as a service any app can ask for.",
            present ? 1 : 0, present ? 1 : 0, l is null ? "not read yet" : present ? "1 available" : "none available", items);
    }

    public static ComponentSection? StereoCamera(InfoComponents info)
    {
        if (info.StereoCamera is not { } kv) return null;
        var item = new ComponentItem("Stereo camera source", Level.Plain, Array.Empty<(string, Level)>(),
            kv.Select(p => $"{p.Key}: {p.Value}").ToArray());
        return new ComponentSection("stereo_camera", "Stereo camera source",
            "A stereo camera source supplies left / right camera frames to apps through the runtime.",
            1, 0, "reported by the runtime", new[] { item });
    }
}

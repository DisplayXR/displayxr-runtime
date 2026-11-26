// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using DisplayXR.Dashboard.Feed;

namespace DisplayXR.Dashboard.Model;

// A display driver's settings (ADR-065: the driver serves 2D and 3D at run time; everything else,
// such as which kind of 3D, is a per-driver SETTING the driver declares and the runtime stores).
// Read from `displayxr-cli dp options <id> --json`; written with `dp set <id> <KEY> <value>` (per
// user) and cleared with `dp unset <id> <KEY>`. The Dashboard stores nothing itself.
//
// The friendly presentation (a label, value labels, an order) of the few settings an owner is
// expected to touch is DATA (FriendlyOptions.Table): adding one is a row there, not UI code.

/// <summary>The declared type of a setting (<c>dp options --json</c> <c>type</c>).</summary>
public enum SettingType { Bool, Int, Float, Enum, String, Secret }

/// <summary>Where the effective value came from (<c>dp options --json</c> <c>source</c>).</summary>
public enum SettingSource { Absent, Environment, User, Machine, Manifest }

/// <summary>Whether the value in force takes effect on this machine (<c>dp options --json</c> <c>status.state</c>).</summary>
public enum SettingState { Unknown, Applies, NotSupported, RestartRequired }

/// <summary>
/// What the driver says about a setting on this machine (<c>status</c>, reported through its
/// <c>get_setting_status</c> slot). <see cref="EffectiveValue"/>: with <see cref="SettingState.NotSupported"/>,
/// the value the driver uses instead (a value of the setting); <see cref="Note"/>: the driver's one sentence.
/// </summary>
public sealed record SettingStatus(SettingState State, string? EffectiveValue = null, string? Note = null)
{
    public static readonly SettingStatus Unknown = new(SettingState.Unknown);

    /// <summary>The <c>status</c> object of a row; <see cref="Unknown"/> when absent (an older CLI) or not that shape.</summary>
    public static SettingStatus From(JsonElement row)
    {
        if (row.ValueKind != JsonValueKind.Object || !row.TryGetProperty("status", out var st) || st.ValueKind != JsonValueKind.Object)
            return Unknown;
        var state = st.Str("state") switch
        {
            "applies" => SettingState.Applies,
            "not_supported" => SettingState.NotSupported,
            "restart_required" => SettingState.RestartRequired,
            _ => SettingState.Unknown,
        };
        if (state == SettingState.Unknown) return Unknown;
        string? Clean(string? v) => string.IsNullOrWhiteSpace(v) ? null : v.Trim();
        return new SettingStatus(state, state == SettingState.NotSupported ? Clean(st.StrOrNull("effective_value")) : null,
                                 Clean(st.StrOrNull("note")));
    }
}

/// <summary>
/// One declared setting. <see cref="Value"/> is null when nothing stores it (the default stands) and
/// always for a secret, whose <see cref="IsSet"/> says whether one is stored. <see cref="Values"/>: an enum's
/// accepted words as the CLI lists them (<c>values</c>, interim: read from the help), null when it lists none
/// or predates the key. <see cref="Status"/>: whether the value in force takes effect here (null = not reported).
/// </summary>
public sealed record DriverSetting(string Key, SettingType Type, string Default, string Help, string? Value, bool IsSet,
                                   SettingSource Source, IReadOnlyList<string>? Values = null, SettingStatus? Status = null)
{
    /// <summary>The value in force: the stored one, else the declared default.</summary>
    public string Effective => Value ?? Default;
}

/// <summary>
/// <c>displayxr-cli dp options &lt;id&gt; --json</c>. Tolerant: a setting the driver stores but does not
/// declare (<c>type: null</c>), or of a type this Dashboard does not know, is left out; a document
/// that is not that shape reads as no options.
/// </summary>
public sealed record DriverOptions(string Id, string EnvPrefix, IReadOnlyList<DriverSetting> Settings)
{
    public static DriverOptions None(string id) => new(id, "", Array.Empty<DriverSetting>());

    public bool IsEmpty => Settings.Count == 0;

    public DriverSetting? Find(string key) => Settings.FirstOrDefault(s => s.Key == key);

    public static bool TryParse(string? json, out DriverOptions? options)
    {
        options = null;
        if (string.IsNullOrWhiteSpace(json)) return false;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object || !root.TryGetProperty("settings", out var arr) || arr.ValueKind != JsonValueKind.Array)
                return false;
            var list = new List<DriverSetting>();
            foreach (var e in arr.EnumerateArray())
            {
                if (e.ValueKind != JsonValueKind.Object) continue;
                string key = e.Str("key");
                if (key.Length == 0 || (e.Has("declared") && !e.Bool("declared", true))) continue;
                bool secret = e.Bool("secret");
                SettingType? type = secret ? SettingType.Secret : TypeOf(e.Str("type"));
                if (type is not { } t) continue; // unknown or undeclared: not shown
                var source = SourceOf(e.Str("source"));
                string? value = t == SettingType.Secret ? null : e.StrOrNull("value");
                if (source == SettingSource.Absent) value = null;
                bool isSet = t == SettingType.Secret ? e.Bool("set", source != SettingSource.Absent) : source != SettingSource.Absent;
                IReadOnlyList<string>? values = null;
                if (t == SettingType.Enum && e.TryGetProperty("values", out var va) && va.ValueKind == JsonValueKind.Array)
                {
                    var words = va.EnumerateArray().Where(w => w.ValueKind == JsonValueKind.String)
                                  .Select(w => w.GetString() ?? "").Where(w => w.Length > 0)
                                  .Distinct(StringComparer.OrdinalIgnoreCase).ToArray();
                    if (words.Length > 0) values = words;
                }
                var status = SettingStatus.From(e);
                list.Add(new DriverSetting(key, t, e.Str("default"), e.Str("help"), value, isSet, source, values,
                                           status.State == SettingState.Unknown ? null : status));
            }
            options = new DriverOptions(root.Str("id"), root.Str("env_prefix"), list);
            return true;
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException) { return false; }
    }

    /// <summary>
    /// A CLI answer: the options when the run succeeded and the document has that shape; null when it did
    /// not (a failed run, an older CLI without the verb, another shape). Only a successful read can say
    /// "no options" (an empty list); a failed one says nothing about them.
    /// </summary>
    public static DriverOptions? From(string id, bool ok, string json) =>
        ok && TryParse(json, out var o) ? o! with { Id = o.Id.Length > 0 ? o.Id : id } : null;

    private static SettingType? TypeOf(string t) => t switch
    {
        "bool" => SettingType.Bool,
        "int" => SettingType.Int,
        "float" => SettingType.Float,
        "enum" => SettingType.Enum,
        "string" => SettingType.String,
        "secret" => SettingType.Secret,
        _ => null,
    };

    private static SettingSource SourceOf(string s) => s switch
    {
        "environment" => SettingSource.Environment,
        "user" => SettingSource.User,
        "machine" => SettingSource.Machine,
        "manifest" => SettingSource.Manifest,
        _ => SettingSource.Absent,
    };
}

/// <summary>How a setting is offered.</summary>
public enum OptionControl
{
    /// <summary>A drop-down of named values (an enum whose values are known: a friendly entry's, or the CLI's <c>values</c>).</summary>
    Choice,
    /// <summary>On / off (a bool): on writes 1, off writes 0.</summary>
    Switch,
    /// <summary>Two named states of a bool (<see cref="FriendlyOption.Choices"/>: the 0 one, then the 1 one).</summary>
    TwoWay,
    /// <summary>
    /// A pin: on writes <see cref="FriendlyOption.PinValue"/>, off clears it. Any other stored value
    /// that names a state in <see cref="FriendlyOption.Choices"/> is shown as that state.
    /// </summary>
    Pin,
    /// <summary>A text box with Apply (int, float, string, and an enum whose values the CLI does not list).</summary>
    Text,
    /// <summary>Never shown: set or not set, and Clear.</summary>
    Secret,
}

/// <summary>A value and its words.</summary>
public sealed record OptionChoice(string Value, string Label);

/// <summary>
/// The friendly presentation of one setting. <see cref="DriverId"/> null = this key on any display
/// driver. <see cref="Aliases"/> map other spellings the driver accepts to an offered value;
/// <see cref="Notes"/> is a sentence shown under the control while that value is in force. The entry
/// applies only to a setting declared with <see cref="Type"/>; any other is listed under "More options".
/// Values match without regard to case. <see cref="NoKindValues"/>: values that choose no kind at all
/// (shown as <see cref="FriendlyOptions.NoKindText"/>); <see cref="InertWithoutKindOf"/>: the key of the
/// setting without whose kind this one has no effect. <see cref="Is3DMode"/>: this setting chooses the
/// display's 3D mode, which the Displays page offers per display where the driver reports its modes; the
/// Options block then leaves it out, so the same thing is not offered twice.
/// </summary>
public sealed record FriendlyOption(string? DriverId, string Key, string Label, OptionControl Control, string Help,
                                    IReadOnlyList<OptionChoice> Choices, int Order, SettingType Type,
                                    IReadOnlyDictionary<string, string>? Aliases = null,
                                    IReadOnlyDictionary<string, string>? Notes = null,
                                    string? PinValue = null,
                                    IReadOnlyList<string>? NoKindValues = null,
                                    string? InertWithoutKindOf = null,
                                    bool Is3DMode = false);

/// <summary>The settings an owner is expected to touch, in words. Everything else is listed under "More options" by its key.</summary>
public static class FriendlyOptions
{
    /// <summary>The simulated display driver's id (the runtime's own, vendor-neutral fallback).</summary>
    public const string SimDisplay = "sim-display";

    /// <summary>
    /// The value of the simulated display's OUTPUT with more than two views. The CLI's JSON carries no
    /// per-value view count (and the status snapshot only the kind in force), so the four-view note is
    /// keyed on this value name.
    /// </summary>
    public const string FourViewKind = "quad";

    public const string FourViewNote = "For checking four-view apps. Most apps show only two of the views, or stay in 2D.";

    /// <summary>The 3D preview kind is set to a value that chooses none (<c>2d</c> / <c>passthrough</c>).</summary>
    public const string NoKindText = "No 3D preview kind is chosen: apps stay in 2D";

    /// <summary>Under "Start in" while no kind is chosen.</summary>
    public const string InertNote = "Has no effect until a 3D preview kind is chosen.";

    public static readonly IReadOnlyList<FriendlyOption> Table = new[]
    {
        // ADR-065: the 3D kind mode 1 draws. `2d` / `passthrough` are not offered: "Start in" and
        // "Always 2D" cover 2D. No top-and-bottom kind exists in sim_display_modes.h.
        new FriendlyOption(SimDisplay, "OUTPUT", "3D preview kind", OptionControl.Choice,
            "What the 3D preview looks like when an app turns 3D on.",
            new[]
            {
                new OptionChoice("anaglyph", "Red / cyan (anaglyph)"),
                new OptionChoice("sbs", "Side by side"),
                new OptionChoice("squeezed", "Side by side, squeezed"),
                new OptionChoice(FourViewKind, "Four views (2x2)"),
                new OptionChoice("interlaced", "Interlaced"),
                new OptionChoice("blend", "Blend"),
            }, 10, SettingType.Enum,
            Aliases: new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) { ["squeezed_sbs"] = "squeezed", ["interlace"] = "interlaced" },
            Notes: new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase) { [FourViewKind] = FourViewNote },
            NoKindValues: new[] { "2d", "passthrough" }, Is3DMode: true),
        new FriendlyOption(SimDisplay, "START_3D", "Start in", OptionControl.TwoWay,
            "How apps start. They can still switch between 2D and the 3D preview.",
            new[] { new OptionChoice("0", "2D"), new OptionChoice("1", "3D preview") }, 20, SettingType.Bool,
            InertWithoutKindOf: "OUTPUT"),
        new FriendlyOption(SimDisplay, "FORCE_MODE", "Always 2D", OptionControl.Pin,
            "Keep every app in 2D, whatever it asks for.",
            new[] { new OptionChoice("1", "Always 3D preview") }, 30, SettingType.Int, PinValue: "0"),
        new FriendlyOption(null, "MULTI_VIEWER", "Several viewers", OptionControl.Choice,
            "Whether 3D is shown for more than one person at a time.",
            new[]
            {
                new OptionChoice("system", "Follow the display's own setting"),
                new OptionChoice("on", "On"),
                new OptionChoice("off", "Off"),
            }, 40, SettingType.Enum),
    };

    /// <summary>The friendly entry for <paramref name="key"/> of driver <paramref name="driverId"/>: its own first, then one for any driver.</summary>
    public static FriendlyOption? Find(string driverId, string key) =>
        Table.FirstOrDefault(f => f.DriverId == driverId && f.Key == key) ?? Table.FirstOrDefault(f => f.DriverId is null && f.Key == key);

    /// <summary>The friendly entry for a declared setting: only when the declared type is the one the entry expects.</summary>
    public static FriendlyOption? For(string driverId, DriverSetting s) =>
        s.Type != SettingType.Secret && Find(driverId, s.Key) is { } f && f.Type == s.Type ? f : null;
}

/// <summary>Everything a row of the Options block shows, worked out from one setting.</summary>
/// <param name="Selected">The offered value in force (a Choice / TwoWay value), or null when none is.</param>
/// <param name="On">A Switch / Pin: whether it is on.</param>
/// <param name="State">A Pin's other state in force ("Always 3D preview"), shown as its own state.</param>
/// <param name="ValueText">The value in force, in words (for a Text control: as stored).</param>
/// <param name="ReadOnly">Set in the environment of the process: it wins, the Dashboard cannot change it.</param>
/// <param name="CanReset">Something the Dashboard can clear is stored for this user ("Use the default").</param>
/// <param name="Note">The sentence under the control for the value in force, if any.</param>
/// <param name="Tooltip">The technical lines: key, type, default, the driver's own help, the environment name.</param>
public sealed record OptionView(DriverSetting Setting, string Label, OptionControl Control, string Help, bool Friendly,
                                IReadOnlyList<OptionChoice> Choices, string? Selected, bool On, string? State,
                                string ValueText, string SourceText, bool ReadOnly, bool CanReset, string? Note,
                                string Tooltip, FriendlyOption? Entry, bool NoKind = false, OptionStatusView? Status = null)
{
    public string Key => Setting.Key;
}

/// <summary>
/// What the row says when the driver reports that the value in force does not simply apply here.
/// <paramref name="Chip"/>: the warning chip next to the control; <paramref name="Note"/>: the driver's sentence;
/// <paramref name="Using"/>: the value used instead, in the row's words ("Using: …"), or null when the driver names none.
/// </summary>
public sealed record OptionStatusView(SettingState State, string Chip, string? Note, string? Using);

/// <summary>The two parts of a driver's Options block.</summary>
public sealed record OptionsBlock(string DriverId, IReadOnlyList<OptionView> Friendly, IReadOnlyList<OptionView> More)
{
    public bool IsEmpty => Friendly.Count == 0 && More.Count == 0;
}

public static class OptionViews
{
    /// <summary>
    /// The sentence under the block. Apps that go through DisplayXR in the background (the browser, shell
    /// apps, an app forced through the service) are hosted by the DisplayXR service, which read the settings
    /// when it started: they see a change only once DisplayXR itself restarts.
    /// </summary>
    public const string AppliesNote =
        "Changes apply the next time a 3D app starts; apps already running keep what they started with. " +
        "Apps that run through DisplayXR in the background (the browser, shell apps) pick up a change only after DisplayXR restarts.";

    /// <summary>The chip of a setting whose value cannot take effect on this machine.</summary>
    public const string NotSupportedChip = "Not supported here";

    /// <summary>The chip of a setting that takes effect after a restart.</summary>
    public const string RestartChip = "Takes effect after a restart";

    /// <summary>The sentence when the driver says the value is not supported here but gives no reason.</summary>
    public const string NotSupportedFallbackNote =
        "This display driver can't use this value on this PC. Your choice is kept and applies where it is supported.";

    /// <summary>"Now: …" normally; "Chosen: …" while the chosen value cannot take effect here (it is not what is in force).</summary>
    public static string NowLine(OptionView v)
    {
        string head = v.Control == OptionControl.Text && v.ValueText == "Not set"
            ? "Not set"
            : $"{(v.Status?.State == SettingState.NotSupported ? "Chosen" : "Now")}: {v.ValueText}";
        return $"{head}{StatusText.Sep}{v.SourceText}";
    }

    public const string EnvironmentNote = "Set outside the Dashboard, and that value wins: a change here would not apply.";

    /// <summary>Where the value in force comes from, in plain words.</summary>
    public static string SourceText(SettingSource s) => s switch
    {
        SettingSource.User => "Your choice",
        SettingSource.Machine or SettingSource.Manifest => "Set for everyone on this PC",
        SettingSource.Environment => "Set outside the Dashboard",
        _ => "Default",
    };

    public static string AccessibleName(string label, string driver) => $"{label}: {driver}";
    public static string ResetName(string label, string driver) => $"Use the default for {label}: {driver}";
    /// <summary>The accessible name of a secret's "Set" button.</summary>
    public static string SetName(string label, string driver) => $"Set {label}: {driver}";
    public static string MoreName(string driver) => $"More options: {driver}";
    public static string RetryName(string driver) => $"Try again reading options: {driver}";

    /// <summary>The last read of the options failed (an earlier read's controls stay below it).</summary>
    public const string ReadFailedLine = "Couldn't read its options.";

    /// <summary>A successful read with no setting this Dashboard can show.</summary>
    public const string NoOptionsLine = "This display driver has no options to set here.";

    public static bool IsTrue(string v) =>
        v.Trim().ToLowerInvariant() is "1" or "true" or "on" or "yes";

    /// <summary>In the block when the 3D mode setting is left out of it (<see cref="FriendlyOption.Is3DMode"/>).</summary>
    public const string Mode3DOnDisplaysNote = "The 3D mode is chosen for each display, on Displays.";

    /// <summary>
    /// The block for one driver: the friendly settings in table order, then every other one by its key, in the driver's order.
    /// <paramref name="modeOnDisplays"/>: a display of this driver shows the 3D mode picker on Displays, so the
    /// setting that chooses the same thing (<see cref="FriendlyOption.Is3DMode"/>) is left out.
    /// </summary>
    public static OptionsBlock Build(string driverId, DriverOptions options, bool modeOnDisplays = false)
    {
        var friendly = new List<(int Order, OptionView View)>();
        var more = new List<OptionView>();
        foreach (var s in options.Settings)
        {
            var entry = FriendlyOptions.For(driverId, s);
            if (modeOnDisplays && entry is { Is3DMode: true }) continue;
            var view = View(s, entry, options.EnvPrefix);
            if (entry is not null) friendly.Add((entry.Order, view));
            else more.Add(view);
        }
        // A setting that has no effect without another's kind says so while that one chooses none.
        var noKind = friendly.Where(f => f.View.NoKind).Select(f => f.View.Key).ToHashSet();
        for (int i = 0; i < friendly.Count; i++)
            if (friendly[i].View.Entry?.InertWithoutKindOf is { } k && noKind.Contains(k))
                friendly[i] = (friendly[i].Order, friendly[i].View with { Note = FriendlyOptions.InertNote });
        return new OptionsBlock(driverId, friendly.OrderBy(f => f.Order).Select(f => f.View).ToArray(), more);
    }

    public static OptionView View(DriverSetting s, FriendlyOption? f, string envPrefix = "")
    {
        var v = ViewOf(s, f, envPrefix);
        return s.Status is { } st && StatusOf(v, st) is { } sv ? v with { Status = sv } : v;
    }

    /// <summary>
    /// The row's status line from the driver's report: nothing for applies / unknown; for not supported, the
    /// chip, the driver's note (or a plain fallback) and the value used instead named with the row's own labels.
    /// </summary>
    public static OptionStatusView? StatusOf(OptionView v, SettingStatus st)
    {
        switch (st.State)
        {
            case SettingState.NotSupported:
                return new OptionStatusView(st.State, NotSupportedChip, st.Note ?? NotSupportedFallbackNote,
                                            st.EffectiveValue is { } ev ? ValueWords(v, ev) : null);
            case SettingState.RestartRequired:
                return new OptionStatusView(st.State, RestartChip, st.Note, null);
            default:
                return null;
        }
    }

    /// <summary>A value of the setting in the words the row gives it: its choice label (aliases counted), On / Off, else quoted.</summary>
    public static string ValueWords(OptionView v, string value)
    {
        string w = value.Trim();
        if (v.Entry?.Aliases is { } al && al.TryGetValue(w, out var a)) w = a;
        var label = v.Choices.FirstOrDefault(c => string.Equals(c.Value, w, StringComparison.OrdinalIgnoreCase))?.Label
                    ?? v.Entry?.Choices.FirstOrDefault(c => string.Equals(c.Value, w, StringComparison.OrdinalIgnoreCase))?.Label;
        if (label is not null) return label;
        if (v.Control is OptionControl.Switch) return IsTrue(w) ? "On" : "Off";
        return Quoted(w);
    }

    private static OptionView ViewOf(DriverSetting s, FriendlyOption? f, string envPrefix)
    {
        bool readOnly = s.Source == SettingSource.Environment;
        bool canReset = !readOnly && s.Source == SettingSource.User;
        string source = SourceText(s.Source);
        var tip = new List<string> { $"{s.Key} ({TypeWord(s.Type)})" };
        if (s.Type != SettingType.Secret && s.Default.Length > 0) tip.Add($"default {s.Default}");
        if (s.Help.Length > 0) tip.Add(s.Help);
        if (envPrefix.Length > 0) tip.Add($"environment: {envPrefix}{s.Key}");
        string tooltip = string.Join("\n", tip);

        if (s.Type == SettingType.Secret)
            return new OptionView(s, s.Key, OptionControl.Secret, s.Help, false, Array.Empty<OptionChoice>(), null, s.IsSet, null,
                                  s.IsSet ? "Set" : "Not set", source, readOnly, canReset && s.IsSet, null, tooltip, null);

        string effective = s.Effective;
        if (f is null && s.Type == SettingType.Enum && s.Values is { Count: > 0 } words)
        {
            // Raw, with the words the CLI lists: a drop-down, the raw words as labels.
            var raw = words.Select(w => new OptionChoice(w, w)).ToArray();
            var hit = raw.FirstOrDefault(c => string.Equals(c.Value, effective.Trim(), StringComparison.OrdinalIgnoreCase));
            return new OptionView(s, s.Key, OptionControl.Choice, s.Help, false, raw, hit?.Value, false, null,
                                  hit?.Label ?? Quoted(effective), source, readOnly, canReset, null, tooltip, null);
        }
        if (f is null)
        {
            // Raw: the key is the label and the driver's own help is the help. An enum whose values the
            // CLI does not list is a text box too.
            var control = s.Type == SettingType.Bool ? OptionControl.Switch : OptionControl.Text;
            bool on = s.Type == SettingType.Bool && IsTrue(effective);
            string text = s.Type == SettingType.Bool ? (on ? "On" : "Off") : effective.Length > 0 ? effective : "Not set";
            return new OptionView(s, s.Key, control, s.Help, false, Array.Empty<OptionChoice>(), null, on, null,
                                  text, source, readOnly, canReset, null, tooltip, null);
        }

        switch (f.Control)
        {
            case OptionControl.Choice:
            {
                string value = effective.Trim();
                if (f.NoKindValues?.Any(x => string.Equals(x, value, StringComparison.OrdinalIgnoreCase)) == true)
                    return new OptionView(s, f.Label, OptionControl.Choice, f.Help, true, f.Choices, null, false, null,
                                          FriendlyOptions.NoKindText, source, readOnly, canReset, null, tooltip, f, NoKind: true);
                string resolved = f.Aliases is not null && f.Aliases.TryGetValue(value, out var a) ? a : value;
                var choices = ChoicesFor(f, s.Values);
                var choice = choices.FirstOrDefault(c => string.Equals(c.Value, resolved, StringComparison.OrdinalIgnoreCase));
                string? note = choice is not null && f.Notes is not null && f.Notes.TryGetValue(choice.Value, out var n) ? n : null;
                return new OptionView(s, f.Label, OptionControl.Choice, f.Help, true, choices, choice?.Value, false, null,
                                      choice?.Label ?? Quoted(effective), source, readOnly, canReset, note, tooltip, f);
            }
            case OptionControl.TwoWay:
            {
                bool on = IsTrue(effective);
                var choice = f.Choices.Count >= 2 ? f.Choices[on ? 1 : 0] : null;
                return new OptionView(s, f.Label, OptionControl.TwoWay, f.Help, true, f.Choices, choice?.Value, on, null,
                                      choice?.Label ?? Quoted(effective), source, readOnly, canReset, null, tooltip, f);
            }
            case OptionControl.Pin:
            {
                bool on = f.PinValue is not null && string.Equals(effective.Trim(), f.PinValue, StringComparison.OrdinalIgnoreCase);
                var other = on ? null : f.Choices.FirstOrDefault(c => string.Equals(c.Value, effective.Trim(), StringComparison.OrdinalIgnoreCase));
                string text = on ? "On" : other?.Label ?? "Off";
                return new OptionView(s, f.Label, OptionControl.Pin, f.Help, true, f.Choices, other?.Value, on, other?.Label,
                                      text, source, readOnly, canReset, null, tooltip, f);
            }
            case OptionControl.Switch:
            {
                bool on = IsTrue(effective);
                return new OptionView(s, f.Label, OptionControl.Switch, f.Help, true, f.Choices, null, on, null,
                                      on ? "On" : "Off", source, readOnly, canReset, null, tooltip, f);
            }
            default:
                return new OptionView(s, f.Label, OptionControl.Text, f.Help, true, f.Choices, null, false, null,
                                      effective.Length > 0 ? effective : "Not set", source, readOnly, canReset, null, tooltip, f);
        }
    }

    private static string Quoted(string v) => v.Length > 0 ? $"“{v}”" : "Not set";

    /// <summary>
    /// A friendly drop-down's values against the words the CLI lists (<paramref name="cliValues"/>, null = it
    /// lists none: the entry's own, unchanged). The entry keeps its labels and order, but drops a value the CLI
    /// does not list (nor any spelling of it, <see cref="FriendlyOption.Aliases"/>), and appends, as its raw word,
    /// one the CLI lists that the entry does not know (not a value, an alias or a no-kind value of the entry).
    /// </summary>
    public static IReadOnlyList<OptionChoice> ChoicesFor(FriendlyOption f, IReadOnlyList<string>? cliValues)
    {
        if (cliValues is not { Count: > 0 }) return f.Choices;
        var listed = new HashSet<string>(cliValues, StringComparer.OrdinalIgnoreCase);
        bool Listed(OptionChoice c) => listed.Contains(c.Value)
            || (f.Aliases?.Any(kv => listed.Contains(kv.Key) && string.Equals(kv.Value, c.Value, StringComparison.OrdinalIgnoreCase)) ?? false);
        bool Known(string w) => f.Choices.Any(c => string.Equals(c.Value, w, StringComparison.OrdinalIgnoreCase))
            || (f.Aliases?.ContainsKey(w) ?? false)
            || (f.NoKindValues?.Any(x => string.Equals(x, w, StringComparison.OrdinalIgnoreCase)) ?? false);
        return f.Choices.Where(Listed).Concat(cliValues.Where(w => !Known(w)).Select(w => new OptionChoice(w, w))).ToArray();
    }

    private static string TypeWord(SettingType t) => t.ToString().ToLowerInvariant();

    /// <summary>
    /// A text box's value checked before it is written: an int as a whole number, a float as a
    /// number with a dot for decimals (no comma), anything else as one line, kept as typed (not only
    /// spaces, no NUL). A value starting with "--" is
    /// refused (the CLI would read it as a flag). Null with <paramref name="error"/> set when it
    /// cannot be written.
    /// </summary>
    public static string? Validate(SettingType type, string? text, out string? error)
    {
        error = null;
        string raw = text ?? "";
        // A string keeps exactly what was typed; a number or a bool may carry spaces around it.
        string v = type is SettingType.Int or SettingType.Float or SettingType.Bool ? raw.Trim() : raw;
        if (raw.Trim().Length == 0) { error = "Type a value first."; return null; }
        if (raw.Contains('\0')) { error = "That value has a character that can't be stored."; return null; }
        if (raw.Contains('\n') || raw.Contains('\r')) { error = "One line only."; return null; }
        if (v.StartsWith("--", StringComparison.Ordinal)) { error = "A value can't start with two dashes."; return null; }
        switch (type)
        {
            case SettingType.Int:
                if (!long.TryParse(v, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var l))
                {
                    error = "Type a whole number.";
                    return null;
                }
                return l.ToString(CultureInfo.InvariantCulture);
            case SettingType.Float:
                // No thousands separators and no decimal comma: "1,000" is refused, never read as 1 or 1000.
                if (v.Contains(',') || !double.TryParse(v, NumberStyles.Float, CultureInfo.InvariantCulture, out var d) || !double.IsFinite(d))
                {
                    error = "Type a number, with a dot for decimals.";
                    return null;
                }
                return d.ToString("R", CultureInfo.InvariantCulture);
            case SettingType.Bool:
                return IsTrue(v) ? "1" : v.ToLowerInvariant() is "0" or "false" or "off" or "no" ? "0" : Fail("Type 1 or 0.", out error);
            default:
                return v;
        }
    }

    private static string? Fail(string message, out string? error)
    {
        error = message;
        return null;
    }

    /// <summary>
    /// The value a Pin's switch writes: on, the pin value; off, the driver's declared default, always, for
    /// this user. Off never unsets: <c>dp options</c> reports only the winning layer, so clearing the
    /// user's value could leave a machine-wide pin winning (the switch snaps back) or reveal another one.
    /// "Use the default" is the action that unsets.
    /// </summary>
    public static string PinWrite(OptionView v, bool on) =>
        on ? v.Entry?.PinValue ?? "1" : v.Setting.Default;
}

/// <summary>The CLI verbs of the Options block: argument vectors, never strings.</summary>
public static class OptionCommands
{
    /// <summary>The action area the Options block's writes report under.</summary>
    public const string Area = "dp-options";

    public static CliArgs Read(string id) => new("dp", "options", id, "--json");
    public static CliArgs Set(string id, string key, string value) => new("dp", "set", id, key, value);
    public static CliArgs Unset(string id, string key) => new("dp", "unset", id, key);

    /// <summary>A secret's write: the value goes on standard input, never in the argument vector.</summary>
    public static CliArgs SetStdin(string id, string key) => new("dp", "set", id, key, "--stdin");

    /// <summary>What the Options block says when the CLI cannot take a value on standard input.</summary>
    public const string SecretSetUnsupported = "This version of DisplayXR can only clear this value.";

    /// <summary>
    /// A <c>set ... --stdin</c> refused by a CLI that predates <c>--stdin</c>: it skips the unknown flag, finds
    /// two arguments and answers that <c>set</c> needs <c>&lt;id&gt; &lt;KEY&gt; &lt;value&gt;</c>. A CLI that knows
    /// the flag names it in every refusal of this verb, so its own errors (an empty value, too long) are not
    /// mistaken for this.
    /// </summary>
    public static bool IsStdinUnsupported(CliResult r)
    {
        if (r.Ok) return false;
        string text = r.Stdout + "\n" + r.Stderr;
        return text.Contains("needs <id> <KEY> <value>", StringComparison.Ordinal)
               && !text.Contains("--stdin", StringComparison.Ordinal);
    }

    /// <summary>
    /// A secret's text box, checked before it is written: one line, not empty, no NUL, at most the 2047
    /// bytes (UTF-8) the store takes. Null with <paramref name="error"/> set when it cannot be written. The
    /// value is not trimmed: a credential is written as typed.
    /// </summary>
    public static string? ValidateSecret(string? text, out string? error)
    {
        error = null;
        string v = text ?? "";
        if (v.Length == 0) { error = "Type a value first."; return null; }
        if (v.Contains('\0')) { error = "That value has a character that can't be stored."; return null; }
        if (v.Contains('\n') || v.Contains('\r')) { error = "One line only."; return null; }
        if (System.Text.Encoding.UTF8.GetByteCount(v) > SecretMaxBytes) { error = "That value is too long."; return null; }
        return v;
    }

    /// <summary>The longest secret the store takes, in bytes (2048 with the terminator).</summary>
    public const int SecretMaxBytes = 2047;
}

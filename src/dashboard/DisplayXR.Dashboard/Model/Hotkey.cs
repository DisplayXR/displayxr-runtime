// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;

namespace DisplayXR.Dashboard.Model;

[Flags]
public enum HotkeyModifiers { None = 0, Ctrl = 1, Shift = 2, Alt = 4, Win = 8 }

/// <summary>
/// A workspace-controller launch combo, as the CLI spells it: modifiers in the
/// order Ctrl, Shift, Alt, Win, then one key, joined by '+' ("Ctrl+Space",
/// "Ctrl+Shift+F9"). Valid = at least one modifier and exactly one key.
/// </summary>
public readonly record struct Hotkey(HotkeyModifiers Modifiers, string Key)
{
    private static readonly Dictionary<string, HotkeyModifiers> ModNames = new(StringComparer.OrdinalIgnoreCase)
    {
        ["ctrl"] = HotkeyModifiers.Ctrl, ["control"] = HotkeyModifiers.Ctrl, ["ctl"] = HotkeyModifiers.Ctrl,
        ["shift"] = HotkeyModifiers.Shift,
        ["alt"] = HotkeyModifiers.Alt, ["menu"] = HotkeyModifiers.Alt,
        ["win"] = HotkeyModifiers.Win, ["windows"] = HotkeyModifiers.Win, ["meta"] = HotkeyModifiers.Win,
        ["super"] = HotkeyModifiers.Win, ["cmd"] = HotkeyModifiers.Win,
    };

    private static readonly Dictionary<string, string> KeyAliases = new(StringComparer.OrdinalIgnoreCase)
    {
        ["return"] = "Enter", ["escape"] = "Esc", ["back"] = "Backspace", ["del"] = "Delete", ["ins"] = "Insert",
        ["pgup"] = "PageUp", ["prior"] = "PageUp", ["pgdn"] = "PageDown", ["next"] = "PageDown",
        ["spacebar"] = "Space", ["printscreen"] = "PrintScreen", ["prtsc"] = "PrintScreen",
    };

    private static readonly HashSet<string> NamedKeys = new(StringComparer.OrdinalIgnoreCase)
    {
        "Space", "Tab", "Enter", "Esc", "Backspace", "Delete", "Insert", "Home", "End", "PageUp", "PageDown",
        "Up", "Down", "Left", "Right", "Pause", "PrintScreen", "ScrollLock", "CapsLock", "NumLock",
        "`", "-", "=", "[", "]", "\\", ";", "'", ",", ".", "/",
        "Num0", "Num1", "Num2", "Num3", "Num4", "Num5", "Num6", "Num7", "Num8", "Num9",
        "NumAdd", "NumSubtract", "NumMultiply", "NumDivide", "NumDecimal",
    };

    public bool IsValid => Modifiers != HotkeyModifiers.None && Key.Length > 0;

    /// <summary>The canonical spelling, e.g. "Ctrl+Shift+F9".</summary>
    public override string ToString()
    {
        var parts = new List<string>();
        if (Modifiers.HasFlag(HotkeyModifiers.Ctrl)) parts.Add("Ctrl");
        if (Modifiers.HasFlag(HotkeyModifiers.Shift)) parts.Add("Shift");
        if (Modifiers.HasFlag(HotkeyModifiers.Alt)) parts.Add("Alt");
        if (Modifiers.HasFlag(HotkeyModifiers.Win)) parts.Add("Win");
        if (Key.Length > 0) parts.Add(Key);
        return string.Join("+", parts);
    }

    /// <summary>A key token in its canonical spelling, or null if it is not a key this control accepts.</summary>
    public static string? NormalizeKey(string token)
    {
        string t = token.Trim();
        if (t.Length == 0) return null;
        if (KeyAliases.TryGetValue(t, out var alias)) return alias;
        if (t.Length == 1 && char.IsLetterOrDigit(t[0])) return char.ToUpperInvariant(t[0]).ToString();
        if ((t[0] == 'F' || t[0] == 'f') && int.TryParse(t[1..], out int f) && f >= 1 && f <= 24) return "F" + f;
        var named = NamedKeys.FirstOrDefault(k => string.Equals(k, t, StringComparison.OrdinalIgnoreCase));
        return named;
    }

    /// <summary>
    /// Parse "Ctrl+Shift+F9" (case-insensitive, aliases accepted, whitespace
    /// around '+' ignored). "+" itself as the key is written "Plus".
    /// </summary>
    public static bool TryParse(string? text, out Hotkey hotkey, out string? error)
    {
        hotkey = default;
        error = null;
        if (string.IsNullOrWhiteSpace(text)) { error = "empty"; return false; }
        var mods = HotkeyModifiers.None;
        string? key = null;
        foreach (var raw in text.Split('+'))
        {
            string tok = raw.Trim();
            if (tok.Length == 0) { error = "empty part"; return false; }
            if (ModNames.TryGetValue(tok, out var m))
            {
                if (mods.HasFlag(m)) { error = $"'{tok}' twice"; return false; }
                mods |= m;
                continue;
            }
            string? k = string.Equals(tok, "Plus", StringComparison.OrdinalIgnoreCase) ? "Plus" : NormalizeKey(tok);
            if (k is null) { error = $"unknown key '{tok}'"; return false; }
            if (key is not null) { error = "more than one key"; return false; }
            key = k;
        }
        if (key is null) { error = "no key, only modifiers"; return false; }
        if (mods == HotkeyModifiers.None) { error = "needs at least one of Ctrl / Shift / Alt / Win"; return false; }
        hotkey = new Hotkey(mods, key);
        return true;
    }

    /// <summary>The reason a captured combo is not acceptable, or null.</summary>
    public static string? Validate(HotkeyModifiers mods, string? key) =>
        key is null || key.Length == 0 ? "Press a key together with the modifiers."
        : mods == HotkeyModifiers.None ? "Use at least one of Ctrl / Shift / Alt / Win plus a key."
        : null;
}

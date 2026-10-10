// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using Avalonia.Input;
using DisplayXR.Dashboard.Model;

namespace DisplayXR.Dashboard.Ui;

/// <summary>Avalonia key events → the hotkey spelling of <see cref="Hotkey"/>.</summary>
public static class KeyNames
{
    public static bool IsModifier(Key k) => k is Key.LeftCtrl or Key.RightCtrl or Key.LeftShift or Key.RightShift
        or Key.LeftAlt or Key.RightAlt or Key.LWin or Key.RWin or Key.System;

    public static HotkeyModifiers Modifiers(KeyModifiers m)
    {
        var r = HotkeyModifiers.None;
        if (m.HasFlag(KeyModifiers.Control)) r |= HotkeyModifiers.Ctrl;
        if (m.HasFlag(KeyModifiers.Shift)) r |= HotkeyModifiers.Shift;
        if (m.HasFlag(KeyModifiers.Alt)) r |= HotkeyModifiers.Alt;
        if (m.HasFlag(KeyModifiers.Meta)) r |= HotkeyModifiers.Win;
        return r;
    }

    /// <summary>The key's name, or null for a key the control does not accept.</summary>
    public static string? Name(Key k)
    {
        if (k >= Key.A && k <= Key.Z) return ((char)('A' + (k - Key.A))).ToString();
        if (k >= Key.D0 && k <= Key.D9) return ((char)('0' + (k - Key.D0))).ToString();
        if (k >= Key.NumPad0 && k <= Key.NumPad9) return "Num" + (k - Key.NumPad0);
        if (k >= Key.F1 && k <= Key.F24) return "F" + (k - Key.F1 + 1);
        return k switch
        {
            Key.Space => "Space", Key.Tab => "Tab", Key.Enter => "Enter", Key.Back => "Backspace",
            Key.Delete => "Delete", Key.Insert => "Insert", Key.Home => "Home", Key.End => "End",
            Key.PageUp => "PageUp", Key.PageDown => "PageDown",
            Key.Up => "Up", Key.Down => "Down", Key.Left => "Left", Key.Right => "Right",
            Key.Pause => "Pause", Key.PrintScreen => "PrintScreen", Key.Scroll => "ScrollLock",
            Key.OemTilde => "`", Key.OemMinus => "-", Key.OemPlus => "=", Key.OemOpenBrackets => "[",
            Key.OemCloseBrackets => "]", Key.OemPipe => "\\", Key.OemSemicolon => ";", Key.OemQuotes => "'",
            Key.OemComma => ",", Key.OemPeriod => ".", Key.OemQuestion => "/",
            Key.Add => "NumAdd", Key.Subtract => "NumSubtract", Key.Multiply => "NumMultiply",
            Key.Divide => "NumDivide", Key.Decimal => "NumDecimal",
            _ => null,
        };
    }
}

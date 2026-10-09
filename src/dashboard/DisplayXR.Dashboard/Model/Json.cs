// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Collections.Generic;
using System.Globalization;
using System.Text.Json;

namespace DisplayXR.Dashboard.Model;

/// <summary>
/// Tolerant readers over <see cref="JsonElement"/>. The CLI's JSON only ever
/// adds keys (ADR-051 §3), and every renderer must survive an older or newer
/// CLI: a missing key, a <c>null</c> or a value of the wrong type reads as the
/// fallback instead of throwing.
/// </summary>
internal static class Json
{
    public static JsonElement? Obj(this JsonElement e, string key) =>
        e.ValueKind == JsonValueKind.Object && e.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.Object ? v : null;

    public static JsonElement? Obj(this JsonElement? e, string key) => e is { } x ? x.Obj(key) : null;

    public static IEnumerable<JsonElement> Arr(this JsonElement e, string key)
    {
        if (e.ValueKind == JsonValueKind.Object && e.TryGetProperty(key, out var v) && v.ValueKind == JsonValueKind.Array)
            foreach (var item in v.EnumerateArray()) yield return item;
    }

    public static IEnumerable<JsonElement> Arr(this JsonElement? e, string key) =>
        e is { } x ? x.Arr(key) : System.Array.Empty<JsonElement>();

    public static string Str(this JsonElement e, string key, string fallback = "")
    {
        if (e.ValueKind != JsonValueKind.Object || !e.TryGetProperty(key, out var v)) return fallback;
        return v.ValueKind switch
        {
            JsonValueKind.String => v.GetString() ?? fallback,
            JsonValueKind.Number => v.GetRawText(),
            JsonValueKind.True => "true",
            JsonValueKind.False => "false",
            _ => fallback,
        };
    }

    public static string Str(this JsonElement? e, string key, string fallback = "") => e is { } x ? x.Str(key, fallback) : fallback;

    /// <summary>The string, or null when the key is absent, null or empty.</summary>
    public static string? StrOrNull(this JsonElement e, string key)
    {
        var s = e.Str(key, "");
        return s.Length == 0 ? null : s;
    }

    public static string? StrOrNull(this JsonElement? e, string key) => e is { } x ? x.StrOrNull(key) : null;

    public static double Num(this JsonElement e, string key, double fallback = 0)
    {
        if (e.ValueKind != JsonValueKind.Object || !e.TryGetProperty(key, out var v)) return fallback;
        if (v.ValueKind == JsonValueKind.Number && v.TryGetDouble(out var d) && double.IsFinite(d)) return d;
        if (v.ValueKind == JsonValueKind.String &&
            double.TryParse(v.GetString(), NumberStyles.Float, CultureInfo.InvariantCulture, out var p) && double.IsFinite(p)) return p;
        return fallback;
    }

    public static double Num(this JsonElement? e, string key, double fallback = 0) => e is { } x ? x.Num(key, fallback) : fallback;

    public static long Long(this JsonElement e, string key, long fallback = 0)
    {
        double d = e.Num(key, double.NaN);
        if (double.IsNaN(d)) return fallback;
        if (d >= long.MaxValue) return long.MaxValue;
        if (d <= long.MinValue) return long.MinValue;
        return (long)d;
    }

    public static long Long(this JsonElement? e, string key, long fallback = 0) => e is { } x ? x.Long(key, fallback) : fallback;

    public static int Int(this JsonElement e, string key, int fallback = 0)
    {
        long l = e.Long(key, fallback);
        return l > int.MaxValue ? int.MaxValue : l < int.MinValue ? int.MinValue : (int)l;
    }

    public static int Int(this JsonElement? e, string key, int fallback = 0) => e is { } x ? x.Int(key, fallback) : fallback;

    public static bool Bool(this JsonElement e, string key, bool fallback = false)
    {
        if (e.ValueKind != JsonValueKind.Object || !e.TryGetProperty(key, out var v)) return fallback;
        return v.ValueKind switch
        {
            JsonValueKind.True => true,
            JsonValueKind.False => false,
            JsonValueKind.Number => v.TryGetDouble(out var d) && d != 0,
            _ => fallback,
        };
    }

    public static bool Bool(this JsonElement? e, string key, bool fallback = false) => e is { } x ? x.Bool(key, fallback) : fallback;

    public static bool Has(this JsonElement e, string key) => e.ValueKind == JsonValueKind.Object && e.TryGetProperty(key, out _);
}

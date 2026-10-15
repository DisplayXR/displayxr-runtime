// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using System.Text.Json;

namespace DisplayXR.Dashboard.Model;

// ADR-054 N3e (#2052): the per-class policy the Components page can change —
// Enabled and Preferred for display processors, input providers and conversion
// providers. The dashboard renders what `<class> list --json` returns and drives
// every change through the CLI (ADR-051 D4, ADR-054 decision 6): no registry
// access here, ever. Rows are read tolerantly: a CLI that predates a field
// reads as "unknown", never as a crash.

/// <summary>The three component classes with a policy store (ADR-054 §2).</summary>
public enum ComponentClass
{
    Display,
    Input,
    Conversion,
}

public static class ComponentClasses
{
    /// <summary>The CLI verb family: <c>dp</c>, <c>input</c>, <c>conv</c>.</summary>
    public static string Verb(this ComponentClass c) => c switch
    {
        ComponentClass.Display => "dp",
        ComponentClass.Input => "input",
        _ => "conv",
    };

    /// <summary>The ADR-054 N3 vocabulary: one noun per class, never "plug-in" or "driver".</summary>
    public static string Noun(this ComponentClass c) => c switch
    {
        ComponentClass.Display => "display processor",
        ComponentClass.Input => "input provider",
        _ => "conversion provider",
    };

    /// <summary>The Components section id the class's rows live in.</summary>
    public static string SectionId(this ComponentClass c) => c switch
    {
        ComponentClass.Display => "display_processors",
        ComponentClass.Input => "input_providers",
        _ => "conversion_providers",
    };

    /// <summary>
    /// When a change takes effect, per class (ADR-054 N3d / plugin-discovery §4.2):
    /// the CLI prints the exact line after a write; this is the standing summary.
    /// </summary>
    public static string AppliesNote(this ComponentClass c) => c switch
    {
        ComponentClass.Display => "Applies now on secondary screens; the primary screen and in-process apps at the next session.",
        ComponentClass.Input => "Applies now when the service is running (providers are re-ranked live); otherwise at the next start.",
        _ => "Applies to the next stream or job; a live stream never moves.",
    };

    public static readonly ComponentClass[] All = { ComponentClass.Display, ComponentClass.Input, ComponentClass.Conversion };
}

/// <summary>
/// One row of <c>dp|input|conv list --json</c>: the policy-relevant fields.
/// <see cref="EnabledSource"/> / <see cref="Provenance"/> are the CLI's own words
/// (<c>user</c> / <c>machine</c> / <c>manifest</c> / <c>default</c>; <c>signed</c> /
/// <c>allowlisted</c> / <c>platform-trusted</c> / <c>dev-override</c> / <c>unverified</c>),
/// shown verbatim; "" when the CLI predates the field.
/// </summary>
public sealed record PolicyRow(ComponentClass Class, string Id, string Name, bool Enabled, string EnabledSource,
                               bool Preferred, bool Active, string Load, string Provenance, string? Signer,
                               string? ProvenanceReason, bool Hidden = false)
{
    /// <summary>Whether the CLI reported an Enabled value at all (older CLIs do not).</summary>
    public bool EnabledKnown => EnabledSource.Length > 0;

    /// <summary>The chip for the Enabled state and where it came from: "enabled" is the quiet default.</summary>
    public (string Text, Level Level)? EnabledChip => !EnabledKnown ? null
        : Enabled ? (EnabledSource is "user" or "machine" ? ($"enabled ({EnabledSource})", Level.Plain) : null)
        : ($"disabled ({EnabledSource})", Level.Plain);

    /// <summary>The provenance chip (ADR-054 N4b): what the loader decided about the binary.</summary>
    public (string Text, Level Level)? ProvenanceChip => Provenance switch
    {
        null or "" or "unknown" => null,
        "signed" => (Signer is { Length: > 0 } s ? $"signed by {s}" : "signed", Level.Ok),
        "allowlisted" => ("allowlisted", Level.Ok),
        "platform-trusted" => ("platform-trusted", Level.Ok),
        "dev-override" => ("dev override", Level.Info),
        "unverified" => ("unverified", Level.Warn),
        var other => (other, Level.Plain),
    };

    /// <summary>The CLI refused the binary (enforcing provenance gate): the row needs an allowlist row, not a toggle.</summary>
    public bool Untrusted => string.Equals(Load, "UNTRUSTED", StringComparison.Ordinal);
}

/// <summary>A conversion task as <c>conv list --json</c> lists it: which provider offers it and whether selection picked that one.</summary>
public sealed record PolicyTask(string Task, string Provider, bool Selected, string Verdict);

/// <summary>
/// <c>&lt;class&gt; list --json</c> as the policy controls see it. <see cref="Preferred"/> is the
/// global preference (display / input); conversion prefers per task (<see cref="PreferredByTask"/>,
/// from the <c>policy.preferred</c> object).
/// </summary>
public sealed record PolicyList(ComponentClass Class, string? Preferred, IReadOnlyList<PolicyRow> Rows,
                                IReadOnlyDictionary<string, string> PreferredByTask, IReadOnlyList<PolicyTask> Tasks,
                                bool ForceQwerty = false)
{
    public PolicyRow? Row(string id) => Rows.FirstOrDefault(r => r.Id == id);

    /// <summary>The tasks a conversion provider offers, in the CLI's order.</summary>
    public IEnumerable<PolicyTask> TasksOf(string providerId) => Tasks.Where(t => t.Provider == providerId);

    /// <summary>True when any row carries an Enabled value: the CLI has the N3 verbs, so the controls can show.</summary>
    public bool Writable => Rows.Any(r => r.EnabledKnown) || Class == ComponentClass.Conversion && Rows.Count > 0;

    public static bool TryParse(ComponentClass cls, string? json, out PolicyList? list)
    {
        list = null;
        if (string.IsNullOrWhiteSpace(json)) return false;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object) return false;
            string arrayKey = cls == ComponentClass.Display ? "plugins" : "providers";
            if (!root.Has(arrayKey)) return false; // not this verb's output (an older CLI, "unknown command")
            var rows = root.Arr(arrayKey).Where(e => e.ValueKind == JsonValueKind.Object && e.Str("id").Length > 0)
                .Select(e => new PolicyRow(cls, e.Str("id"), e.Str("display_name", e.Str("name", e.Str("id"))),
                    // conv rows always carry `enabled`; dp / input rows only from the N3 CLI (enabled_source says so).
                    e.Has("enabled") ? e.Bool("enabled") : true,
                    e.Str("enabled_source", cls == ComponentClass.Conversion && e.Has("enabled") ? "default" : ""),
                    e.Bool("preferred"), e.Bool("active"), e.Str("load", e.Str("load_result")),
                    e.Str("provenance"), e.StrOrNull("signer"), e.StrOrNull("provenance_reason"), e.Bool("hidden")))
                .ToArray();
            var byTask = new Dictionary<string, string>(StringComparer.Ordinal);
            var tasks = new List<PolicyTask>();
            if (cls == ComponentClass.Conversion)
            {
                if (root.Obj("policy").Obj("preferred") is { } pref)
                    foreach (var p in pref.EnumerateObject())
                        if (p.Value.ValueKind == JsonValueKind.String && p.Value.GetString() is { Length: > 0 } v) byTask[p.Name] = v;
                tasks.AddRange(root.Arr("tasks").Where(t => t.ValueKind == JsonValueKind.Object && t.Str("task").Length > 0)
                    .Select(t => new PolicyTask(t.Str("task"), t.Str("provider"), t.Bool("selected"), t.Str("verdict"))));
            }
            list = new PolicyList(cls, root.StrOrNull("preferred"), rows, byTask, tasks, root.Bool("force_qwerty"));
            return true;
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException) { return false; }
    }
}

/// <summary>
/// The CLI lines the controls run — pure, so a test can pin them. Per-user by
/// default; <paramref name="machine"/> adds <c>--machine</c> (HKLM), which only an
/// elevated dashboard may attempt (the page shows the command instead otherwise).
/// </summary>
public static class PolicyCommands
{
    private static string Q(string s) => StatusText.Quote(s);
    private static string M(bool machine) => machine ? " --machine" : "";

    public static string Enable(ComponentClass c, string id, bool machine = false) => $"{c.Verb()} enable {Q(id)}{M(machine)}";
    public static string Disable(ComponentClass c, string id, bool machine = false) => $"{c.Verb()} disable {Q(id)}{M(machine)}";

    /// <summary>Global preference (display / input).</summary>
    public static string Prefer(ComponentClass c, string id, bool machine = false) => $"{c.Verb()} use {Q(id)}{M(machine)}";
    public static string Reset(ComponentClass c, bool machine = false) =>
        c == ComponentClass.Conversion ? $"conv reset all{M(machine)}" : $"{c.Verb()} reset{M(machine)}";

    /// <summary>Conversion prefers per task: <c>conv use &lt;task&gt; &lt;id&gt;</c> / <c>conv reset &lt;task&gt;</c>.</summary>
    public static string PreferTask(string task, string id, bool machine = false) => $"conv use {Q(task)} {Q(id)}{M(machine)}";
    public static string ResetTask(string task, bool machine = false) => $"conv reset {Q(task)}{M(machine)}";

    /// <summary>The action area a class's writes report under (DashboardContext.RunActionsAsync).</summary>
    public static string Area(ComponentClass c) => "policy-" + c.Verb();
}

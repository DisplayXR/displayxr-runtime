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
/// shown verbatim; "" when the CLI predates the field — never invented here.
/// <see cref="BuiltIn"/>: the conversion class's built-in <c>dp-lift</c> adapter, which has
/// no Enabled value and is never a provider to prefer (the CLI refuses
/// <c>conv enable|disable dp-lift</c>).
/// </summary>
public sealed record PolicyRow(ComponentClass Class, string Id, string Name, bool Enabled, string EnabledSource,
                               bool Preferred, bool Active, string Load, string Provenance, string? Signer,
                               string? ProvenanceReason, bool Hidden = false, bool BuiltIn = false)
{
    /// <summary>
    /// Whether the row has an Enabled value the CLI reported (older CLIs do not, and a
    /// built-in row has none): the Enabled control shows only then.
    /// </summary>
    public bool EnabledKnown => EnabledSource.Length > 0 && !BuiltIn;

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
/// from the <c>policy.preferred</c> object). <see cref="PolicyLock"/> is the CLI's
/// <c>policy.policy_lock</c>: HKLM <c>Policy\Lock</c> = 1, so per-user values are ignored.
/// </summary>
public sealed record PolicyList(ComponentClass Class, string? Preferred, IReadOnlyList<PolicyRow> Rows,
                                IReadOnlyDictionary<string, string> PreferredByTask, IReadOnlyList<PolicyTask> Tasks,
                                bool ForceQwerty = false, bool PolicyLock = false)
{
    /// <summary>
    /// What the lock hides from a per-user write: every class's per-user Enabled, and for
    /// conversion also the per-user per-task preference (conversion-provider-discovery.md
    /// §3.1). A display / input <c>PreferredPlugin</c> is not under the lock.
    /// </summary>
    public bool LockHidesUserEnabled => PolicyLock;
    public bool LockHidesUserPreference => PolicyLock && Class == ComponentClass.Conversion;

    /// <summary>The section's "locked" line, or null when the class is not locked.</summary>
    public string? LockText => !PolicyLock ? null
        : Class == ComponentClass.Conversion
            ? "Locked by machine policy (HKLM Policy\\Lock): per-user Enabled values and per-task preferences are ignored. An administrator changes the machine-wide values (--machine)."
            : "Locked by machine policy (HKLM Policy\\Lock): per-user Enabled values are ignored. An administrator changes the machine-wide value (--machine).";

    public PolicyRow? Row(string id) => Rows.FirstOrDefault(r => r.Id == id);

    /// <summary>The tasks a conversion provider offers, in the CLI's order.</summary>
    public IEnumerable<PolicyTask> TasksOf(string providerId) => Tasks.Where(t => t.Provider == providerId);

    /// <summary>The distinct task names a conversion provider offers, in the CLI's order.</summary>
    public IEnumerable<string> TaskNamesOf(string providerId) => TasksOf(providerId).Select(t => t.Task).Distinct();

    /// <summary>Is <paramref name="providerId"/> the preference for <paramref name="task"/> (a bare id or <c>id/cap</c>)?</summary>
    public bool IsPreferredFor(string task, string providerId) =>
        PreferredByTask.TryGetValue(task, out var pref)
        && (pref == providerId || pref.StartsWith(providerId + "/", StringComparison.Ordinal));

    /// <summary>True when any row carries an Enabled value: the CLI has the N3 verbs, so the controls can show.</summary>
    public bool Writable => Rows.Any(r => r.EnabledKnown) || Class == ComponentClass.Conversion && Rows.Count > 0;

    /// <summary>The class whose Components section is <paramref name="sectionId"/>, if any.</summary>
    public static ComponentClass? ClassOfSection(string sectionId) =>
        ComponentClasses.All.Cast<ComponentClass?>().FirstOrDefault(c => c!.Value.SectionId() == sectionId);

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
                    e.Has("enabled") ? e.Bool("enabled") : true,
                    // The CLI's word for where `enabled` came from; "" (no Enabled control) when it
                    // does not say — a conv row's bare `enabled` is not a policy value.
                    e.Str("enabled_source"),
                    e.Bool("preferred"), e.Bool("active"), e.Str("load", e.Str("load_result")),
                    e.Str("provenance"), e.StrOrNull("signer"), e.StrOrNull("provenance_reason"), e.Bool("hidden"),
                    // `built_in` from a current CLI; the load verdict says the same on an older one.
                    e.Bool("built_in") || string.Equals(e.Str("load"), "BUILT_IN", StringComparison.Ordinal)))
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
            list = new PolicyList(cls, root.StrOrNull("preferred"), rows, byTask, tasks, root.Bool("force_qwerty"),
                                  root.Obj("policy").Bool("policy_lock"));
            return true;
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException) { return false; }
    }
}

/// <summary>
/// The CLI commands the controls run — pure, so a test can pin the argument vectors. Per-user
/// by default; <paramref name="machine"/> adds <c>--machine</c> (HKLM), which only an elevated
/// dashboard may attempt (the page shows the command instead otherwise). Ids travel as single
/// arguments (<see cref="CliArgs"/>), whatever a manifest put in them.
/// </summary>
public static class PolicyCommands
{
    private static CliArgs Cmd(bool machine, params string[] a) => machine ? new CliArgs(a).With("--machine") : new CliArgs(a);

    public static CliArgs Enable(ComponentClass c, string id, bool machine = false) => Cmd(machine, c.Verb(), "enable", id);
    public static CliArgs Disable(ComponentClass c, string id, bool machine = false) => Cmd(machine, c.Verb(), "disable", id);

    /// <summary>The Enabled toggle's command for a row now <paramref name="enabled"/>: the flip.</summary>
    public static CliArgs Toggle(ComponentClass c, string id, bool enabled, bool machine = false) =>
        enabled ? Disable(c, id, machine) : Enable(c, id, machine);

    /// <summary>Global preference (display / input).</summary>
    public static CliArgs Prefer(ComponentClass c, string id, bool machine = false) => Cmd(machine, c.Verb(), "use", id);
    public static CliArgs Reset(ComponentClass c, bool machine = false) =>
        c == ComponentClass.Conversion ? Cmd(machine, "conv", "reset", "all") : Cmd(machine, c.Verb(), "reset");

    /// <summary>Conversion prefers per task: <c>conv use &lt;task&gt; &lt;id&gt;</c> / <c>conv reset &lt;task&gt;</c>.</summary>
    public static CliArgs PreferTask(string task, string id, bool machine = false) => Cmd(machine, "conv", "use", task, id);
    public static CliArgs ResetTask(string task, bool machine = false) => Cmd(machine, "conv", "reset", task);

    /// <summary>The action area a class's writes report under (DashboardContext.RunActionsAsync).</summary>
    public static string Area(ComponentClass c) => "policy-" + c.Verb();

    /// <summary>
    /// The machine-wide (<c>--machine</c>) equivalent of every control the row shows, in the
    /// order they appear: the Enabled flip, then the global Prefer / Reset (display / input)
    /// or each task's Prefer / Reset (conversion). What a non-elevated dashboard prints for an
    /// administrator instead of attempting it. A built-in row has no Enabled and is never
    /// preferred; a task it already holds keeps its Reset.
    /// </summary>
    public static IReadOnlyList<CliArgs> MachineHints(PolicyRow r, PolicyList list)
    {
        var c = r.Class;
        var hints = new List<CliArgs>();
        bool usable = r.Enabled && !r.Untrusted;
        if (r.EnabledKnown && !r.Untrusted) hints.Add(Toggle(c, r.Id, r.Enabled, machine: true));
        if (c != ComponentClass.Conversion)
        {
            if (r.Preferred) hints.Add(Reset(c, machine: true));
            else if (usable) hints.Add(Prefer(c, r.Id, machine: true));
        }
        else
        {
            foreach (var task in list.TaskNamesOf(r.Id))
            {
                if (list.IsPreferredFor(task, r.Id)) hints.Add(ResetTask(task, machine: true));
                else if (!r.BuiltIn && usable) hints.Add(PreferTask(task, r.Id, machine: true));
            }
        }
        return hints;
    }
}

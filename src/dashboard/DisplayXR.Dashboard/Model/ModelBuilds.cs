// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace DisplayXR.Dashboard.Model;

/*
 * Locally built models (conversion-provider-discovery.md §5.6.5): a research-only model whose
 * weights DisplayXR may not publish, built on this PC from its official file by a signed builder.
 * One row per such model in the 2D-to-3D models section. Everything here is read from, and done
 * through, the CLI: `conv models list --json` for the row, `conv research on` + `conv models fetch
 * <pack> --model <name> --accept-license <id> --events` for Build, `conv models cancel|remove`,
 * `conv models license withdraw`. The Dashboard decides nothing the CLI does not say.
 */

/// <summary>One source licence of a buildable model (`local_build.sources[].license`).</summary>
public sealed record BuildLicence(string Id, string Name, string Link, string TextPath, string TextSha256, bool TextVerified,
                                  bool Accepted, DateTimeOffset? AcceptedAt);

public sealed record BuildSource(string Role, long Size, BuildLicence Licence);

/// <summary>One buildable model, as `conv models list --json` lists it.</summary>
public sealed record BuildableModel(
    string Pack, string Manifest, string Name, string Task, bool Installed, long Size,
    string Recipe, string Variant, string ToolId, string ToolCatalogId, string ToolMinVersion,
    bool ToolInstalled, string ToolVersion, bool ToolCurrent,
    long NeedsRamMb, long NeedsDiskMb, IReadOnlyList<BuildSource> Sources, bool LicenceAccepted)
{
    public string Key => Pack + "/" + Name;
    public long DownloadSize => Sources.Sum(s => s.Size);

    /// <summary>The distinct licences of its sources, in order (each needs its own acceptance).</summary>
    public IReadOnlyList<BuildLicence> Licences =>
        Sources.Select(s => s.Licence).GroupBy(l => l.Id).Select(g => g.First()).ToArray();
}

/// <summary>`conv models list --json`: the buildable models, the research gate, the machine.</summary>
public sealed record ModelBuildList(IReadOnlyList<BuildableModel> Models, bool AllowResearchOnly, bool ResearchLockedOff,
                                    long? PhysMb, long? FreeMb, string StoreRoot)
{
    public static bool TryParse(string? json, out ModelBuildList? list)
    {
        list = null;
        if (string.IsNullOrWhiteSpace(json)) return false;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object || !root.Has("packs")) return false;
            var models = new List<BuildableModel>();
            foreach (var p in root.Arr("packs").Where(p => p.ValueKind == JsonValueKind.Object))
            {
                foreach (var m in p.Arr("models").Where(m => m.ValueKind == JsonValueKind.Object))
                {
                    if (m.Obj("local_build") is not { } lb) continue;
                    var tool = lb.Obj("tool");
                    var needs = lb.Obj("needs");
                    var sources = lb.Arr("sources").Where(s => s.ValueKind == JsonValueKind.Object).Select(s =>
                    {
                        var l = s.Obj("license");
                        DateTimeOffset? at = l.Long("accepted_at") is > 0 and var t ? DateTimeOffset.FromUnixTimeSeconds(t) : null;
                        return new BuildSource(s.Str("role"), s.Long("size"),
                            new BuildLicence(l.Str("id"), l.Str("name", l.Str("id")), l.Str("link"), l.Str("text"), l.Str("text_sha256"),
                                             l.Bool("text_verified"), l.Bool("accepted"), at));
                    }).ToArray();
                    models.Add(new BuildableModel(
                        p.Str("pack"), p.Str("manifest"), m.Str("name"), m.Str("task"), m.Bool("installed"), m.Long("size"),
                        lb.Str("recipe"), lb.Str("variant"), tool.Str("id"), tool.Str("catalog_id"), tool.Str("min_version"),
                        lb.Bool("tool_installed"), lb.Str("tool_version"), lb.Bool("tool_current"),
                        needs.Long("ram_mb"), needs.Long("disk_mb"), sources, m.Bool("license_accepted")));
                }
            }
            var research = root.Obj("research");
            var machine = root.Obj("machine");
            list = new ModelBuildList(models, research.Bool("allow_research_only"), research.Bool("locked_off"),
                machine is { } mc && mc.Has("phys_mb") ? mc.Long("phys_mb") : null,
                machine is { } md && md.Has("free_mb") ? md.Long("free_mb") : null,
                root.Str("store_root"));
            return true;
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException) { return false; }
    }
}

/// <summary>The CLI argument vectors behind the row's actions. Never a shell string, never `--installer`.</summary>
public static class ModelBuildCommands
{
    public static CliArgs List => new("conv", "models", "list", "--json");

    /// <summary>The accept tick turns on the per-user research-only gate (§5.6.4): this runs first.</summary>
    public static CliArgs ResearchOn => new("conv", "research", "on");

    public static CliArgs Build(BuildableModel m, bool accept)
    {
        var a = new List<string> { "conv", "models", "fetch", PackArg(m), "--model", m.Name };
        if (accept)
            foreach (var l in m.Licences) { a.Add("--accept-license"); a.Add(l.Id); }
        a.Add("--events");
        return new CliArgs(a);
    }

    public static CliArgs Cancel(BuildableModel m) => new("conv", "models", "cancel", PackArg(m), "--model", m.Name);
    public static CliArgs Remove(BuildableModel m) => new("conv", "models", "remove", PackArg(m), "--model", m.Name, "--json");
    public static CliArgs Withdraw(BuildLicence l) => new("conv", "models", "license", "withdraw", l.Id, "--json");

    /// <summary>The manifest path the CLI listed, else the pack id (the CLI resolves a registered one).</summary>
    private static string PackArg(BuildableModel m) => m.Manifest.Length > 0 ? m.Manifest : m.Pack;
}

/// <summary>Where a build is, from its `--events` stream.</summary>
public sealed record BuildProgress(string Text, double? Fraction, string? Result = null, string? Hint = null, string? Log = null)
{
    public static BuildProgress Starting => new("Starting", null);

    /// <summary>
    /// Apply one NDJSON line. "Downloading the official file: X of Y MB" (the store's
    /// download-source lines), "Converting on this PC: step n of m (phase)" (the builder's phases),
    /// "Checking and storing" (ingest); the result line ends it. Unknown lines change nothing.
    /// </summary>
    public BuildProgress Apply(string line)
    {
        try
        {
            using var doc = JsonDocument.Parse(line);
            var o = doc.RootElement;
            if (o.ValueKind != JsonValueKind.Object || o.Int("v") != 1) return this;
            string ev = o.Str("event"), phase = o.Str("phase");
            switch (ev)
            {
                case "phase" when phase == "download-source":
                    return new BuildProgress("Downloading the official file", null);
                case "progress" when phase == "download-source":
                {
                    long done = o.Long("done"), total = o.Long("total");
                    return new BuildProgress($"Downloading the official file: {Mb(done)} of {Mb(total)} MB",
                                             total > 0 ? Math.Clamp((double)done / total, 0, 1) : null);
                }
                case "phase" when phase == "ingest":
                    return new BuildProgress("Checking and storing", null);
                case "phase":
                {
                    int step = o.Int("step"), steps = o.Int("steps");
                    var text = steps > 0 ? $"Converting on this PC: step {step} of {steps} ({phase})" : $"Converting on this PC ({phase})";
                    return new BuildProgress(text, null);
                }
                case "progress":
                {
                    long done = o.Long("done"), total = o.Long("total");
                    var head = Text.StartsWith("Converting", StringComparison.Ordinal) ? Text : $"Converting on this PC ({phase})";
                    return this with { Text = head, Fraction = total > 0 ? Math.Clamp((double)done / total, 0, 1) : null };
                }
                case "result":
                {
                    var m = o.Arr("models").FirstOrDefault(x => x.ValueKind == JsonValueKind.Object);
                    var build = m.ValueKind == JsonValueKind.Object ? m.Obj("build") : null;
                    return this with
                    {
                        Result = m.ValueKind == JsonValueKind.Object ? m.Str("result", "build-failed") : "build-failed",
                        Hint = m.ValueKind == JsonValueKind.Object ? m.StrOrNull("hint") : null,
                        Log = build.StrOrNull("log"),
                    };
                }
                default: return this;
            }
        }
        catch (JsonException) { return this; }
    }

    private static string Mb(long bytes) => Math.Round(bytes / (1024.0 * 1024.0)).ToString(CultureInfo.InvariantCulture);
}

/// <summary>The one message a finished build shows, by result (§5.6.5). Null for a success.</summary>
public static class ModelBuildMessages
{
    public static (string Text, Level Level)? For(string result, string drive, long needDiskMb, string? log)
    {
        string logPart = log is { Length: > 0 } ? log : "the Dashboard's log";
        return result switch
        {
            "built" or "ok" => null,
            "build-license" => ("The licence was not accepted. Tick the box to build.", Level.Warn),
            "build-license-text" => ("The licence text could not be verified.", Level.Warn),
            "build-download" or "bad-download" => ("The official file could not be downloaded. Check the connection and try again.", Level.Warn),
            "build-source-mismatch" => ("The downloaded file is not the expected one, so nothing was built.", Level.Warn),
            "build-output-mismatch" => ("The built model did not match the expected files, so it was not installed. Update the builder and try again.", Level.Warn),
            "build-memory" => ("This PC ran out of memory while building. Close other apps and try again.", Level.Warn),
            "build-disk" => ($"Not enough free space on {drive}. Free {Gb(needDiskMb)} GB and try again.", Level.Warn),
            "build-unsupported" => ("This PC cannot build this model.", Level.Warn),
            "build-cancelled" => ("Cancelled. Nothing was installed.", Level.Info),
            "build-tool-missing" => ("Get the builder first.", Level.Warn),
            "build-tool-old" => ("Update the builder first.", Level.Warn),
            "build-tool-untrusted" or "build-tool-mismatch" => ("The installed builder could not be verified. Reinstall it from the Catalog.", Level.Warn),
            "policy" => ("Turn on research-only models to build this.", Level.Warn),
            "policy-locked" => (ModelBuildRow.LockedText, Level.Info),
            "busy" => ("Another build of this model is running on this PC. Try again when it has finished.", Level.Info),
            "build-stalled" or "build-failed" => ($"The build stopped unexpectedly. Try again; the log is in {logPart}.", Level.Warn),
            _ => ($"The model could not be stored. Try again; the log is in {logPart}.", Level.Warn),
        };
    }

    public static string Gb(long mb) => (mb / 1024.0).ToString(mb % 1024 == 0 ? "0" : "0.#", CultureInfo.InvariantCulture);
}

/// <summary>Which action a buildable row offers now.</summary>
public enum ModelBuildAction { None, GetBuilder, UpdateBuilder, Build, Cancel, Remove }

/// <summary>
/// The row of one buildable model (§5.6.5), pure: what it says and what it lets the user do,
/// from the CLI's list, the machine, the tick, whether this model is building, and the licence
/// text as read from disk (verified here against the pack's hash, before it is shown).
/// </summary>
public sealed record ModelBuildRow(
    BuildableModel Model, string Badge, string Title, IReadOnlyList<string> Lines,
    IReadOnlyList<(string Label, string Text)> Facts,
    string? LicenceText, bool LicenceVerified, string? LicenceNote,
    bool TickShown, string TickText, string? AcceptedLine, BuildLicence? WithdrawLicence,
    IReadOnlyList<ModelBuildAction> Actions, bool BuildEnabled, string? Blocker, bool Hidden)
{
    public const string BadgeText = "Research preview (non-commercial)";
    public const string TickLabel = "I have read this licence. I will use this model for non-commercial research only, and I allow DisplayXR to use research-only models on this account.";
    public const string LockedText = "Your administrator has turned off research-only models.";
    public const string UnverifiedText = "The licence text could not be verified.";
    public const string NotHostedLine = "Built on this PC from the official file. DisplayXR does not host this model.";

    /// <param name="ticked">The accept tick, as the user left it this session (never pre-ticked).</param>
    /// <param name="building">This model is building now.</param>
    /// <param name="readText">Reads a licence text file (null when it cannot be read).</param>
    /// <param name="driveOfStore">The store's drive or volume, as the user knows it ("C:").</param>
    public static ModelBuildRow For(BuildableModel m, ModelBuildList list, bool ticked, bool building,
                                    Func<string, byte[]?> readText, string driveOfStore)
    {
        var facts = new List<(string, string)>
        {
            ("Download", MbText(m.DownloadSize)),
            ("Space needed while building", GbText(m.NeedsDiskMb)),
            ("Memory needed to build", GbText(m.NeedsRamMb)),
            ("Size when installed", MbText(m.Size)),
            ("Licence", string.Join(", ", m.Licences.Select(l => l.Name))),
        };
        var lines = new List<string> { NotHostedLine };
        foreach (var l in m.Licences)
            lines.Add($"Model and licence: {l.Name}" + (l.Link.Length > 0 ? $", {l.Link}" : ""));

        // The full licence text, shown only after its sha256 matched the pack's (this read is the check).
        var sb = new StringBuilder();
        bool verified = m.Licences.Count > 0;
        foreach (var l in m.Licences)
        {
            var bytes = l.TextPath.Length > 0 ? readText(l.TextPath) : null;
            bool ok = bytes is not null && l.TextSha256.Length == 64 &&
                      Convert.ToHexString(SHA256.HashData(bytes)).Equals(l.TextSha256, StringComparison.OrdinalIgnoreCase);
            verified &= ok;
            if (ok)
            {
                if (sb.Length > 0) sb.Append("\n\n");
                sb.Append(Encoding.UTF8.GetString(bytes!));
            }
        }

        // The tick: never pre-ticked; with this user's record for this text it reads "accepted on".
        bool accepted = m.Licences.Count > 0 && m.Licences.All(l => l.Accepted);
        string? acceptedLine = accepted && m.Licences.Select(l => l.AcceptedAt).Max() is { } at
            ? $"Licence accepted on {at.ToLocalTime().ToString("d MMMM yyyy", CultureInfo.CurrentCulture)}" : accepted ? "Licence accepted" : null;

        var actions = new List<ModelBuildAction>();
        string? blocker = null;
        bool hidden = list.ResearchLockedOff;
        if (building) actions.Add(ModelBuildAction.Cancel);
        else if (m.Installed) actions.Add(ModelBuildAction.Remove);
        else if (!m.ToolInstalled) actions.Add(ModelBuildAction.GetBuilder);
        else if (!m.ToolCurrent) actions.Add(ModelBuildAction.UpdateBuilder);
        else actions.Add(ModelBuildAction.Build);

        if (list.ResearchLockedOff) blocker = LockedText;
        else if (!verified) blocker = UnverifiedText;
        else if (list.PhysMb is { } phys && m.NeedsRamMb > 0 && phys < m.NeedsRamMb)
            blocker = $"This PC has {GbNumber(phys)} GB of memory; building needs {GbNumber(m.NeedsRamMb)} GB.";
        else if (list.FreeMb is { } free && m.NeedsDiskMb > 0 && free < m.NeedsDiskMb)
            blocker = $"Building needs {GbNumber(m.NeedsDiskMb)} GB free on {driveOfStore}; {GbNumber(free)} GB is free.";

        // Build is enabled only when ticked (or already accepted), the machine meets the needs, and
        // the gate can be turned on. Hidden when the licence text cannot be verified or the gate is locked.
        if (actions.Contains(ModelBuildAction.Build) && (blocker == UnverifiedText || list.ResearchLockedOff))
            actions.Remove(ModelBuildAction.Build);
        // The tick both records the acceptance and turns the per-user gate on: it is needed again when
        // either is missing (a new licence text, or the gate turned off in Settings since).
        bool needTick = !(accepted && list.AllowResearchOnly);
        bool buildEnabled = actions.Contains(ModelBuildAction.Build) && blocker is null && (ticked || !needTick);
        return new ModelBuildRow(m, BadgeText, m.Name, lines, facts, verified ? sb.ToString() : null, verified,
            verified ? null : UnverifiedText, TickShown: needTick && !list.ResearchLockedOff && verified, TickLabel, acceptedLine,
            accepted ? m.Licences.FirstOrDefault() : null, actions, buildEnabled, blocker, hidden);
    }

    /// <summary>What Build runs, in order: the tick turns the gate on and records the acceptance; without it, the fetch alone.</summary>
    public static IReadOnlyList<CliArgs> BuildCommands(BuildableModel m, bool ticked) =>
        ticked ? new[] { ModelBuildCommands.ResearchOn, ModelBuildCommands.Build(m, accept: true) }
               : new[] { ModelBuildCommands.Build(m, accept: false) };

    private static string MbText(long bytes) => bytes <= 0 ? "unknown" : $"{Math.Max(1, Math.Round(bytes / (1024.0 * 1024.0))).ToString(CultureInfo.InvariantCulture)} MB";
    private static string GbText(long mb) => mb <= 0 ? "unknown" : $"{GbNumber(mb)} GB";
    private static string GbNumber(long mb) => (mb / 1024.0).ToString("0.#", CultureInfo.InvariantCulture);

    /// <summary>The drive a user knows the store's volume by: "C:" on Windows, else the store's root.</summary>
    public static string DriveOf(string storeRoot) =>
        storeRoot.Length >= 2 && storeRoot[1] == ':' ? storeRoot[..2].ToUpperInvariant() : storeRoot.Length > 0 ? storeRoot : "this PC";

    /// <summary>The default text reader: the file's bytes, at most 1 MB; null when it cannot be read.</summary>
    public static byte[]? ReadFile(string path)
    {
        try
        {
            var fi = new FileInfo(path);
            return fi.Exists && fi.Length <= 1024 * 1024 ? File.ReadAllBytes(path) : null;
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException) { return null; }
    }
}

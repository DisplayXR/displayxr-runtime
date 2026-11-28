// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;

namespace DisplayXR.Dashboard.Model;

// The Components page is ONE list: what is plugged into the runtime (Model/Components.cs)
// joined with what the catalog offers (Model/Catalog.cs). Everything here is pure: which
// catalog entry belongs to which installed row, the one status a row has, what may be
// offered on it, and the filters. The page only lays these out; the tests pin them.

/// <summary>The one status of a row that is, or can be, installed.</summary>
public enum RowStatus
{
    /// <summary>Present on this machine. Also what is said while no catalog has been read (nothing is claimed about it).</summary>
    Installed,
    /// <summary>Present, and the catalog has another build of it.</summary>
    UpdateAvailable,
    /// <summary>In the catalog, not on this machine.</summary>
    Available,
    /// <summary>Present, and the catalog does not list it, or lists it but did not find this copy (a dev registration, another installer, a preinstall).</summary>
    NotFromCatalog,
    /// <summary>Present, not a catalog install, and shipped by the runtime's own installer (its binary is in the runtime's <c>plugins</c> folder).</summary>
    PartOfRuntime,
}

public enum RowFilter { All, Installed, Updates }

/// <summary>Which catalog list the rows are joined with, and whether it is the live one.</summary>
/// <param name="Complete">The list is a catalog, so a component it does not list is not from it. False for the receipts alone.</param>
public sealed record CatalogSource(CatalogList? List, bool Stale, bool Complete)
{
    public static readonly CatalogSource None = new(null, false, false);

    /// <summary>
    /// The live list when the last read gave one. Else the last list read in this session, marked
    /// stale, so the rows it added are not dropped. Else what the failed read still carried (the
    /// engine's receipts when offline), or nothing.
    /// </summary>
    public static CatalogSource From(CatalogRead? read, CatalogList? lastGood) =>
        read is { Status: CatalogStatus.Ready, List: { } live } ? new(live, false, true)
        : read is not null && lastGood is not null ? new(lastGood, true, true)
        : new(read?.List, false, false);

    /// <summary>What the banner adds when the rows come from an earlier read.</summary>
    public static string StaleNote(DateTime lastRead) =>
        $"Items that are not installed are shown as last read at {lastRead.ToString("HH:mm", System.Globalization.CultureInfo.InvariantCulture)} and may have changed.";
}

/// <summary>
/// One row: the installed component, its catalog entry, or both. <see cref="Status"/> is null for a
/// row that is neither installed nor installable (a connected client, a summary line).
/// </summary>
public sealed record ComponentRow(string Key, ComponentItem? Installed, CatalogItem? Catalog, RowStatus? Status, bool Stale = false)
{
    public string Name => Installed?.Name ?? Catalog?.Name ?? "";

    public (string Text, Level Level)? StatusChip => Status switch
    {
        RowStatus.Installed => ("Installed", Level.Ok),
        RowStatus.UpdateAvailable => ("Update available", Level.Info),
        RowStatus.Available => ("Available", Level.Plain),
        RowStatus.NotFromCatalog => ("Installed separately", Level.Plain),
        RowStatus.PartOfRuntime => ("Part of the runtime", Level.Plain),
        _ => null,
    };

    /// <summary>
    /// The main button. Nothing is ever offered on a copy the catalog did not find: installing over
    /// it would replace something the user put there another way. Nor Install / Update on a row kept
    /// from an earlier read (<see cref="Stale"/>): the catalog cannot be reached now, so the engine
    /// would only answer "offline", and what the row says may no longer be true.
    /// </summary>
    public CatalogAction Action =>
        Catalog is null || Status is RowStatus.NotFromCatalog or RowStatus.PartOfRuntime ? CatalogAction.None
        : Stale && Catalog.Action is CatalogAction.Install or CatalogAction.Update ? CatalogAction.None
        : Catalog.Action;

    /// <summary>
    /// Uninstall only when it can succeed: the engine installed it (a receipt) and says it has an
    /// uninstall command for it (<see cref="CatalogItem.CanUninstall"/>). Nothing else.
    /// </summary>
    public bool CanUninstall => Catalog is { CanUninstall: true } && Status is RowStatus.Installed or RowStatus.UpdateAvailable;

    /// <summary>The catalog's detail (what it does, version, licence) can be opened.</summary>
    public bool HasDetail => Catalog is not null && Status is not (RowStatus.NotFromCatalog or RowStatus.PartOfRuntime);

    /// <summary>
    /// How an installed row that cannot be removed from here is removed; null when Uninstall is
    /// offered, nothing is installed, or the row is part of the runtime (it goes with the runtime).
    /// </summary>
    public string? RemoveNote =>
        Status == RowStatus.NotFromCatalog ? "Remove with its own uninstaller."
        : Status is RowStatus.Installed or RowStatus.UpdateAvailable && Catalog is not null && !CanUninstall
            ? CatalogText.RemoveFromSettings
        : null;

    /// <summary>The row's face (see <see cref="RowFace"/>).</summary>
    public RowFace Face
    {
        get
        {
            bool catalogFacts = Catalog is not null && HasDetail;
            var status = StatusChip is { } st && Stale && catalogFacts ? ($"{st.Text} (last known)", st.Level) : StatusChip;
            var details = new List<(string Label, string Text)>();
            // One line per label: several values of one label (a client's flags) are listed together.
            foreach (var g in (Installed?.DetailFacts ?? Array.Empty<(string Label, string Text)>()).GroupBy(f => f.Label))
                details.Add((g.Key, string.Join(", ", g.Select(f => f.Text))));
            // The catalog's own facts that are not already in its detail (version, size, category, licence are).
            if (catalogFacts && Catalog!.Recommended) details.Add(("Catalog", "Recommended"));
            return new RowFace(status, PlainStates.Chip(Installed?.State ?? PlainState.None),
                               catalogFacts && Catalog!.DetectedHere, details);
        }
    }

    public bool Shows(RowFilter filter) => filter switch
    {
        RowFilter.Installed => Status != RowStatus.Available,
        RowFilter.Updates => Status == RowStatus.UpdateAvailable,
        _ => true,
    };
}

/// <summary>
/// What a row shows on itself, and what its Details lists. On the row: its status (with "last known"
/// when it comes from an earlier catalog read), at most one plain state word, and "Works with your
/// display" when the engine matched this machine. Every other chip the row used to carry is a
/// labelled line of <see cref="Details"/> ("Load state: LOADED", "Signature: signed by ...").
/// </summary>
public sealed record RowFace((string Text, Level Level)? Status, (string Text, Level Level)? State, bool WorksWithDisplay,
                             IReadOnlyList<(string Label, string Text)> Details);

/// <summary>One group of the page: a Components section and its rows.</summary>
public sealed record ComponentGroup(ComponentSection Section, IReadOnlyList<ComponentRow> Rows);

public static class ComponentRows
{
    /// <summary>The last group: catalog items that are not runtime components.</summary>
    public const string AppsGroupId = "apps";

    /// <summary>The first group: the catalog's row for the runtime itself.</summary>
    public const string RuntimeGroupId = "runtime";

    /// <summary>
    /// Shipped by the runtime's own installer: the component's registered binary is inside the
    /// <c>plugins</c> folder of the runtime directory (the folder of the <c>displayxr-cli</c> this
    /// Dashboard runs). The installer puts its own plug-ins there and nothing else does: a plug-in
    /// with its own installer has its own folder, and a build registered from another tree is
    /// outside it. Compared as text (either slash, any case) so it does not depend on the host OS.
    /// </summary>
    public static bool ShippedByRuntime(string? binaryPath, string? runtimeDir)
    {
        if (string.IsNullOrWhiteSpace(binaryPath) || string.IsNullOrWhiteSpace(runtimeDir)) return false;
        string dir = runtimeDir.Replace('\\', '/').TrimEnd('/');
        return dir.Length > 0 && binaryPath.Replace('\\', '/').StartsWith(dir + "/plugins/", StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>The catalog class whose items live in a Components section; null for a section the catalog has no class for.</summary>
    public static string? CatalogClassOf(string sectionId) => sectionId switch
    {
        "display_processors" => "display_processor",
        "input_providers" => "input_provider",
        "tracking_providers" => "tracking_provider",
        "conversion_providers" => "conversion_provider",
        "workspace_controller" => "workspace_controller",
        // §5.6.6: the model builder sits with the models it builds.
        "conversion_models" => "model_builder",
        _ => null,
    };

    private static readonly string[] ComponentClassNames =
        { "display_processor", "input_provider", "tracking_provider", "conversion_provider", "workspace_controller" };

    /// <summary>The id an installed row is registered under, when it has one (a connected client or a summary line has none).</summary>
    public static string? ComponentId(ComponentItem item) =>
        item.Policy is { Id.Length: > 0 } p ? p.Id
        : item.Controller is { Id.Length: > 0 } c ? c.Id
        : item.Id is { Length: > 0 } id ? id
        : null;

    /// <summary>The key of a row: the same before and after an install, so an open row stays open.</summary>
    public static string KeyOf(string catalogClass, string componentId) => catalogClass + ":" + componentId;

    public static string KeyOf(CatalogItem item) => KeyOf(item.Class, item.ComponentId);

    /// <summary>
    /// Every group, in page order: the Components sections as they are, each with the catalog items
    /// of its class joined in (an installed row and its entry become one row; entries with nothing
    /// installed follow), then "Apps" for catalog items that are neither components nor the runtime. With no catalog
    /// (<paramref name="catalog"/> null) the installed rows are all there is, and nothing is said
    /// about where they came from; the same holds for a list that is only the engine's receipts.
    /// The catalog's row for the runtime itself opens the page, in a group of its own ("Runtime").
    /// <paramref name="runtimeDir"/> is the runtime directory (see <see cref="ShippedByRuntime"/>):
    /// with it, a component the catalog did not install but the runtime ships reads "Part of the runtime".
    /// </summary>
    public static IReadOnlyList<ComponentGroup> Join(IReadOnlyList<ComponentSection> sections, CatalogSource? source,
                                                     string? runtimeDir = null)
    {
        source ??= CatalogSource.None;
        var catalog = source.List;
        bool stale = source.Stale;
        var groups = new List<ComponentGroup>();
        var items = catalog?.Items ?? Array.Empty<CatalogItem>();
        foreach (var s in sections)
        {
            var rows = new List<ComponentRow>();
            string? cls = CatalogClassOf(s.Id);
            var used = new HashSet<string>(StringComparer.Ordinal);
            for (int n = 0; n < s.Items.Count; n++)
            {
                var item = s.Items[n];
                string? id = cls is null ? null : ComponentId(item);
                if (cls is null || id is null)
                {
                    rows.Add(new ComponentRow($"{s.Id}#{n}", item, null, null));
                    continue;
                }
                var entry = items.FirstOrDefault(c => c.Class == cls && c.ComponentId == id && !used.Contains(c.Id));
                if (entry is not null) used.Add(entry.Id);
                // A built-in row (the conversion class's built-in adapter) is part of the runtime
                // binary itself: it has no install of its own, whatever the catalog says.
                RowStatus status =
                    item.BuiltIn || item.Policy is { BuiltIn: true } ? RowStatus.PartOfRuntime
                    : catalog is null ? RowStatus.Installed
                    : entry is null ? (source.Complete ? RowStatus.NotFromCatalog : RowStatus.Installed)
                    : !entry.IsInstalled ? RowStatus.NotFromCatalog
                    : entry.HasUpdate ? RowStatus.UpdateAvailable
                    : RowStatus.Installed;
                if (status == RowStatus.NotFromCatalog && ShippedByRuntime(item.Policy?.BinaryPath, runtimeDir))
                    status = RowStatus.PartOfRuntime;
                rows.Add(new ComponentRow(KeyOf(cls, id), item, entry, status, stale && entry is not null));
            }
            if (cls is not null)
                rows.AddRange(Ordered(items.Where(c => c.Class == cls && !used.Contains(c.Id))).Select(c => CatalogOnly(c, stale)));
            groups.Add(new ComponentGroup(s, rows));
        }
        var runtime = Ordered(items.Where(c => c.IsRuntime)).Select(c => CatalogOnly(c, stale)).ToArray();
        if (runtime.Length > 0)
            groups.Insert(0, CatalogGroup(RuntimeGroupId, "DisplayXR", "DisplayXR itself: what every add-on and app here runs on.", runtime));
        // A class whose section is shown is joined there (a model builder under 2D-to-3D models);
        // with that section absent it is listed with the apps rather than dropped.
        var joined = sections.Select(x => CatalogClassOf(x.Id)).Where(c => c is not null).ToHashSet();
        var apps = Ordered(items.Where(c => !c.IsRuntime && !ComponentClassNames.Contains(c.Class) && !joined.Contains(c.Class)))
            .Select(c => CatalogOnly(c, stale)).ToArray();
        if (apps.Length > 0)
            groups.Add(CatalogGroup(AppsGroupId, "Apps", "Apps and tools for your display. They run on DisplayXR.", apps));
        return groups;
    }

    /// <summary>A group made of catalog rows only ("Runtime", "Apps"), with its count line.</summary>
    private static ComponentGroup CatalogGroup(string id, string title, string role, ComponentRow[] rows)
    {
        int installed = rows.Count(r => r.Status != RowStatus.Available);
        return new ComponentGroup(new ComponentSection(id, title, role, rows.Length, installed,
            CatalogText.Counts(installed, rows.Length - installed), Array.Empty<ComponentItem>()), rows);
    }

    /// <summary>A catalog item with no installed row beside it: installed (the engine found it), or available.</summary>
    private static ComponentRow CatalogOnly(CatalogItem c, bool stale) =>
        new(KeyOf(c), null, c, c.HasUpdate ? RowStatus.UpdateAvailable : c.IsInstalled ? RowStatus.Installed : RowStatus.Available, stale);

    /// <summary>What matches this machine first, then what the catalog recommends, then by name.</summary>
    private static IEnumerable<CatalogItem> Ordered(IEnumerable<CatalogItem> items) =>
        items.OrderBy(i => CatalogClasses.Rank(i.Class)).ThenByDescending(i => i.IsInstalled)
             .ThenByDescending(i => i.DetectedHere).ThenByDescending(i => i.Recommended)
             .ThenBy(i => i.Name, StringComparer.OrdinalIgnoreCase);

    /// <summary>
    /// The groups a filter shows. All and Installed keep every section (an empty one says so);
    /// Updates keeps only the groups that have one.
    /// </summary>
    public static IReadOnlyList<ComponentGroup> Filter(IReadOnlyList<ComponentGroup> groups, RowFilter filter) =>
        filter == RowFilter.All ? groups
        : groups.Select(g => g with { Rows = g.Rows.Where(r => r.Shows(filter)).ToArray() })
                .Where(g => filter == RowFilter.Installed ? g.Section.Id is not (AppsGroupId or RuntimeGroupId) || g.Rows.Count > 0 : g.Rows.Count > 0)
                .ToArray();

    /// <summary>The updates the nav badge counts: the ones the Update button can do from here.</summary>
    public static int Updates(CatalogList? live) =>
        live?.Items.Count(i => i.HasUpdate && i.Action == CatalogAction.Update) ?? 0;
}

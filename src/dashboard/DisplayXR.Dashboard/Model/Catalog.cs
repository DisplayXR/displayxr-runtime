// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;

namespace DisplayXR.Dashboard.Model;

// ADR-064 step 8: the Catalog tab's model. Everything here is pure: the install
// engine's `list --json` (docs/specs/runtime/dxr-get.md § list) read tolerantly, the
// words the page shows, what each exit code means to a person, the licence block
// and the URL rule. The page only lays these out; the tests pin them.

/// <summary>An external prerequisite of an item (<c>prerequisites[]</c>): something DisplayXR does not ship.</summary>
public sealed record CatalogPrereq(string Name, bool Met, bool Installable, string? InfoUrl)
{
    /// <summary>What to do about it, in one line. A met one needs nothing.</summary>
    public string NextStep => Met ? "Ready on this machine."
        : Installable ? "Installed for you together with this item."
        : SafeUrl.TryHttps(InfoUrl, out var u) ? $"Get it from {u!.Host}, then come back."
        : "Get it from its maker, then come back.";
}

/// <summary>What the main button of an item does.</summary>
public enum CatalogAction { None, Install, Update, OpenListing }

/// <summary>
/// One item of <c>dxr-get list --json</c>. Every field is the engine's; a field an older
/// engine or a schema-1 catalog does not carry reads as empty, never as a failure.
/// </summary>
public sealed record CatalogItem(
    string Id, string Name, string Class, string? RegisteredId, string Publisher, string Tier,
    string? Installed, string? Available, string State, bool Installable, bool Receipt, string? Listing,
    string Summary, IReadOnlyList<string> Aliases, bool Recommended, string Detected, string? Evidence,
    long Size, long Download, bool Compatible, string? IncompatibleReason, IReadOnlyList<CatalogPrereq> Prerequisites)
{
    public bool IsRuntime => Class == "runtime";

    /// <summary>Present on this machine, whoever installed it.</summary>
    public bool IsInstalled => State is "current" or "outdated" or "rebuilt" or "newer" or "installed";

    /// <summary>The catalog has a newer (or another) build than the one installed.</summary>
    public bool HasUpdate => State is "outdated" or "rebuilt";

    /// <summary>
    /// The state chip. Positive facts only: an item that is not installed has no chip (its
    /// button says "Get it").
    /// </summary>
    public (string Text, Level Level)? StateChip =>
        HasUpdate ? ("Update available", Level.Info) : IsInstalled ? ("Installed", Level.Ok) : null;

    /// <summary>"Works with your display" when the engine matched this machine's hardware with high confidence.</summary>
    public bool DetectedHere => Detected == "high";

    public IReadOnlyList<CatalogPrereq> Unmet => Prerequisites.Where(p => !p.Met).ToArray();

    /// <summary>
    /// The main button. The runtime is never installed from here: the Dashboard and the
    /// engine both live in the runtime's own directory, which its installer replaces.
    /// </summary>
    public CatalogAction Action =>
        IsRuntime ? CatalogAction.None
        : !Compatible ? CatalogAction.None
        : HasUpdate && Installable ? CatalogAction.Update
        : !IsInstalled && Installable ? CatalogAction.Install
        : !IsInstalled && SafeUrl.TryHttps(Listing, out _) ? CatalogAction.OpenListing
        : CatalogAction.None;

    /// <summary>Only what the engine installed can be removed by it (a receipt); the rest has its own uninstaller.</summary>
    public bool CanUninstall => IsInstalled && Receipt && !IsRuntime;

    /// <summary>"48 MB": what Install would still fetch, else the item's own size; null when unknown.</summary>
    public string? SizeText
    {
        get
        {
            long bytes = !IsInstalled || HasUpdate ? (Download > 0 ? Download : Size) : Size;
            return bytes > 0 ? CatalogText.Bytes(bytes) : null;
        }
    }

    /// <summary>The version line of the detail view.</summary>
    public string VersionText =>
        HasUpdate && Installed is not null && Available is not null ? $"{Installed} installed · {Available} available"
        : Installed is not null ? $"{Installed} installed"
        : Available ?? "";

    /// <summary>The policy class whose <c>list --json</c> carries this item's licence, if it has one.</summary>
    public ComponentClass? PolicyClass => Class switch
    {
        "display_processor" => ComponentClass.Display,
        "input_provider" => ComponentClass.Input,
        "conversion_provider" => ComponentClass.Conversion,
        "tracking_provider" => ComponentClass.Tracking,
        _ => null,
    };

    /// <summary>
    /// The id the item registers under in its class (the Components row), else its catalog id. Always
    /// one safe argument: an unsafe <c>registered_id</c> is dropped when the list is read.
    /// </summary>
    public string ComponentId => RegisteredId is { Length: > 0 } r ? r : Id;

    public bool Matches(string query)
    {
        query = query.Trim();
        if (query.Length == 0) return true;
        foreach (var word in query.Split(' ', StringSplitOptions.RemoveEmptyEntries))
        {
            bool hit = new[] { Name, Publisher, Summary, Id, CatalogClasses.Label(Class) }.Concat(Aliases)
                .Any(s => s.Contains(word, StringComparison.OrdinalIgnoreCase));
            if (!hit) return false;
        }
        return true;
    }
}

/// <summary>The catalog classes in the order the page shows them, with their friendly names.</summary>
public static class CatalogClasses
{
    private static readonly (string Class, string Label, string Lead, string Others)[] Known =
    {
        ("display_processor", "Displays", "Make every DisplayXR app show 3D on your display.", "display plug-ins"),
        ("input_provider", "Input", "Controllers, hands and pens for your apps.", "input devices"),
        ("conversion_provider", "2D to 3D", "Turn photos and video into 3D.", "2D-to-3D converters"),
        ("tracking_provider", "Viewer tracking", "Look-around that follows your eyes.", "viewer-tracking sources"),
        ("workspace_controller", "Workspaces", "A spatial desktop for your apps.", "workspaces"),
        ("app", "Apps", "Ready to open on your display.", "apps"),
        ("companion", "Companions", "Works alongside your display.", "companions"),
        ("sdk", "Developer tools", "Build your own.", "tools"),
        ("runtime", "DisplayXR", "The runtime everything here plugs into.", "components"),
    };

    public static IEnumerable<string> Order => Known.Select(k => k.Class);
    public static string Label(string cls) => Known.FirstOrDefault(k => k.Class == cls).Label ?? "More";
    public static string Lead(string cls) => Known.FirstOrDefault(k => k.Class == cls).Lead ?? "";
    /// <summary>"your other 2D-to-3D converters": the class in the plural, for the licence reassurance line.</summary>
    public static string Others(string cls) => Known.FirstOrDefault(k => k.Class == cls).Others ?? "components";
    public static int Rank(string cls)
    {
        int i = Array.FindIndex(Known, k => k.Class == cls);
        return i < 0 ? Known.Length : i;
    }
}

/// <summary><c>dxr-get list --json</c>: <c>{"schema":N,"items":[…]}</c>.</summary>
public sealed record CatalogList(int Schema, IReadOnlyList<CatalogItem> Items)
{
    public CatalogItem? Find(string id) =>
        Items.FirstOrDefault(i => i.Id == id) ?? Items.FirstOrDefault(i => i.Aliases.Contains(id));

    /// <summary>The catalog item behind a Components row of <paramref name="cls"/>, by its registered id.</summary>
    public CatalogItem? FindComponent(ComponentClass cls, string componentId) =>
        Items.FirstOrDefault(i => i.PolicyClass == cls && i.ComponentId == componentId);

    /// <summary>The classes that have items, in page order.</summary>
    public IReadOnlyList<string> Classes =>
        Items.Select(i => i.Class).Distinct().OrderBy(CatalogClasses.Rank).ThenBy(c => c, StringComparer.Ordinal).ToArray();

    /// <summary>
    /// What Browse shows: grouped by class in page order; inside a group, what matches this
    /// machine first, then what is recommended, then by name.
    /// </summary>
    public IReadOnlyList<(string Class, IReadOnlyList<CatalogItem> Items)> Browse(string query, string? cls, bool installedOnly) =>
        Items.Where(i => (cls is null || i.Class == cls) && (!installedOnly || i.IsInstalled) && i.Matches(query))
            .GroupBy(i => i.Class)
            .OrderBy(g => CatalogClasses.Rank(g.Key)).ThenBy(g => g.Key, StringComparer.Ordinal)
            .Select(g => (g.Key, (IReadOnlyList<CatalogItem>)g
                .OrderByDescending(i => i.DetectedHere).ThenByDescending(i => i.Recommended)
                .ThenBy(i => i.Name, StringComparer.OrdinalIgnoreCase).ToArray()))
            .ToArray();

    /// <summary>An id is one argument of the engine: letters, digits, <c>. _ -</c>, never a leading dash.</summary>
    public static bool IdIsSafe(string id) =>
        id.Length is > 0 and <= 128 && char.IsAsciiLetterOrDigit(id[0])
        && id.All(c => char.IsAsciiLetterOrDigit(c) || c is '.' or '_' or '-');

    public static bool TryParse(string? json, out CatalogList? list)
    {
        list = null;
        if (string.IsNullOrWhiteSpace(json)) return false;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object || !root.Has("items")) return false;
            var items = root.Arr("items").Where(e => e.ValueKind == JsonValueKind.Object && IdIsSafe(e.Str("id")))
                .Select(e => new CatalogItem(
                    e.Str("id"), e.Str("name", e.Str("id")), e.Str("class"),
                    // The registered id becomes an argument of the CLI (activate, activate-begin): same rule as the id.
                    e.StrOrNull("registered_id") is { } reg && IdIsSafe(reg) ? reg : null,
                    CatalogText.PublisherLabel(e.Str("publisher"), e.StrOrNull("publisher_name")), e.Str("tier"), e.StrOrNull("installed"), e.StrOrNull("available"),
                    e.Str("state", "not_installed"), e.Bool("installable"), e.Bool("receipt"), e.StrOrNull("listing"),
                    e.Str("summary"),
                    e.Arr("aliases").Where(a => a.ValueKind == JsonValueKind.String).Select(a => a.GetString() ?? "").ToArray(),
                    e.Bool("recommended"), e.Str("detected", "none"), e.StrOrNull("evidence"),
                    e.Long("size"), e.Long("download"),
                    // An engine that predates the field judges nothing: the install itself will say.
                    e.Has("compatible") ? e.Bool("compatible") : true, e.StrOrNull("incompatible_reason"),
                    e.Arr("prerequisites").Where(p => p.ValueKind == JsonValueKind.Object && p.Str("name").Length > 0)
                        .Select(p => new CatalogPrereq(p.Str("name"), p.Bool("met"), p.Bool("installable"), p.StrOrNull("info_url")))
                        .ToArray()))
                .ToArray();
            list = new CatalogList(root.Int("schema"), items);
            return true;
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException) { return false; }
    }

    /// <summary>
    /// <c>dxr-get list --installed --json</c> (<c>{"installed":[…]}</c>, the receipts: no catalog,
    /// no network) as items, for the offline view: what the engine installed and can remove.
    /// </summary>
    public static bool TryParseInstalled(string? json, out CatalogList? list)
    {
        list = null;
        if (string.IsNullOrWhiteSpace(json)) return false;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            if (root.ValueKind != JsonValueKind.Object || !root.Has("installed")) return false;
            var items = root.Arr("installed").Where(e => e.ValueKind == JsonValueKind.Object && IdIsSafe(e.Str("id")))
                .Select(e => new CatalogItem(
                    e.Str("id"), e.Str("name", e.Str("id")), e.Str("class"), null,
                    CatalogText.PublisherLabel(e.Str("publisher"), null), "",
                    e.StrOrNull("version"), null, "installed", false, true, null, "", Array.Empty<string>(), false,
                    "none", null, 0, 0, true, null, Array.Empty<CatalogPrereq>()))
                .ToArray();
            list = new CatalogList(0, items);
            return true;
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException) { return false; }
    }
}

/// <summary>Why the page has the content it has.</summary>
public enum CatalogStatus
{
    /// <summary>The engine answered with the catalog.</summary>
    Ready,
    /// <summary>Exit 10: no catalog (no network, a captive portal, an expired document). Nothing was changed.</summary>
    Offline,
    /// <summary>This platform has no install engine yet (macOS, Linux): installs come from the Download page.</summary>
    NoEngineHere,
    /// <summary>Windows, but <c>dxr-get.exe</c> is not installed: a runtime that predates the Catalog.</summary>
    EngineMissing,
    /// <summary>Exit 11: the engine refused a catalog it could not verify.</summary>
    Refused,
    /// <summary>Anything else: the engine did not start, timed out, or printed something unreadable.</summary>
    Failed,
}

/// <summary>One read of the catalog: the list when there is one, else why not, in words for the page.</summary>
public sealed record CatalogRead(CatalogStatus Status, CatalogList? List, string Title, string Text, string? Detail = null)
{
    public static CatalogRead Ready(CatalogList list) => new(CatalogStatus.Ready, list, "", "");

    public static readonly CatalogRead NoEngineHere = new(CatalogStatus.NoEngineHere, null,
        "Installs come from the Download page for now",
        "On this system, DisplayXR and its add-ons are installed from the Download page. Everything you install there shows up in Components.");

    public static CatalogRead EngineMissing(string lookedAt) => new(CatalogStatus.EngineMissing, null,
        "The Catalog arrives with the next DisplayXR update",
        "Update DisplayXR from the Download page and the Catalog appears here, ready to install add-ons in one click.",
        $"Looked for the install engine at {lookedAt}");

    /// <summary>What a finished <c>list --json</c> run means.</summary>
    public static CatalogRead From(int exitCode, string json, bool started, bool timedOut, string? startError)
    {
        if (!started)
            return new(CatalogStatus.Failed, null, "The Catalog couldn't be opened",
                       "The install engine did not start. Try again; if it keeps happening, reinstall DisplayXR.", startError);
        if (timedOut)
            return new(CatalogStatus.Failed, null, "The Catalog is taking too long",
                       "The install engine did not answer in time. Check your connection and try again.");
        if (exitCode == 0 && CatalogList.TryParse(json, out var list)) return Ready(list!);
        if (exitCode == CatalogExit.Offline)
            return new(CatalogStatus.Offline, null, "You're offline",
                       "The Catalog couldn't be reached. Everything you have installed keeps working; connect and try again to browse and install.");
        if (exitCode == CatalogExit.BadSignature)
            return new(CatalogStatus.Refused, null, "The Catalog couldn't be verified",
                       "DisplayXR only reads a catalog it can verify, so nothing was shown or changed. Try again later; if it keeps happening, update DisplayXR.");
        return new(CatalogStatus.Failed, null, "The Catalog couldn't be read",
                   "The install engine answered in a way this Dashboard does not understand. Try again; updating DisplayXR usually fixes it.",
                   $"dxr-get list exited with code {exitCode}");
    }
}

/// <summary>The verbs the page runs.</summary>
public enum CatalogVerb { Install, Update, Uninstall }

/// <summary>How an operation ended, in words: never a bare code, never a spinner left behind.</summary>
public sealed record CatalogOutcome(bool Ok, Level Level, string Title, string Text, int? ExitCode = null)
{
    /// <summary>
    /// The engine is still at work (the wait was stopped or timed out): completes with its exit code.
    /// While it runs, nothing else is started: the engine takes no lock of its own.
    /// </summary>
    public System.Threading.Tasks.Task<int>? StillRunning { get; init; }

    /// <summary>The list may have changed: re-read it.</summary>
    public bool Changed => Ok;
}

/// <summary>The engine's exit codes (dxr-get.md § Exit codes) and what each means to a person.</summary>
public static class CatalogExit
{
    public const int Ok = 0, Usage = 2, Offline = 10, BadSignature = 11, DownloadFailed = 20, VerifyFailed = 21,
        InstallerFailed = 30, NotFetchable = 40, Incompatible = 41, NoReceipt = 42, CannotUninstall = 43, NeedsAdmin = 50;

    public static CatalogOutcome Describe(CatalogVerb verb, int code, string name)
    {
        bool removing = verb == CatalogVerb.Uninstall;
        string doing = verb switch { CatalogVerb.Install => "install", CatalogVerb.Update => "update", _ => "remove" };
        return code switch
        {
            Ok => new(true, Level.Ok,
                verb switch { CatalogVerb.Install => $"{name} is installed", CatalogVerb.Update => $"{name} is up to date", _ => $"{name} was removed" },
                removing ? "Everything else keeps working." : "It is ready to use.", code),
            Offline => new(false, Level.Info, "You're offline",
                $"The Catalog couldn't be reached, so nothing was changed. Connect and {doing} {name} again.", code),
            BadSignature => new(false, Level.Warn, "The Catalog couldn't be verified",
                "Nothing was downloaded or changed. Try again later; if it keeps happening, update DisplayXR.", code),
            DownloadFailed => new(false, Level.Info, "The download didn't finish",
                $"Nothing was installed. What was downloaded is kept, so trying again picks up where it stopped.", code),
            VerifyFailed => new(false, Level.Warn, "The download couldn't be verified",
                $"The file for {name} did not match what its publisher signed, so it was not installed. Nothing was changed. Try again later.", code),
            InstallerFailed => new(false, Level.Warn,
                removing ? $"{name} could not be removed" : $"{name} did not finish installing",
                removing ? "Its uninstaller did not finish. Close apps that use it and try again, or remove it from Windows Settings > Apps."
                         : "Its installer did not finish. Close apps that use DisplayXR and try again.", code),
            NotFetchable => new(false, Level.Info, $"{name} isn't available for this system yet",
                "The Catalog has no build of it to install here.", code),
            Incompatible => new(false, Level.Info, $"{name} needs a different DisplayXR version",
                "Update DisplayXR from the Download page, then come back: it installs in one click.", code),
            NoReceipt => new(false, Level.Info, $"{name} was installed by its own installer",
                "Remove it from Windows Settings > Apps > Installed apps.", code),
            CannotUninstall => new(false, Level.Info, $"{name} can't be removed from here",
                "Something else you installed still uses it, or it came without an uninstaller. Remove what depends on it first, or use Windows Settings > Apps.", code),
            NeedsAdmin => new(false, Level.Info, "Administrator permission is needed",
                $"Windows asks once before DisplayXR can {doing} {name}. Choose Yes when it asks.", code),
            Usage => new(false, Level.Warn, "This Dashboard and the install engine don't match",
                "Update DisplayXR so both are the same version, then try again.", code),
            _ => new(false, Level.Warn, removing ? $"{name} could not be removed" : $"{name} could not be installed",
                $"The install engine stopped with code {code}. Nothing else was changed; try again.", code),
        };
    }

    /// <summary>The Windows permission prompt was answered No.</summary>
    public static CatalogOutcome Declined(string name) => new(false, Level.Info, "Nothing was changed",
        $"The permission prompt was closed, so {name} is as it was. Start again whenever you like.");

    public static CatalogOutcome StartFailed(string? why) => new(false, Level.Warn, "The install engine did not start",
        $"Nothing was changed. {(why is { Length: > 0 } ? why : "Try again; if it keeps happening, reinstall DisplayXR.")}");

    /// <summary>The work run outlasted the wait. It was not stopped: it is detached from the Dashboard and never killed.</summary>
    public static CatalogOutcome TimedOut(CatalogVerb verb, string name) => new(false, Level.Warn, "This is taking longer than expected",
        $"The Dashboard stopped waiting. {name} is still being worked on in the background and was not interrupted; this page updates when it ends, or press Refresh.");

    /// <summary>The user stopped waiting. The work run is detached from the Dashboard and is never stopped, elevated Dashboard or not.</summary>
    public static CatalogOutcome StoppedWaiting(string name) => new(false, Level.Info, "Still working in the background",
        $"The Dashboard stopped waiting, but {name} keeps going so it is never left half done, even if you close the Dashboard. This page updates when it ends, or press Refresh.");
}

/// <summary>
/// The licence block of an installed item (ADR-064 §5), from the component's own <c>licence</c>
/// object in <c>&lt;class&gt; list --json</c>. The component decides its licence; this only says it.
/// </summary>
public sealed record LicenceView(string Headline, Level Level, string? Message, bool OfferBuy, Uri? PurchaseUrl,
                                 bool OfferKey, string? Reassurance)
{
    /// <summary>
    /// Null for a component with no licence to report. <c>source = hardware</c> / <c>oem</c> is the
    /// zero-click case: the entitlement came with the display or the machine, so there is nothing to
    /// buy, no key and no handler involved, in any state. Buy and Paste a key are for the <c>key</c>
    /// and <c>trial</c> sources only.
    /// </summary>
    public static LicenceView? For(PolicyRow row, string catalogClass)
    {
        string? day = CatalogText.Day(row.LicenceExpires);
        bool bundled = row.LicenceSource is "hardware" or "oem";
        string included = row.LicenceSource == "oem" ? "Included with this machine" : "Included with your display";
        string? message = row.LicenceMessage is { Length: > 0 } m ? m : null;
        string? reassure = $"Not licensed? Your other {CatalogClasses.Others(catalogClass)} keep working.";
        bool actionable = !bundled && row.Licence is "trial" or "expired" or "unlicensed";
        Uri? buy = null;
        bool offerBuy = actionable && SafeUrl.TryHttps(row.LicencePurchaseUrl, out buy);
        // A component that says it takes no key gets no key box; one that predates the flag gets it.
        bool offerKey = actionable && row.LicenceKeyActivation != false;
        return row.Licence switch
        {
            "licensed" => new(bundled ? included : day is null ? "Licensed" : $"Licensed until {day}", Level.Ok, message,
                              false, null, false, null),
            "trial" => new(day is null ? "Trial" : $"Trial — ends {day}", Level.Info, message, offerBuy, buy, offerKey, reassure),
            "expired" => new(day is null ? "Licence ended" : $"Licence ended {day}", Level.Warn, message, offerBuy, buy, offerKey,
                             bundled ? null : reassure),
            "unlicensed" => new(bundled ? "Comes with a supported display" : "Ready to activate", Level.Info, message, offerBuy, buy,
                                offerKey, reassure),
            _ => null,
        };
    }
}

/// <summary>What Buy does, decided from <c>activate-begin</c>'s answer.</summary>
public enum BuyKind
{
    /// <summary>An activation is pending: open the complete URL; the vendor's page comes back through <c>displayxr://activate</c>.</summary>
    OpenWithReturn,
    /// <summary>No deep link (the component does not take one, or the runtime predates the verb): open the purchase page; the user pastes a key.</summary>
    OpenPurchasePage,
    /// <summary>There is no acceptable purchase page: nothing to open, and no Buy button from now on.</summary>
    NoPurchasePage,
    /// <summary>The activation could not be started now (too many pending, the store, not loaded): say so calmly; the key box stays.</summary>
    NotNow,
}

/// <summary>
/// One press of Buy (activation-deep-link.md §3). <see cref="Url"/> is what to open, exactly as given:
/// for <see cref="BuyKind.OpenWithReturn"/> it is <c>activate-begin</c>'s complete <c>url</c> and nothing
/// is ever appended to it. <see cref="Expires"/>: when the pending activation ends (15 minutes), which
/// is how long the Dashboard keeps looking for the licence to change. <see cref="NonceId"/> is the
/// loggable id, never the nonce.
/// </summary>
public sealed record BuyPlan(BuyKind Kind, string Result, Uri? Url, DateTimeOffset? Expires, string Title, string Text, string NonceId = "")
{
    public bool Opens => Url is not null && Kind is BuyKind.OpenWithReturn or BuyKind.OpenPurchasePage;

    /// <summary>The licence's own purchase page, when it is an acceptable https URL; else there is nothing to buy from here.</summary>
    public static BuyPlan Fallback(string name, Uri? purchase, string result = "unavailable") =>
        purchase is not null && SafeUrl.TryHttps(purchase.AbsoluteUri, out var u)
            ? new(BuyKind.OpenPurchasePage, result, u, null, "Finish in your browser",
                  $"When you have your licence key, come back and choose Paste a key. A key goes in the masked box, never in a link.")
            : NoPage(name, result);

    private static BuyPlan NoPage(string name, string result) => new(BuyKind.NoPurchasePage, result, null, null,
        $"{name} isn't sold from here", "If you have a licence key, choose Paste a key. Otherwise its maker can tell you where to get one.");

    /// <summary>
    /// Read <c>activate-begin --json</c>: <c>{class, id, result, url, return_url, nonce_id, ttl_s,
    /// expires_unix, expires, licence}</c>. No JSON or no <c>result</c> (an unknown-verb or usage
    /// error from a runtime without the verb) means the verb is not available: the purchase page as it is.
    /// </summary>
    public static BuyPlan FromActivateBegin(bool exitOk, string json, string name, Uri? purchase, DateTimeOffset now)
    {
        string result = "", url = "", nonceId = "";
        long expiresUnix = 0;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            result = root.Str("result");
            url = root.Str("url");
            nonceId = root.Str("nonce_id");
            expiresUnix = root.Long("expires_unix");
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException) { }
        switch (result)
        {
            case "":
                return Fallback(name, purchase);
            case "ok" when exitOk && SafeUrl.TryHttps(url, out var open):
            {
                // The record lives 15 minutes; never trust a time further out than a little over that.
                var expires = expiresUnix > 0 ? DateTimeOffset.FromUnixTimeSeconds(expiresUnix) : now.AddMinutes(15);
                if (expires > now.AddMinutes(20)) expires = now.AddMinutes(20);
                return new(BuyKind.OpenWithReturn, result, open, expires, "Finish in your browser",
                    $"When you are done, choose the button on that page that returns to DisplayXR. It brings back a one-time code for {name}, and this page updates by itself.",
                    nonceId);
            }
            case "ok": // "ok" without an https URL to open: nothing was started that the user can finish
                return new(BuyKind.NotNow, "bad-url", null, null, "The purchase couldn't be started",
                    "Try again in a moment. You can still paste a key.");
            case "no-deep-link":
                return Fallback(name, purchase, result);
            case "no-purchase-url":
                return NoPage(name, result);
            case "too-many":
                return new(BuyKind.NotNow, result, null, null, "A few purchases are still waiting to finish",
                    "Finish one in your browser, or try again in a few minutes: each one waits for 15 minutes. You can still paste a key.");
            case "not-registered":
                return new(BuyKind.NotNow, result, null, null, $"{name} isn't loaded right now",
                    "Open Components to see why, then try again. You can still paste a key.");
            default: // store-error, and any result a newer CLI adds
                return new(BuyKind.NotNow, result, null, null, "The purchase couldn't be started",
                    "DisplayXR could not prepare it on this machine. Try again in a moment. You can still paste a key.");
        }
    }
}

/// <summary>A Buy the user has started and may still be finishing in the browser.</summary>
public sealed record BuyPending(string ItemId, string Name, ComponentClass Class, string ComponentId, BuyPlan Plan,
                                string LicenceWas, DateTimeOffset Until);

/// <summary>What <c>&lt;class&gt; activate &lt;id&gt; @file --json</c> answered. Never carries the key.</summary>
public sealed record ActivationOutcome(bool Accepted, string Title, string Text)
{
    /// <summary>Read the CLI's JSON (<c>{"result","accepted","error","licence":{…}}</c>); exit code decides when it has none.</summary>
    public static ActivationOutcome From(bool ok, string json, string name)
    {
        string result = "", error = "";
        bool accepted = ok;
        try
        {
            using var doc = JsonDocument.Parse(json);
            var root = doc.RootElement;
            result = root.Str("result");
            error = root.Str("error");
            if (root.Has("accepted")) accepted = root.Bool("accepted");
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException) { }
        if (accepted) return new(true, $"{name} is activated", "Thank you. It is ready to use.");
        return result.ToLowerInvariant().Replace('_', '-') switch
        {
            "unsupported" => new(false, $"{name} doesn't take a key here", "Activate it with its maker's own tool, or use Buy."),
            "not-loaded" => new(false, $"{name} couldn't be reached", "It is installed but did not load. Open Components to see why, then try the key again."),
            "bad-token" => new(false, "That key is empty or too long", "Paste the key exactly as you received it."),
            _ => new(false, "That key wasn't accepted",
                     error.Length > 0 ? $"{name} said: {error}" : "Check the key and try again, or contact its maker."),
        };
    }
}

/// <summary>The one rule for a URL the Dashboard opens: absolute https, a host, no credentials, nothing odd.</summary>
public static class SafeUrl
{    public static bool TryHttps(string? text, out Uri? url)
    {
        url = null;
        if (string.IsNullOrWhiteSpace(text) || text.Length > 2048) return false;
        if (text.Any(c => char.IsControl(c) || char.IsWhiteSpace(c) || c is '"' or '\'' or '`' or '<' or '>' or '\\')) return false;
        if (!text.StartsWith("https://", StringComparison.OrdinalIgnoreCase)) return false;
        if (!Uri.TryCreate(text, UriKind.Absolute, out var u)) return false;
        if (u.Scheme != Uri.UriSchemeHttps || u.Host.Length == 0 || u.UserInfo.Length > 0) return false;
        if (u.HostNameType is not (UriHostNameType.Dns or UriHostNameType.IPv4 or UriHostNameType.IPv6)) return false;
        url = u;
        return true;
    }
}

/// <summary>One install / update / uninstall in flight. <see cref="Phase"/> is what the page says it is doing.</summary>
public sealed class CatalogOperation
{
    public CatalogOperation(string itemId, string name, CatalogVerb verb, DateTime started)
    {
        ItemId = itemId;
        Name = name;
        Verb = verb;
        Started = started;
    }

    public string ItemId { get; }
    public string Name { get; }
    public CatalogVerb Verb { get; }
    public DateTime Started { get; }
    public string Phase { get; set; } = "Starting…";
    public System.Threading.CancellationTokenSource StopWaiting { get; } = new();

    public string Title => Verb switch
    {
        CatalogVerb.Install => $"Installing {Name}",
        CatalogVerb.Update => $"Updating {Name}",
        _ => $"Removing {Name}",
    };
}

/// <summary>An engine run still at work after the Dashboard stopped waiting for it.</summary>
public sealed record CatalogBackground(string ItemId, string Name, CatalogVerb Verb)
{
    public string Line => Verb switch
    {
        CatalogVerb.Uninstall => $"{Name} is still being removed in the background.",
        CatalogVerb.Update => $"{Name} is still being updated in the background.",
        _ => $"{Name} is still being installed in the background.",
    } + " Other installs wait until it is done; this page updates by itself, or press Refresh.";
}

/// <summary>
/// What another page asked the Catalog to open: an item id, or the Components row
/// (<paramref name="Class"/> + <paramref name="ComponentId"/>) whose catalog item it is.
/// </summary>
public sealed record CatalogFocus(string? ItemId, ComponentClass? Class, string? ComponentId, string? Name);

public static class CatalogText
{
    /// <summary>The Download page: where macOS / Linux installs, and the runtime itself, come from.</summary>
    public const string DownloadPage = "https://displayxr.org/download";

    /// <summary>
    /// The publisher as a name. The engine's list carries the publisher's catalog id
    /// (<c>displayxr</c>), not its display name; a <c>publisher_name</c> is used when an engine
    /// sends one, else the id is tidied ("acme-labs" reads "Acme Labs"). Empty stays empty
    /// (a schema-1 catalog names no publisher).
    /// </summary>
    public static string PublisherLabel(string id, string? name)
    {
        if (name is { Length: > 0 }) return name;
        if (id.Length == 0) return "";
        if (id == "displayxr") return "DisplayXR";
        return string.Join(" ", id.Split(new[] { '-', '_', '.' }, StringSplitOptions.RemoveEmptyEntries)
            .Select(w => char.ToUpperInvariant(w[0]) + w[1..]));
    }

    /// <summary>The one calm line for an item the engine would not install on this runtime.</summary>
    public const string OtherVersion = "Made for a different DisplayXR version. Updating DisplayXR usually makes it available.";

    public static string Bytes(long bytes)
    {
        double mb = bytes / (1024.0 * 1024.0);
        if (mb >= 1024) return (mb / 1024).ToString("0.0", CultureInfo.InvariantCulture) + " GB";
        if (mb >= 10) return mb.ToString("0", CultureInfo.InvariantCulture) + " MB";
        if (mb >= 1) return mb.ToString("0.0", CultureInfo.InvariantCulture) + " MB";
        return Math.Max(1, Math.Round(bytes / 1024.0)).ToString("0", CultureInfo.InvariantCulture) + " KB";
    }

    /// <summary>"1 Jan 2100" from an ISO 8601 time; null when there is none to show.</summary>
    public static string? Day(string? iso)
    {
        if (iso is not { Length: >= 10 }) return null;
        return DateTime.TryParseExact(iso[..10], "yyyy-MM-dd", CultureInfo.InvariantCulture, DateTimeStyles.None, out var d)
            ? d.ToString("d MMM yyyy", CultureInfo.InvariantCulture)
            : null;
    }

    /// <summary>What an item does when its catalog entry carries no summary (every schema-1 item).</summary>
    public static string About(CatalogItem i) => i.Summary.Length > 0 ? i.Summary : i.Class switch
    {
        "display_processor" => "A display plug-in: with it, every DisplayXR app shows 3D on the display it supports.",
        "input_provider" => "An input add-on: it brings its device into every DisplayXR app.",
        "conversion_provider" => "A 2D-to-3D converter, shared by every DisplayXR app.",
        "tracking_provider" => "A viewer-tracking source: it lets the picture follow your eyes.",
        "workspace_controller" => "A workspace: a spatial desktop for your DisplayXR apps.",
        "runtime" => "The DisplayXR runtime: what every app and add-on here runs on.",
        "app" => "An app for your DisplayXR display.",
        _ => "An add-on for DisplayXR.",
    };

    public static string Elapsed(TimeSpan t) =>
        t.TotalMinutes >= 1 ? $"{(int)t.TotalMinutes} min {t.Seconds:00} s" : $"{Math.Max(0, (int)t.TotalSeconds)} s";
}

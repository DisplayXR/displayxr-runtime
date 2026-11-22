// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.IO;
using System.Linq;
using System.Text.Json.Nodes;
using System.Threading;
using System.Threading.Tasks;
using DisplayXR.Dashboard.Model;

namespace DisplayXR.Dashboard.Feed;

/// <summary>
/// <c>--fixture</c> with a <c>catalog-list.json</c> beside the snapshot file: the install engine,
/// simulated in memory, so the catalog rows of the Components page can be laid out and clicked through on any platform with
/// no network, no engine and no permission prompt. The file is a <c>dxr-get list --json</c> document
/// (or the engine's offline document, <c>{"offline":true,…}</c>, to see the offline view).
///
/// <para><c>install</c> / <c>uninstall</c> follow the engine's order of answers: what elevation cannot
/// fix first, then exit 50, then the elevated run (a short delay), which moves the item's state. Two
/// keys an item may carry, for the unhappy paths: <c>"fixture_exit": N</c> (the install ends with
/// that exit code; 10, 11 and 41 are answered before the prompt, the rest after it) and
/// <c>"fixture_prompt": "declined"</c> (the permission prompt is answered No).</para>
/// </summary>
public sealed class FixtureCatalogEngine : ICatalogEngine
{
    private readonly JsonObject _doc;
    private readonly object _gate = new();
    private readonly TimeSpan _work;

    public FixtureCatalogEngine(string path, TimeSpan? work = null)
    {
        Path = path;
        _doc = JsonNode.Parse(File.ReadAllText(path)) as JsonObject ?? new JsonObject();
        _work = work ?? TimeSpan.FromSeconds(3);
    }

    /// <summary><c>catalog-list.json</c> beside the fixture file, when there is one.</summary>
    public static FixtureCatalogEngine? Beside(string fixturePath)
    {
        try
        {
            string file = System.IO.Path.Combine(System.IO.Path.GetDirectoryName(System.IO.Path.GetFullPath(fixturePath)) ?? ".", "catalog-list.json");
            return File.Exists(file) ? new FixtureCatalogEngine(file) : null;
        }
        catch (Exception ex)
        {
            DashboardLog.Warn($"fixture catalog: {ex.Message}");
            return null;
        }
    }

    public EngineAvailability Availability => EngineAvailability.Ready;
    public string Path { get; }
    public bool AlreadyElevated => false;

    private bool OfflineDoc => _doc["offline"]?.GetValue<bool>() == true;

    private JsonObject? Item(string id) =>
        (_doc["items"] as JsonArray)?.OfType<JsonObject>().FirstOrDefault(i => i["id"]?.GetValue<string>() == id);

    private static CliResult Exit(int code, string stdout = "", string stderr = "") => new(code, stdout, stderr, false, null);

    public Task<CliResult> RunAsync(CliArgs args, TimeSpan timeout, CancellationToken cancel)
    {
        var a = args.Args.ToArray();
        lock (_gate)
        {
            if (a is ["list", "--installed", "--json"])
            {
                var have = new JsonArray(((_doc["items"] ?? _doc["receipts"]) as JsonArray ?? new JsonArray()).OfType<JsonObject>()
                    .Where(i => i["receipt"]?.GetValue<bool>() == true)
                    .Select(i => (JsonNode)new JsonObject
                    {
                        ["id"] = i["id"]?.GetValue<string>(), ["name"] = i["name"]?.GetValue<string>(),
                        ["class"] = i["class"]?.GetValue<string>(), ["publisher"] = i["publisher"]?.GetValue<string>() ?? "",
                        ["version"] = i["installed"]?.GetValue<string>() ?? "", ["can_uninstall"] = true,
                    }).ToArray());
                return Task.FromResult(Exit(0, new JsonObject { ["installed"] = have }.ToJsonString()));
            }
            if (OfflineDoc && a is ["list" or "install", ..])
                return Task.FromResult(Exit(CatalogExit.Offline, _doc.ToJsonString(), "OFFLINE: the DisplayXR catalog could not be reached (or it has expired)."));
            if (a is ["list", ..]) return Task.FromResult(Exit(0, _doc.ToJsonString()));
            if (a is ["install", var id, ..])
            {
                if (Item(id) is not { } it) return Task.FromResult(Exit(CatalogExit.NotFetchable));
                int forced = it["fixture_exit"]?.GetValue<int>() ?? 0;
                if (forced is CatalogExit.Offline or CatalogExit.BadSignature or CatalogExit.Incompatible) return Task.FromResult(Exit(forced));
                if (it["compatible"]?.GetValue<bool>() == false) return Task.FromResult(Exit(CatalogExit.Incompatible));
                if (it["state"]?.GetValue<string>() is "current" or "newer") return Task.FromResult(Exit(0));
                return Task.FromResult(Exit(CatalogExit.NeedsAdmin, "", "installing needs an elevated process"));
            }
            if (a is ["uninstall", var uid, ..])
            {
                if (Item(uid) is not { } it || it["receipt"]?.GetValue<bool>() != true) return Task.FromResult(Exit(CatalogExit.NoReceipt));
                if (it["fixture_uninstall_exit"]?.GetValue<int>() is CatalogExit.CannotUninstall) return Task.FromResult(Exit(CatalogExit.CannotUninstall));
                return Task.FromResult(Exit(CatalogExit.NeedsAdmin));
            }
        }
        return Task.FromResult(Exit(CatalogExit.Usage, "", $"fixture: unsupported '{args}'"));
    }

    public async Task<ElevatedResult> RunElevatedAsync(CliArgs args, TimeSpan timeout, CancellationToken stopWaiting, Action? started = null)
    {
        var a = args.Args.ToArray();
        string id = a.Length > 1 ? a[1] : "";
        JsonObject? it;
        lock (_gate) it = Item(id);
        if (it is null) return new(ElevatedEnd.Exited, CatalogExit.NotFetchable);
        if (it["fixture_prompt"]?.GetValue<string>() == "declined") return new(ElevatedEnd.Declined);
        started?.Invoke();
        // Like the real engine, the work is never interrupted: stopping the wait leaves it running.
        var work = Task.Run(async () =>
        {
            await Task.Delay(_work).ConfigureAwait(false);
            return Finish(a, it);
        });
        try { return new(ElevatedEnd.Exited, await work.WaitAsync(stopWaiting).ConfigureAwait(false)); }
        catch (OperationCanceledException) { return new(ElevatedEnd.StoppedWaiting, StillRunning: work); }
    }

    private int Finish(string[] a, JsonObject it)
    {
        lock (_gate)
        {
            if (a[0] == "install")
            {
                if (it["fixture_exit"]?.GetValue<int>() is { } forced and not 0) return forced;
                it["installed"] = it["available"]?.GetValue<string>();
                it["state"] = "current";
                it["receipt"] = true;
                it["download"] = 0;
                return 0;
            }
            if (a[0] == "uninstall")
            {
                it["installed"] = null;
                it["state"] = "not_installed";
                it["receipt"] = false;
                it["download"] = it["size"]?.GetValue<long>() ?? 0;
                return 0;
            }
        }
        return CatalogExit.Usage;
    }
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.ComponentModel;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Threading;
using System.Threading.Tasks;
using DisplayXR.Dashboard.Model;

namespace DisplayXR.Dashboard.Feed;

// ADR-064 step 8. THE ONE FILE that knows how the Catalog tab reaches the install
// engine. Today it runs `dxr-get.exe` directly; when `displayxr-cli catalog …` exists
// (ADR-064 step 4), CatalogCommands and DxrGetEngine are what change, and nothing else
// in the Dashboard does.

/// <summary>The engine's command lines, pure so a test can pin them.</summary>
public static class CatalogCommands
{
    /// <summary>
    /// One call answers the page. <c>--no-runtime</c>: the Dashboard never moves the runtime (it
    /// lives in the runtime's directory), so compatibility is judged against the runtime that is here.
    /// </summary>
    public static CliArgs List() => new("list", "--json", "--no-runtime");

    /// <summary>The receipts: no catalog, no network. What the offline view shows.</summary>
    public static CliArgs ListInstalled() => new("list", "--installed", "--json");

    /// <summary>Install and Update are the same verb: <c>install</c> brings an item to the catalog's version.</summary>
    public static CliArgs Install(string id) => new("install", Checked(id), "--yes", "--json", "--no-runtime");

    public static CliArgs Uninstall(string id) => new("uninstall", Checked(id), "--json");

    public static CliArgs For(CatalogVerb verb, string id) => verb == CatalogVerb.Uninstall ? Uninstall(id) : Install(id);

    private static string Checked(string id) =>
        CatalogList.IdIsSafe(id) ? id : throw new ArgumentException("not a catalog id", nameof(id));
}

public enum EngineAvailability { Ready, NoEngineOnThisPlatform, Missing }

public enum ElevatedEnd { Exited, Declined, StartFailed, TimedOut, StoppedWaiting }

/// <summary>How one elevated engine run ended. <see cref="ExitCode"/> only for <see cref="ElevatedEnd.Exited"/>.</summary>
public sealed record ElevatedResult(ElevatedEnd End, int ExitCode = -1, string? Error = null);

/// <summary>The install engine as the Dashboard sees it. The app runs <c>dxr-get</c>; fixtures and tests fake it.</summary>
public interface ICatalogEngine
{
    EngineAvailability Availability { get; }

    /// <summary>Where the engine is (or was looked for), for the log and the "engine missing" view.</summary>
    string Path { get; }

    /// <summary>This process is already elevated: one run does the work, no second prompt.</summary>
    bool AlreadyElevated { get; }

    /// <summary>Run a verb as this user, output captured. Cancelling kills it (it changes nothing un-elevated).</summary>
    Task<CliResult> RunAsync(CliArgs args, TimeSpan timeout, CancellationToken cancel);

    /// <summary>
    /// Run a verb elevated: one Windows permission prompt, no captured output (an elevated child's
    /// pipes cannot be read from here), the exit code when it ends. Cancelling stops the WAIT, never
    /// the engine: a non-elevated process cannot stop an elevated one, and an installer is not a
    /// thing to interrupt.
    /// </summary>
    Task<ElevatedResult> RunElevatedAsync(CliArgs args, TimeSpan timeout, CancellationToken stopWaiting);
}

/// <summary><c>dxr-get.exe</c>: beside the CLI (the runtime installer puts both there).</summary>
public sealed class DxrGetEngine : ICatalogEngine
{
    public const string ExeName = "dxr-get.exe";
    private readonly CliProcessSource? _runner;

    public DxrGetEngine(string cliPath) : this(
        OperatingSystem.IsWindows(), Environment.GetEnvironmentVariable("DXR_DASHBOARD_GET"), cliPath,
        Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), File.Exists)
    { }

    public DxrGetEngine(bool windows, string? overridePath, string cliPath, string programFiles, Func<string, bool> exists)
    {
        Path = Resolve(windows, overridePath, cliPath, programFiles, exists);
        Availability = !windows ? EngineAvailability.NoEngineOnThisPlatform
            : exists(Path) ? EngineAvailability.Ready : EngineAvailability.Missing;
        if (Availability == EngineAvailability.Ready) _runner = new CliProcessSource(Path);
    }

    /// <summary><c>DXR_DASHBOARD_GET</c> (development), beside the CLI, then the installed runtime's.</summary>
    public static string Resolve(bool windows, string? overridePath, string cliPath, string programFiles, Func<string, bool> exists)
    {
        if (!string.IsNullOrWhiteSpace(overridePath) && exists(overridePath)) return overridePath;
        char sep = windows ? '\\' : '/';
        int cut = cliPath.LastIndexOfAny(new[] { '\\', '/' });
        string beside = (cut >= 0 ? cliPath[..(cut + 1)] : "") + ExeName;
        if (exists(beside)) return beside;
        string installed = programFiles.TrimEnd('\\', '/') + sep + "DisplayXR" + sep + "Runtime" + sep + ExeName;
        return windows && exists(installed) ? installed : beside;
    }

    public EngineAvailability Availability { get; }
    public string Path { get; }
    public bool AlreadyElevated => Ui.Elevation.IsElevated;

    public Task<CliResult> RunAsync(CliArgs args, TimeSpan timeout, CancellationToken cancel) =>
        _runner is null
            ? Task.FromResult(new CliResult(-1, "", "", false, $"{ExeName} not found (looked at {Path})."))
            : _runner.RunAsync(args, timeout, cancel);

    /// <summary>
    /// The start info of the elevated run: the shell's <c>runas</c> verb on the engine itself (never a
    /// command interpreter), arguments as a vector, no window.
    /// </summary>
    public static ProcessStartInfo ElevatedStartInfo(string enginePath, CliArgs args)
    {
        var psi = new ProcessStartInfo(enginePath)
        {
            UseShellExecute = true,
            Verb = "runas",
            WindowStyle = ProcessWindowStyle.Hidden,
            WorkingDirectory = System.IO.Path.GetDirectoryName(enginePath) ?? "",
        };
        foreach (var a in args.Args) psi.ArgumentList.Add(a);
        return psi;
    }

    public async Task<ElevatedResult> RunElevatedAsync(CliArgs args, TimeSpan timeout, CancellationToken stopWaiting)
    {
        if (Availability != EngineAvailability.Ready) return new(ElevatedEnd.StartFailed, Error: $"{ExeName} not found.");
        Process? p;
        try
        {
            // Off the UI thread: ShellExecuteEx blocks until the permission prompt is answered.
            p = await Task.Run(() => Process.Start(ElevatedStartInfo(Path, args)), CancellationToken.None).ConfigureAwait(false);
        }
        catch (Win32Exception ex) when (ex.NativeErrorCode == 1223 /* ERROR_CANCELLED: the prompt was declined */)
        {
            return new(ElevatedEnd.Declined);
        }
        catch (Exception ex)
        {
            return new(ElevatedEnd.StartFailed, Error: ex.Message);
        }
        if (p is null) return new(ElevatedEnd.StartFailed, Error: "Windows did not start it.");
        using (p)
        {
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(stopWaiting);
            cts.CancelAfter(timeout);
            try
            {
                await p.WaitForExitAsync(cts.Token).ConfigureAwait(false);
                return new(ElevatedEnd.Exited, p.ExitCode);
            }
            catch (OperationCanceledException)
            {
                return new(stopWaiting.IsCancellationRequested ? ElevatedEnd.StoppedWaiting : ElevatedEnd.TimedOut);
            }
            catch (Exception ex)
            {
                return new(ElevatedEnd.StartFailed, Error: ex.Message);
            }
        }
    }
}

/// <summary>
/// What the Catalog tab does, over an engine and the CLI: read the catalog, install / update /
/// uninstall with one permission prompt, activate with a pasted key, open a checkout page.
/// No UI, no UI thread: every method is awaitable and bounded.
/// </summary>
public sealed class CatalogService
{
    /// <summary>The catalog fetch is bounded by the engine (timeouts and four attempts); this is the outer bound.</summary>
    public static readonly TimeSpan ReadTimeout = TimeSpan.FromMinutes(3);
    /// <summary>The engine waits at most an hour for a component installer; a little more than that here.</summary>
    public static readonly TimeSpan WorkTimeout = TimeSpan.FromMinutes(75);
    public static readonly TimeSpan ActivateTimeout = TimeSpan.FromSeconds(60);

    private readonly ICatalogEngine _engine;
    private readonly IProcessSource _cli;

    public CatalogService(ICatalogEngine engine, IProcessSource cli)
    {
        _engine = engine;
        _cli = cli;
    }

    public ICatalogEngine Engine => _engine;

    /// <summary><c>list --json</c>, or why there is no list. Offline: the receipts, so what was installed can still be removed.</summary>
    public async Task<CatalogRead> ListAsync(CancellationToken cancel = default)
    {
        switch (_engine.Availability)
        {
            case EngineAvailability.NoEngineOnThisPlatform: return CatalogRead.NoEngineHere;
            case EngineAvailability.Missing: return CatalogRead.EngineMissing(_engine.Path);
        }
        var r = await _engine.RunAsync(CatalogCommands.List(), ReadTimeout, cancel).ConfigureAwait(false);
        var read = CatalogRead.From(r.ExitCode, r.Json, r.Started, r.TimedOut, r.StartError);
        if (read.Status == CatalogStatus.Offline)
        {
            var inst = await _engine.RunAsync(CatalogCommands.ListInstalled(), TimeSpan.FromSeconds(30), cancel).ConfigureAwait(false);
            if (inst.Ok && CatalogList.TryParseInstalled(inst.Json, out var have) && have!.Items.Count > 0)
                read = read with { List = have };
        }
        return read;
    }

    /// <summary>
    /// Install, update or uninstall one item. First as this user: the engine answers what elevation
    /// cannot fix (offline, not compatible, nothing to do, no receipt) with no prompt at all, and exit
    /// 50 when elevating is all that is missing. Only then elevated, once.
    /// </summary>
    public async Task<CatalogOutcome> RunAsync(CatalogVerb verb, CatalogItem item, Action<string> phase, CancellationToken stopWaiting)
    {
        if (_engine.Availability != EngineAvailability.Ready)
            return CatalogExit.StartFailed("The install engine is not available on this system.");
        CliArgs args;
        try { args = CatalogCommands.For(verb, item.Id); }
        catch (ArgumentException) { return CatalogExit.StartFailed("That item has an id the install engine would not accept."); }

        phase("Checking…");
        // Already elevated: this run is the real one, so it gets the long bound.
        var first = await _engine.RunAsync(args, _engine.AlreadyElevated ? WorkTimeout : ReadTimeout, stopWaiting).ConfigureAwait(false);
        if (stopWaiting.IsCancellationRequested && !_engine.AlreadyElevated)
            return new CatalogOutcome(false, Level.Info, "Nothing was changed", $"{item.Name} is as it was.");
        if (!first.Started) return CatalogExit.StartFailed(first.StartError);
        if (first.TimedOut) return CatalogExit.TimedOut(verb, item.Name);
        if (first.ExitCode != CatalogExit.NeedsAdmin) return CatalogExit.Describe(verb, first.ExitCode, item.Name);

        phase("Waiting for your permission…");
        var run = _engine.RunElevatedAsync(args, WorkTimeout, stopWaiting);
        // The prompt is up to the user; once the engine runs there is nothing finer to report
        // (the engine prints no progress a UI can render), so say what it is doing in general.
        _ = Task.Delay(TimeSpan.FromSeconds(4), stopWaiting).ContinueWith(t =>
        {
            if (!t.IsCanceled && !run.IsCompleted)
                phase(verb == CatalogVerb.Uninstall ? "Removing…" : "Downloading, verifying and installing…");
        }, TaskScheduler.Default);
        var r = await run.ConfigureAwait(false);
        return r.End switch
        {
            ElevatedEnd.Exited => CatalogExit.Describe(verb, r.ExitCode, item.Name),
            ElevatedEnd.Declined => CatalogExit.Declined(item.Name),
            ElevatedEnd.TimedOut => CatalogExit.TimedOut(verb, item.Name),
            ElevatedEnd.StoppedWaiting => CatalogExit.StoppedWaiting(item.Name),
            _ => CatalogExit.StartFailed(r.Error),
        };
    }

    // ── Licence ─────────────────────────────────────────────────────────────

    /// <summary><c>&lt;class&gt; activate &lt;id&gt; @&lt;file&gt; --json</c>: the key travels in a file only its owner can read.</summary>
    public static CliArgs ActivateCommand(ComponentClass cls, string id, string keyFile) =>
        new(cls.Verb(), "activate", id, "@" + keyFile, "--json");

    /// <summary>
    /// Deliver a pasted key to a component. The key is written to a new file readable by this user
    /// only, the CLI is given the file's path (never the key: a command line is visible to every
    /// process and lands in logs), and the file is overwritten and deleted whatever happens.
    /// </summary>
    public async Task<ActivationOutcome> ActivateAsync(ComponentClass cls, string id, string name, string key,
                                                       string? keyDirectory = null, CancellationToken cancel = default)
    {
        key = key.Trim();
        if (key.Length == 0 || key.Any(c => c is '\r' or '\n'))
            return new ActivationOutcome(false, "That key is empty or has a line break", "Paste the key exactly as you received it.");
        string? file = null;
        try
        {
            file = KeyFile.Write(key, keyDirectory);
            var cmd = ActivateCommand(cls, id, file);
            DashboardLog.Info($"action: displayxr-cli {cls.Verb()} activate {CliArgs.Display(id)} @<key file> --json");
            var r = await _cli.RunAsync(cmd, ActivateTimeout, cancel).ConfigureAwait(false);
            if (!r.Started) return new ActivationOutcome(false, "The key could not be delivered", r.Summary);
            if (r.TimedOut) return new ActivationOutcome(false, "The key could not be delivered", "DisplayXR did not answer in time. Try again.");
            return ActivationOutcome.From(r.Ok, r.Json, name);
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException or NotSupportedException)
        {
            DashboardLog.Warn($"activate: {ex.GetType().Name}"); // never the message: it may quote a path, not a key, but keep it terse
            return new ActivationOutcome(false, "The key could not be delivered", "DisplayXR could not prepare the key for the component. Try again.");
        }
        finally
        {
            if (file is not null) KeyFile.Destroy(file);
        }
    }

    /// <summary><c>&lt;class&gt; activate-begin &lt;id&gt; --json</c> (activation-deep-link.md §3).</summary>
    public static CliArgs ActivateBeginCommand(ComponentClass cls, string id) => new(cls.Verb(), "activate-begin", id, "--json");

    /// <summary>
    /// What Buy does for one component. When the component takes a deep-link token, the CLI's
    /// <c>activate-begin</c> starts the activation and prints the COMPLETE URL to open (the vendor's
    /// page plus the single-use nonce and the return address): it is opened as it is, nothing is
    /// added to it here. A runtime without the verb, or a component without deep-link activation,
    /// falls back to the licence's <c>purchase_url</c>. Nothing is opened by this method; the caller
    /// opens <see cref="BuyPlan.Url"/> (https only, in the browser, never through a shell).
    /// </summary>
    public async Task<BuyPlan> BeginBuyAsync(ComponentClass cls, string id, string name, Uri? purchase, bool? deepLink,
                                            CancellationToken cancel = default)
    {
        // The component says it takes no deep-link token: do not load it just to hear "no-deep-link".
        if (deepLink == false) return BuyPlan.Fallback(name, purchase);
        try
        {
            var r = await _cli.RunAsync(ActivateBeginCommand(cls, id), TimeSpan.FromSeconds(60), cancel).ConfigureAwait(false);
            if (!r.Started || r.TimedOut) return BuyPlan.Fallback(name, purchase);
            var plan = BuyPlan.FromActivateBegin(r.Ok, r.Json, name, purchase, DateTimeOffset.UtcNow);
            DashboardLog.Info($"activate-begin {cls.Verb()} {CliArgs.Display(id)}: {plan.Result}{(plan.NonceId.Length > 0 ? $" (nonce id {plan.NonceId})" : "")}");
            return plan;
        }
        catch (Exception ex)
        {
            DashboardLog.Warn($"activate-begin: {ex.GetType().Name}");
            return BuyPlan.Fallback(name, purchase);
        }
    }
}

/// <summary>The short-lived file a pasted key travels in.</summary>
public static class KeyFile
{
    /// <summary>
    /// A new file with a random name, holding <paramref name="key"/> and readable by its owner only:
    /// mode 0600 on POSIX; on Windows an ACL with one entry (this user), inheritance off, in the
    /// user's own local application data.
    /// </summary>
    public static string Write(string key, string? directory = null)
    {
        string dir = directory ?? System.IO.Path.Combine(
            Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData, Environment.SpecialFolderOption.Create), "DisplayXR");
        Directory.CreateDirectory(dir);
        string path = System.IO.Path.Combine(dir, "activate-" + Convert.ToHexString(RandomNumberGenerator.GetBytes(16)).ToLowerInvariant() + ".key");
        var options = new FileStreamOptions { Mode = FileMode.CreateNew, Access = FileAccess.Write, Share = FileShare.None };
        if (!OperatingSystem.IsWindows()) options.UnixCreateMode = UnixFileMode.UserRead | UnixFileMode.UserWrite;
        using (var fs = new FileStream(path, options))
        {
            if (OperatingSystem.IsWindows()) RestrictToOwner(path);
            byte[] bytes = Encoding.UTF8.GetBytes(key + "\n");
            fs.Write(bytes);
            fs.Flush(true);
            CryptographicOperations.ZeroMemory(bytes);
        }
        return path;
    }

    private static void RestrictToOwner(string path)
    {
        using var me = System.Security.Principal.WindowsIdentity.GetCurrent();
        if (me.User is null) return;
        var acl = new System.Security.AccessControl.FileSecurity();
        acl.SetAccessRuleProtection(isProtected: true, preserveInheritance: false);
        acl.SetOwner(me.User);
        acl.AddAccessRule(new System.Security.AccessControl.FileSystemAccessRule(
            me.User, System.Security.AccessControl.FileSystemRights.FullControl, System.Security.AccessControl.AccessControlType.Allow));
        new FileInfo(path).SetAccessControl(acl);
    }

    /// <summary>Overwrite, then delete. Never throws: a file that is already gone is fine.</summary>
    public static void Destroy(string path)
    {
        try
        {
            if (File.Exists(path))
            {
                long n = new FileInfo(path).Length;
                using (var fs = new FileStream(path, FileMode.Open, FileAccess.Write, FileShare.None))
                {
                    fs.Write(new byte[n]);
                    fs.Flush(true);
                }
                File.Delete(path);
            }
        }
        catch (Exception ex) when (ex is IOException or UnauthorizedAccessException)
        {
            try { File.Delete(path); } catch { }
            DashboardLog.Warn($"key file cleanup: {ex.GetType().Name}");
        }
    }
}

/// <summary>Opening a web page in the default browser, without a command interpreter.</summary>
public static class UrlLauncher
{
    /// <summary>
    /// Windows: the shell's default verb on the URL itself (it is https, checked, so the handler is a
    /// browser). macOS / Linux: <c>open</c> / <c>xdg-open</c> with the URL as one argument.
    /// </summary>
    public static ProcessStartInfo StartInfo(Uri url, bool windows, bool macos)
    {
        if (!SafeUrl.TryHttps(url.OriginalString, out var checkedUrl) && !SafeUrl.TryHttps(url.AbsoluteUri, out checkedUrl))
            throw new ArgumentException("only an https URL is opened", nameof(url));
        if (windows) return new ProcessStartInfo(checkedUrl!.AbsoluteUri) { UseShellExecute = true };
        var psi = new ProcessStartInfo(macos ? "open" : "xdg-open") { UseShellExecute = false, CreateNoWindow = true };
        psi.ArgumentList.Add(checkedUrl!.AbsoluteUri);
        return psi;
    }

    /// <summary>True when the browser was asked to open it. Only https; anything else is refused and logged by host.</summary>
    public static bool Open(string? text)
    {
        if (!SafeUrl.TryHttps(text, out var url))
        {
            DashboardLog.Warn("refused to open a URL that is not plain https");
            return false;
        }
        try
        {
            using var p = Process.Start(StartInfo(url!, OperatingSystem.IsWindows(), OperatingSystem.IsMacOS()));
            DashboardLog.Info($"opened https://{url!.Host}/… in the browser");
            return true;
        }
        catch (Exception ex)
        {
            DashboardLog.Warn($"open URL: {ex.Message}");
            return false;
        }
    }
}

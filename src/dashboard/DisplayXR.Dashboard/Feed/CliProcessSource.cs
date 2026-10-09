// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Diagnostics;
using System.IO;
using System.Text;
using System.Threading;
using System.Threading.Tasks;

namespace DisplayXR.Dashboard.Feed;

/// <summary>
/// Spawns the sibling <c>displayxr-cli.exe</c>: next to this exe (an installed
/// runtime, or <c>_package\bin</c>), else the installed runtime's. Every child
/// joins a kill-on-close job object, so no <c>displayxr-cli</c> outlives the
/// dashboard, crash or not.
/// </summary>
public sealed class CliProcessSource : IProcessSource
{
    public const string ExeName = "displayxr-cli.exe";

    public CliProcessSource(string? cliPath = null)
    {
        CliPath = cliPath ?? Resolve();
    }

    /// <summary>The CLI this dashboard runs (it may not exist; a run then reports why).</summary>
    public string CliPath { get; }

    public bool CliExists => File.Exists(CliPath);

    public static string Resolve()
    {
        string? overridePath = Environment.GetEnvironmentVariable("DXR_DASHBOARD_CLI");
        if (!string.IsNullOrWhiteSpace(overridePath) && File.Exists(overridePath)) return overridePath;
        string beside = Path.Combine(AppContext.BaseDirectory, ExeName);
        if (File.Exists(beside)) return beside;
        string installed = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles), "DisplayXR", "Runtime", ExeName);
        if (File.Exists(installed)) return installed;
        return beside;
    }

    private ProcessStartInfo Info(string arguments, bool stdin) => new(CliPath, arguments)
    {
        UseShellExecute = false,
        CreateNoWindow = true,
        RedirectStandardOutput = true,
        RedirectStandardError = true,
        RedirectStandardInput = stdin,
        StandardOutputEncoding = Encoding.UTF8,
        StandardErrorEncoding = Encoding.UTF8,
        WorkingDirectory = Path.GetDirectoryName(CliPath) ?? AppContext.BaseDirectory,
    };

    public IWatchProcess StartWatch(Action<string> onLine, Action<int?> onExit)
    {
        if (!CliExists) throw new FileNotFoundException($"{ExeName} not found (looked at {CliPath})");
        var p = new Process { StartInfo = Info("status --watch --json", stdin: true), EnableRaisingEvents = false };
        p.Start();
        ChildJob.Add(p);
        DashboardLog.Info($"status --watch started (pid {p.Id})");
        // stderr carries the plug-in WARN lines; drain it so the child never blocks on a full pipe.
        p.ErrorDataReceived += (_, _) => { };
        p.BeginErrorReadLine();
        var watch = new WatchProcess(p);
        _ = Task.Run(async () =>
        {
            int? code = null;
            try
            {
                string? line;
                while ((line = await p.StandardOutput.ReadLineAsync().ConfigureAwait(false)) is not null)
                    onLine(line);
                await p.WaitForExitAsync().ConfigureAwait(false);
                code = p.ExitCode;
            }
            catch (Exception ex)
            {
                DashboardLog.Warn($"status --watch reader: {ex.Message}");
                try { code = p.HasExited ? p.ExitCode : -1; } catch { code = -1; }
            }
            finally
            {
                watch.MarkExited();
                try { p.Dispose(); } catch { }
            }
            onExit(code);
        });
        return watch;
    }

    private sealed class WatchProcess : IWatchProcess
    {
        private readonly Process _p;
        private int _exited;
        private int _stopping;

        public WatchProcess(Process p) { _p = p; }

        public void MarkExited() => Interlocked.Exchange(ref _exited, 1);

        public void Stop()
        {
            if (Interlocked.Exchange(ref _stopping, 1) != 0 || Volatile.Read(ref _exited) != 0) return;
            int pid = -1;
            try { pid = _p.Id; } catch { }
            // EOF on stdin is the CLI's documented stop signal (cli_cmd_status.c
            // stdin_closed); the kill is the backstop for a child stuck in an IPC call.
            try { _p.StandardInput.Close(); } catch { }
            _ = Task.Run(async () =>
            {
                try
                {
                    using var cts = new CancellationTokenSource(TimeSpan.FromMilliseconds(1500));
                    await _p.WaitForExitAsync(cts.Token).ConfigureAwait(false);
                }
                catch (OperationCanceledException)
                {
                    try { if (!_p.HasExited) _p.Kill(entireProcessTree: true); } catch { }
                    DashboardLog.Info($"status --watch (pid {pid}) killed after its stdin closed");
                }
                catch { }
            });
        }
    }

    public async Task<CliResult> RunAsync(string arguments, TimeSpan timeout, CancellationToken cancel = default)
    {
        if (!CliExists) return new CliResult(-1, "", "", false, $"{ExeName} not found (looked at {CliPath}).");
        Process p;
        try
        {
            p = new Process { StartInfo = Info(arguments, stdin: true) };
            p.Start();
        }
        catch (Exception ex)
        {
            return new CliResult(-1, "", "", false, $"Could not start {ExeName}: {ex.Message}");
        }
        using (p)
        {
            ChildJob.Add(p);
            try { p.StandardInput.Close(); } catch { }
            var stdout = p.StandardOutput.ReadToEndAsync(CancellationToken.None);
            var stderr = p.StandardError.ReadToEndAsync(CancellationToken.None);
            bool timedOut = false;
            using var cts = CancellationTokenSource.CreateLinkedTokenSource(cancel);
            cts.CancelAfter(timeout);
            try
            {
                await p.WaitForExitAsync(cts.Token).ConfigureAwait(false);
            }
            catch (OperationCanceledException)
            {
                timedOut = true;
                try { p.Kill(entireProcessTree: true); } catch { }
            }
            string o = "", e = "";
            try { o = await stdout.WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false); } catch { }
            try { e = await stderr.WaitAsync(TimeSpan.FromSeconds(5)).ConfigureAwait(false); } catch { }
            int code = -1;
            try { if (p.HasExited) code = p.ExitCode; } catch { }
            return new CliResult(code, o, e, timedOut, null);
        }
    }
}

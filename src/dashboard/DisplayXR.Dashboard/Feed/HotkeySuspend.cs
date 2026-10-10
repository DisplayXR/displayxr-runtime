// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Threading.Tasks;

namespace DisplayXR.Dashboard.Feed;

/// <summary>
/// While the hotkey capture box has focus, the service's workspace hotkey hook
/// is suspended (`displayxr-cli workspace hotkey-suspend on|off`, phase 8), so
/// pressing the current combo reaches the dashboard instead of launching the
/// controller. <see cref="Begin"/> on focus; <see cref="End"/> on every exit
/// (focus lost, Esc, a combo accepted or rejected, the page or window going
/// away). The service also resumes on its own when the suspending client
/// disconnects or after 60 s, so a missed End cannot strand the hook off.
/// Calls run one after another, never overlapping, so "off" always follows "on".
/// </summary>
public sealed class HotkeySuspend
{
    public static readonly TimeSpan Timeout = TimeSpan.FromSeconds(15);

    private readonly IProcessSource _cli;
    private Task _chain = Task.CompletedTask;
    private readonly object _gate = new();

    public HotkeySuspend(IProcessSource cli) { _cli = cli; }

    /// <summary>Suspended (from the dashboard's side): Begin ran and End has not.</summary>
    public bool Active { get; private set; }

    /// <summary>The CLI does not know the verb: capture works as before, with a note.</summary>
    public bool Unsupported { get; private set; }

    /// <summary>Raised (on any thread) when <see cref="Unsupported"/> becomes known.</summary>
    public event Action? Changed;

    public void Begin()
    {
        if (Active) return;
        Active = true;
        Enqueue("workspace hotkey-suspend on", checkSupport: true);
    }

    /// <summary>Always runs "off" when Begin ran, whatever happened in between (even if "on" failed).</summary>
    public void End()
    {
        if (!Active) return;
        Active = false;
        Enqueue("workspace hotkey-suspend off", checkSupport: false);
    }

    /// <summary>Completes when every queued call has finished (tests, shutdown).</summary>
    public Task Drain()
    {
        lock (_gate) return _chain;
    }

    private void Enqueue(string args, bool checkSupport)
    {
        lock (_gate)
        {
            _chain = _chain.ContinueWith(async _ =>
            {
                try
                {
                    var r = await _cli.RunAsync(args, Timeout).ConfigureAwait(false);
                    if (checkSupport && r.Started && !r.TimedOut && r.ExitCode != 0 && !Unsupported)
                    {
                        Unsupported = true;
                        DashboardLog.Info($"workspace hotkey-suspend unavailable ({r.Summary}); capture keeps the hook");
                        Changed?.Invoke();
                    }
                }
                catch (Exception ex) { DashboardLog.Warn($"{args}: {ex.Message}"); }
            }, TaskScheduler.Default).Unwrap();
        }
    }
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using Avalonia.Controls;

namespace DisplayXR.Dashboard.Pages;

/// <summary>
/// One page of the rail. Built in code; <see cref="Update"/> runs on every
/// feed / context change while the page is current and rebuilds only when the
/// page's own key moved, so a counter ticking on another page (or the same
/// facts arriving again) never resets a hover, a scroll or a half-made click.
/// </summary>
public abstract class Page : UserControl
{
    private string? _key;

    protected Page(string id, string title)
    {
        Id = id;
        Title = title;
    }

    public string Id { get; }
    public string Title { get; }
    protected DashboardContext Ctx { get; private set; } = null!;

    /// <summary>Pages that show status hold the feed while they are on screen (ADR-051 D5.2).</summary>
    public virtual bool HoldsFeed => true;

    /// <summary>The line under the page title in the shell header.</summary>
    public virtual string Caption => "";

    /// <summary>Page actions placed in the shell header, left of the global ones (rebuilt with the page).</summary>
    public virtual Control? HeaderActions => null;

    public void Bind(DashboardContext ctx) => Ctx = ctx;

    /// <summary>Raised after a rebuild, so the shell re-reads the caption and header actions.</summary>
    public event System.Action? Rebuilt;

    public virtual void OnShown() { }
    public virtual void OnHidden() { }

    /// <summary>What the page shows, as a string: equal keys, no rebuild.</summary>
    protected abstract string Key();

    protected abstract Control Build();

    /// <summary>
    /// True while the user is mid-interaction (an open drop-down): a rebuild now
    /// would close it under the pointer. The page calls <see cref="Update"/> again
    /// when the interaction ends.
    /// </summary>
    protected virtual bool DeferRebuild => false;

    /// <summary>Volatile values updated in place, without a rebuild (counters).</summary>
    protected virtual void UpdateInPlace() { }

    /// <summary>Returns true when the page rebuilt (the shell then refreshes its header actions).</summary>
    public bool Update(bool force = false)
    {
        string key;
        try { key = Key(); }
        catch (Exception ex) { DashboardLog.Error($"{Id} key", ex); key = Guid.NewGuid().ToString(); }
        bool rebuilt = false;
        if (!force && key != _key && Content is not null && DeferRebuild)
        {
            // keep _key stale: the next Update after the interaction rebuilds
        }
        else if (force || key != _key || Content is null)
        {
            _key = key;
            try { Content = Build(); }
            catch (Exception ex)
            {
                DashboardLog.Error($"{Id} build", ex);
                Content = Ui.U.Notice(Model.Level.Critical, "This page could not be drawn",
                    $"{ex.GetType().Name}: {ex.Message} (logged to {DashboardLog.Path})");
            }
            rebuilt = true;
        }
        try { UpdateInPlace(); }
        catch (Exception ex) { DashboardLog.Error($"{Id} update", ex); }
        if (rebuilt) Rebuilt?.Invoke();
        return rebuilt;
    }
}

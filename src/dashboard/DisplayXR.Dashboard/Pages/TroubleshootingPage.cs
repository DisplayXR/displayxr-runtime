// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System;
using System.Collections.Generic;
using System.Linq;
using Avalonia;
using Avalonia.Controls;
using DisplayXR.Dashboard.Model;
using DisplayXR.Dashboard.Ui;

namespace DisplayXR.Dashboard.Pages;

/// <summary>
/// Troubleshooting (page id "troubleshooting"): the one place for what needs the runtime's own
/// vocabulary, set apart at the bottom of the rail. A small tab strip (radio buttons named by their
/// labels) switches between its three parts, each an ordinary page hosted here:
/// "Running 3D apps" (<see cref="WindowsPage"/>, was the "windows" page), "Graphics cards"
/// (<see cref="GraphicsDetailsPage"/>, the graphics pipeline's technical lines from Home) and
/// "Advanced" (<see cref="DeveloperPage"/>, was the "developer" page, with the Current values table).
/// The header shows the part's caption and header actions. It holds the status feed (the running
/// apps are live); the part shown is kept for the session only.
/// </summary>
public sealed class TroubleshootingPage : Page
{
    private readonly (string Id, string Label, Page Page)[] _parts =
    {
        (PageRoutes.RunningApps, "Running 3D apps", new WindowsPage()),
        (PageRoutes.Graphics, "Graphics cards", new GraphicsDetailsPage()),
        (PageRoutes.Advanced, "Advanced", new DeveloperPage()),
    };

    private string _section = PageRoutes.RunningApps;
    private bool _shown;
    private readonly ContentControl _host = new();

    public TroubleshootingPage() : base(PageRoutes.Troubleshooting, "Troubleshooting")
    {
        System.Diagnostics.Debug.Assert(_parts.Select(p => p.Id).SequenceEqual(PageRoutes.TroubleshootingSections));
        foreach (var (_, _, page) in _parts)
            page.Rebuilt += () => { if (ReferenceEquals(page, Current)) RaiseRebuilt(); };
    }

    /// <summary>The part on screen: <see cref="PageRoutes.RunningApps"/>, <see cref="PageRoutes.Graphics"/> or <see cref="PageRoutes.Advanced"/>.</summary>
    public string Section => _section;

    private Page Current => _parts.First(p => p.Id == _section).Page;

    public override void Bind(DashboardContext ctx)
    {
        base.Bind(ctx);
        foreach (var (_, _, page) in _parts) page.Bind(ctx);
    }

    // Running 3D apps is live; holding the feed for the whole page keeps the shell's hold / release balanced.
    public override bool HoldsFeed => true;

    public override string Caption => Current.Caption;

    public override Control? HeaderActions => Current.HeaderActions;

    /// <summary>Open one part (from a route such as <c>--page developer</c>, or the tab strip).</summary>
    public void ShowSection(string section)
    {
        if (!_parts.Any(p => p.Id == section) || section == _section) return;
        var old = Current;
        _section = section;
        if (_shown)
        {
            try { old.OnHidden(); } catch (Exception ex) { DashboardLog.Error("OnHidden", ex); }
            try { Current.OnShown(); } catch (Exception ex) { DashboardLog.Error("OnShown", ex); }
        }
        Update(force: true);
    }

    public override void OnShown()
    {
        _shown = true;
        Current.OnShown();
    }

    public override void OnHidden()
    {
        _shown = false;
        Current.OnHidden();
    }

    public override void OnRefresh() => Current.OnRefresh();

    protected override string Key() => _section;

    protected override Control Build()
    {
        var strip = U.Flow(8, 6);
        string group = "troubleshooting" + Guid.NewGuid().ToString("N");
        foreach (var (id, label, _) in _parts)
        {
            var rb = new RadioButton { GroupName = group, IsChecked = id == _section, Content = label };
            rb.Classes.Add("option");
            Avalonia.Automation.AutomationProperties.SetName(rb, label);
            string target = id;
            rb.IsCheckedChanged += (_, _) => { if (rb.IsChecked == true) ShowSection(target); };
            U.AddFlow(strip, rb, 18, 6);
        }
        var page = Current;
        U.Detach(page);
        _host.Content = page;
        page.Update(force: true);
        U.Detach(_host);
        return U.VStack(16, strip, _host);
    }

    /// <summary>The part's own update runs on every tick: it rebuilds only when its own key moves.</summary>
    protected override void UpdateInPlace() => Current.Update();
}

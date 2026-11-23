// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

namespace DisplayXR.Dashboard.Model;

/// <summary>Where a page id lands: a rail page and, for Troubleshooting, the part of it.</summary>
public sealed record Route(string Page, string? Section = null);

/// <summary>
/// Page ids the shell answers to (<c>--page</c>, <c>DashboardContext.Navigate</c>). Every id an earlier
/// Dashboard had still lands on the place its content moved to: <c>catalog</c> on Components,
/// <c>performance</c> on Settings.
/// </summary>
public static class PageRoutes
{
    public const string Home = "home", Displays = "displays", Components = "components", Settings = "settings",
                        Troubleshooting = "troubleshooting";

    /// <summary>Troubleshooting's parts, in the order of its tab strip.</summary>
    public const string RunningApps = "apps", Graphics = "graphics", Advanced = "advanced";

    public static Route Route(string id)
    {
        id = (id ?? "").Trim().ToLowerInvariant();
        return id switch
        {
            "catalog" => new(Components),                 // the catalog tab was merged into Components
            "performance" => new(Settings),               // Performance became Settings
            _ => new(id),
        };
    }

    /// <summary>The rail page an id opens.</summary>
    public static string Resolve(string id) => Route(id).Page;
}

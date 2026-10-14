// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Linq;
using DisplayXR.Dashboard.Model;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>
/// The Developer page's read-only lever list (<c>perf list --all --json</c>,
/// perf-settings Phase 2A): tolerant parsing, managed vs environment-only rows,
/// the non-default count, and the in-place filters.
/// </summary>
public class DevSettingsTests
{
    private static DevLeverList Sample()
    {
        Assert.True(DevLeverList.TryParse(TestData.Text("perf-list-all.json"), out var list));
        return list!;
    }

    [Fact]
    public void Parse_ReadsEveryField()
    {
        var list = Sample();
        Assert.True(list.IsCatalog);
        Assert.Equal("2026-10-13", list.UserWritten);
        Assert.Contains("starting now", list.ResolvedFor);
        Assert.Equal(6, list.Levers.Count);

        var gpu = list.Lever("DXR_D3D_FORCE_GPU")!;
        Assert.Equal(1, gpu.Tier);
        Assert.Equal("Both", gpu.Proc);
        Assert.Equal("unset", gpu.Default);
        Assert.True(gpu.Managed);
        Assert.Equal("dgpu", gpu.Value);
        Assert.Equal("user", gpu.Source);
        Assert.True(gpu.NonDefault);
        Assert.True(gpu.Resettable);

        var pace = list.Lever("DXR_COMMIT_PACE")!;
        Assert.Equal(2, pace.Tier);
        Assert.False(pace.Managed);
        Assert.Null(pace.Value);
        Assert.False(pace.NonDefault);
        Assert.False(pace.Resettable);
    }

    [Fact]
    public void NonDefaultCount_IsEverySourceButDefault()
    {
        var list = Sample();
        Assert.Equal(2, list.NonDefaultCount); // user-set D3D_FORCE_GPU + env-set LATE_WEAVE
        Assert.Equal("2 of 6 levers non-default", StatusText.DevSettingsCount(list));
        var clean = list with { Levers = list.Levers.Select(l => l with { Value = null, Source = "default" }).ToArray() };
        Assert.Equal(0, clean.NonDefaultCount);
        Assert.Equal("6 levers, all on their defaults", StatusText.DevSettingsCount(clean));
        Assert.Equal("", StatusText.DevSettingsCount(null));
    }

    [Fact]
    public void EnvOnlyRows_NameTheVariable_AndNeverReset()
    {
        var list = Sample();
        var late = list.Lever("DXR_LATE_WEAVE")!;
        Assert.False(late.Managed);
        Assert.True(late.NonDefault);
        Assert.False(late.Resettable); // env-sourced and not managed: nothing the dashboard can write
        Assert.Equal("environment only — set DXR_LATE_WEAVE before launching the app", StatusText.DevEnvOnlyHint(late));
        // A service-only lever points at the service, not an app.
        Assert.Equal("environment only — set DXR_COMMIT_PACE before launching the service",
                     StatusText.DevEnvOnlyHint(list.Lever("DXR_COMMIT_PACE")!));
    }

    [Fact]
    public void ManagedRows_ResetOnlyWhatThePerUserFileHolds()
    {
        var w = Sample().Lever("DXR_WEAVE_ON_SCANOUT")!;
        Assert.True(w.Managed);
        Assert.False(w.Resettable); // on its default
        Assert.NotNull(StatusText.DevResetBlocked(w));
        Assert.True((w with { Value = "0", Source = "user" }).Resettable);
        Assert.Null(StatusText.DevResetBlocked(w with { Value = "0", Source = "user" }));
        Assert.False((w with { Value = "0", Source = "env" }).Resettable);  // env outranks the file
        Assert.False((w with { Value = "0", Source = "machine" }).Resettable); // HKLM is not ours to write
        Assert.Equal(Level.Warn, StatusText.DevSourceLevel("env"));
        Assert.Equal(Level.Info, StatusText.DevSourceLevel("user"));
        Assert.Equal(Level.Plain, StatusText.DevSourceLevel("default"));
    }

    [Fact]
    public void Filters_SearchTierAndProcess()
    {
        var levers = Sample().Levers;
        string[] Names(string? q, int tier, ProcFilter p) => levers.Where(l => l.Matches(q, tier, p)).Select(l => l.Name).ToArray();

        Assert.Equal(6, Names(null, 0, ProcFilter.All).Length);
        Assert.Equal(new[] { "DXR_COMMIT_PACE", "DXR_SPLIT_SAME_ADAPTER" }, Names("", 2, ProcFilter.All));
        Assert.Equal(new[] { "DXR_LATE_WEAVE" }, Names("  late_weave ", 0, ProcFilter.All)); // trimmed, case-insensitive
        Assert.Equal(new[] { "DXR_COMMIT_PACE" }, Names("backpressure", 0, ProcFilter.All)); // description match
        // Service: Svc, "App + Svc" and Both.
        Assert.Equal(new[] { "DXR_D3D_FORCE_GPU", "DXR_LATE_WEAVE", "DXR_COMMIT_PACE" }, Names(null, 0, ProcFilter.Service));
        Assert.DoesNotContain("DXR_COMMIT_PACE", Names(null, 0, ProcFilter.App));
        Assert.Equal("6 levers", StatusText.DevShownText(6, 6));
        Assert.Equal("2 of 6 levers shown", StatusText.DevShownText(2, 6));
    }

    [Fact]
    public void Parse_IsTolerant()
    {
        // Every key optional; a row without a name is dropped; wrong types read as fallbacks.
        Assert.True(DevLeverList.TryParse("""
            { "levers": [ { "name": "DXR_X" }, { "tier": 1 }, 7, { "name": "DXR_Y", "tier": "2", "managed": 1, "value": "", "source": null } ] }
            """, out var list));
        Assert.Equal(2, list!.Levers.Count);
        var x = list.Lever("DXR_X")!;
        Assert.Equal(0, x.Tier);
        Assert.Equal("default", x.Source);
        Assert.False(x.Managed);
        Assert.False(x.NonDefault);
        Assert.Equal("", x.Proc);
        var y = list.Lever("DXR_Y")!;
        Assert.Equal(2, y.Tier);   // numeric string accepted
        Assert.True(y.Managed);    // non-zero number reads as true
        Assert.Null(y.Value);      // empty string reads as unset
        Assert.Equal("default", y.Source);
        Assert.True(list.IsCatalog); // a row carries a tier

        Assert.True(DevLeverList.TryParse("{}", out var empty));
        Assert.Empty(empty!.Levers);
        Assert.False(empty.IsCatalog);

        Assert.False(DevLeverList.TryParse("", out _));
        Assert.False(DevLeverList.TryParse("[1,2]", out _));
        Assert.False(DevLeverList.TryParse("{\"levers\":", out _));
    }

    [Fact]
    public void OlderCli_IgnoringAll_IsNotACatalog()
    {
        // A CLI that predates --all prints the plain allow-listed list: no tiers, no resolved_for.
        Assert.True(DevLeverList.TryParse(TestData.Text("perf-list.json"), out var old));
        Assert.Equal(6, old!.Levers.Count);
        Assert.False(old.IsCatalog);
    }
}

// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Linq;
using DisplayXR.Dashboard.Feed;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

public class HotkeySuspendTests
{
    private const string On = "workspace hotkey-suspend on";
    private const string Off = "workspace hotkey-suspend off";

    private static (HotkeySuspend S, FakeSource Src) Make(int onExit = 0)
    {
        DashboardLog.Disabled = true;
        var src = new FakeSource { Run = a => new CliResult(a == On ? onExit : 0, "", onExit != 0 && a == On ? "usage: displayxr-cli workspace <list|set|launch>" : "", false, null) };
        return (new HotkeySuspend(src), src);
    }

    // Each exit path of the capture box (focus lost, Esc, a combo accepted, a combo
    // rejected, the page hidden, the window closing) ends in End(): "off" follows "on".
    [Theory]
    [InlineData("focus lost")]
    [InlineData("escape")]
    [InlineData("combo accepted")]
    [InlineData("combo rejected")]
    [InlineData("page hidden")]
    [InlineData("window closing")]
    public void EveryExitPath_ResumesTheHotkey(string path)
    {
        var (s, src) = Make();
        s.Begin();                       // the capture box gained focus
        Assert.True(s.Active);
        s.End();                         // `path`
        s.Drain().Wait();
        Assert.Equal(new[] { On, Off }, src.Runs);
        Assert.False(s.Active);
        Assert.False(s.Unsupported, path);
    }

    [Fact]
    public void RepeatedFocusAndExits_AreIdempotent()
    {
        var (s, src) = Make();
        s.End();                          // nothing to resume
        s.Begin();
        s.Begin();                        // a second GotFocus while capturing
        s.End();
        s.End();                          // LostFocus after Esc already ended it
        s.Begin();                        // a second capture
        s.End();
        s.Drain().Wait();
        Assert.Equal(new[] { On, Off, On, Off }, src.Runs);
    }

    [Fact]
    public void UnknownVerb_FallsBack_AndStillSendsOff()
    {
        var (s, src) = Make(onExit: 2);
        int changed = 0;
        s.Changed += () => changed++;
        s.Begin();
        s.Drain().Wait();
        Assert.True(s.Unsupported);       // the page shows the "other than the current one" note
        Assert.Equal(1, changed);
        s.End();                          // error path: "off" is still sent
        s.Drain().Wait();
        Assert.Equal(new[] { On, Off }, src.Runs);
        s.Begin();                        // a later capture reports nothing new
        s.End();
        s.Drain().Wait();
        Assert.Equal(1, changed);
    }

    [Fact]
    public void StartFailure_DoesNotMarkUnsupported_AndOffStillRuns()
    {
        DashboardLog.Disabled = true;
        var src = new FakeSource { Run = _ => new CliResult(-1, "", "", false, "displayxr-cli.exe not found") };
        var s = new HotkeySuspend(src);
        s.Begin();
        s.End();
        s.Drain().Wait();
        Assert.False(s.Unsupported);      // could not start: not evidence the verb is unknown
        Assert.Equal(2, src.Runs.Count);
        Assert.Equal(Off, src.Runs.Last());
    }
}

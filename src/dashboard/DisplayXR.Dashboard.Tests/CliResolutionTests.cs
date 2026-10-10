// Copyright 2026, The DisplayXR Project
// SPDX-License-Identifier: BSL-1.0

using System.Collections.Generic;
using DisplayXR.Dashboard.Feed;
using Xunit;

namespace DisplayXR.Dashboard.Tests;

/// <summary>
/// Where the dashboard finds displayxr-cli on each platform: the override, the
/// sibling of the executable, then the installed locations (Windows Program
/// Files; Linux the .deb's /usr/lib/displayxr/bin and the tarball's
/// ~/.local/share/displayxr/bin). Checked for both platforms from any host.
/// </summary>
public class CliResolutionTests
{
    private static System.Func<string, bool> Only(params string[] present)
    {
        var set = new HashSet<string>(present);
        return set.Contains;
    }

    [Fact]
    public void FileName_HasNoExeSuffixOffWindows()
    {
        Assert.Equal("displayxr-cli.exe", CliProcessSource.CliFileName(true));
        Assert.Equal("displayxr-cli", CliProcessSource.CliFileName(false));
    }

    [Fact]
    public void Linux_DebLayout_FindsTheSibling()
    {
        string p = CliProcessSource.Resolve(false, null, "/usr/lib/displayxr/bin/", "/home/u",
            Only("/usr/lib/displayxr/bin/displayxr-cli"));
        Assert.Equal("/usr/lib/displayxr/bin/displayxr-cli", p);
    }

    [Fact]
    public void Linux_TarballLayout_FindsTheSibling()
    {
        string p = CliProcessSource.Resolve(false, null, "/home/u/.local/share/displayxr/bin", "/home/u",
            Only("/home/u/.local/share/displayxr/bin/displayxr-cli"));
        Assert.Equal("/home/u/.local/share/displayxr/bin/displayxr-cli", p);
    }

    [Fact]
    public void Linux_NotBeside_FallsBackToInstalledLocationsInOrder()
    {
        Assert.Equal("/usr/lib/displayxr/bin/displayxr-cli",
            CliProcessSource.Resolve(false, null, "/opt/dev/", "/home/u",
                Only("/usr/lib/displayxr/bin/displayxr-cli", "/home/u/.local/share/displayxr/bin/displayxr-cli")));
        Assert.Equal("/home/u/.local/share/displayxr/bin/displayxr-cli",
            CliProcessSource.Resolve(false, null, "/opt/dev/", "/home/u",
                Only("/home/u/.local/share/displayxr/bin/displayxr-cli", "/usr/bin/displayxr-cli")));
        Assert.Equal("/usr/bin/displayxr-cli",
            CliProcessSource.Resolve(false, null, "/opt/dev/", "/home/u", Only("/usr/bin/displayxr-cli")));
    }

    [Fact]
    public void Linux_NothingFound_ReportsTheSibling()
    {
        Assert.Equal("/opt/dev/displayxr-cli", CliProcessSource.Resolve(false, null, "/opt/dev", "/home/u", Only()));
    }

    [Fact]
    public void Windows_SiblingThenProgramFiles()
    {
        Assert.Equal(@"C:\pkg\bin\displayxr-cli.exe",
            CliProcessSource.Resolve(true, null, @"C:\pkg\bin\", @"C:\Program Files", Only(@"C:\pkg\bin\displayxr-cli.exe")));
        Assert.Equal(@"C:\Program Files\DisplayXR\Runtime\displayxr-cli.exe",
            CliProcessSource.Resolve(true, null, @"C:\pkg\bin\", @"C:\Program Files",
                Only(@"C:\Program Files\DisplayXR\Runtime\displayxr-cli.exe")));
    }

    [Fact]
    public void Override_WinsWhenItExists()
    {
        Assert.Equal("/tmp/cli", CliProcessSource.Resolve(false, "/tmp/cli", "/usr/lib/displayxr/bin", "/home/u",
            Only("/tmp/cli", "/usr/lib/displayxr/bin/displayxr-cli")));
        Assert.Equal("/usr/lib/displayxr/bin/displayxr-cli", CliProcessSource.Resolve(false, "/tmp/missing", "/usr/lib/displayxr/bin", "/home/u",
            Only("/usr/lib/displayxr/bin/displayxr-cli")));
    }
}

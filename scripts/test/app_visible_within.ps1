# "Can the user see this app, and how long did it take?" -- a launch test for a
# real, already-built app. Windows.
#
# WHY A DEADLINE AND NOT A SNAPSHOT
#   Both shipped incarnations of this bug are about TIME, not just state:
#     * the white-window bug -- the app WAS eventually visible, ~10 s late, because
#       the un-cloak's only caller was a Unity coroutine blocked by the scene load;
#     * the v2.16.0 no-runtime regression -- the app was NEVER visible, because with
#       no session there is no overlay and nothing ever un-cloaked the main window.
#   One measurement covers both: poll until a deadline and report TIME TO FIRST
#   PRESENTABLE WINDOW. Never presentable is a hard failure; presentable but later
#   than the budget is a regression in the thing users actually complain about.
#
#   A snapshot at a fixed wait cannot do this. Sample too early and a healthy app
#   fails; too late and a 10-s-late app passes.
#
# THE RULE IS PER-PROCESS
#   Cloaking the engine's main window is CORRECT when the runtime is present -- the
#   plug-in's overlay is what the user sees. So the question is not "is any window
#   cloaked" but "does this process own AT LEAST ONE window that could be seen".
#   See window_visibility_check.ps1 for the same rule with its positive control.
#
# Usage
#   app_visible_within.ps1 -Exe "C:\path\app.exe" -DeadlineSec 60 -BudgetSec 20
#   app_visible_within.ps1 -Exe ... -NoRuntime          # clear XR_RUNTIME_JSON: the
#                                                       # arm that regressed
#   app_visible_within.ps1 -Exe ... -RuntimeJson <path> # pin a dev runtime
#   ... -Json out.json -Args "-batchmode"
param(
  [Parameter(Mandatory = $true)][string]$Exe,
  [string]$Arguments = '',
  [int]$DeadlineSec = 60,
  [int]$BudgetSec = 20,
  [int]$PollMs = 250,
  [switch]$NoRuntime,
  [string]$RuntimeJson = '',
  [string]$Json = ''
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Windows.Forms

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;

public class AppVis
{
    public const int DWMWA_CLOAKED = 14;
    public const int SW_SHOWMINIMIZED = 2;

    [StructLayout(LayoutKind.Sequential)] public struct RECT { public int left, top, right, bottom; }
    [StructLayout(LayoutKind.Sequential)] public struct POINT { public int x, y; }
    [StructLayout(LayoutKind.Sequential)]
    public struct WINDOWPLACEMENT
    {
        public int length, flags, showCmd;
        public POINT ptMinPosition, ptMaxPosition;
        public RECT rcNormalPosition;
    }

    [DllImport("user32.dll")] public static extern bool IsWindow(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool IsIconic(IntPtr h);
    [DllImport("user32.dll")] public static extern bool GetWindowRect(IntPtr h, out RECT r);
    [DllImport("user32.dll")] public static extern bool GetWindowPlacement(IntPtr h, ref WINDOWPLACEMENT p);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr param);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int size);

    public delegate bool EnumProc(IntPtr h, IntPtr param);

    public static string Title(IntPtr h) { var sb = new StringBuilder(512); GetWindowTextW(h, sb, sb.Capacity); return sb.ToString(); }
    public static string Klass(IntPtr h) { var sb = new StringBuilder(256); GetClassNameW(h, sb, sb.Capacity); return sb.ToString(); }

    // EnumWindows filtered by pid -- NOT Process.MainWindowHandle, which returns
    // whichever window Windows considers "main". On a multi-window app that is
    // routinely the wrong one (#1323), and here the wrong one is the difference
    // between catching this bug and missing it.
    public static List<IntPtr> TopLevelWindows(uint pid)
    {
        var found = new List<IntPtr>();
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint wpid; GetWindowThreadProcessId(h, out wpid);
            if (wpid == pid) found.Add(h);
            return true;
        }, IntPtr.Zero);
        return found;
    }

    public static int CloakedState(IntPtr h)
    {
        int v; int hr = DwmGetWindowAttribute(h, DWMWA_CLOAKED, out v, sizeof(int));
        return (hr == 0) ? v : -1;   // -1 => DWM did not answer
    }
}
'@

if (-not (Test-Path $Exe)) { throw "no exe at $Exe" }
$monitors = @([System.Windows.Forms.Screen]::AllScreens)

function Get-WindowState([IntPtr]$h) {
  $cl = [AppVis]::CloakedState($h)
  $wp = New-Object AppVis+WINDOWPLACEMENT
  $wp.length = [System.Runtime.InteropServices.Marshal]::SizeOf($wp)
  [void][AppVis]::GetWindowPlacement($h, [ref]$wp)
  $r = New-Object AppVis+RECT
  [void][AppVis]::GetWindowRect($h, [ref]$r)
  $w = $r.right - $r.left; $ht = $r.bottom - $r.top
  $on = $false
  foreach ($m in $monitors) {
    $b = $m.Bounds
    if ($r.left -lt ($b.X + $b.Width) -and ($r.left + $w) -gt $b.X -and
        $r.top -lt ($b.Y + $b.Height) -and ($r.top + $ht) -gt $b.Y) { $on = $true }
  }
  # Every arm that CAN be evaluated must hold. An arm that cannot be evaluated
  # here (no monitor to land on, DWM silent) is not allowed to pass silently --
  # it is reported, and the caller decides.
  $visOk  = [AppVis]::IsWindowVisible($h)
  $clOk   = ($cl -eq 0)
  $clEnf  = ($cl -ge 0)
  $minOk  = -not ([AppVis]::IsIconic($h) -or $wp.showCmd -eq [AppVis]::SW_SHOWMINIMIZED)
  $geoOk  = (($w -gt 0 -and $ht -gt 0) -and $on)
  $geoEnf = ($monitors.Count -gt 0)
  return [pscustomobject]@{
    hwnd = ("0x{0:X}" -f [int64]$h); title = [AppVis]::Title($h); klass = [AppVis]::Klass($h)
    ws_visible = $visOk; cloaked = $cl; not_minimised = $minOk; rect_on_a_monitor = $geoOk
    presentable = ($visOk -and (-not $clEnf -or $clOk) -and $minOk -and (-not $geoEnf -or $geoOk))
    cloak_enforced = $clEnf; geo_enforced = $geoEnf
    rect = ("({0},{1}) {2}x{3}" -f $r.left, $r.top, $w, $ht)
  }
}

# ------------------------------------------------------------------ launch ---
$saved = @{}
function Set-Env($k, $v) { $saved[$k] = [Environment]::GetEnvironmentVariable($k); [Environment]::SetEnvironmentVariable($k, $v) }

if ($NoRuntime) {
  # THE ARM THAT REGRESSED. A machine with no DisplayXR gets no session, so a
  # cloak whose un-cloak is gated on a session never lifts. Note this is also
  # CI's DEFAULT state -- a fresh runner has nothing registered.
  Set-Env 'XR_RUNTIME_JSON' $null
} elseif ($RuntimeJson -ne '') {
  if (-not (Test-Path $RuntimeJson)) { throw "no runtime manifest at $RuntimeJson" }
  Set-Env 'XR_RUNTIME_JSON' (Resolve-Path $RuntimeJson).Path
}

$sw = [System.Diagnostics.Stopwatch]::StartNew()
$proc = if ($Arguments -ne '') {
  Start-Process -FilePath $Exe -ArgumentList $Arguments -PassThru -WorkingDirectory (Split-Path $Exe)
} else {
  Start-Process -FilePath $Exe -PassThru -WorkingDirectory (Split-Path $Exe)
}
Write-Host ("launched pid {0}; deadline {1}s, budget {2}s; runtime={3}" -f `
  $proc.Id, $DeadlineSec, $BudgetSec, $(if ($NoRuntime) { 'NONE (regression arm)' } elseif ($RuntimeJson) { $RuntimeJson } else { 'machine default' }))

# ------------------------------------------------- poll to first presentable ---
$firstMs = $null
$timeline = @()
$last = $null
try {
  while ($sw.Elapsed.TotalSeconds -lt $DeadlineSec) {
    if ($proc.HasExited) {
      Write-Host ("::warning::process exited after {0:N1}s (code {1})" -f $sw.Elapsed.TotalSeconds, $proc.ExitCode)
      break
    }
    $wins = @([AppVis]::TopLevelWindows([uint32]$proc.Id)) |
      Where-Object { [AppVis]::IsWindow($_) } | ForEach-Object { Get-WindowState $_ }
    $seen = @($wins | Where-Object { $_.presentable })

    $sig = "{0}/{1}" -f $seen.Count, $wins.Count
    if ($sig -ne $last) {
      $timeline += [pscustomobject]@{ at_ms = [int]$sw.Elapsed.TotalMilliseconds; presentable = $seen.Count; windows = $wins.Count }
      Write-Host ("  t+{0,6:N0} ms  windows={1} presentable={2}" -f $sw.Elapsed.TotalMilliseconds, $wins.Count, $seen.Count)
      $last = $sig
    }
    if ($seen.Count -gt 0) { $firstMs = [int]$sw.Elapsed.TotalMilliseconds; $final = $wins; break }
    $final = $wins
    Start-Sleep -Milliseconds $PollMs
  }
} finally {
  if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force -EA SilentlyContinue }
  Get-Process -Name 'UnityCrashHandler64' -EA SilentlyContinue | Stop-Process -Force -EA SilentlyContinue
  foreach ($k in $saved.Keys) { [Environment]::SetEnvironmentVariable($k, $saved[$k]) }
}

# ----------------------------------------------------------------- verdict ---
$verdict = [pscustomobject]@{
  exe                  = $Exe
  arm                  = $(if ($NoRuntime) { 'no_runtime' } else { 'runtime' })
  ever_presentable     = ($null -ne $firstMs)
  first_presentable_ms = $firstMs
  within_budget        = ($null -ne $firstMs -and $firstMs -le ($BudgetSec * 1000))
  budget_ms            = $BudgetSec * 1000
  deadline_ms          = $DeadlineSec * 1000
  windows_at_end       = @($final)
  timeline             = $timeline
  environment          = [pscustomobject]@{ monitors = $monitors.Count; os = [System.Environment]::OSVersion.VersionString }
}
if ($Json -ne '') { Set-Content -Path $Json -Value ($verdict | ConvertTo-Json -Depth 8) -Encoding utf8 }

Write-Host ""
if ($verdict.ever_presentable) {
  Write-Host ("VISIBLE after {0} ms (budget {1} ms) -- within_budget={2}" -f $firstMs, $verdict.budget_ms, $verdict.within_budget)
} else {
  Write-Host ("NEVER PRESENTABLE within {0}s. The process ran and owned {1} window(s) that a user could not see." -f `
    $DeadlineSec, @($final).Count)
}
foreach ($w in @($final)) {
  Write-Host ("  {0} [{1}] '{2}' presentable={3} visible={4} cloaked={5} notMin={6} onMonitor={7} rect={8}" -f `
    $w.hwnd, $w.klass, $w.title, $w.presentable, $w.ws_visible, $w.cloaked, $w.not_minimised, $w.rect_on_a_monitor, $w.rect)
}

if (-not $verdict.ever_presentable) { exit 1 }
if (-not $verdict.within_budget) { exit 2 }   # visible, but late -- the white-window shape
exit 0

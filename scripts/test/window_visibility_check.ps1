# Window-presentability gate + its own positive control.
#
# WHY THIS EXISTS
#   A cloaked window passes every "did it start" check: the process is alive, the
#   log emits, the HWND exists, and IsWindowVisible() returns TRUE. That is how a
#   permanently-invisible app shipped twice (plug-in #256 in v2.13.0, and again in
#   v2.16.0 when an early DWM cloak had no un-cloak on the no-runtime path -- with
#   no session there is no overlay to un-cloak, so the app came up cloaked, parked
#   and invisible). DWMWA_CLOAKED appears nowhere in the runtime tree, so the
#   predicate the codebase already uses is structurally blind to it.
#
# WHAT IT DOES
#   Creates a real top-level window, applies -Mode, then evaluates the window the
#   way an external checker would (EnumWindows filtered by pid -- never
#   MainWindowHandle, see docs/getting-started/troubleshooting.md) against five
#   arms, and emits a JSON verdict.
#
#   -Mode normal    a plainly visible window        EXPECT presentable  (exit 0)
#   -Mode cloak     THE BUG, reproduced             EXPECT caught       (exit 1)
#   -Mode minimize  minimised                       EXPECT caught       (exit 1)
#   -Mode offscreen parked at -32000,-32000         EXPECT caught       (exit 1)
#
#   `cloak` is the positive control. A gate that cannot fail on a known-bad input
#   proves nothing, which is the whole lesson of this bug class.
#
# ENFORCEMENT IS REPORTED, NOT ASSUMED
#   Arms that cannot be evaluated on this machine (no monitors attached, DWM not
#   answering) are marked enforced=false and do NOT fail the run. The JSON states
#   which arms were enforced, so a caller can assert that the arm it cares about
#   actually ran -- the same belt-and-braces the CI self-test already applies to
#   the input-provider check ("a silently-skipped path would pass the exit code").
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File window_visibility_check.ps1 -Mode cloak
#   ... -Json out.json      also write the verdict to a file
param(
  [ValidateSet('normal', 'cloak', 'minimize', 'offscreen')]
  [string]$Mode = 'normal',
  [int]$SettleMs = 1500,
  [string]$Json = ''
)
$ErrorActionPreference = 'Stop'

Add-Type -AssemblyName System.Windows.Forms
Add-Type -AssemblyName System.Drawing

Add-Type @'
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;

public class WinVis
{
    public const int DWMWA_CLOAK   = 13; // write: the app cloaks itself
    public const int DWMWA_CLOAKED = 14; // read: nonzero => cloaked (1 app, 2 shell, 4 inherited)
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
    [DllImport("user32.dll")] public static extern int  GetWindowTextLength(IntPtr h);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr param);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int val, int size);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int size);

    public delegate bool EnumProc(IntPtr h, IntPtr param);

    // Top-level windows owned by `pid`. This is what an external checker has to
    // do: Process.MainWindowHandle returns whichever window Windows considers
    // "main", which on a multi-window app is routinely the wrong one (#1323).
    public static List<IntPtr> TopLevelWindows(uint pid)
    {
        var found = new List<IntPtr>();
        EnumWindows(delegate(IntPtr h, IntPtr p) {
            uint wpid; GetWindowThreadProcessId(h, out wpid);
            if (wpid == pid) { found.Add(h); }
            return true;
        }, IntPtr.Zero);
        return found;
    }

    // -1 => DWM did not answer (attribute unsupported, or DWM not running).
    public static int CloakedState(IntPtr h)
    {
        int v; int hr = DwmGetWindowAttribute(h, DWMWA_CLOAKED, out v, sizeof(int));
        return (hr == 0) ? v : -1;
    }

    public static int Cloak(IntPtr h)
    {
        int on = 1; return DwmSetWindowAttribute(h, DWMWA_CLOAK, ref on, sizeof(int));
    }
}
'@

# ---------------------------------------------------------------- the window ---
$form = New-Object System.Windows.Forms.Form
$form.Text = "DXR visibility probe [$Mode]"
$form.Size = New-Object System.Drawing.Size(480, 320)
$form.StartPosition = 'Manual'
$form.Location = New-Object System.Drawing.Point(80, 80)
$form.Show()

$cloakHr = $null
switch ($Mode) {
  'cloak' {
    # THE BUG: cloak and never un-cloak. Exactly the shape of the regression --
    # the cloak fires at window birth, the un-cloak is gated on something that
    # never happens (no runtime => no session => no overlay), and nothing else
    # in the system objects.
    $cloakHr = [WinVis]::Cloak($form.Handle)
  }
  'minimize'  { $form.WindowState = 'Minimized' }
  'offscreen' { [void][WinVis]::SetWindowPos($form.Handle, [IntPtr]::Zero, -32000, -32000, 480, 320, 0x0004) }
}

# Pump, so the window reaches its steady state before it is judged.
$deadline = [DateTime]::UtcNow.AddMilliseconds($SettleMs)
while ([DateTime]::UtcNow -lt $deadline) {
  [System.Windows.Forms.Application]::DoEvents()
  Start-Sleep -Milliseconds 50
}

# ------------------------------------------------------------ the environment ---
$monitors = @([System.Windows.Forms.Screen]::AllScreens)
$dwmAnswers = ([WinVis]::CloakedState($form.Handle) -ge 0)

# ------------------------------------------------------- discover, then judge ---
$pid_ = [System.Diagnostics.Process]::GetCurrentProcess().Id
$candidates = @([WinVis]::TopLevelWindows([uint32]$pid_)) |
  Where-Object { [WinVis]::IsWindow($_) -and [WinVis]::GetWindowTextLength($_) -gt 0 }

$hwnd = $form.Handle   # the HWND under test; discovery is asserted as arm 1
$arms = @()
function Add-Arm($name, $ok, $enforced, $detail) {
  $script:arms += [pscustomobject]@{ name = $name; ok = [bool]$ok; enforced = [bool]$enforced; detail = "$detail" }
}

# 1. a top-level window exists and is discoverable from outside
$discovered = @($candidates | Where-Object { $_ -eq $hwnd }).Count -gt 0
Add-Arm 'window_exists' $discovered $true ("top-level windows for pid {0}: {1}; target 0x{2:X} discovered={3}" -f `
  $pid_, $candidates.Count, [int64]$hwnd, $discovered)

# 2. WS_VISIBLE
$vis = [WinVis]::IsWindowVisible($hwnd)
Add-Arm 'ws_visible' $vis $true "IsWindowVisible=$vis"

# 3. NOT CLOAKED -- the arm nothing in the tree currently has
$cl = [WinVis]::CloakedState($hwnd)
$clDetail = if ($cl -lt 0) { "DwmGetWindowAttribute(DWMWA_CLOAKED) did not answer -- DWM unavailable" }
            elseif ($cl -eq 0) { "DWMWA_CLOAKED=0 (not cloaked)" }
            else { "DWMWA_CLOAKED=$cl (1=app 2=shell 4=inherited) -- WINDOW IS CLOAKED" }
Add-Arm 'not_cloaked' ($cl -eq 0) ($cl -ge 0) $clDetail

# 4. not minimised
$wp = New-Object WinVis+WINDOWPLACEMENT
$wp.length = [System.Runtime.InteropServices.Marshal]::SizeOf($wp)
[void][WinVis]::GetWindowPlacement($hwnd, [ref]$wp)
$iconic = [WinVis]::IsIconic($hwnd)
$minimised = ($iconic -or $wp.showCmd -eq [WinVis]::SW_SHOWMINIMIZED)
Add-Arm 'not_minimised' (-not $minimised) $true "IsIconic=$iconic showCmd=$($wp.showCmd)"

# 5. rect is non-degenerate AND lands on a monitor.
#    Enforced only where there is a monitor to land on -- a headless runner has
#    none, and an arm that cannot be evaluated must say so rather than pass.
$r = New-Object WinVis+RECT
[void][WinVis]::GetWindowRect($hwnd, [ref]$r)
$w = $r.right - $r.left; $h = $r.bottom - $r.top
$nonDegenerate = ($w -gt 0 -and $h -gt 0)
$onScreen = $false
foreach ($m in $monitors) {
  $b = $m.Bounds
  if ($r.left -lt ($b.X + $b.Width) -and ($r.left + $w) -gt $b.X -and
      $r.top  -lt ($b.Y + $b.Height) -and ($r.top + $h) -gt $b.Y) { $onScreen = $true }
}
Add-Arm 'rect_on_a_monitor' ($nonDegenerate -and $onScreen) ($monitors.Count -gt 0) `
  ("rect=({0},{1}) {2}x{3}; monitors={4}; nonDegenerate={5} onScreen={6}" -f `
   $r.left, $r.top, $w, $h, $monitors.Count, $nonDegenerate, $onScreen)

# ----------------------------------------------------------------- the verdict ---
$enforced = @($arms | Where-Object { $_.enforced })
$failed   = @($enforced | Where-Object { -not $_.ok })

$verdict = [pscustomobject]@{
  mode            = $Mode
  presentable     = ($failed.Count -eq 0)
  arms            = $arms
  enforced_count  = $enforced.Count
  failed_arms     = @($failed | ForEach-Object { $_.name })
  environment     = [pscustomobject]@{
    monitors        = $monitors.Count
    dwm_answers     = $dwmAnswers
    cloak_set_hr    = $cloakHr
    os              = [System.Environment]::OSVersion.VersionString
    session_name    = $env:SESSIONNAME
  }
}

$form.Close()

$out = $verdict | ConvertTo-Json -Depth 6
Write-Output $out
if ($Json -ne '') { Set-Content -Path $Json -Value $out -Encoding utf8 }

Write-Host ""
Write-Host ("mode={0}  presentable={1}  enforced={2}/{3}  failed=[{4}]" -f `
  $Mode, $verdict.presentable, $enforced.Count, $arms.Count, ($verdict.failed_arms -join ','))
foreach ($a in $arms) {
  $tag = if (-not $a.enforced) { 'SKIP' } elseif ($a.ok) { 'ok  ' } else { 'FAIL' }
  Write-Host ("  [{0}] {1,-18} {2}" -f $tag, $a.name, $a.detail)
}

if ($verdict.presentable) { exit 0 } else { exit 1 }

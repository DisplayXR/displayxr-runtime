# Presentability gate for a PROCESS, plus its own positive control.
#
# WHY THIS EXISTS
#   A cloaked window passes every "did it start" check: the process is alive, the
#   log emits, the HWND exists, and IsWindowVisible() returns TRUE. That is how a
#   permanently-invisible app shipped twice -- plug-in #256 in v2.13.0, and again
#   in v2.16.0 when an early DWM cloak had no un-cloak on the no-runtime path.
#   DWMWA_CLOAKED appears nowhere in the runtime tree, so the window predicate the
#   codebase already uses is structurally blind to it.
#
# WHY THE VERDICT IS PER-PROCESS, NOT PER-WINDOW
#   Cloaking the engine's main window is CORRECT when the runtime is present --
#   that is the whole point of the feature; the plug-in's overlay is what the user
#   sees instead. So "no window may be cloaked" would reject the shipping design,
#   and "any window is fine" passes trivially. The question that matches what a
#   user experiences is: does this process own AT LEAST ONE window that could be
#   seen? With a runtime, the overlay answers yes. With no runtime there is no
#   session, so no overlay is ever created, nothing un-cloaks the main window, and
#   the answer is no. That is the bug, and it is what this gate keys on.
#
# MODES
#   normal            one plainly visible window                   EXPECT presentable
#   cloak             one window, cloaked                          EXPECT caught
#   minimize          one window, minimised                        EXPECT caught
#   offscreen         one window, parked at -32000,-32000          EXPECT caught
#   runtime_present   main CLOAKED + overlay visible               EXPECT presentable
#   no_runtime        main CLOAKED, overlay never created  <== THE BUG, reproduced
#
#   `no_runtime` is the positive control against `runtime_present`: the pair differs
#   by exactly the thing that broke. A gate that cannot tell them apart proves
#   nothing, and this bug class is precisely a family of checks that could not fail.
#
# ENFORCEMENT IS REPORTED, NOT ASSUMED
#   Arms that cannot be evaluated here (no monitors, DWM silent) are marked
#   enforced=false and do not fail the run; the JSON says which arms ran, so a
#   caller can assert the arm it cares about actually fired. Same belt-and-braces
#   build-windows.yml already applies to its input-provider check ("a
#   silently-skipped path would pass the exit code").
#
# Only windows titled "DXR probe*" are judged -- this process also owns a console
# window, and a real checker filters to the HWNDs the app reports instead.
param(
  [ValidateSet('normal', 'cloak', 'minimize', 'offscreen', 'runtime_present', 'no_runtime')]
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
using System.Text;

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
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowTextW(IntPtr h, StringBuilder s, int max);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr param);
    [DllImport("user32.dll")] public static extern uint GetWindowThreadProcessId(IntPtr h, out uint pid);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    [DllImport("dwmapi.dll")] public static extern int DwmSetWindowAttribute(IntPtr h, int attr, ref int val, int size);
    [DllImport("dwmapi.dll")] public static extern int DwmGetWindowAttribute(IntPtr h, int attr, out int val, int size);

    public delegate bool EnumProc(IntPtr h, IntPtr param);

    public static string Title(IntPtr h)
    {
        var sb = new StringBuilder(512);
        GetWindowTextW(h, sb, sb.Capacity);
        return sb.ToString();
    }

    // Top-level windows owned by `pid`. This is what an external checker must do:
    // Process.MainWindowHandle returns whichever window Windows considers "main",
    // which on a multi-window app is routinely the wrong one (#1323) -- and here
    // the wrong one is exactly the difference between catching the bug and not.
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

function New-ProbeWindow([string]$name, [int]$x, [int]$y) {
  $f = New-Object System.Windows.Forms.Form
  $f.Text = "DXR probe $name"
  $f.Size = New-Object System.Drawing.Size(420, 280)
  $f.StartPosition = 'Manual'
  $f.Location = New-Object System.Drawing.Point($x, $y)
  $f.Show()
  return $f
}

# ------------------------------------------------------- build the scenario ---
$forms = @()
$cloakHr = $null

switch ($Mode) {
  'normal' { $forms += New-ProbeWindow 'main' 80 80 }
  'cloak' {
    $m = New-ProbeWindow 'main' 80 80
    $cloakHr = [WinVis]::Cloak($m.Handle)
    $forms += $m
  }
  'minimize'  { $m = New-ProbeWindow 'main' 80 80; $m.WindowState = 'Minimized'; $forms += $m }
  'offscreen' {
    $m = New-ProbeWindow 'main' 80 80
    [void][WinVis]::SetWindowPos($m.Handle, [IntPtr]::Zero, -32000, -32000, 420, 280, 0x0004)
    $forms += $m
  }
  'runtime_present' {
    # The SHIPPING design: the engine's own window is cloaked on purpose, and the
    # plug-in's overlay is what the user actually sees. Must PASS -- a gate that
    # rejects this rejects the feature.
    $m = New-ProbeWindow 'main' 80 80
    $cloakHr = [WinVis]::Cloak($m.Handle)
    $forms += $m
    $forms += New-ProbeWindow 'overlay' 140 140
  }
  'no_runtime' {
    # THE BUG: same cloak, but no runtime means no session, so the overlay is
    # never created and nothing ever un-cloaks the main window. Process owns a
    # window, is alive, logs fine -- and the user sees nothing.
    $m = New-ProbeWindow 'main' 80 80
    $cloakHr = [WinVis]::Cloak($m.Handle)
    $forms += $m
  }
}

# Pump, so every window reaches its steady state before it is judged.
$deadline = [DateTime]::UtcNow.AddMilliseconds($SettleMs)
while ([DateTime]::UtcNow -lt $deadline) {
  [System.Windows.Forms.Application]::DoEvents()
  Start-Sleep -Milliseconds 50
}

# ------------------------------------------------------------ environment ---
$monitors = @([System.Windows.Forms.Screen]::AllScreens)

# --------------------------------------------- discover, then judge each one ---
$procId = [System.Diagnostics.Process]::GetCurrentProcess().Id
$hwnds = @([WinVis]::TopLevelWindows([uint32]$procId)) |
  Where-Object { [WinVis]::IsWindow($_) -and ([WinVis]::Title($_)).StartsWith('DXR probe') }

function Test-Window([IntPtr]$h) {
  # Built inline rather than through a nested helper: a helper writing to
  # $script:arms targets a DIFFERENT variable than the local $arms, and $null += a
  # PSObject throws op_Addition.
  $arms = @()

  $vis = [WinVis]::IsWindowVisible($h)
  $arms += [pscustomobject]@{ name = 'ws_visible'; ok = [bool]$vis; enforced = $true; detail = "IsWindowVisible=$vis" }

  $cl = [WinVis]::CloakedState($h)
  $d = if ($cl -lt 0) { 'DWMWA_CLOAKED did not answer -- DWM unavailable' }
       elseif ($cl -eq 0) { 'DWMWA_CLOAKED=0 (not cloaked)' }
       else { "DWMWA_CLOAKED=$cl -- CLOAKED" }
  $arms += [pscustomobject]@{ name = 'not_cloaked'; ok = ($cl -eq 0); enforced = ($cl -ge 0); detail = $d }

  $wp = New-Object WinVis+WINDOWPLACEMENT
  $wp.length = [System.Runtime.InteropServices.Marshal]::SizeOf($wp)
  [void][WinVis]::GetWindowPlacement($h, [ref]$wp)
  $ic = [WinVis]::IsIconic($h)
  $min = ($ic -or $wp.showCmd -eq [WinVis]::SW_SHOWMINIMIZED)
  $arms += [pscustomobject]@{ name = 'not_minimised'; ok = (-not $min); enforced = $true; detail = "IsIconic=$ic showCmd=$($wp.showCmd)" }

  $r = New-Object WinVis+RECT
  [void][WinVis]::GetWindowRect($h, [ref]$r)
  $w = $r.right - $r.left; $ht = $r.bottom - $r.top
  $on = $false
  foreach ($m in $monitors) {
    $b = $m.Bounds
    if ($r.left -lt ($b.X + $b.Width) -and ($r.left + $w) -gt $b.X -and
        $r.top -lt ($b.Y + $b.Height) -and ($r.top + $ht) -gt $b.Y) { $on = $true }
  }
  $geoOk = (($w -gt 0 -and $ht -gt 0) -and $on)
  $arms += [pscustomobject]@{ name = 'rect_on_a_monitor'; ok = $geoOk; enforced = ($monitors.Count -gt 0);
                              detail = ("rect=({0},{1}) {2}x{3} onScreen={4}" -f $r.left, $r.top, $w, $ht, $on) }

  $enf = @($arms | Where-Object { $_.enforced })
  $bad = @($enf | Where-Object { -not $_.ok })
  return [pscustomobject]@{
    hwnd        = ("0x{0:X}" -f [int64]$h)
    title       = [WinVis]::Title($h)
    presentable = ($bad.Count -eq 0)
    failed      = @($bad | ForEach-Object { $_.name })
    arms        = $arms
  }
}

$windows = @($hwnds | ForEach-Object { Test-Window $_ })

# THE PROCESS-LEVEL RULE: at least one window a user could see.
$seen = @($windows | Where-Object { $_.presentable })
$presentable = ($windows.Count -gt 0 -and $seen.Count -gt 0)

# Did the arm that matters actually run anywhere? (belt-and-braces)
$cloakEnforced = @($windows | ForEach-Object { $_.arms } |
                   Where-Object { $_.name -eq 'not_cloaked' -and $_.enforced }).Count -gt 0

$verdict = [pscustomobject]@{
  mode                = $Mode
  presentable         = $presentable
  windows_total       = $windows.Count
  windows_presentable = $seen.Count
  not_cloaked_enforced = $cloakEnforced
  windows             = $windows
  environment         = [pscustomobject]@{
    monitors     = $monitors.Count
    cloak_set_hr = $cloakHr
    os           = [System.Environment]::OSVersion.VersionString
    session_name = $env:SESSIONNAME
  }
}

foreach ($f in $forms) { $f.Close() }

$out = $verdict | ConvertTo-Json -Depth 8
if ($Json -ne '') { Set-Content -Path $Json -Value $out -Encoding utf8 }

Write-Host ("mode={0}  PROCESS presentable={1}  ({2}/{3} windows presentable)  not_cloaked_enforced={4}" -f `
  $Mode, $presentable, $seen.Count, $windows.Count, $cloakEnforced)
foreach ($w in $windows) {
  Write-Host ("  {0} {1,-22} presentable={2} failed=[{3}]" -f $w.hwnd, $w.title, $w.presentable, ($w.failed -join ','))
  foreach ($a in $w.arms) {
    $tag = if (-not $a.enforced) { 'SKIP' } elseif ($a.ok) { 'ok  ' } else { 'FAIL' }
    Write-Host ("     [{0}] {1,-18} {2}" -f $tag, $a.name, $a.detail)
  }
}

if ($presentable) { exit 0 } else { exit 1 }

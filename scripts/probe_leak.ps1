<#
.SYNOPSIS
  Characterize the per-session resource leak: run a session-churning CTS test
  against the dev runtime while sampling the conformance_cli process's handle
  count, GDI/USER objects, and working set. Registry snapshot/restore in finally.
#>
param(
  [string]$TestSpec = "xrCreateSession,xrDestroySession,SessionState",
  [int]   $TimeoutSec = 300,
  [string]$Tag = "leakprobe"
)
$ErrorActionPreference = "Stop"
$wt   = (Resolve-Path "$PSScriptRoot\..").Path
$base = "$wt\build-cts\build\src\conformance\conformance_cli"
$exe  = "$base\RelWithDebInfo\conformance_cli.exe"
$devManifest = "$wt\build\Release\openxr_displayxr-dev.json"
$tmp  = $env:TEMP
$sample = "$tmp\cts_${Tag}_samples.csv"

Add-Type @'
using System;using System.Runtime.InteropServices;
public class GObj {
  [DllImport("user32.dll")] public static extern int GetGuiResources(IntPtr h,int flags);
}
'@

$xrKey="HKLM:\Software\Khronos\OpenXR\1"; $dpKey="HKLM:\Software\DisplayXR\DisplayProcessors\sim-display"
$origRT=(Get-ItemProperty $xrKey -Name ActiveRuntime).ActiveRuntime
$origPO=(Get-ItemProperty $dpKey -Name ProbeOrder).ProbeOrder
$env:DISPLAYXR_MCP="0"; $env:VK_LOADER_LAYERS_DISABLE="*"
"t_s,handles,gdi,user,ws_mb" | Out-File $sample -Encoding ASCII
try {
  Set-ItemProperty $xrKey -Name ActiveRuntime -Value $devManifest -Type String
  Set-ItemProperty $dpKey -Name ProbeOrder -Value 5 -Type DWord
  $p = Start-Process -FilePath $exe -ArgumentList @($TestSpec,"-G","d3d11","--apiVersion","1.1") `
         -WorkingDirectory $base -PassThru -NoNewWindow -RedirectStandardOutput "$tmp\cts_${Tag}.out" -RedirectStandardError "$tmp\cts_${Tag}.err"
  $t0 = 0
  while (-not $p.HasExited -and $t0 -lt $TimeoutSec) {
    Start-Sleep -Seconds 2; $t0 += 2
    try {
      $p.Refresh()
      $gdi  = [GObj]::GetGuiResources($p.Handle, 0)  # GR_GDIOBJECTS
      $user = [GObj]::GetGuiResources($p.Handle, 1)  # GR_USEROBJECTS
      "$t0,$($p.HandleCount),$gdi,$user,$([math]::Round($p.WorkingSet64/1MB))" | Out-File $sample -Append -Encoding ASCII
    } catch {}
  }
  if (-not $p.HasExited) { try { $p.Kill() } catch {}; "RESULT: TIMEOUT/killed" } else { "RESULT: exited code=$($p.ExitCode)" }
}
finally {
  Set-ItemProperty $xrKey -Name ActiveRuntime -Value $origRT -Type String
  Set-ItemProperty $dpKey -Name ProbeOrder -Value ([int]$origPO) -Type DWord
  "RESTORED rt=$((Get-ItemProperty $xrKey -Name ActiveRuntime).ActiveRuntime) po=$((Get-ItemProperty $dpKey -Name ProbeOrder).ProbeOrder)"
}
Write-Output "samples -> $sample"

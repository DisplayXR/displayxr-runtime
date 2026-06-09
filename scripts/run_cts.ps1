<#
.SYNOPSIS
  Run the OpenXR CTS against the freshly-built DisplayXR dev runtime, backed by
  a chosen display-processor plugin, with guaranteed registry restore.

.DESCRIPTION
  On this box the Khronos loader runs elevated and therefore ignores
  XR_RUNTIME_JSON, so we point it at the dev build via HKLM ActiveRuntime, and
  force the desired DP plugin by temporarily lowering its ProbeOrder (lower =
  wins). BOTH registry edits are snapshotted and restored in a finally block —
  the box is left exactly as found even if the run hangs and is killed.

  Defaults run the non-interactive ("automated") subset on D3D11 / OpenXR 1.1,
  emitting a ctsxml report (for the pass/fail matrix) plus a console log.

.PARAMETER Plugin     sim-display (default) | leia-sr | none (leave plugins as-is)
.PARAMETER Graphics   d3d11 (default) | d3d12 | vulkan | opengl
.PARAMETER ApiVersion 1.1 (default) | 1.0
.PARAMETER TestSpec   Catch2 spec; default "exclude:[interactive]"
.PARAMETER TimeoutSec Kill + restore after this many seconds (default 1800)
.PARAMETER Tag        Label for output files (default automated_<graphics>_<api>)
#>
param(
  [string]$Plugin     = "sim-display",
  [string]$Graphics   = "d3d11",
  [string]$ApiVersion = "1.1",
  [string]$TestSpec   = "exclude:[interactive]",
  [int]   $TimeoutSec = 1800,
  [string]$Tag        = "",
  # Enable the CTS's required XR_APILAYER_KHRONOS_runtime_conformance layer for a
  # submission-valid run. Registered in HKLM (the elevated loader ignores
  # XR_API_LAYER_PATH) and requested via -L; snapshot/restored like the rest.
  [switch]$ConformanceLayer
)

$ErrorActionPreference = "Stop"
$wt   = (Resolve-Path "$PSScriptRoot\..").Path   # repo/worktree root (scripts/..)
$base = "$wt\build-cts\build\src\conformance\conformance_cli"
$exe  = "$base\RelWithDebInfo\conformance_cli.exe"
$devManifest = "$wt\build\Release\openxr_displayxr-dev.json"

if (-not $Tag) { $Tag = "${Graphics}_${ApiVersion}" }
$tmp     = $env:TEMP
$xml     = "$tmp\cts_${Tag}.xml"
$console = "$tmp\cts_${Tag}_console.log"

foreach ($p in @($exe,$devManifest)) { if (-not (Test-Path $p)) { throw "missing: $p" } }

$xrKey  = "HKLM:\Software\Khronos\OpenXR\1"
$dpKey  = "HKLM:\Software\DisplayXR\DisplayProcessors\$Plugin"

# ---- snapshot ----
# A clean machine (CI runner with no OpenXR runtime installed) has no Khronos key
# at all; track that so we can fully undo what we create.
$xrKeyPreexisted = Test-Path $xrKey
$origRuntime   = (Get-ItemProperty $xrKey -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime
$origProbe     = $null
if ($Plugin -ne "none") {
  $origProbe = (Get-ItemProperty $dpKey -Name ProbeOrder -ErrorAction SilentlyContinue).ProbeOrder
}
Write-Output "SNAPSHOT ActiveRuntime = $origRuntime"
Write-Output "SNAPSHOT $Plugin ProbeOrder = $origProbe"

# Both CTS layers: the runtime-conformance validation layer (requested via -L)
# and the conformance_test_layer (== conformance_test.dll; validApiLayer requests
# it itself). Register both as Explicit so the elevated loader can find them.
$layerJsons  = @("$base\XrApiLayer_runtime_conformance.json", "$base\XrApiLayer_conformance_test_layer.json")
$explicitKey  = "$xrKey\ApiLayers\Explicit"
$explicitKeyPreexisted = Test-Path $explicitKey
$preRegistered = @{}
if ($ConformanceLayer -and $explicitKeyPreexisted) {
  foreach ($lj in $layerJsons) { $preRegistered[$lj] = $null -ne (Get-ItemProperty $explicitKey -Name $lj -ErrorAction SilentlyContinue) }
}

try {
  # ---- apply ----
  if (-not (Test-Path $xrKey)) { New-Item -Path $xrKey -Force | Out-Null }
  Set-ItemProperty $xrKey -Name ActiveRuntime -Value $devManifest -Type String
  if ($Plugin -ne "none") {
    Set-ItemProperty $dpKey -Name ProbeOrder -Value 5 -Type DWord   # below leia-sr (50)
  }
  Write-Output "APPLIED ActiveRuntime -> $devManifest"
  Write-Output "APPLIED $Plugin ProbeOrder -> 5 (wins)"

  if ($ConformanceLayer) {
    if (-not (Test-Path $explicitKey)) { New-Item -Path $explicitKey -Force | Out-Null }
    foreach ($lj in $layerJsons) { New-ItemProperty -Path $explicitKey -Name $lj -Value 0 -PropertyType DWord -Force | Out-Null }
    Write-Output "APPLIED $($layerJsons.Count) conformance layers (HKLM Explicit)"
  }

  if (Test-Path $xml)     { Remove-Item $xml -Force }
  if (Test-Path $console) { Remove-Item $console -Force }

  # Reduce per-instance overhead/noise: the CTS creates hundreds of instances.
  # MCP spins a named-pipe server per instance; implicit Vulkan layers (e.g. an
  # FPS overlay) can crash the runtime's internal VK device. Neither is under test.
  $env:DISPLAYXR_MCP = "0"
  $env:VK_LOADER_LAYERS_DISABLE = "*"
  # The CTS app has no window of its own, so the D3D11 native compositor
  # self-creates one per session. Keep it windowed (not fullscreen) so a run
  # doesn't repeatedly take over the display.
  $env:XRT_COMPOSITOR_START_WINDOWED = "true"

  $cliArgs = @(
    $TestSpec, "-G", $Graphics, "--apiVersion", $ApiVersion,
    "--reporter", "ctsxml::out=$xml",
    "--reporter", "console::out=$console"
  )
  if ($ConformanceLayer) { $cliArgs += @("-L", "XR_APILAYER_KHRONOS_runtime_conformance") }
  Write-Output "RUN: conformance_cli $($cliArgs -join ' ')"
  Write-Output "CWD: $base"

  $proc = Start-Process -FilePath $exe -ArgumentList $cliArgs -WorkingDirectory $base -PassThru -NoNewWindow
  if (-not $proc.WaitForExit($TimeoutSec * 1000)) {
    Write-Output "TIMEOUT after ${TimeoutSec}s - killing."
    try { $proc.Kill() } catch {}
    $proc.WaitForExit(10000) | Out-Null
    Write-Output "EXITCODE: (killed)"
  } else {
    Write-Output "EXITCODE: $($proc.ExitCode)"
  }
}
finally {
  # ---- restore (always) ----
  if ($Plugin -ne "none" -and $null -ne $origProbe) {
    Set-ItemProperty $dpKey -Name ProbeOrder -Value ([int]$origProbe) -Type DWord
  }
  # Remove the conformance-layer registrations we added (before any wholesale
  # key removal below).
  if ($ConformanceLayer -and (Test-Path $explicitKey)) {
    foreach ($lj in $layerJsons) {
      if (-not $preRegistered[$lj]) { Remove-ItemProperty -Path $explicitKey -Name $lj -ErrorAction SilentlyContinue }
    }
    if (-not $explicitKeyPreexisted) { Remove-Item -Path $explicitKey -Force -ErrorAction SilentlyContinue }
    Write-Output "RESTORED conformance layer registration removed"
  }
  if ($null -ne $origRuntime) {
    Set-ItemProperty $xrKey -Name ActiveRuntime -Value $origRuntime -Type String
    Write-Output "RESTORED ActiveRuntime = $((Get-ItemProperty $xrKey -Name ActiveRuntime -ErrorAction SilentlyContinue).ActiveRuntime)"
  } elseif (-not $xrKeyPreexisted) {
    # Clean machine: we created the Khronos key — remove it wholesale.
    Remove-Item -Path $xrKey -Recurse -Force -ErrorAction SilentlyContinue
    Write-Output "RESTORED ActiveRuntime = (Khronos key removed; did not pre-exist)"
  } else {
    Remove-ItemProperty -Path $xrKey -Name ActiveRuntime -ErrorAction SilentlyContinue
    Write-Output "RESTORED ActiveRuntime = (value removed; did not pre-exist)"
  }
  if ($Plugin -ne "none") {
    Write-Output "RESTORED $Plugin ProbeOrder = $((Get-ItemProperty $dpKey -Name ProbeOrder -ErrorAction SilentlyContinue).ProbeOrder)"
  }
}

Write-Output "XML:     $xml"
Write-Output "CONSOLE: $console"

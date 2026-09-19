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
                      Also sets DXR_PLUGIN_EXCLUSIVE=<Plugin> for the run, so
                      the named plug-in is the ONLY display plug-in the runtime
                      loads — ProbeOrder alone decides which one WINS, not
                      which ones get loaded (#1545). Input providers are
                      skipped too (DXR_INPUT_PROVIDERS=0). Both are process
                      env, so nothing on the box changes.
.PARAMETER Graphics   d3d11 (default) | d3d12 | opengl | vulkan | vulkan2
                      These are the five CTS graphics plugins Windows supports,
                      and a conformance submission owes one automated run per
                      plugin (#1523). `vulkan` and `vulkan2` are SEPARATE
                      plugins exercising XR_KHR_vulkan_enable and
                      XR_KHR_vulkan_enable2; we advertise both. $Graphics is
                      deliberately un-ValidateSet'd and passed verbatim as -G,
                      so any plugin a newer CTS adds works without editing this
                      script.
.PARAMETER ApiVersion 1.1 (default) | 1.0
.PARAMETER TestSpec   Catch2 spec; default "exclude:[interactive]"
                      (the full non-interactive suite, nothing excluded by name).
                      xrLocateSpace_xrLocateViews ran excluded by name from
                      #1491 until #1502 landed (#1516): its two assertions were
                      fixed by #1486 (views.size() == 2) and #1502 (VIEW ==
                      centroid), verified 9/0 on the win box, so it now runs in
                      the default lane. History + rationale:
                      docs/reference/view-configuration-model.md § CTS.
                      Catch2 note if a by-name exclusion is ever needed again:
                      patterns inside ONE filter are ANDed (all m_required
                      match, no m_forbidden matches); a comma starts a SECOND
                      filter and the two are OR'd, which would exclude nothing.
                      Append with "~name", no comma.
.PARAMETER TimeoutSec Kill + restore after this many seconds (default 1800).
                      **0 (or any value <= 0) disables the timeout entirely** —
                      required for the interactive categories, which are paced by
                      a human and would otherwise be killed mid-run. -Interactive
                      defaults this to 0.
.PARAMETER Tag        Label for output files (default automated_<graphics>_<api>)
.PARAMETER Interactive
                      composition | scenario | actions. Selects one of the three
                      HUMAN-EVALUATED CTS categories. These are hand-run only —
                      cts.yml never sets this, because every one of them needs an
                      operator at a Windows box with a display. Setting it:
                        * defaults -TestSpec to "[<category>][interactive]"
                        * defaults -TimeoutSec to 0 (no timeout)
                        * names the outputs the way a conformance submission
                          package wants them, i.e.
                            interactive_composition_<graphics>.xml
                            interactive_scenario_<graphics>.xml
                            interactive_actions_<graphics>_<profile-slug>.xml
                          with both logs alongside, on the same stem
                          (…_console.log, …_stdout.log).
                      An explicit -TestSpec / -TimeoutSec / -Tag still wins.
                      NOTE for an operator: conformance_cli's own stdout is
                      redirected to …_stdout.log and only replayed to the
                      terminal once the process EXITS, so an interactive run
                      shows no live console progress. That is fine — the prompts
                      you act on are rendered composition layers, not stdout —
                      but do not read the silent terminal as a hang, and tail
                      the RUNTIME log (%LOCALAPPDATA%\DisplayXR\) rather than
                      this one when a test needs a log to answer it (e.g. the
                      haptic-confirmation prompt).
                      Operator procedure (what to look at, what counts as a pass,
                      how to package the XML):
                      docs/reference/cts-interactive-procedure.md
.PARAMETER InteractionProfile
                      Interaction profile for conformance_cli's -I argument. The
                      CTS usage guide wants ONE [actions][interactive] result
                      file per supported interaction profile, and -I is how you
                      pick which one a run exercises. Required with
                      "-Interactive actions" unless you name the output yourself
                      with -Tag (otherwise the per-profile result files would all
                      collide on one filename); optional everywhere else.
                      Which profiles this runtime can bind at all is set by the
                      qwerty driver's binding_profiles — see the procedure doc.

                      TRAP, and why this parameter normalises its value: the CTS
                      wants the profile SHORT NAME — "khr/simple_controller" —
                      *without* the "/interaction_profiles/" prefix
                      (OpenXR-CTS src/conformance/usage/configuration.adoc). It
                      compares -I case-insensitively against a shortname built
                      by stripping exactly that prefix, and it never normalises
                      the other way, so a full path matches NOTHING, enables NO
                      profile, and SILENTLY skips every profile-gated [actions]
                      test instead of erroring. (conformance_cli's own -I help
                      string is misleading on this point.) Accept either spelling
                      here and strip the prefix before handing it over.
.PARAMETER ExtraCliArgs
                      Extra arguments appended verbatim to the conformance_cli
                      command line. Needed for "-Interactive actions": qwerty's
                      controllers cannot be unplugged, so the tests' "Turn off
                      /user/hand/left" prompts can never be satisfied and the run
                      needs --nonDisconnectableDevices. That flag MUST be called
                      out and justified in a conformance submission — which is
                      exactly why it is an explicit argument and not something
                      -Interactive turns on behind your back. Do NOT pass
                      --autoSkipTimeout: it auto-advances interactive tests and
                      emits a WARN, and an unexplained warning invalidates a
                      submission.
.PARAMETER QuarantineList
                      Path to a newline-separated list of test-case names to
                      exclude, appended to -TestSpec as `~name` filters. Used
                      by the software-rasterizer tier only
                      (scripts/cts_quarantine_software_tier.txt, #1525); pass
                      nothing on a real-GPU tier so its exclusion set stays
                      empty by construction. A missing file is a hard error, so
                      a typo'd path can never silently mean "no exclusions".
#>
param(
  [string]$Plugin     = "sim-display",
  [string]$Graphics   = "d3d11",
  [string]$ApiVersion = "1.1",
  [string]$TestSpec   = "exclude:[interactive]",
  [int]   $TimeoutSec = 1800,
  [string]$Tag        = "",
  [ValidateSet("", "composition", "scenario", "actions")]
  [string]$Interactive        = "",
  [string]$InteractionProfile = "",
  [string[]]$ExtraCliArgs     = @(),
  [string]$QuarantineList     = "",
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

# Normalise the interaction profile to the CTS's SHORT form. A full path silently
# enables nothing (see the .PARAMETER note), so accept either and strip.
if ($InteractionProfile) {
  $InteractionProfile = $InteractionProfile -replace '^/?interaction_profiles?/', ''
}

# ---- interactive categories (hand-run; cts.yml never sets -Interactive) ----
# Defaults only: anything the caller passed explicitly is left alone.
if ($Interactive) {
  if (-not $PSBoundParameters.ContainsKey('TestSpec'))   { $TestSpec   = "[$Interactive][interactive]" }
  if (-not $PSBoundParameters.ContainsKey('TimeoutSec')) { $TimeoutSec = 0 }
  if (-not $Tag) {
    $Tag = "interactive_${Interactive}_${Graphics}"
    if ($Interactive -eq "actions") {
      # One result file per interaction profile (CTS usage guide). Without the
      # profile in the name every profile's run would overwrite the last one.
      if (-not $InteractionProfile) {
        throw "-Interactive actions requires -InteractionProfile (the submission wants one result file per interaction profile)"
      }
      $Tag += "_" + (($InteractionProfile -replace '[^A-Za-z0-9]+', '_').Trim('_'))
    }
  }
}

if (-not $Tag) { $Tag = "${Graphics}_${ApiVersion}" }
$tmp     = $env:TEMP
# Interactive runs are named for the submission package (interactive_<cat>_<gfx>…);
# automated runs keep the historical cts_<tag> stem. All three outputs share the
# stem so a run's XML, reporter log and stdout log always sort together.
$stem    = "cts_$Tag"
if ($Interactive) { $stem = $Tag }
$xml     = "$tmp\$stem.xml"
$console = "$tmp\${stem}_console.log"
# conformance_cli prints its frame-timing block (Average xrWaitFrame wait time,
# Overhead score, ...) on its own STDOUT, not through the Catch2 console
# reporter — so those numbers were never in $console and anything scraping
# $console for them found nothing. Capture stdout separately; $console keeps
# the reporter output untouched.
$stdoutLog = "$tmp\${stem}_stdout.log"
# Which implementation produced this result. A CTS XML records the graphics
# PLUGIN, never the renderer that answered it, so a run on llvmpipe and a run
# on an RTX card are indistinguishable after the fact — an ambiguity a tiered
# lane cannot afford (#1525). Same stem as everything else.
$identity  = "$tmp\${stem}_graphics_identity.txt"

foreach ($p in @($exe,$devManifest)) { if (-not (Test-Path $p)) { throw "missing: $p" } }

# ---- quarantine list -> Catch2 `~name` exclusions (#1525) ----
# Same Catch2 trap documented for -TestSpec above: patterns inside ONE filter
# are ANDed (every m_required matches, no m_forbidden matches); a COMMA starts
# a SECOND filter and filters are OR'd, so a comma here would exclude nothing.
# Append "~name" with NO comma.
if ($QuarantineList) {
  if (-not (Test-Path $QuarantineList)) { throw "quarantine list not found: $QuarantineList" }
  $names = @(Get-Content $QuarantineList |
             ForEach-Object { ($_ -replace '#.*$', '').Trim() } |
             Where-Object { $_ })
  if ($names.Count -eq 0) {
    Write-Output "QUARANTINE: $QuarantineList is empty — nothing excluded by name"
  } else {
    foreach ($n in $names) {
      # Quote only when needed: Catch2 reads an unquoted token as a name
      # pattern up to the next filter character, which is fine for the
      # underscore-heavy CTS names.
      $pat = if ($n -match '[\s,\[\]~"]') { '"' + $n + '"' } else { $n }
      $TestSpec += "~$pat"
      Write-Output "QUARANTINE: excluding '$n'"
    }
    Write-Output "QUARANTINE: $($names.Count) test(s) excluded from $QuarantineList"
  }
}

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
#
# Discover each manifest instead of hardcoding a path: openxr-cts-1.1.44+ moved
# the layer JSON copy from conformance_cli's binary dir to each layer's own
# TARGET_FILE_DIR (Khronos MR 3576-era CMake change) — the old
# .../conformance_cli/XrApiLayer_*.json paths no longer exist, so the loader
# reported "failed to find layer" -> XR_ERROR_API_LAYER_NOT_PRESENT (#726). The
# manifest's library_path is "./<dll>" (same-dir relative), so we must register
# the copy that sits next to the built DLL; pick the one whose sibling DLL exists.
$ctsBuild = "$wt\build-cts\build"
function Resolve-LayerManifest {
  param([string]$JsonName, [string]$DllName)
  $hits = @(Get-ChildItem -Path $ctsBuild -Recurse -Filter $JsonName -File -ErrorAction SilentlyContinue)
  foreach ($h in $hits) {
    if (Test-Path (Join-Path $h.DirectoryName $DllName)) { return $h.FullName }
  }
  # Fall back to the first hit (prefer a RelWithDebInfo copy) even if we can't
  # confirm the sibling DLL, so failures surface as a loader error not a silent skip.
  $rel = $hits | Where-Object { $_.FullName -match 'RelWithDebInfo' } | Select-Object -First 1
  if ($rel) { return $rel.FullName }
  if ($hits.Count) { return $hits[0].FullName }
  return $null
}
$layerJsons = @()
if ($ConformanceLayer) {
  $runtimeConf = Resolve-LayerManifest 'XrApiLayer_runtime_conformance.json' 'XrApiLayer_runtime_conformance.dll'
  $testLayer   = Resolve-LayerManifest 'XrApiLayer_conformance_test_layer.json' 'conformance_test.dll'
  foreach ($m in @($runtimeConf, $testLayer)) { if ($m) { $layerJsons += $m } }
  if (-not $runtimeConf) { throw "could not locate XrApiLayer_runtime_conformance.json under $ctsBuild" }
  if (-not $testLayer)   { throw "could not locate XrApiLayer_conformance_test_layer.json under $ctsBuild" }
  Write-Output "RESOLVED runtime-conformance layer manifest: $runtimeConf"
  Write-Output "RESOLVED conformance-test layer manifest:    $testLayer"
}
$explicitKey  = "$xrKey\ApiLayers\Explicit"
$explicitKeyPreexisted = Test-Path $explicitKey
$preRegistered = @{}
if ($ConformanceLayer -and $explicitKeyPreexisted) {
  foreach ($lj in $layerJsons) { $preRegistered[$lj] = $null -ne (Get-ItemProperty $explicitKey -Name $lj -ErrorAction SilentlyContinue) }
}

# ---- the CTS's VULKAN conformance layer (#1525) ----
# XR_APILAYER_KHRONOS_runtime_conformance ships a second face: a *Vulkan* layer
# `VK_LAYER_OPENXR_xr_runtime_conformance`, implemented in the same DLL. When
# the OpenXR layer is enabled, the CTS's Vulkan graphics plugin hard-requires
# it — graphics_plugin_vulkan.cpp XRC_CHECK_THROWs on
# `it != availableLayers.end()` — so EVERY session-creating test in the vulkan
# and vulkan2 arms dies with "Check failed / Origin: it != availableLayers.end()"
# if the Vulkan loader cannot see it. Not a runtime defect and not a
# software-rasterizer limitation: the CTS is a from-source build, its generated
# VkLayer_OPENXR_xr_runtime_conformance.json sits in the build tree, and
# nothing puts that on the Vulkan loader's search path.
#
# Register it two ways on purpose:
#   * HKLM ExplicitLayers — authoritative, and the only one that survives an
#     ELEVATED process (the Vulkan loader reads its path env vars through a
#     secure getenv, exactly like the Khronos OpenXR loader ignores
#     XR_RUNTIME_JSON when elevated — the reason this script uses HKLM at all).
#   * VK_ADD_LAYER_PATH — covers a non-elevated run.
# Both are snapshotted and undone in the finally, like everything else here.
#
# The generated JSON's library_path is "./XrApiLayer_runtime_conformance.dll",
# relative to the JSON — but under Ninja Multi-Config the DLL lands in a
# per-config subdirectory while the JSON does not. Rather than depend on that
# layout, write our own copy carrying an ABSOLUTE library_path.
$vkLayerDir  = $null
$vkLayerJson = $null
$vkLayerKey  = "HKLM:\SOFTWARE\Khronos\Vulkan\ExplicitLayers"
$vkLayerKeyPreexisted = $false
if ($ConformanceLayer -and $Graphics -match '^vulkan') {
  $confDll = Join-Path (Split-Path $runtimeConf) 'XrApiLayer_runtime_conformance.dll'
  if (-not (Test-Path $confDll)) {
    $hit = Get-ChildItem -Path $ctsBuild -Recurse -Filter 'XrApiLayer_runtime_conformance.dll' -File -ErrorAction SilentlyContinue |
           Select-Object -First 1
    if ($hit) { $confDll = $hit.FullName }
  }
  if (-not (Test-Path $confDll)) {
    throw "could not locate XrApiLayer_runtime_conformance.dll under $ctsBuild — the vulkan/vulkan2 arms cannot run with -ConformanceLayer"
  }
  $vkLayerDir  = Join-Path $tmp "dxr_cts_vk_layer"
  New-Item -ItemType Directory -Force -Path $vkLayerDir | Out-Null
  $vkLayerJson = Join-Path $vkLayerDir "VkLayer_OPENXR_xr_runtime_conformance.json"
  $vkManifest = [ordered]@{
    file_format_version = "1.0.0"
    layer = [ordered]@{
      name                   = "VK_LAYER_OPENXR_xr_runtime_conformance"
      type                   = "GLOBAL"
      library_path           = $confDll
      api_version            = "1.0.0"
      implementation_version = "1"
      description            = "API Layer to validate OpenXR runtime conformance"
      disable_environment    = @{ OPENXR_xr_runtime_conformance_disabled = "1" }
    }
  }
  $vkManifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $vkLayerJson -Encoding UTF8
  $vkLayerKeyPreexisted = Test-Path $vkLayerKey
  Write-Output "RESOLVED Vulkan conformance layer DLL: $confDll"
  Write-Output "WROTE    Vulkan layer manifest:        $vkLayerJson"
}

# ---- the software Vulkan ICD, registered where an ELEVATED loader looks (#1525) ----
# scripts/fetch_mesa_rasterizers.ps1 exports VK_DRIVER_FILES for lavapipe, and
# that is enough for an ordinary process. It is NOT enough here: the Vulkan
# loader reads every path-override variable (VK_DRIVER_FILES, VK_ICD_FILENAMES,
# VK_LAYER_PATH, VK_ADD_LAYER_PATH) through a secure getenv and DISCARDS them in
# a high-integrity process — the same hazard this script already works around
# for the Khronos OpenXR loader and XR_RUNTIME_JSON. A GitHub-hosted windows
# runner runs its steps elevated, so the ICD was invisible and vkCreateInstance
# returned VK_ERROR_INCOMPATIBLE_DRIVER while the env var sat there looking
# correct. Register in HKLM as well; snapshot + restore as usual.
$vkIcdKey = "HKLM:\SOFTWARE\Khronos\Vulkan\Drivers"
$vkIcdPaths = @()
if ($env:VK_DRIVER_FILES) {
  $vkIcdPaths = @($env:VK_DRIVER_FILES -split ';' | Where-Object { $_ -and (Test-Path $_) })
}
$vkIcdKeyPreexisted = Test-Path $vkIcdKey
$vkIcdPreRegistered = @{}
foreach ($p in $vkIcdPaths) {
  $vkIcdPreRegistered[$p] = $vkIcdKeyPreexisted -and ($null -ne (Get-ItemProperty $vkIcdKey -Name $p -ErrorAction SilentlyContinue))
}

# Elevation is load-bearing for everything above, so state it rather than
# leaving a future reader to infer it from a confusing symptom.
$isElevated = ([Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent()
              ).IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
Write-Output "ELEVATED: $isElevated (if true, the OpenXR and Vulkan loaders ignore their path env vars; HKLM is the only channel)"

$runStart = (Get-Date).AddSeconds(-5)   # slack for clock skew on the log stamps

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

  if ($vkLayerJson) {
    if (-not (Test-Path $vkLayerKey)) { New-Item -Path $vkLayerKey -Force | Out-Null }
    New-ItemProperty -Path $vkLayerKey -Name $vkLayerJson -Value 0 -PropertyType DWord -Force | Out-Null
    $env:VK_ADD_LAYER_PATH = $vkLayerDir
    Write-Output "APPLIED VK_LAYER_OPENXR_xr_runtime_conformance (HKLM ExplicitLayers + VK_ADD_LAYER_PATH)"
  }

  if ($vkIcdPaths.Count) {
    if (-not (Test-Path $vkIcdKey)) { New-Item -Path $vkIcdKey -Force | Out-Null }
    foreach ($p in $vkIcdPaths) { New-ItemProperty -Path $vkIcdKey -Name $p -Value 0 -PropertyType DWord -Force | Out-Null }
    Write-Output "APPLIED $($vkIcdPaths.Count) Vulkan ICD(s) (HKLM Drivers): $($vkIcdPaths -join '; ')"
  }

  if (Test-Path $xml)     { Remove-Item $xml -Force }
  if (Test-Path $console) { Remove-Item $console -Force }
  if (Test-Path $stdoutLog) { Remove-Item $stdoutLog -Force }

  # Reduce per-instance overhead/noise: the CTS creates hundreds of instances.
  # MCP spins a named-pipe server per instance; implicit Vulkan layers (e.g. an
  # FPS overlay) can crash the runtime's internal VK device. Neither is under test.
  $env:DISPLAYXR_MCP = "0"
  # `~implicit~`, NOT `*`: the blanket form also hides EXPLICIT layers from
  # vkEnumerateInstanceLayerProperties, including the CTS's own
  # VK_LAYER_OPENXR_xr_runtime_conformance — which the Vulkan graphics plugin
  # hard-requires whenever the OpenXR conformance layer is on, so the whole
  # vulkan/vulkan2 arm errors out before any test does real work (#1525).
  # `~implicit~` keeps the original intent (no third-party overlay injecting
  # itself into the runtime's VK device) and nothing else.
  $env:VK_LOADER_LAYERS_DISABLE = "~implicit~"
  # The CTS app has no window of its own, so the D3D11 native compositor
  # self-creates one per session. Keep it windowed (not fullscreen) so a run
  # doesn't repeatedly take over the display.
  $env:XRT_COMPOSITOR_START_WINDOWED = "true"
  # #1545: run the lane EXCLUSIVE — the named display plug-in is the only one
  # loaded, and input providers are not discovered at all.
  #
  # Lowering $Plugin's ProbeOrder only decides which plug-in WINS; every other
  # registered one is still LoadLibrary'd, because display-claim collection
  # consults all of them (#69 / ADR-015). On a box with the Leia SR plug-in
  # installed that drags SimulatedRealityOpenGL.dll -> opengl32 -> the NVIDIA
  # GL ICD into a `-G d3d11` process, and the multithreading case then faults
  # in an NV ICD worker with rip=0 after ~547 instance cycles. The other half
  # of that crash is the Ultraleap provider re-spinning its LeapC thread pool
  # on every xrCreateInstance, which is what creates the racing thread.
  #
  # Set on the PowerShell process (NOT via the run script) so the child's CRT
  # captures them at startup: the runtime DLL has its own static-CRT
  # environment block, same reason XRT_FORCE_MODE is set this way.
  if ($Plugin -ne "none") {
    $env:DXR_PLUGIN_EXCLUSIVE = $Plugin
    Write-Output "APPLIED DXR_PLUGIN_EXCLUSIVE=$Plugin (no other display plug-in is loaded)"
  }
  $env:DXR_INPUT_PROVIDERS = "0"
  Write-Output "APPLIED DXR_INPUT_PROVIDERS=0 (input-provider discovery skipped; hands stay on qwerty)"

  # OpenXR-CTS renamed the API-version CLI arg `--apiVersion` -> `--minApiVersion`
  # in openxr-cts-1.1.44+ (Khronos CHANGELOG.CTS.md, internal MR 3576). Under
  # 1.1.51 the old spelling is rejected as an "Unrecognised token" and
  # conformance_cli exits (code 2) before running a single test — no result XML,
  # which surfaced as the bogus "crashed before writing results" in #726.
  $cliArgs = @(
    $TestSpec, "-G", $Graphics, "--minApiVersion", $ApiVersion,
    "--reporter", "ctsxml::out=$xml",
    "--reporter", "console::out=$console"
  )
  if ($ConformanceLayer)    { $cliArgs += @("-L", "XR_APILAYER_KHRONOS_runtime_conformance") }
  # Always pass -I explicitly, never rely on the CTS's "khr/simple_controller"
  # default: the default is injected into globalData AFTER Options is snapshotted,
  # so the ctsxml <cts:enabledInteractionProfiles> element comes out EMPTY and the
  # result file does not record which profile was tested.
  if ($InteractionProfile)  { $cliArgs += @("-I", $InteractionProfile) }
  if ($ExtraCliArgs.Count)  { $cliArgs += $ExtraCliArgs }
  Write-Output "RUN: conformance_cli $($cliArgs -join ' ')"
  Write-Output "CWD: $base"

  # CWD must be the exe's own dir: the CTS loads its data assets (brdf_lut.png,
  # *.glb, ...) relative to the working directory, and the D3D11 plugin's
  # InitializeDevice swallows the failed read into `return false` -> every
  # session-creating test errors with XR_ERROR_RUNTIME_FAILURE (#830).
  $proc = Start-Process -FilePath $exe -ArgumentList $cliArgs -WorkingDirectory (Split-Path $exe) -PassThru -NoNewWindow -RedirectStandardOutput $stdoutLog
  $null = $proc.Handle   # cache the handle NOW or .ExitCode reads back empty after exit (PS quirk)
  if ($TimeoutSec -le 0) {
    # No timeout. The interactive categories are paced by a human pressing Select
    # per test; the 1800 s default would kill the run partway and leave a
    # truncated XML that still LOOKS like a result file.
    #
    # Ctrl+C here is not a clean exit: PowerShell may tear the pipeline down
    # without running the finally block below, leaving ActiveRuntime pointed at
    # the dev build. If you abort, verify with `displayxr-cli runtime status`
    # and re-point with `displayxr-cli runtime activate <manifest>`.
    Write-Output "NO TIMEOUT (interactive) - waiting for conformance_cli to exit."
    $proc.WaitForExit()
    Write-Output "EXITCODE: $($proc.ExitCode)"
  } elseif (-not $proc.WaitForExit($TimeoutSec * 1000)) {
    Write-Output "TIMEOUT after ${TimeoutSec}s - killing."
    try { $proc.Kill() } catch {}
    $proc.WaitForExit(10000) | Out-Null
    Write-Output "EXITCODE: (killed)"
  } else {
    Write-Output "EXITCODE: $($proc.ExitCode)"
    if ($proc.ExitCode -eq -1073741515) {
      # Decoding this by hand cost a CI round once (#1525) — do it here forever.
      Write-Output "HINT: 0xC0000135 STATUS_DLL_NOT_FOUND. conformance_cli died at LOAD time, before any test ran, so there is no XML and no console log to read. A dependent DLL is missing from the exe's own directory. The two this harness puts there: vulkan-1.dll (fetch_build_cts.bat, required once the CTS builds with Vulkan — it breaks the d3d11/d3d12 arms too) and Mesa's libgallium_wgl.dll beside opengl32.dll (fetch_mesa_rasterizers.ps1)."
    }
  }

  # -RedirectStandardOutput can only target a FILE, so the live view of
  # conformance_cli's progress in the terminal / CI job log would otherwise
  # disappear. Replay it once the process is done — as ONE write, because
  # piping 40k+ lines through Write-Host takes minutes.
  if (Test-Path $stdoutLog) {
    $raw = Get-Content $stdoutLog -Raw
    if ($raw) { Write-Host $raw }
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
  if ($vkLayerJson) {
    if (Test-Path $vkLayerKey) {
      Remove-ItemProperty -Path $vkLayerKey -Name $vkLayerJson -ErrorAction SilentlyContinue
      if (-not $vkLayerKeyPreexisted) { Remove-Item -Path $vkLayerKey -Force -ErrorAction SilentlyContinue }
    }
    Remove-Item -Path Env:\VK_ADD_LAYER_PATH -ErrorAction SilentlyContinue
    Write-Output "RESTORED Vulkan conformance layer registration removed"
  }
  if ($vkIcdPaths.Count -and (Test-Path $vkIcdKey)) {
    foreach ($p in $vkIcdPaths) {
      if (-not $vkIcdPreRegistered[$p]) { Remove-ItemProperty -Path $vkIcdKey -Name $p -ErrorAction SilentlyContinue }
    }
    if (-not $vkIcdKeyPreexisted) { Remove-Item -Path $vkIcdKey -Force -ErrorAction SilentlyContinue }
    Write-Output "RESTORED Vulkan ICD registration removed"
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

# ---- graphics identity (#1525) ----
# "Every result file records the renderer/device that produced it." Scrape it
# out of the runtime's own log and drop it beside the XML as an artefact.
# Sources, both WARN-level so they are present without raising any log level:
#   GL  — comp_gl_win32_client.c / comp_gl_compositor.cpp "GLAD loaded: …
#         renderer: <GL_RENDERER>" / "OpenGL context: GL_RENDERER: …"
#   VK  — vk_bundle_init.c       "Vulkan selected GPU n: <deviceName> (<type>)"
try {
  $idLines = New-Object System.Collections.Generic.List[string]
  $idLines.Add("tag:        $Tag")
  $idLines.Add("graphics:   $Graphics (OpenXR $ApiVersion)")
  $idLines.Add("plugin:     $Plugin")
  $idLines.Add("exclusive:  DXR_PLUGIN_EXCLUSIVE=$($env:DXR_PLUGIN_EXCLUSIVE) DXR_INPUT_PROVIDERS=$($env:DXR_INPUT_PROVIDERS)")
  $swVer = $env:DXR_CTS_SOFTWARE_GFX_VERSION
  if ($swVer) {
    $idLines.Add("software:   $swVer")
    $idLines.Add("            GALLIUM_DRIVER=$($env:GALLIUM_DRIVER) VK_DRIVER_FILES=$($env:VK_DRIVER_FILES)")
  } else {
    $idLines.Add("software:   (none provisioned — DXR_CTS_SOFTWARE_GFX_VERSION unset)")
  }

  $logDir = Join-Path $env:LOCALAPPDATA "DisplayXR"
  $hits = @()
  if (Test-Path $logDir) {
    # Strip the leading "[timestamp] " before de-duplicating, or the adapter
    # line — emitted once per session, i.e. hundreds of times — comes back as
    # hundreds of "unique" strings and buries the one identity line that matters.
    $hits = @(Get-ChildItem -Path $logDir -Filter "DisplayXR_conformance_cli*.log" -File -ErrorAction SilentlyContinue |
              Where-Object { $_.LastWriteTime -ge $runStart } |
              Select-String -Pattern 'GLAD loaded:|OpenGL context:|Vulkan selected GPU|render adapter: no adapter survived' |
              ForEach-Object { ($_.Line -replace '^\s*\[[0-9][^\]]*\]\s*', '').Trim() } |
              Select-Object -Unique |
              Select-Object -First 10)
  }
  if ($hits.Count) {
    $idLines.Add("renderer:")
    foreach ($h in $hits) { $idLines.Add("  $h") }
  } else {
    $idLines.Add("renderer:   (no GLAD/Vulkan identity line found in $logDir)")
  }
  Set-Content -LiteralPath $identity -Value $idLines -Encoding UTF8
  Write-Output "--- graphics identity ---"
  $idLines | ForEach-Object { Write-Output $_ }
} catch {
  Write-Output "graphics-identity scrape failed (non-fatal): $_"
}

Write-Output "XML:      $xml"
Write-Output "CONSOLE:  $console"
Write-Output "STDOUT:   $stdoutLog"
Write-Output "IDENTITY: $identity"

# Unset the two exclusivity vars now the identity block has recorded them.
# `& run_cts.ps1` runs in the CALLER's session, so leaving DXR_PLUGIN_EXCLUSIVE
# set would silently pin the next app launched from that same prompt to one
# display plug-in — a confusing state to debug hours later.
Remove-Item -Path Env:\DXR_PLUGIN_EXCLUSIVE -ErrorAction SilentlyContinue
Remove-Item -Path Env:\DXR_INPUT_PROVIDERS  -ErrorAction SilentlyContinue

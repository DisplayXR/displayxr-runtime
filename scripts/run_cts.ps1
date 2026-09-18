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
  if (Test-Path $stdoutLog) { Remove-Item $stdoutLog -Force }

  # Reduce per-instance overhead/noise: the CTS creates hundreds of instances.
  # MCP spins a named-pipe server per instance; implicit Vulkan layers (e.g. an
  # FPS overlay) can crash the runtime's internal VK device. Neither is under test.
  $env:DISPLAYXR_MCP = "0"
  $env:VK_LOADER_LAYERS_DISABLE = "*"
  # The CTS app has no window of its own, so the D3D11 native compositor
  # self-creates one per session. Keep it windowed (not fullscreen) so a run
  # doesn't repeatedly take over the display.
  $env:XRT_COMPOSITOR_START_WINDOWED = "true"

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
Write-Output "STDOUT:  $stdoutLog"

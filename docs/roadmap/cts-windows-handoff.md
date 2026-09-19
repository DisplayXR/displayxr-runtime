# CTS bring-up — Windows handoff runbook (#33)

Picks up where the macOS session left off. The platform-agnostic half of
"pass the non-interactive OpenXR CTS subset" is done and on this branch
(`feature/cts-conformance-automation`). The remaining work is **inherently
Windows**: build the Khronos conformance suite, run it against the runtime, and
iterate. First target: **Windows + D3D11**.

> The Phase 1–4 narrative below is the original bring-up plan and is kept for
> provenance. For **what the lane runs today**, read the next section first.

## Current state — certified version + the arm matrix (#1523)

**Certified against: OpenXR-CTS `1.1.63.0`, Khronos loader `1.1.63`.** Both pins
are single-sourced — the CTS tag in `scripts/fetch_build_cts.bat` (`CTS_TAG`,
which carries the rationale for *why* 1.1.63.0 and not an older release) and the
loader version in `.github/workflows/cts.yml`'s *Install OpenXR loader* step.
They must move together (#1487). The device under test is the vendor-neutral
**`sim-display`** plug-in, registered into
`HKLM\Software\DisplayXR\DisplayProcessors` by the lane itself — the runs are
hardware-free by construction.

A submission owes **one automated run per graphics plugin per platform**. The
plugins we advertise, and therefore owe:

| Platform | Graphics plugins | Automated runs |
|---|---|---:|
| **Windows** | `d3d11`, `d3d12`, `opengl`, `vulkan`, `vulkan2` | 5 |
| **Linux** (Vulkan-only, X11/XCB) | `vulkan`, `vulkan2` | 2 |
| **Android** (Vulkan-only) | `vulkan`, `vulkan2` | 2 |

`vulkan` and `vulkan2` are **separate CTS plugins** (`XR_KHR_vulkan_enable` vs
`XR_KHR_vulkan_enable2`); we advertise both, so neither substitutes for the
other. macOS / `metal` is deliberately **out of this matrix** — the platform is
deferred.

### Which arms actually execute, and on what

`cts.yml` builds the runtime and the CTS **once** and fans the arms out over
that single build (`plan` → `build` → `run` matrix → `summary`). What each arm
runs on today:

| Arm | Backing | Gated? | Blocker |
|---|---|---|---|
| `d3d11` | GitHub-hosted `windows-2022`, **WARP** (D3D software rasterizer, always present in the image) | **yes** | — |
| `d3d12` | GitHub-hosted `windows-2022`, **WARP** | **yes** | — |
| `opengl` | needs a **software ICD** (Mesa llvmpipe) — the hosted image has no usable GL 4.x context | no — `continue-on-error`, reported only | #1525, then #1522 (GL teardown race) |
| `vulkan` | needs a **software ICD** (lavapipe / SwiftShader) — the hosted image ships no Vulkan ICD | no — `continue-on-error`, reported only | #1525 |
| `vulkan2` | same as `vulkan` | no — `continue-on-error`, reported only | #1525 |
| Linux `vulkan` / `vulkan2` | **real GPU** (hardware-validated on NVIDIA / Ubuntu 22.04) — no runner yet | not in CI | #1523 part 2 |
| Android `vulkan` / `vulkan2` | **real device** — no runner yet | not in CI | #1523 part 2, #1212 |

The three experimental Windows arms are driven by **one flag**: the
`EXPERIMENTAL="opengl vulkan vulkan2"` line in `cts.yml`'s `plan` job. It sets
each arm's `continue-on-error` and tells the `summary` job which arms to exclude
from the gate. #1525 empties that string and nothing else changes.

Beyond software rasterizers, **#1526** tracks a self-hosted Windows runner on a
hybrid iGPU+dGPU box — a real-GPU lane is what turns the three software-ICD arms
from "it ran" into a defensible submission, and it is also the only way to cover
adapter selection (`DXR_D3D_FORCE_GPU` / `DXR_VK_FORCE_GPU`).

### Result artefacts

Each arm writes `%TEMP%\cts_ci_<graphics>.xml` (ctsxml) plus
`cts_ci_<graphics>_console.log`, staged and uploaded as
**`cts-results-<graphics>`**. The set is assembled to be droppable into a
Khronos submission package unmodified — one XML per graphics plugin, named after
it. The `summary` job downloads every arm, prints one table (arm / tests /
failures / errors / status) into the run summary, and **fails the lane if any
non-experimental arm was red or produced no XML**. A single arm no longer gates
alone.

Triggers: PR → `smoke` / `d3d11` only (fast); nightly cron + `v*` tag → `full`
over all five Windows arms; `workflow_dispatch` → any single plugin or `all`,
at either scope.

## What's already done (this branch)

`XR_EXT_conformance_automation` — the mechanism the CTS uses to inject synthetic
controller input — is implemented (was header-only before). See commit
`feat(#33): implement XR_EXT_conformance_automation`:

- 5 entry points wired + advertised: `xrSetInputDeviceActive/StateBool/
  StateFloat/StateVector2f/LocationEXT`.
- Per-session override store in `oxr_conformance.c` keyed by the input **source
  path** (== `oxr_action_input.bound_path`, so apply is a direct XrPath compare).
- Value overrides applied in `oxr_input_combine_input` (`oxr_input.c`); pose
  overrides in `oxr_space_locate` (`oxr_space.c`). State freed in
  `oxr_session_destroy` — nothing survives the CTS load/unload cycling.
- Compiles + links clean on macOS (full runtime build).

## Setup — work in a worktree on this branch

This box may have its own working state; do not disturb it. Create an isolated
worktree on the existing branch:

```bat
cd C:\path\to\displayxr-runtime
git fetch origin
git worktree add .claude\worktrees\cts feature/cts-conformance-automation
cd .claude\worktrees\cts
```

Build the runtime from the worktree per CLAUDE.md (`scripts\build_windows.bat
all`). Confirm the conformance build compiles on MSVC (macOS only proved Clang).

## Phase 1 — stand up the CTS harness + baseline

1. Fetch + build the Khronos **OpenXR-CTS** (`conformance_cli` +
   `conformance_test`) for Windows/x64. Add a fetch script under `scripts\`
   (out-of-tree, like the vcpkg/loader fetch in `build_windows.bat`).
2. Register the dev runtime so the **Khronos loader** finds it — reuse the
   `openxr_displayxr-dev.json` manifest + `HKLM\...\Khronos\OpenXR\1\
   ActiveRuntime`. Run `conformance_cli` from a **non-elevated** shell (the
   bundled loader ignores `XR_RUNTIME_JSON` when elevated — see CLAUDE.md).
3. Run with **core + `XR_KHR_D3D11_enable` only**, selecting the non-interactive
   categories. Confirm via `%LOCALAPPDATA%\DisplayXR\...log` (`loaded from:`)
   that our DLL was loaded.
4. **Capture the failure baseline.** Triage into: (a) needs conformance_
   automation, (b) error-code/quirk mismatches, (c) over-advertisement, (d)
   graphics/frame-submission. Expect much to pass already (Monado lineage).

## Phase 2 — validate + iterate conformance_automation

This is the part that needed real CTS to verify. Confirm an input test that
previously errored now passes: `xrSetInputDeviceStateBoolEXT` →
`xrSyncActions` → `xrGetActionStateBoolean` returns the injected value with
correct `isActive` / `lastChangeTime`, on `/interaction_profiles/khr/
simple_controller`.

Three known items flagged in the implementation to resolve against CTS output:

1. **R1 — role ownership (verify first).** The oxr-level injection only surfaces
   if the **qwerty** device owns `/user/hand/left|right` under the CTS launch
   config. `sim_display` claims only `head`, so qwerty's `certain.left/right`
   (`target_builder_qwerty.c`) should win — but assert it (a `displayxr-cli`
   line or log that L/R roles resolve to a Qwerty device). If a display plug-in
   ever claims hand roles, the fallback is a dedicated `drv_conformance` device.
2. **Device-inactive suppression.** `xrSetInputDeviceActiveEXT(false)` currently
   suppresses only *overridden* inputs, not un-overridden qwerty defaults. If
   CTS asserts a fully-inactive device reports `isActive=false` for all sources,
   extend the apply path to gate qwerty's own inputs for that top-level path
   (needs the action_input's top-level path at combine time — thread it through
   or compare the bound_path's owning role).
3. **Pose space-relative.** `xrSetInputDeviceLocationEXT` applies the pose in
   base-space; if CTS pose tests fail, resolve it through the supplied `XrSpace`
   in `oxr_api_conformance.c` / the `oxr_space_locate` hook.

## Phase 3 — conformance-targeted run config

Run CTS requesting **only** what the runtime can back: core + D3D11 +
conformance_automation + the profiles qwerty advertises (simple/touch/index/
vive/WMR). Document the in-scope vs excluded set. For anything CTS still selects
that's unbackable (hand_tracking, eye_gaze, vendor controllers): implement
minimal backing or exclude via the run manifest — and **log every exclusion**
(no silent truncation).

## Phase 4 — triage to green + tiered CI

- Error-code quirks go through the existing `quirks` mechanism (pattern at
  `oxr_api_space.c:205` `no_validation_error_in_create_ref_space`).
- Graphics/frame-submission failures: fix in the D3D11 native compositor path.
- Watch for the upstream teardown/destruction race (CTS loads/unloads hundreds
  of times — `docs/legacy-monado/tracing-perfetto.md`). Our conformance state is
  per-session and freed in destroy, but validate clean re-init under load.
- **CI cadence (don't run full CTS on every PR):**
  - PR, path-filtered to `state_trackers/oxr/` · compositor · CTS harness →
    **smoke subset, D3D11** (~2–5 min).
  - Nightly cron on `main` → **full** non-interactive suite.
  - Release tag `v*` → **full** suite as a hard gate (next to the ABI gate).
  - `workflow_dispatch` → on-demand full/any-API.
- Update #33 with the supported-category matrix.
- **The interactive categories are in scope, and have a written procedure:**
  [CTS interactive procedure](../reference/cts-interactive-procedure.md).
  (This bullet used to say they were out of scope by design, "CTS interactive
  tests assume an HMD + controllers". Half of that is true and the conclusion
  was not: there is no HMD, but every controller input the CTS needs is backed
  by the **qwerty** driver, which `target_builder_sim_display.c` adds
  unconditionally and which carries binding-profile remaps for five interaction
  profiles. A conformance submission *requires* one interactive-composition run
  per graphics API, so skipping them was never an option — #1523 § 4.)

## Software-rasterizer tier — what produced a result file (#1525)

A GitHub-hosted `windows-2022` runner has **no GPU driver**: its only DXGI
adapter is Microsoft Basic Render Driver (WARP), its only OpenGL is the GDI
generic **1.1** implementation, and it has **no Vulkan ICD at all**. So without
provisioning, three of the five Windows arms cannot start — this is not "they
fail", it is "they never reach a test".

`scripts/fetch_mesa_rasterizers.ps1` provisions both from one pinned
`pal1000/mesa-dist-win` archive (exact version + SHA256, verified before
unpack):

| Arm | Backed by | How it is wired |
|---|---|---|
| `d3d11`, `d3d12` | **WARP** | nothing provisioned; the D3D adapter resolver excludes software adapters and logs `no adapter survived the exclusions`, then the run continues on WARP |
| `opengl` | Mesa **llvmpipe** | `opengl32.dll` + `libgallium_wgl.dll` staged into the *application directory* (opengl32 is not a KnownDLL, so the app dir beats System32), `GALLIUM_DRIVER=llvmpipe` |
| `vulkan`, `vulkan2` | Mesa **lavapipe** | `lvp_icd.x86_64.json`, registered in `HKLM\SOFTWARE\Khronos\Vulkan\Drivers` **and** `VK_DRIVER_FILES` |

### The Vulkan arms are BLOCKED, and not by anything a quarantine can fix

lavapipe is provisioned, selected and confirmed live on the hosted lane:

```
[WARN ] [select_physical_device] Vulkan selected GPU 0: llvmpipe (LLVM 23.1.1, 256 bits) (VK_PHYSICAL_DEVICE_TYPE_CPU, driver 0x06801008)
```

so the ADR-037 worry does not apply — the Vulkan selector only *ranks* a CPU
device lowest (`vk_bundle_init.c` `device_type_priority`), it never rejects
one. `vkCreateDevice` is where it stops:

```
[ERROR] [build_device_extensions] VkPhysicalDevice does not support required extension VK_KHR_external_memory_win32
```

On Windows (`XRT_GRAPHICS_BUFFER_HANDLE_IS_WIN32_HANDLE` /
`XRT_GRAPHICS_SYNC_HANDLE_IS_WIN32_HANDLE`) `required_vk_device_extensions` in
`oxr_vulkan.c` demands `VK_KHR_external_memory_win32`,
`VK_KHR_external_semaphore_win32` **and** `VK_KHR_external_fence_win32`. No
Windows software ICD provides that set — checked against upstream source, not
guessed:

| ICD | `external_memory_win32` | `external_semaphore_win32` | `external_fence_win32` |
|---|---|---|---|
| Mesa **lavapipe** (`lvp_device.c`) | ✗ — `_fd` only | ✗ — `_fd` only | ✗ — `_fd` only |
| Mesa **dzn** / Dozen (`dzn_device.c`) | ✓ | ✓ | ✗ — no `external_fence` at all |
| **SwiftShader** (`libVulkan.cpp`) | ✗ — opaque-FD and Fuchsia only | ✗ | ✗ |

So switching ICD does not rescue this, and neither does naming tests in the
quarantine list: the failure is at device creation, so it takes out *every*
session-creating test rather than a nameable few. The `vulkan`/`vulkan2` arms
need either the real-GPU tier (#1526) or a deliberate decision about that
required set — note the asymmetry that the POSIX block marks the equivalent
`_fd` sync extensions **optional** while the Win32 block marks them required,
which looks inherited rather than reasoned. That is a runtime behaviour change
and wants its own issue and hardware validation, not a CI workaround.

What the hosted lane *does* now give for Vulkan: the CTS is built with the
`vulkan`/`vulkan2` plugins at all (see below), and the arm gets as far as
device creation with a named reason. Both are prerequisites for #1526.

Three more things that are easy to get wrong here:

- **The CTS must be *built* with Vulkan or there is no `vulkan`/`vulkan2` plugin
  to select.** `find_package(Vulkan)` gates `XR_USE_GRAPHICS_API_VULKAN`, and
  `fetch_build_cts.bat` used to clobber `VULKAN_SDK` unconditionally — the
  hosted lane therefore built a Vulkan-less CTS and said so only in one
  `-- Could NOT find Vulkan` line. It now honours a pre-set `VULKAN_SDK`, and
  `cts.yml` points it at the vcpkg loader+headers the runtime already links.
- **Enabling Vulkan in the CTS build puts a `vulkan-1.dll` import on the
  d3d11 and d3d12 arms too.** `conformance_cli.exe`, `conformance_test.dll`
  and `XrApiLayer_runtime_conformance.dll` all link the loader's import
  library once `find_package(Vulkan)` succeeds. On a machine with no Vulkan
  runtime installed the process then dies at *load* with `0xC0000135`
  STATUS_DLL_NOT_FOUND — exit code `-1073741515`, no XML, no console log, no
  message, and arms that never asked for Vulkan go down with it.
  `fetch_build_cts.bat` stages the loader it linked against next to the exe as
  part of the build, so it is cached with the build and no per-run cleanup
  removes it. `run_cts.ps1` decodes that exit code in the log.
- **CI steps run ELEVATED, and both loaders discard their path env vars
  there.** The Vulkan loader reads `VK_DRIVER_FILES`, `VK_ICD_FILENAMES`,
  `VK_LAYER_PATH` and `VK_ADD_LAYER_PATH` through a secure getenv and ignores
  them in a high-integrity process — the same reason this harness drives the
  Khronos OpenXR loader through `HKLM ActiveRuntime` instead of
  `XR_RUNTIME_JSON`. An env var that shows up correctly in the log and is
  being thrown away is the worst failure shape there is, so `run_cts.ps1`
  registers the ICD and the CTS's Vulkan layer in HKLM as well, restores both
  in its `finally`, and prints the elevation state on every run.
- **The CTS's OpenXR conformance layer has a Vulkan face.** Enabling
  `XR_APILAYER_KHRONOS_runtime_conformance` makes the CTS's Vulkan plugin
  hard-require `VK_LAYER_OPENXR_xr_runtime_conformance` (same DLL). It is a
  from-source build, so nothing puts its generated manifest on the loader's
  path; `run_cts.ps1` writes one with an absolute `library_path` — the
  generated manifest says `./<dll>`, and Ninja Multi-Config puts the DLL in a
  per-config subdirectory the JSON is not in.
- **Unstage before the cache is saved.** The `build-cts` cache is shared with
  the d3d11/d3d12 lanes. A Mesa `opengl32.dll` left beside `conformance_cli.exe`
  would be restored into an arm that never asked for software rendering, which
  is a silent result change, not a build failure.
- **Every result file must name its renderer.** `run_cts.ps1` writes
  `cts_<tag>_graphics_identity.txt` next to the XML, carrying the pinned Mesa
  version plus the runtime's own WARN lines (`GLAD loaded: … renderer: …` and
  `Vulkan selected GPU n: …`). A CTS XML records the *plugin*, never the
  implementation that answered it.

**Quarantine:** `scripts/cts_quarantine_software_tier.txt` is the single place
software-tier exclusions live, passed via `run_cts.ps1 -QuarantineList` and
**only** on this tier — a real-GPU tier gets an empty exclusion set by
construction. A test quarantined on every tier is hiding a defect.

Kill switch: workflow-level `DXR_CTS_SOFTWARE_ICD: '0'` in `cts.yml`.

## Reference

- **Known-red exclusions in the default spec: there are none.** The default spec
  is `exclude:[interactive]` — the whole non-interactive suite, nothing excluded
  by name. `xrLocateSpace_xrLocateViews` was excluded from #1491 until both of
  its assertions were fixed (`views.size() == 2` by #1486, `VIEW` == centroid of
  the located view origins by #1502); the exclusion was dropped in `c1e4fe00d`
  and the test runs in the default lane — see
  [View-Configuration Model](../reference/view-configuration-model.md). If a
  by-name exclusion is ever needed again it must carry a comment naming its
  issue, in **both** `cts.yml` and `run_cts.ps1`, and Catch2's comma rule
  applies (a comma starts a second, OR'd filter and would exclude nothing —
  append `~name` with no comma).
- **`conformance_cli.exe` DPI manifest (#1506):** the Khronos CTS binary is
  built with no DPI manifest, so on a scaled display it ran DPI-unaware and
  every geometric measurement (window size/position, Kooima projection, view
  poses) came back wrong by the scale factor — this inflated #1502 with a
  spurious axis. `fetch_build_cts.bat` now embeds + asserts the manifest
  post-build; see
  [DPI awareness: the DLL rule](../reference/dpi-awareness.md) § *The CTS
  runner is third-party*.
- Full plan: this repo's planning notes (the `unified-bubbling-perlis` plan).
- Extension spec: `XR_EXT_conformance_automation` (openxr.h, spec v3).
- Eye-tracking / sim_display fake-tracking knobs: CLAUDE.md "Simulating eye
  tracking without hardware".

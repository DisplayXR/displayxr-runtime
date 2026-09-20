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
| `opengl` | GitHub-hosted `windows-2022`, Mesa **llvmpipe** (provisioned — the image's own GL is GDI generic 1.1) | **yes** (#1523) | — · one software-tier quarantine entry, below |
| `vulkan` | GitHub-hosted `windows-2022`, Mesa **lavapipe** (provisioned — the image ships no Vulkan ICD) | **yes** (#1523) | — |
| `vulkan2` | same as `vulkan` | **yes** (#1523) | — |
| Linux `vulkan` / `vulkan2` | GitHub-hosted `ubuntu-latest`, Mesa **lavapipe** under **Xvfb** (#1527) | **yes** (#1527) | — · see § Linux arms |
| Android `vulkan` / `vulkan2` | **real device** — no runner yet | not in CI | #1523 part 2, #1212 |

**The whole hosted matrix gates — 5 Windows + 2 Linux.** Windows: `d3d11`/`d3d12`
on WARP, `opengl` on Mesa llvmpipe, `vulkan`/`vulkan2` on Mesa lavapipe. Linux:
`vulkan`/`vulkan2` on Mesa lavapipe under Xvfb. A red arm on either platform
fails the lane; no arm is exempt.

The exemption mechanism still exists and is deliberately kept: the
`EXPERIMENTAL=""` and `EXPERIMENTAL_LINUX=""` lines in `cts.yml`'s `plan` job
(one per platform — they are separate strings because the two legs are separate
jobs). Putting an arm's name back in the right one sets its `continue-on-error`
and tells the `summary` job to leave it out of the gate — nothing else in the
file changes. Prefer that to deleting an
arm if one ever regresses beyond a quick fix: a reported-but-ungated arm still
produces numbers every run, and a deleted arm produces silence. Record the
reason here when you do.

### Evidence — what earned each flip

Gating is earned by a **run**, never by "no known blocker". Each arm needed the
software ICD from #1525 plus real runtime fixes:

| Arm | Runtime fixes it needed | Flip evidence |
|---|---|---|
| `d3d11`, `d3d12` | — (WARP is in the image) | gated from the start |
| `opengl` | #1540 (qwerty use-after-free at instance teardown), #1549 (`comp_gl` NULL layer slots) | [run 35433846568][gl-run] — first full GL suite to completion on llvmpipe: 27515 assertions, **1 failure**, 0 errors, 42 skipped. That one failure is the quarantine entry below. |
| `vulkan`, `vulkan2` | #1539 (Win32 external memory/semaphore/fence trio made optional-if-present → past `vkCreateDevice`), #1550 (the #1542 `SessionState` SIGSEGV), #1560 (#1558 — map every swapchain usage bit through `vk_csci_get_image_usage_flags`) | [run 35458623141][all-run] below |

**The all-arm zero-red run.** `graphics=all scope=full software_gfx=on`,
[run 35458623141][all-run], on `f39d6bc63` (the #1560 branch tip; squash-merged
to `main` as `fc1cb421b`, same content):

| Arm | assertions | failures | errors | skipped | status |
|---|---:|---:|---:|---:|---|
| `d3d11` | 38140 | 0 | 0 | 42 | PASS |
| `d3d12` | 37942 | 0 | 0 | 42 | PASS |
| `opengl` | 27511 | 0 | 0 | 42 | PASS |
| `vulkan` | 40086 | 0 | 0 | 42 | PASS |
| `vulkan2` | 40070 | 0 | 0 | 42 | PASS |

Read the **assertion** column as the CTS reports it (`tests` in the JUnit XML).
Subtracting `skipped` gives a number 42 lower per arm — a real quantity, but not
the one the summary table prints, so quoting the two interchangeably will make
two correct reports look like they disagree.

**Caveat on that run, because it is easy to over-read.** `software_gfx=on` is an
explicit override that forces `software: true` for **every** arm, so `d3d11` and
`d3d12` were handed the software-tier quarantine list too and skipped
`Timed_Pipelined_Frame_Submission` along with the rest. Their zero-red above is
therefore *with* that test excluded. On `auto` — which is what the nightly, the
tag lane and the PR lane use — the WARP arms get `software: false`, no quarantine
list, and they do run it. The PR lane exercises exactly that and is green.

**Real-GPU cross-check (off-CI, hand-run).** Against `main` `27260eaee` on an
**NVIDIA RTX 3080**, full non-interactive suite: `vulkan` 40027 and `vulkan2`
40011 assertions, with only **two** reds, both the known layer-not-enabled case
rather than runtime defects. Recorded here as **reported, not independently
verified** — it was a manual run on hardware this lane cannot reach, there is no
artefact URL to cite, and the numbers differ from the hosted lane's because the
runs are not the same build. It is corroboration for #1526, not a substitute.

[gl-run]: https://github.com/DisplayXR/displayxr-runtime/actions/runs/35433846568
[all-run]: https://github.com/DisplayXR/displayxr-runtime/actions/runs/35458623141

**The `opengl` quarantine entry, and its one cost.** The lone failure in the GL
run above is `Timed_Pipelined_Frame_Submission`
(`REQUIRE( timingResults.GetOverheadFactor() < 0.5 )`, expansion
`1.08553478400000003 < 0.5` — an *Overhead score : 108.6%* against the CTS's hard
50% cap): a CPU rasterizer on a 2-vCPU runner missing a frame budget predicted as
if a GPU were scanning out. It is quarantined **by name, on the software tier
only**. The cost is that `Report frame-timing metrics` scrapes that same test to
trend runner frame timing (#589), so on the software tier it prints `No
Timed_Pipelined_Frame_Submission metrics in console log` instead of a percentage;
every tier that does not pass the quarantine list still gets the number.


Beyond software rasterizers, **#1526** tracks a self-hosted Windows runner on a
hybrid iGPU+dGPU box. A software-rasterized green is a real result but not a
submittable one — a submission owes a run on an implementation a user could
have. The real-GPU lane is also what retires the one quarantine entry (the
frame budget is only unmeetable on a CPU rasterizer) and the only way to cover
adapter selection (`DXR_D3D_FORCE_GPU` / `DXR_VK_FORCE_GPU`).

### Result artefacts

Each arm writes `%TEMP%\cts_ci_<graphics>.xml` (ctsxml) plus
`cts_ci_<graphics>_console.log`, staged and uploaded as
**`cts-results-<graphics>`**. The set is assembled to be droppable into a
Khronos submission package unmodified — one XML per graphics plugin, named after
it. The `summary` job downloads every arm, prints one table (arm / tests /
failures / errors / status) into the run summary, and **fails the lane if any
non-experimental arm was red or produced no XML** — which, with `EXPERIMENTAL`
empty, means every arm. A single arm no longer gates alone.

Triggers: PR → `smoke` / `d3d11` only (fast); nightly cron + `v*` tag → `full`
over all five Windows arms **plus both Linux arms**; `workflow_dispatch` → any
single plugin or `all`, at either scope.

## Linux arms — `vulkan` + `vulkan2` on the hosted lane (#1527)

Despite the file's name, the Linux arms live in the same `cts.yml`, so they are
documented here rather than in a second runbook. They are the **Linux mirror of
the Windows software tier**: GitHub-hosted `ubuntu-latest`, Mesa **lavapipe**
(from `mesa-vulkan-drivers`, not a download), an X11 session from **Xvfb**, the
vendor-neutral `sim-display` plug-in, and the same pinned CTS.

**Why hosted-and-software rather than a rented GPU.** #1527 originally argued
for a `g4dn`-class NVIDIA instance, on the reasoning that Linux is Vulkan-only
so the GPU-family problem is small and the validated configuration *is* NVIDIA.
That is still the right argument for **coverage**, and it is still owed. It is
the wrong argument for **first coverage**: today the number is zero, a hosted
lane costs nothing on a public repo, and a software tier is what caught three
real runtime defects on Windows (#1539, #1550, #1560) before any hardware was
involved. Hosted first, hardware after — the two are complements, and the
real-GPU tier is the one that retires the frame-timing quarantine and covers
adapter selection, exactly as on Windows.

### Shape

`plan` resolves `arms_linux` as the intersection of the requested arms with
`{vulkan, vulkan2}`, so the PR lane (`d3d11`) produces an **empty** Linux leg
and both Linux jobs skip for free. `build-linux-cts` builds the runtime
(`scripts/build_linux.sh`, `CMAKE_BUILD_TYPE=Release`, ending in the usual
headless `displayxr-cli selftest`) plus the CTS (`scripts/fetch_build_cts.sh`,
cached on that script's hash + the pin), then hands the build tree to the
`run-linux` arms as an artifact. Results upload as
**`cts-results-linux-<graphics>`**, and the files inside keep the Windows names
(`cts_ci_<graphics>.xml`, `_console.log`, `_stdout.log`,
`_graphics_identity.txt`, plus a Linux-only `_runtime.log` and `_xvfb.log`) so
they drop into a submission package unmodified. Because **both** platforms write
`cts_ci_vulkan.xml`, the `summary` job resolves each row by **artifact
directory**, not by filename — changing that back would report a Linux XML as a
Windows result.

### Both arms gate

`EXPERIMENTAL_LINUX=""` in `plan`: neither arm runs with `continue-on-error`,
and a red Linux arm fails the lane exactly like a red Windows one.

They did not start that way. Both were held experimental on two arguments, and
it is worth recording how each was discharged:

1. *Every Windows arm earned its gate with a whole-suite zero-red run, never
   with "no known blocker."* Discharged by running it — see the table below.
2. *Linux is Preview, not GA, so a red arm is information first.* Still true of
   the platform, but it stopped being a reason once the arms were green: an arm
   that passes and does not gate teaches the lane to ignore it, and the next
   regression then lands unnoticed in a column nobody reads. Preview is a reason
   to be careful about what a red arm **means**, not a reason to let it pass
   silently.

**To re-add an arm:** put its name back in `EXPERIMENTAL_LINUX`. Nothing else in
`cts.yml` changes. Record the reason here.

| Arm | Latest full-run evidence | Gated? |
|---|---|---|
| Linux `vulkan` | [run 35487653057][lx-vk] — 40062 assertions, **0 failures, 0 errors**, 39 skipped | **yes** |
| Linux `vulkan2` | [run 35488930910][lx-vk2] — 40046 assertions, **0 failures, 0 errors**, 39 skipped | **yes** |

[lx-vk]: https://github.com/DisplayXR/displayxr-runtime/actions/runs/35487653057
[lx-vk2]: https://github.com/DisplayXR/displayxr-runtime/actions/runs/35488930910

Those two are the post-#1577 runs that earned the flip, one arm each, taken on
the #1577 branch (`d7f3a994d` and `904edc131`) before it squash-merged to `main`
as `510f6d834`. Read the assertion counts the way the summary table prints them
(`tests` in the JUnit XML); subtracting `skipped` gives numbers 39 lower per arm
on Linux — the same trap as the Windows arms' 42, and the same advice: do not
quote the two forms interchangeably.

**The Linux arms are already covered by the software-tier quarantine.**
`run-linux` passes `--quarantine-list scripts/cts_quarantine_software_tier.txt`
whenever `matrix.software` is true — the *same* file as Windows, because the one
entry in it (`Timed_Pipelined_Frame_Submission`) is a property of running on a
CPU rasterizer and lavapipe is one. Both runs above log
`QUARANTINE: 1 test(s) excluded`, and the frame-timing scrape correspondingly
prints `No Timed_Pipelined_Frame_Submission metrics in console log`. So there is
no Linux-specific quarantine entry to add, and no second list: if that test ever
needs excluding somewhere, it is already excluded on every software tier, and
anywhere else would need its own evidence under the file's rule 2.

### The Linux finding that had to be fixed first: `xrGetVulkanDeviceExtensionsKHR` was unfiltered off Windows

**Fixed by #1577 (issue #1576).** Kept here because it is the clearest example
of why the enable1 and enable2 arms are not redundant.

Before the fix, all 62 `vulkan` errors were the same exception, thrown by the
**CTS's own** device creation (`graphics_plugin_vulkan.cpp:1383`,
`InitializeDevice`) before any runtime code ran:

```
VkResult failure ERROR_EXTENSION_NOT_PRESENT
```

`XR_KHR_vulkan_enable` (enable1) has the **app** create the `VkDevice`, enabling
verbatim whatever `xrGetVulkanDeviceExtensionsKHR` hands back. On desktop Linux
that string (`comp_vk_glue.c`, the `XRT_GRAPHICS_BUFFER_HANDLE_IS_FD` arm)
unconditionally names four extensions that a headless lavapipe does not expose —
`lvp_device.c` enables each only behind `HAVE_LIBDRM` *and* a real DRM device's
dmabuf / `native_fence_fd` caps, and a hosted runner has no `/dev/dri`:

* `VK_EXT_external_memory_dma_buf` and `VK_EXT_image_drm_format_modifier`
  (added for the PipeWire desktop-background capture, runtime#757)
* `VK_KHR_external_semaphore_fd` and `VK_KHR_external_fence_fd`

`vulkan2` is clean because **enable2 has the runtime create the device**, and
`oxr_vulkan.c` already treats the FD sync pair as optional-if-present and never
asks for the dma-buf pair at all. So the two lists disagree: what enable2 asks
for and what the enable1 string advertises are not the same set.

The fix shape already existed — #1539 built exactly this filter, dropping
not-present names from the `xrGetVulkanDeviceExtensionsKHR` answer — but it was
written inside `#ifdef OXR_HAVE_WIN32_EXTERNAL_LIST`
(`oxr_vulkan.c::oxr_vk_device_exts_for_system`), so on Linux the function
returned the compile-time constant unchanged. **#1577 generalised it** (issue
#1576): the desktop-Linux external-memory / modifier / fd extensions are now
optional-if-present for enable1, with the kill switch scoped so Linux restores
only the enable1 string. `vulkan` went 62 errors → **0**.

Identity recorded on both arms: `llvmpipe (LLVM 20.1.2, 256 bits)`,
`PHYSICAL_DEVICE_TYPE_CPU`, Mesa 25.2.8, ICD
`/usr/share/vulkan/icd.d/lvp_icd.json`.

Note what this is and is not. It is a real runtime/environment mismatch that a
real-GPU Linux box would not show (an NVIDIA or Mesa-on-DRM device exposes all
four), which is precisely the class of thing a software tier exists to find —
the same shape as #1539 on Windows. It is **not** a quarantine candidate: a
device-creation failure takes out every session-creating test rather than a
nameable few, which is rule 2 of
`scripts/cts_quarantine_software_tier.txt`.

### What is Linux-specific, and what deliberately is not

`scripts/run_cts.sh` is a port of `run_cts.ps1`'s essentials, not a
transliteration. Same CTS pin, same `-TestSpec` strings, same output stems, same
`DXR_PLUGIN_EXCLUSIVE` / `DXR_INPUT_PROVIDERS=0` exclusivity, same
quarantine-list semantics. What is **not** ported, and why:

* **No registry, no restore, no `finally`.** All of that exists on Windows
  because the Khronos and Vulkan loaders read their path env vars through a
  secure `getenv` and discard them in an elevated process, so `HKLM` is the only
  channel. Linux has no such downgrade: `XR_RUNTIME_JSON`, `XR_API_LAYER_PATH`,
  `VK_ADD_LAYER_PATH` and `VK_DRIVER_FILES` are honoured as-is, everything the
  script sets is process env, and there is no machine state that an aborted run
  could leave mis-pointed.
* **No DPI manifest.** #1506's embed exists because a DPI-unaware Win32 process
  reads virtualised window geometry and every geometric CTS measurement comes
  back wrong by the scale factor. X11 has no per-process geometry
  virtualisation; the compositor reads real root-window pixels through
  `xcb_get_geometry` / `xcb_translate_coordinates`.
* **No loader staging next to the exe.** #1525's `vulkan-1.dll` copy exists
  because a bare Windows runner has no Vulkan loader in `System32`. On Linux
  `libvulkan.so.1` comes from `libvulkan1`, a hard dependency of every ICD
  package.

Linux-specific, on the other hand:

* **`DISPLAY` is mandatory, a window manager is not.** The compositor presents
  to an X11/XCB surface (`comp_vk_native_window_xcb.c`) and the CTS passes no
  window binding, so the runtime self-creates the window. `xcb_map_window`
  works on a bare X server, and — the part worth stating, because the Windows
  lane has just learned that FOCUSED timeouts are a real failure mode (#1571) —
  **FOCUSED does not depend on X input focus here**:
  `oxr_session_gfx_vk_native.c` sets `compositor_focused = true`
  unconditionally for the in-process native path, so the
  SYNCHRONIZED → VISIBLE → FOCUSED ladder turns purely on frame submission. No
  `openbox`/`fluxbox` is needed, and adding one would only introduce a
  reparenting WM between the compositor and its own surface.
* **`DXR_WINDOW_FULLSCREEN=0`, not `XRT_COMPOSITOR_START_WINDOWED`.** The latter
  is read in exactly one place, `comp_d3d11_window.cpp`; it is a D3D11 knob and
  setting it on Linux does nothing. Without a WM the EWMH
  `_NET_WM_STATE_FULLSCREEN` property and the `_NET_WM_FULLSCREEN_MONITORS`
  client message have nobody listening, so the request is a silent no-op —
  turning it off keeps the window at the size the compositor asked for and the
  log honest. `Xvfb` is started with `+extension RANDR` so
  `xcb_randr_get_monitors` resolves and the "no RandR monitor" warning (which
  reads like a failure) never fires.
* **Runtime logs go to stderr.** `u_file_logging` is Windows-only
  (`%LOCALAPPDATA%\DisplayXR`), so the graphics-identity scrape reads the
  captured stderr (`Vulkan selected GPU …`, `XCB: created …`) instead of
  sweeping a log directory. The identity file also records the resolved ICD
  manifest and the head of `vulkaninfo --summary`, so a result file can never be
  ambiguous about whether lavapipe or real silicon produced it.
* **Double the Windows timeout.** A CPU rasterizer driving a real X11 present
  path is slower than a WARP blit, and a timeout leaves a truncated XML that
  still looks like a result file — the worst outcome available. Linux is the
  ×1-weighted runner; pay the minutes.

### Running the Linux arms by hand

Same script, on any Linux box:

```bash
./scripts/build_linux.sh                 # runtime + sim-display plug-in
./scripts/fetch_build_cts.sh --apt       # CTS at the pinned tag
# hardware-free, exactly what CI runs:
./scripts/run_cts.sh -g vulkan2 --scope smoke --conformance-layer --xvfb \
    --software --quarantine-list scripts/cts_quarantine_software_tier.txt
# on a real-GPU box, in a real X session — NO --software, so NO quarantine:
./scripts/run_cts.sh -g vulkan --scope full --conformance-layer
```

`run_cts.sh` refuses `--quarantine-list` unless `--software` is also set, so
"real GPU with the software exclusions applied" is not a state a typo can reach.

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

So switching ICD did not rescue this, and neither did naming tests in the
quarantine list: the failure was at device creation, so it took out *every*
session-creating test rather than a nameable few.

**RESOLVED by #1539.** The asymmetry called out here — the POSIX block marks the
equivalent `_fd` sync extensions **optional** while the Win32 block marked them
required — was indeed inherited rather than reasoned. The Win32 trio is now
**optional-if-present** in both app-facing lists, gated at every import/export
call site, with `DXR_VK_REQUIRE_WIN32_EXTERNAL=1` as the kill switch. Measured
on the hosted lane, same build, kill switch as the only variable:

| `DXR_VK_REQUIRE_WIN32_EXTERNAL` | assertions | failures | errors | where it stops |
|---|---:|---:|---:|---|
| `1` (old behaviour) | 1442 | 0 | 12 | `vkCreateDevice`, every session-creating test |
| unset (default) | 418 | 1 | 0 | `Swapchains` **PASSES**; SIGSEGV in `SessionState/Cycle through all states` |

So the next blocker on these arms was a **crash**, not an extension — the Vulkan
sibling of the `opengl` SIGSEGV (#1522), fixed as #1542 by #1550. One more
followed it, #1558 (swapchain usage bits not mapped through
`vk_csci_get_image_usage_flags`), fixed by #1560. With those three in,
`vulkan`/`vulkan2` came back **zero-red on the full suite** and now **gate** —
see the evidence table near the top of this document. The real-GPU tier (#1526)
is still what makes these arms count for a submission: lavapipe green is a real
result, not a submittable one.

Note the null compositor keeps the trio **required** on purpose
(`null_compositor.c`): it creates its own `VkDevice` and is the export side of
the handoff. On lavapipe its Vulkan init fails with the message above and it
falls back to D3D11 at session level — expected, non-fatal, and visible in
every `vulkan`/`vulkan2` log.

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
construction. A test quarantined on every tier is hiding a defect. It holds
**one** entry, `Timed_Pipelined_Frame_Submission` (108.6% frame-timing overhead
vs the CTS's 50% cap on llvmpipe — see *`opengl` graduated* above); the file's
own header carries the evidence and the re-check rule.

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

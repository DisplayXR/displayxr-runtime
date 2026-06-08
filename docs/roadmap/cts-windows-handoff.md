# CTS bring-up — Windows handoff runbook (#33)

Picks up where the macOS session left off. The platform-agnostic half of
"pass the non-interactive OpenXR CTS subset" is done and on this branch
(`feature/cts-conformance-automation`). The remaining work is **inherently
Windows**: build the Khronos conformance suite, run it against the runtime, and
iterate. First target: **Windows + D3D11**.

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
- Update #33 with the supported-category matrix; note form-factor categories
  out of scope by design (CTS interactive tests assume an HMD + controllers).

## Reference

- Full plan: this repo's planning notes (the `unified-bubbling-perlis` plan).
- Extension spec: `XR_EXT_conformance_automation` (openxr.h, spec v3).
- Eye-tracking / sim_display fake-tracking knobs: CLAUDE.md "Simulating eye
  tracking without hardware".

# Android Lume Pad Bring-Up Checklist

> **Plug-in split (post-#268 / ADR-019):** the runtime APK ships
> `libopenxr_displayxr.so` only; the CNSDK weave lives in the
> [`displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin)
> plug-in (`libdxrp050_leia_cnsdk.so`). Both `.so` files must land in the
> **same** runtime APK's `jniLibs/<ABI>/` for the runtime's plug-in loader
> to discover the CNSDK DP at `xrCreateInstance`. The plug-in build script
> (`build-android.sh install-runtime-jnilibs`) does that drop, including the
> 16 CNSDK + SNPE transitive `.so` deps — so **build the plug-in before the
> runtime APK**.

> **ABI major v2 (ADR-020):** the runtime rejects any plug-in not reporting
> plug-in API v2. The branches below are already on v2 (the plug-in pins
> runtime `v1.9.0` headers and sets the DP `struct_size`); don't mix a v1
> plug-in build with a v2 runtime or `xrCreateInstance` fails with
> `XR_ERROR_INITIALIZATION_FAILED` + an "ABI major mismatch" loader log.

## Branches (validated 2026-06-02 on the Android-36 emulator up to `xrCreateInstance -> XR_SUCCESS`)

| Component | Branch | Contents |
|---|---|---|
| **Runtime + test app** | `android/hw-bringup` | Integration of the preload-fixed-point (#343), emulator smoke-test + sentinel (#347), and CAMERA/WAKE_LOCK permissions + `MainActivity` wrapper (#350/#359) stacks, all on v1.9.x (ABI v2). **Superseded by `main` once those PRs merge** — then just use `main`. |
| **Plug-in** | `docs/cnsdk-c-abi-surface` (tip of the plug-in stack) | DP `struct_size` v2 fix (#11), symbol hygiene (#12), `build-android.sh` (#13), CNSDK in-app link + transitive `.so` bundling (#14), `debug.dxr.leia.*` calibration knobs (#17). |

**Skip the `feat/android-cnsdk-poc-day*` historical branches** — pre-audit, known frame-1 failures; keep only for `git bisect`.

Companion docs:
- [`android-build-guide.md`](android-build-guide.md) — prerequisites, CNSDK setup, build commands.
- [`android-bringup-logcat.md`](android-bringup-logcat.md) — pre-written `adb logcat` filters per test + expected line sequences + failure-shortcut table.
- [`android-emulator-setup.md`](android-emulator-setup.md) — half-bringup on the emulator before Lume Pad arrives (catches build/install/loader-discovery bugs; can't render past `xrCreateInstance`).
- [`android-vulkan-extension-survey.md`](android-vulkan-extension-survey.md) — the VK device extensions Lume Pad must provide that the emulator lacks (why `xrCreateVulkanDeviceKHR` fails on the emulator but should pass on Adreno).
- [`displayxr-leia-plugin/docs/cnsdk-android-calibration.md`](https://github.com/DisplayXR/displayxr-leia-plugin/blob/main/docs/cnsdk-android-calibration.md) — face axes / view mapping / UV flip calibration (Test B).

---

## Step 0 (one command): emulator/device smoke test — does the chain reach `xrCreateInstance`?

Before any manual install, run the end-to-end smoke test. It boots the emulator
(or uses an attached device/Lume Pad), builds the plug-in + both APKs, installs,
launches, and greps the sentinel. This is the fastest "did I break the
loader→runtime→plug-in chain" check and is the part that's CI/emulator-provable.

```bash
# from the runtime repo root, on the android/hw-bringup branch
# plug-in repo expected at ../displayxr-leia-plugin (override with PLUGIN_DIR)
# CNSDK extracted at ./cnsdk (override with CNSDK_ROOT)
PLUGIN_BRANCH=docs/cnsdk-c-abi-surface SKIP_EMULATOR=1 scripts/android-smoketest.sh
```

- `SKIP_EMULATOR=1` uses an already-attached device (the Lume Pad). Drop it to boot the default AVD.
- **PASS** = `ANDROID_POC_SENTINEL xrCreateInstance=XR_SUCCESS` (exit 0).
- The script fails fast with a specific exit code on a dead device (3), an instant crash (1), or a real bring-up error (1) — so a timeout (2) genuinely means "never reached the sentinel."

On the **emulator** this passes up to `xrCreateInstance`; `xrCreateVulkanDeviceKHR`
then fails on the emulator's missing VK extensions (expected — see the VK survey
doc). On **Lume Pad** the same run should continue past device creation into the
frame loop. Once Step 0 is green on hardware, do the manual render passes below.

---

## Permissions (one-time per install)

The test app declares `android.permission.CAMERA` (CNSDK front-camera face
tracking) and `android.permission.WAKE_LOCK`. The app's entry point is a thin
Kotlin `MainActivity` wrapping `NativeActivity`; it calls `requestPermissions(CAMERA)`
from `onCreate`, so on Lume Pad the **system dialog appears on first launch — tap
Allow**. Native init / `xrCreateInstance` run in parallel and don't need CAMERA,
so the sentinel appears regardless; CAMERA only gates face tracking at session start.

For non-interactive bring-up (scripted soak, no human at the device), pre-grant:

```bash
adb shell pm grant com.displayxr.cube_handle_vk_android android.permission.CAMERA
```

**Missing-CAMERA symptom on Lume Pad:** session reaches FOCUSED and renders, but
`xrLocateViews` always returns the default centered eye position regardless of head
motion — no crash, no CNSDK error. Confirm the grant with
`adb shell dumpsys package com.displayxr.cube_handle_vk_android | grep CAMERA`.

> Known gap (documented, not yet fixed): granting CAMERA *mid-session* doesn't
> re-trigger the CNSDK camera open (`MainActivity` has no `onRequestPermissionsResult`
> hand-back). If you tap Deny then change your mind, force-stop and relaunch.

---

## Build (manual install path)

```bash
# 1. Plug-in first — builds libdxrp050_leia_cnsdk.so and drops it + the 16
#    CNSDK/SNPE transitive .so into the runtime APK's jniLibs/arm64-v8a/.
cd ../displayxr-leia-plugin
git checkout docs/cnsdk-c-abi-surface
CNSDK_ROOT=/path/to/cnsdk DXR_RUNTIME_SOURCE_DIR=../openxr-3d-display \
    ./scripts/build-android.sh install-runtime-jnilibs

# 2. Runtime APK (now packages the plug-in .so dropped in step 1).
cd ../openxr-3d-display
git checkout android/hw-bringup        # or `main` once the PRs are merged
./gradlew :src:xrt:targets:openxr_android:assembleInProcessDebug

# 3. Test app APK.
./gradlew :test_apps:cube_handle_vk_android:assembleDebug
```

### Install

```bash
# Clean slate
adb uninstall org.freedesktop.monado.openxr_runtime.in_process 2>/dev/null
adb uninstall com.displayxr.cube_handle_vk_android 2>/dev/null

adb install -r src/xrt/targets/openxr_android/build/outputs/apk/inProcess/debug/openxr_android-inProcess-debug.apk
adb install -r test_apps/cube_handle_vk_android/build/outputs/apk/debug/cube_handle_vk_android-debug.apk
```

### Launch + watch

```bash
adb shell setprop debug.dxr.hw.verbose 1
adb logcat -c
# Launch via the LAUNCHER intent, not a hard-coded class — the entry activity
# is the Kotlin .MainActivity wrapper, and the class name has moved before.
adb shell monkey -p com.displayxr.cube_handle_vk_android -c android.intent.category.LAUNCHER 1
adb logcat -s cube_handle_vk_android:V DisplayXR:V leia:V monado:V
```

---

## Step A: First light (atlas mode)

**Goal:** the entire OpenXR loader → DisplayXR runtime → CNSDK DP → Lume Pad
display pipeline runs end-to-end.

**Debug log tags:** `HW_DBG_APP` (test app), `HW_DBG_CNSDK` (runtime CNSDK wrapper),
`HW_DBG_DP` (display processor), each with `[once]` variants. Compiled in only on
the Debug variant. Filter: `adb logcat | grep -E "HW_DBG_(APP|CNSDK|DP)"`.

### Expected log markers (in order)

```
android_main entered
xrInitializeLoaderKHR -> XR_SUCCESS
xrCreateInstance -> XR_SUCCESS
ANDROID_POC_SENTINEL xrCreateInstance=XR_SUCCESS
xrGetSystem(HEAD_MOUNTED_DISPLAY) -> XR_SUCCESS   (or HANDHELD_DISPLAY fallback)
xrGetVulkanGraphicsRequirements2KHR -> XR_SUCCESS
xrCreateVulkanInstanceKHR -> XR_SUCCESS
xrGetVulkanGraphicsDevice2KHR -> XR_SUCCESS
xrCreateVulkanDeviceKHR -> XR_SUCCESS              (emulator stops here: VK_ERROR_EXTENSION_NOT_PRESENT)
xrCreateSession -> XR_SUCCESS
Leia CNSDK DP created (atlas mode)
CNSDK face tracking started (worker)
session state -> 2 (READY)
xrBeginSession -> XR_SUCCESS
frame 60
frame 120
...
```

**Display:** lightfield mode active. Cover one eye → solid red; cover the other →
solid blue (validates left/right tile mapping — see Test B).

### Failure-mode table

| Symptom | Likely cause | Diagnostic / fix |
|---|---|---|
| `xrInitializeLoaderKHR` fails | Loader missing / wrong .aar | `unzip -l <test-apk> \| grep loader` — confirm `lib/arm64-v8a/libopenxr_loader.so` |
| `xrCreateInstance` fails (RUNTIME_UNAVAILABLE) | Loader can't resolve the runtime broker | `adb shell dumpsys package org.freedesktop.monado.openxr_runtime.in_process \| grep -i openxr` — confirm the `RuntimeService` + broker provider |
| `xrCreateInstance` fails (INITIALIZATION_FAILED) + "ABI major mismatch" | v1 plug-in against v2 runtime | rebuild the plug-in on `docs/cnsdk-c-abi-surface` (v2); don't reuse a stale `.so` |
| `xrCreateInstance` fails (INITIALIZATION_FAILED) + "library not found" | preload didn't resolve a transitive `.so` | confirm step 1 dropped all 16 `.so` into `jniLibs/arm64-v8a/`; check the preload log lines |
| `Leia CNSDK DP created` not logged, runtime falls back | plug-in `.so` absent or `probe` failed | `adb logcat \| grep -iE "leia\|cnsdk\|plugin loader"` |
| Black display, all logs OK | CNSDK didn't initialize | `adb logcat \| grep -iE "leia\|cnsdk\|interlacer"` — look for `leia_core_init_async failed` |
| Renders, but no head tracking | CAMERA not granted | see Permissions above; `dumpsys package ... \| grep CAMERA` |

**When A passes → do B. Don't skip B — it confirms the visual output is correct.**

---

## Step B: Calibration walkthrough

**Goal:** validate the three CNSDK convention assumptions (face axes, tile-to-eye
mapping, UV vertical flip) on real hardware. Each takes 1–2 minutes.

**Fast iteration:** the plug-in exposes `debug.dxr.leia.*` setprops (PR #17) so each
flip is a `setprop` + force-stop + relaunch (~5 s) instead of a rebuild (~3 min):

```bash
adb shell setprop debug.dxr.leia.flip_uv 1      # UV vertical flip
adb shell setprop debug.dxr.leia.face_flip_x 1  # face X axis
# face_flip_y / face_flip_z / face_swap_xy likewise
adb shell am force-stop com.displayxr.cube_handle_vk_android
# relaunch via the monkey LAUNCHER command above
```

Full symptom→fix table:
[`displayxr-leia-plugin/docs/cnsdk-android-calibration.md`](https://github.com/DisplayXR/displayxr-leia-plugin/blob/main/docs/cnsdk-android-calibration.md).

1. **Face axes.** Head still, then move right ~10 cm; watch the eye-position values
   in logcat. X should increase. If it decreases → `setprop debug.dxr.leia.face_flip_x 1`.
   Repeat for up (Y) and toward (Z).
2. **Tile-to-eye mapping.** Cover left eye → solid blue (right tile); cover right →
   solid red (left tile). If reversed, that's a runtime-side tile order issue (atlas
   mode has no CNSDK-side swap) — note it; needs a runtime change, not a setprop.
3. **UV vertical flip.** Asymmetric content should be right-side-up. Upside-down →
   `setprop debug.dxr.leia.flip_uv 1`.

Persist any winning setprop as the plug-in default once confirmed.

---

## Step C (optional): third-party OpenXR app

After A/B work, run an app we didn't write to catch bugs our test app happens to
avoid. Build Khronos [`hello_xr`](https://github.com/KhronosGroup/OpenXR-SDK-Source/tree/main/src/tests/hello_xr)
for Android + Vulkan, install it alongside the runtime APK (keep the runtime,
swap the test app), launch, and expect the same marker sequence + lightfield output.

---

## What we deliberately skip

- **`feat/android-cnsdk-poc-day*` historical branches.** Pre-audit; known frame-1
  failures. `git bisect` only — never install on hardware.
- **Per-tile-blit fallback.** The Android DP is **atlas mode only** — CNSDK splits
  the SBS atlas L/R internally, no per-view image management. The old
  `fix/compositor-b7` per-tile-blit path (#270) was superseded and is gone.
- **`outOfProcess*` build variants.** POC is in-process only; multi-app shell + IPC
  is a separate milestone.
- **Mono (2D) mode** beyond the 1×1 passthrough fallback.

## Bottom line

**Step 0 (smoke test) + 2 manual installs.** Step 0 proves the chain to
`xrCreateInstance` (the emulator-provable part). On hardware, A is mandatory, B is
mandatory if A passes, C is optional independent validation. If A→C pass on a Lume
Pad, the POC is "proven on hardware": atlas mode works, axes/views/UV are
calibrated, and the runtime works with third-party apps.

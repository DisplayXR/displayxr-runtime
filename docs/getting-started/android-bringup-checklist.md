# Android Lume Pad Bring-Up Checklist

> **Plug-in split (post-#268):** runtime branches below produce
> `libopenxr_displayxr.so` only; the CNSDK weave lives in the
> [`displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin)
> plug-in (`libdxrp050_leia_cnsdk.so`). Install both into the same APK's
> `jniLibs/<ABI>/` for the runtime's plug-in loader to discover the
> CNSDK DP at `xrCreateInstance`. The test app (`feat/cube-test-app-*`)
> doesn't need the plug-in present to compile — it links against the
> OpenXR loader only — but won't produce woven output without it.

A→B→C→D test plan for the first hardware install of the Android POC. Each step adds exactly one variable. **Skip the day-1 → day-8b historical branches** — those predate the [2026-05-22 audit](../../README.md) and are known to hit Vulkan validation errors on the first frame; keep them only for `git bisect` if a regression appears later.

Companion docs:
- [`android-build-guide.md`](android-build-guide.md) — prerequisites, CNSDK setup, build commands.
- [`android-bringup-logcat.md`](android-bringup-logcat.md) — pre-written `adb logcat` filters per test + expected line sequences + failure-shortcut table.
- [`android-emulator-setup.md`](android-emulator-setup.md) — half-bringup on the Android emulator before Lume Pad arrives (catches build/install/loader-discovery bugs; can't render).
- [`displayxr-leia-plugin/docs/cnsdk-android-calibration.md`](https://github.com/DisplayXR/displayxr-leia-plugin/blob/main/docs/cnsdk-android-calibration.md) — face axes / view mapping / UV flip calibration (Test B).

## What to test

| | Name | Runtime branch | Test app branch | Time |
|---|---|---|---|---|
| **A** | First light — atlas mode (primary) | `feat/android-hw-debug-logs` (atlas mode + audit fixes + HW debug logs) | `feat/cube-test-app-hw-debug-logs` (B13d + HW debug logs) | 5–10 min |
| **B** | Calibration walkthrough | unchanged | unchanged | 3 × 1–2 min |
| **C** *(optional)* | Per-tile-blit fallback | `fix/compositor-b7-hw-debug-logs` (per-tile-blit + HW debug logs) | unchanged | 5 min |
| **D** *(optional)* | Third-party OpenXR app | A or C | Khronos `hello_xr` or similar | 10 min |

**Debug log tags in logcat:** `HW_DBG_APP` (test app), `HW_DBG_CNSDK` (runtime CNSDK wrapper), `HW_DBG_DP` (runtime display processor). Each has `[once]` variants for state transitions. All three are compiled in only when the Debug build variant is installed (release builds compile them to nothing). To filter:
```bash
adb logcat | grep -E "HW_DBG_(APP|CNSDK|DP)"
```

If A passes you can stop. B confirms the CNSDK convention assumptions. C is only needed if A fails (atlas mode regression). D is final independent validation.

---

## Step A: First light (atlas mode)

**Goal:** Verify the entire OpenXR loader → DisplayXR runtime → CNSDK DP → Lume Pad display pipeline runs end-to-end.

### Build

```bash
# Runtime APK — feat/android-hw-debug-logs is the tip of the runtime
# stack: atlas mode + all audit fixes + CMake TARGET guards + AAR
# fallback + build guide + this checklist + verbose HW debug logging.
git checkout feat/android-hw-debug-logs
./gradlew :src:xrt:targets:openxr_android:assembleInProcessDebug

# Test app APK
git checkout feat/cube-test-app-hw-debug-logs
./gradlew :test_apps:cube_handle_vk_android:assembleDebug
```

### Install

```bash
# Clean slate
adb uninstall org.freedesktop.monado.openxr_runtime.in_process 2>/dev/null
adb uninstall com.displayxr.cube_handle_vk_android 2>/dev/null

# Install both
adb install -r src/xrt/targets/openxr_android/build/outputs/apk/inProcess/debug/openxr_android-inProcess-debug.apk
adb install -r test_apps/cube_handle_vk_android/build/outputs/apk/debug/cube_handle_vk_android-debug.apk
```

### Launch + watch

```bash
adb shell am start -n com.displayxr.cube_handle_vk_android/android.app.NativeActivity
adb logcat -s cube_handle_vk_android:V DisplayXR:V leia:V monado:V
```

### Expected log markers (in order)

```
android_main entered
xrInitializeLoaderKHR -> XR_SUCCESS
xrCreateInstance -> XR_SUCCESS
Runtime: "DisplayXR" v?.?.?
xrGetSystem(HEAD_MOUNTED_DISPLAY) -> XR_SUCCESS    (or HANDHELD_DISPLAY fallback)
System: "<vendor name>" vendor=0x???? maxSwapchain=????x???? maxLayers=?
xrGetVulkanGraphicsRequirements2KHR -> XR_SUCCESS
Vulkan API: min=?.?.? max=?.?.?
xrCreateVulkanInstanceKHR -> XR_SUCCESS
xrGetVulkanGraphicsDevice2KHR -> XR_SUCCESS
xrCreateVulkanDeviceKHR -> XR_SUCCESS
Vulkan device ready: queue_family=? queue=0x????
xrCreateSession -> XR_SUCCESS
Bring-up chain complete; awaiting session state events.
Chose swapchain format: 0x?
View 0 swapchain: ?x?, ? images
View 1 swapchain: ?x?, ? images
session state -> 1 (IDLE)
session state -> 2 (READY)
xrBeginSession -> XR_SUCCESS
session state -> 3 (SYNCHRONIZED)
Leia CNSDK DP created (atlas mode)
CNSDK face tracking started (worker)
frame 60
frame 120
...
```

**Display:** lightfield mode active. Cover one eye: should see solid red. Cover the other: should see solid blue. (Validates left/right tile mapping — see Test B.)

### Failure-mode table

| Symptom | Likely cause | Diagnostic / fix |
|---|---|---|
| `xrInitializeLoaderKHR` fails | Loader missing / wrong .aar | Check `lib/arm64-v8a/libopenxr_loader.so` exists in the test APK: `unzip -l <apk> \| grep loader` |
| `xrCreateInstance` fails (RUNTIME_UNAVAILABLE) | Loader can't find runtime APK | `adb shell dumpsys package org.freedesktop.monado.openxr_runtime.in_process \| grep OpenXR` should show `OpenXRRuntimeService` |
| `xrCreateInstance` fails (INSTANCE_LOST) | Runtime native lib crashed on init | Look for native crash signature: `adb logcat \| grep -i "tombstone\|fatal\|abort"` |
| `xrGetSystem(HMD) -> FORM_FACTOR_UNSUPPORTED`, `HANDHELD` also fails | Runtime advertises no system | `adb logcat \| grep -i "leia\|cnsdk\|sim_display\|drv_leia"` — check DP factory ran |
| `xrCreateSession` fails | Vulkan device/queue mismatch | Confirm `xrGetVulkanGraphicsDevice2KHR` chose a real device; check the value isn't `VK_NULL_HANDLE` |
| `Bring-up chain complete` printed but no `frame N` lines | Session stuck before SYNCHRONIZED | Session state events not arriving — check `xrPollEvent` errors in logcat |
| Black display, all logs OK | CNSDK didn't initialize | `adb logcat \| grep -iE "leia\|cnsdk\|interlacer"` — look for "leia_core_init_async failed" or interlacer init errors |
| Validation errors logged | Audit regression OR atlas-mode issue | If on atlas branch and persistent: switch to Test C (per-tile-blit fallback) to isolate |
| `Leia CNSDK DP created` shows "(per-tile blit + CNSDK weave)" instead of "(atlas mode)" | Wrong branch installed | Double-check `git log --oneline -1` shows `985b5fd4a` or descendant |

**When A passes → proceed to B. Don't skip B — it tells you whether the visual output is right before you build apps against it.**

---

## Step B: Calibration walkthrough

**Goal:** Validate the three CNSDK convention assumptions (face axes, tile-to-eye mapping, UV vertical flip) on real hardware. Each takes 1–2 minutes.

**Reference:** [`displayxr-leia-plugin/docs/cnsdk-android-calibration.md`](https://github.com/DisplayXR/displayxr-leia-plugin/blob/main/docs/cnsdk-android-calibration.md) has the full symptom→fix table.

1. **Face axes (audit B15).** Stand in front of the display, head still. Move head right ~10 cm. Watch logcat for eye-position values. X should increase. If decreases → flip sign of `out_x` in `leia_cnsdk_get_primary_face`. Repeat for up (Y) and toward (Z).
2. **Tile-to-eye mapping (audit B17).** Cover left eye — should see solid blue (right tile). Cover right eye — should see solid red (left tile). If reversed → swap args to `set_view_for_texture_array` (or per-tile blit order on the per-tile-blit branch).
3. **UV vertical flip (audit B18).** Add a text overlay or asymmetric content to the clear color (TODO in B13d follow-up). Right-side-up = correct. Upside-down → toggle `leia_interlacer_set_flip_input_uv_vertical` to `false`.

Each fix is a 1-line change in `leia_cnsdk.cpp`. Land as `fix/cnsdk-cal-<axis|view|uv>` on top of the bringup branch as needed.

---

## Step C *(optional)*: Per-tile-blit fallback

**When to run:** Only if A failed in a way that points at atlas mode (e.g., display works but content is wrong, or validation errors that disappear under per-tile blit).

**Branches:** runtime `fix/compositor-b7-hw-debug-logs` (per-tile-blit + audit fixes + HW debug logs) · test app unchanged.

```bash
git checkout fix/compositor-b7-hw-debug-logs
./gradlew :src:xrt:targets:openxr_android:assembleInProcessDebug

adb uninstall org.freedesktop.monado.openxr_runtime.in_process
adb install -r src/xrt/targets/openxr_android/build/outputs/apk/inProcess/debug/openxr_android-inProcess-debug.apk
# Test app unchanged — no reinstall
adb shell am start -n com.displayxr.cube_handle_vk_android/android.app.NativeActivity
```

`Leia CNSDK DP created` should now log "(self-submitting, per-tile blit + CNSDK weave)". Visually should match A. If C works but A doesn't, atlas mode has a real bug — bisect between `fix/compositor-b7` and `feat/cnsdk-atlas-mode`.

---

## Step D *(optional)*: Third-party OpenXR app

**When to run:** After A (or C) works, to confirm the runtime works with apps we didn't write — catches the case where our test app accidentally avoids a runtime bug.

**Candidates:** Khronos [`hello_xr`](https://github.com/KhronosGroup/OpenXR-SDK-Source/tree/main/src/tests/hello_xr) built for Android with Vulkan; any other OpenXR Android demo.

Install the third-party APK alongside the runtime APK (keep the runtime, replace the test app). Launch it; same logcat markers + lightfield output expected.

---

## What we deliberately skip

- **Day-1 through day-8b historical branches.** Pre-audit; known frame-1 failure modes (atlas layout, semaphore double-signal, face position mm-vs-m, destroy UAF). Useful only for `git bisect` if a regression appears in the fix stack. Don't install on hardware.
- **`outOfProcess*` build variants.** POC is in-process only. Multi-app shell + IPC is a separate milestone.
- **Mono (2D) mode.** Compositor passes `tile_columns==1` in 2D mode; the DP currently skips it silently (audit B8). Not POC scope.

## Bottom line

**2–4 installs total.** A is mandatory; B is mandatory if A passed; C and D are conditional. The 12+ historical day-N branches never get installed on hardware.

If you complete A through D successfully on a Lume Pad 2, the POC is "proven on hardware" — atlas mode works, axes/views/UV are calibrated, runtime works with third-party apps, and all the audit fixes have been validated end-to-end.

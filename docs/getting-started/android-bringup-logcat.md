# Android Bring-Up — logcat reference

Pre-written `adb logcat` filter commands for each test in the
[bring-up checklist](android-bringup-checklist.md). Each filter is
narrow enough to spot the load-bearing lines + wide enough to catch
adjacent failure modes.

Run on a separate terminal from the `adb install` / `am start` flow so
you don't lose lines while triaging.

---

## Setup

Enable verbose logging first (debug-build builds drop these to nothing
in release):

```bash
adb shell setprop debug.dxr.hw.verbose 1
adb shell setprop debug.atrace.tags.enableflags 1     # only needed for Test D (ATrace)
adb logcat -c                                          # clear buffer for clean output
```

---

## Test A — First light (atlas mode)

**Watch for, in order:** OpenXR loader binds to runtime → plug-in
loader finds CNSDK → session creates → swapchain creates → per-frame
weave.

```bash
adb logcat -v threadtime -s \
    cube_handle_vk_android:V \
    DisplayXR:V \
    DXR_HW_DBG:V \
    HW_DBG_APP:V \
    HW_DBG_CNSDK:V \
    HW_DBG_DP:V \
    target_plugin_loader:V \
    leia_cnsdk:V \
    leia_dp_cnsdk:V \
    AndroidRuntime:E \
    DEBUG:E \
    libc:E
```

**Expected sequence:**

| Line pattern | Meaning |
|---|---|
| `xrInitializeLoaderKHR -> XR_SUCCESS` | Khronos loader bound to JavaVM + Activity |
| `xrCreateInstance -> XR_SUCCESS, Runtime: "DisplayXR" v?` | Runtime DLL loaded (`openxr_displayxr.so`) |
| `target_plugin_loader: scanning <libdir>` | Plug-in discovery firing |
| `target_plugin_loader: loaded libdxrp050_leia_cnsdk.so, negotiated leia-cnsdk v1` | Plug-in `.so` resolved + `xrtPluginNegotiate` called |
| `leia_cnsdk_create: core init queued on worker` | CNSDK worker thread spawned |
| `leia_dp_cnsdk: is_self_submitting=true` | DP advertising the self-submit flag |
| `xrCreateSession -> XR_SUCCESS` | OpenXR session live, VK native compositor up |
| `xrEnumerateSwapchainFormats -> XR_SUCCESS, picking VK_FORMAT_..._UNORM` | Swapchain format negotiated (prefer UNORM) |
| Per frame: `leia_interlacer_vulkan_do_post_process` | CNSDK weave firing (no DXR-prefixed log — comes from CNSDK SDK itself) |

**Failure shortcuts:** if you see any of these, jump straight to triage:

| Line | Likely cause |
|---|---|
| `AndroidRuntime: FATAL EXCEPTION` | App/runtime crashed — backtrace below |
| `DEBUG: signal 11 (SIGSEGV)` | Native crash, see address + `pid` in next lines |
| `libc: Fatal signal` | Same |
| `target_plugin_loader: no plug-in DLLs found in <libdir>` | `.so` not in APK's `jniLibs/<ABI>/` — rebuild the runtime APK with the plug-in dropped in |
| `target_plugin_loader: xrtPluginNegotiate returned XRT_ERROR_PROBER_NOT_SUPPORTED` | Plug-in declined — check CNSDK init log |
| `leia_cnsdk_create: ... CNSDK init failed` | CNSDK SDK failed on device — likely no Leia hardware (you're on an emulator?) |
| `xrCreateSession -> XR_ERROR_GRAPHICS_DEVICE_LOST` | Vulkan device creation failed — check VK validation lines above |

---

## Test B — Calibration (3 sub-tests)

Each sub-test wants one numeric value off the screen. Filter tight:

### B1 — Face axes (audit B15)

Move head right, watch the X eye position. Should INCREASE.

```bash
adb logcat -v threadtime -s HW_DBG_CNSDK:V leia_cnsdk:V | grep -i "face\|eye"
```

Look for: `face_pos = {x=..., y=..., z=...}` and `eye_pos[0] = {x=..., y=..., z=...}` lines. If X DECREASES when head moves right → flip sign of `out_x` in [`leia_cnsdk_get_primary_face`](https://github.com/DisplayXR/displayxr-leia-plugin/blob/main/src/drv_leia_android/leia_cnsdk.cpp).

### B2 — Tile-to-eye mapping (audit B17)

Cover left eye, the LEFT half-image should disappear (right-eye-only view). If RIGHT half disappears → swap `set_view_for_texture_array` indices in `leia_cnsdk_weave`.

```bash
adb logcat -v threadtime -s HW_DBG_CNSDK:V | grep -i "set_view_for_texture_array\|view ="
```

### B3 — UV vertical flip (audit B18)

Run cube renderer. If cube renders UPSIDE-DOWN → toggle `set_flip_input_uv_vertical(interlacer, true)` to `false` in `leia_cnsdk_weave`.

```bash
adb logcat -v threadtime -s HW_DBG_CNSDK:V | grep -i "flip_input_uv_vertical"
```

Calibration symptom→fix table: [`displayxr-leia-plugin/docs/cnsdk-android-calibration.md`](https://github.com/DisplayXR/displayxr-leia-plugin/blob/main/docs/cnsdk-android-calibration.md).

---

## Test C — Per-tile-blit fallback (only if A fails)

Same filter as Test A but with the per-tile-blit runtime (PR #270's branch
content, which is on the safety branch `feat/android-runtime-poc-rebased`).
Look for `process_atlas_weave: per-tile blit path` instead of
`leia_interlacer_set_interlace_view_texture_atlas`.

```bash
adb logcat -v threadtime -s HW_DBG_DP:V leia_dp_cnsdk:V | grep -E "atlas|per-tile|blit"
```

---

## Test D — Perfetto trace (ATrace)

Doesn't go through logcat — uses Perfetto. Capture command:

```bash
# 5-second trace covering gfx + app categories
adb shell perfetto -o /data/local/tmp/trace.perfetto-trace -t 5s gfx app

# Pull + open in https://ui.perfetto.dev
adb pull /data/local/tmp/trace.perfetto-trace
```

Expected block hierarchy:

```
dxr_app:render_frame
  dxr_app:xrWaitFrame
  dxr_app:record_draw  (×2 views)
  dxr_app:xrEndFrame
    [runtime + plug-in, in xrEndFrame's call stack:]
    dxr_dp:process_atlas_weave
      dxr_cnsdk:weave           (atlas mode, normal path)
      dxr_dp:mono_passthrough_blit  (only on 1×1 mono mode)
```

If `dxr_cnsdk:weave` dominates (>80% of frame time): CNSDK's
`leia_interlacer_vulkan_do_post_process` is the bottleneck. Combined-
submit perf optimization (Tier 3 #10 in the no-Lume-Pad plan) is the
fix.

---

## Saving full logs

For each test, capture a full log for post-test analysis:

```bash
# Start log capture, leave running
adb logcat -v threadtime > test_A.log &
LOGCAT_PID=$!

# Run test (install + start app, observe, kill app)
adb shell am start -n org.example.cube_handle_vk_android/android.app.NativeActivity
# ... observe display ...
adb shell am force-stop org.example.cube_handle_vk_android

# Stop log capture
kill $LOGCAT_PID

# Compress for sharing
gzip test_A.log
```

Attach `test_A.log.gz` + a screenshot of the device (photo if the Leia
weave is too lenticular to screenshot meaningfully) when reporting
bring-up results.

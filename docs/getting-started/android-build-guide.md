# Android Build & Test Guide

Build and deploy DisplayXR on an Android device with a Leia 3D display (Lume Pad-class hardware: Lume Pad 2, Nubia Pad 2).

> **Plug-in split (post-#268):** the CNSDK display-processor plug-in
> now lives in [`displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin)
> and builds to `libdxrp050_leia_cnsdk.so`, which is dropped into the
> runtime APK's `jniLibs/<ABI>/` for the runtime's plug-in loader to
> discover at `xrCreateInstance`. The CNSDK SDK setup + AAR Gradle
> wiring described below applies to the plug-in build, not the runtime
> APK build. The runtime APK builds without CNSDK in scope.

Companion docs:
- [`android-bringup-checklist.md`](android-bringup-checklist.md) — A→B→C→D step-by-step test procedure once both APKs are built.
- [`docs/vendors/leia/cnsdk-android-calibration.md`](../vendors/leia/cnsdk-android-calibration.md) — symptom→fix table for the three CNSDK convention assumptions (face axes, view mapping, UV flip).

## Prerequisites

### Host machine (Windows or macOS)

| Tool | Version | Notes |
|------|---------|-------|
| Android Studio | 2024.1+ | Optional but recommended for first-time setup |
| Android SDK | API 35 | `sharedCompileSdk` in `build.gradle` |
| Android NDK | **26.3.11579264** | Pinned via `ndk_version` in root `build.gradle`. Newer NDKs may work but aren't tested. |
| CMake (Android) | 3.22.1 | Ships with Android SDK; pinned via `cmake_version` |
| Java JDK | 17 | `winget install Microsoft.OpenJDK.17` (Windows) / `brew install openjdk@17` (macOS) |
| Python | 3.6+ | For build scripts |
| ADB | latest | From Android SDK platform-tools |

### On the device

- **Lume Pad 2 or Nubia Pad 2** (Lume Pad-class — has a Leia 3D display + lightfield hardware)
- **Developer options enabled** (Settings → About → tap Build Number 7 times)
- **USB debugging enabled** (Settings → Developer options)
- **Leia Display Service** + **Leia Face Tracking Service** pre-installed (factory image)

Verify:
```bash
adb devices
# Should show your device as "device" (not "unauthorized")
```

## Step 1: CNSDK setup (plug-in repo only)

> This step happens in your [`displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin)
> checkout, **not** the runtime repo — the runtime APK builds without
> CNSDK in scope (see the plug-in-split note at the top). If you only
> need the runtime APK, skip to Step 2; CNSDK is pulled in when you
> build the plug-in's `libdxrp050_leia_cnsdk.so`.

The plug-in's Gradle build expects CNSDK as an extracted release tree at the plug-in repo root in `cnsdk/`. We currently pin **CNSDK 0.7.28**.

### Fetch CNSDK 0.7.28

CNSDK ships as a GitHub LFS-backed zip at
`https://github.com/LeiaInc/leiainc.github.io/tree/master/CNSDK/cnsdk-android-0.7.28.zip`.

Direct raw URLs 404 (LFS); fetch via the GitHub contents API:
```bash
gh api repos/LeiaInc/leiainc.github.io/contents/CNSDK/cnsdk-android-0.7.28.zip \
    --jq .download_url | xargs curl -L -o cnsdk-android-0.7.28.zip
unzip cnsdk-android-0.7.28.zip -d cnsdk
```

Result: `cnsdk/` contains
```
cnsdk/
  VERSION.txt                                       # "0.7.28"
  android/
    sdk-faceTrackingInApp-0.7.28.aar                # JNI lib bundled here
  include/leia/{common,device,headTracking,sdk}/    # C headers
  lib/arm64-v8a/                                    # .so files
  share/cmake/CNSDK/                                # find_package(CNSDK CONFIG) target
```

The `.gitignore` already excludes `/cnsdk/`, so don't commit it.

### Other CNSDK versions

The Gradle build is version-agnostic — it reads `cnsdk/VERSION.txt` and substitutes into AAR paths. The AAR lookup falls back through `sdk-faceTrackingInApp-<ver>.aar` → `sdk-faceTrackingService-<ver>.aar` → `sdk-<ver>.aar` so newer CNSDK packagings work without code changes. Note that 0.10+ may also require updating the CNSDK link target from `CNSDK::leiaSDK` to `CNSDK::leiaCore` in the plug-in's `drv_leia_android` CMakeLists (in `displayxr-leia-plugin`).

## Step 2: Configure `local.properties`

Create or edit `local.properties` in the repo root:
```properties
# Android SDK location
sdk.dir=C:/Users/<you>/AppData/Local/Android/Sdk          # Windows
# sdk.dir=/Users/<you>/Library/Android/sdk                # macOS
```

## Step 3: Build the runtime APK

```bash
# In-process debug variant (no IPC, simplest)
./gradlew :src:xrt:targets:openxr_android:assembleInProcessDebug

# APK output:
# src/xrt/targets/openxr_android/build/outputs/apk/inProcess/debug/openxr_android-inProcess-debug.apk
```

### Build variants

| Variant | Use case |
|---------|---------|
| `inProcessDebug` | First-time hardware testing, single app. **Use this.** |
| `inProcessRelease` | Performance testing |
| `outOfProcessDebug` | Multi-app / shell testing (not POC-supported) |
| `outOfProcessRelease` | Production multi-app |

## Step 4: Build the test app APK

```bash
# Switch to the test-app branch
git checkout feat/cube-handle-vk-android-frame-loop

./gradlew :test_apps:cube_handle_vk_android:assembleDebug

# APK output:
# test_apps/cube_handle_vk_android/build/outputs/apk/debug/cube_handle_vk_android-debug.apk
```

The test app does loader init → `xrCreateInstance` → `xrGetSystem` → `xrCreateVulkanInstanceKHR` → `xrCreateVulkanDeviceKHR` → `xrCreateSession` → per-view swapchains → frame loop with red/blue clear color. It's the canonical OpenXR-runtime smoke test.

## Step 5: Install on device

```bash
adb uninstall org.freedesktop.monado.openxr_runtime.in_process 2>/dev/null
adb uninstall com.displayxr.cube_handle_vk_android 2>/dev/null

adb install -r src/xrt/targets/openxr_android/build/outputs/apk/inProcess/debug/openxr_android-inProcess-debug.apk
adb install -r test_apps/cube_handle_vk_android/build/outputs/apk/debug/cube_handle_vk_android-debug.apk
```

Verify the runtime is registered:
```bash
adb shell pm list packages | grep monado
# package:org.freedesktop.monado.openxr_runtime.in_process

adb shell dumpsys package org.freedesktop.monado.openxr_runtime.in_process | grep -A3 OpenXR
# Should show org.khronos.openxr.OpenXRRuntimeService
# and SoFilename=libopenxr_displayxr.so
```

## Step 6: Smoke test

See `android-bringup-checklist.md` for the full A→B→C→D procedure. Quick check:
```bash
adb shell am start -n com.displayxr.cube_handle_vk_android/android.app.NativeActivity
adb logcat -s cube_handle_vk_android:V DisplayXR:V leia:V monado:V
```

Expected log markers:
```
android_main entered
xrInitializeLoaderKHR -> XR_SUCCESS
xrCreateInstance -> XR_SUCCESS
Runtime: "DisplayXR" v...
xrGetSystem(...) -> XR_SUCCESS
xrCreateSession -> XR_SUCCESS
Bring-up chain complete
frame 60
frame 120
...
```

## Troubleshooting

### Build fails: "No CNSDK AAR found"

The Gradle build expects the CNSDK release tree at `cnsdk/` in the repo root. Re-read Step 1.

If you have CNSDK but a different version, edit `cnsdk/VERSION.txt` to match the AAR filename.

### Build fails: "Eigen3Config.cmake not found"

The Gradle `unpackEigen` task auto-downloads Eigen 3.4.0 and writes a header-only config. If you have Eigen locally, set `eigenCMakeDir=/path/to/cmake` in `local.properties`.

### Build fails: "cmake target target_instance not found"

Should be fixed by `fix/cmake-android-target-guards`. If you see this on an old branch, rebase onto a branch that includes that fix.

### APK installs but runtime not discovered

```bash
adb shell dumpsys package org.freedesktop.monado.openxr_runtime.in_process \
    | grep -A10 OpenXRRuntimeService
```

Should show:
```
filter:
  action: org.khronos.openxr.OpenXRRuntimeService
meta-data:
  org.khronos.openxr.OpenXRRuntime.SoFilename = libopenxr_displayxr.so
  org.khronos.openxr.OpenXRRuntime.MajorVersion = 1
```

If `SoFilename` shows `libopenxr_monado.so` instead, your branch predates day-3 part 2 (`9271ebd29`). Rebase onto a branch that includes the rename.

### Black screen / no 3D interlacing

1. Check Leia Display Service is running:
   ```bash
   adb shell dumpsys activity services | grep -i leia
   ```
2. Check CNSDK initialization in logcat:
   ```bash
   adb logcat | grep -iE "leia|cnsdk|interlacer"
   ```
3. Verify the runtime DP factory succeeded:
   ```bash
   adb logcat | grep "Leia CNSDK DP created"
   # Should log: "Leia CNSDK DP created (atlas mode)"  (or "(self-submitting, per-tile blit + CNSDK weave)" on per-tile-blit branches)
   ```

### Device not found by ADB

```bash
adb kill-server && adb start-server
```

If using USB-C: try a different cable/port. If wireless: `adb connect <device-ip>:5555` with developer-options wireless debugging on.

### vkCreateAndroidSurfaceKHR fails

The Vulkan native compositor needs `VK_KHR_android_surface`. The runtime enables it; if creation still fails, check the device's Vulkan driver reports it via `adb shell dumpsys SurfaceFlinger | grep -i vulkan`.

## Architecture on Android

```
OpenXR App (Vulkan)              ← cube_handle_vk_android test app
       |
  OpenXR Loader for Android
  (libopenxr_loader.so, bundled by app)
       |  (binds via org.khronos.openxr.OpenXRRuntimeService intent)
       |
  DisplayXR Runtime APK
  (libopenxr_displayxr.so)
       |
  Vulkan Native Compositor (VK_KHR_android_surface, ANativeWindow)
       |
  CNSDK Display Processor (self_submitting, atlas mode)
       |
  leia_interlacer_vulkan_do_post_process()
       |
  Leia Display (interlaced lightfield output)
       |
  Leia Face Tracking Service (head position via leia_core_get_primary_face)
```

The runtime APK registers as an OpenXR runtime service via `org.khronos.openxr.OpenXRRuntimeService`. The Khronos OpenXR loader (bundled with the test app via the `openxr_loader_for_android` Maven AAR) discovers it at `xrCreateInstance` time.

## Related runtime issues

- [#125](https://github.com/DisplayXR/displayxr-runtime/issues/125) — CNSDK Vulkan display processor
- [#127](https://github.com/DisplayXR/displayxr-runtime/issues/127) — Vulkan compositor Android support
- [#130](https://github.com/DisplayXR/displayxr-runtime/issues/130) — `XR_EXT_android_surface_binding`
- [#131](https://github.com/DisplayXR/displayxr-runtime/issues/131) — Android CI workflow
- [#133](https://github.com/DisplayXR/displayxr-runtime/issues/133) — Gradle build integration
- [#134](https://github.com/DisplayXR/displayxr-runtime/issues/134) — Android test app (`cube_handle_vk_android`)

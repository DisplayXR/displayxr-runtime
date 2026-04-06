# Android Build & Test Guide

Build and deploy DisplayXR on an Android device with a Leia 3D display (e.g., Nubia Pad 2).

## Prerequisites

### Host machine (macOS or Linux)

| Tool | Version | Install |
|------|---------|---------|
| Android SDK command-line tools | latest | `brew install --cask android-commandlinetools` (macOS) |
| Android SDK Platform | API 35 | `sdkmanager "platforms;android-35"` |
| Android NDK | 26.3.11579264 | `sdkmanager "ndk;26.3.11579264"` |
| CMake (Android) | 3.22.1 | `sdkmanager "cmake;3.22.1"` |
| Build Tools | 34.0.0 | `sdkmanager "build-tools;34.0.0"` |
| Platform Tools (adb) | latest | `sdkmanager "platform-tools"` |
| Java JDK | 17+ | `brew install openjdk@17` (macOS) or any JDK 17+ |

**Quick install (macOS):**
```bash
brew install --cask android-commandlinetools
export ANDROID_HOME="$HOME/Library/Android/sdk"
mkdir -p "$ANDROID_HOME"
SDKMANAGER="/opt/homebrew/share/android-commandlinetools/cmdline-tools/latest/bin/sdkmanager"
yes | "$SDKMANAGER" --sdk_root="$ANDROID_HOME" --licenses
"$SDKMANAGER" --sdk_root="$ANDROID_HOME" \
  "platforms;android-35" "ndk;26.3.11579264" "cmake;3.22.1" \
  "build-tools;34.0.0" "platform-tools"
```

Android Studio is **not required** — the command-line tools are sufficient.
| ADB | latest | Included with Android SDK platform-tools |

### On the device (Nubia Pad 2)

- **Developer options enabled** (Settings > About > tap Build Number 7 times)
- **USB debugging enabled** (Settings > Developer options)
- **Leia Display Service** installed (pre-installed on Nubia Pad 2)
- **Leia Face Tracking Service** installed (pre-installed on Nubia Pad 2)

Verify device connection:
```bash
adb devices
# Should show your device as "device" (not "unauthorized")
```

## Step 1: Set up CNSDK

The Gradle build expects CNSDK as a **release package** in the repo root at `cnsdk/`. The CNSDK source repo at `/Users/david.fattal/Documents/GitHub/CNSDK` needs to be built first.

### Option A: Build CNSDK from source (recommended)

```bash
cd /Users/david.fattal/Documents/GitHub/CNSDK

# Build the CNSDK Android release package
# This produces the AAR, CMake configs, and VERSION.txt
python buildAll.py --platform android --arch arm64-v8a --config Release

# The built SDK should be in a release/ or build/ directory
# Check for the output location after build completes
```

Then create a symlink or copy the release output:
```bash
cd /Users/david.fattal/Documents/GitHub/openxr-3d-display
ln -s /path/to/cnsdk-release-output cnsdk
```

The `cnsdk/` directory must contain:
```
cnsdk/
  VERSION.txt                              # e.g., "0.9.0"
  android/
    sdk-faceTrackingService-{version}.aar  # Face tracking AAR
  lib/
    cmake/                                 # CMake find_package configs
  leia/
    core/                                  # C headers (interlacer.h, etc.)
    sdk/                                   # SDK headers
```

### Option B: Use a pre-built CNSDK release

If you have a pre-built CNSDK release ZIP:
```bash
cd /Users/david.fattal/Documents/GitHub/openxr-3d-display
unzip /path/to/cnsdk-release.zip -d cnsdk
```

### Option C: Temporary workaround (source repo symlink)

If the CNSDK source repo layout is close enough, you can try symlinking directly:
```bash
cd /Users/david.fattal/Documents/GitHub/openxr-3d-display
ln -s /Users/david.fattal/Documents/GitHub/CNSDK cnsdk
```

This may require creating a `VERSION.txt` manually:
```bash
echo "0.9.0" > cnsdk/VERSION.txt
```

And the face tracking AAR may need to be built separately or skipped. If the AAR is missing, temporarily comment out the `addFileDependency` line at the bottom of `src/xrt/targets/openxr_android/build.gradle`.

## Step 2: Configure local.properties

Create or edit `local.properties` in the repo root:

```properties
# Android SDK location (adjust to your path)
sdk.dir=/Users/david.fattal/Library/Android/sdk

# Eigen3 CMake directory (Gradle auto-downloads if not found)
# eigenCMakeDir=/opt/homebrew/share/eigen3/cmake
```

## Step 3: Build the runtime APK

### From command line (recommended)

```bash
cd /Users/david.fattal/Documents/GitHub/openxr-3d-display

# Build the in-process debug variant (simplest — no IPC needed)
./gradlew :src:xrt:targets:openxr_android:assembleInProcessDebug

# APK output location:
# src/xrt/targets/openxr_android/build/outputs/apk/inProcess/debug/
```

### From Android Studio

1. Open the repo root in Android Studio
2. Wait for Gradle sync to complete
3. Select build variant: **inProcessDebug** (Build > Select Build Variant)
4. Build > Build Bundle(s) / APK(s) > Build APK(s)

### Build variants

| Variant | Description | Use when |
|---------|-------------|----------|
| `inProcessDebug` | Runtime runs in-process with the app. Debug symbols. | First-time testing, single app |
| `inProcessRelease` | Same but optimized, no debug symbols | Performance testing |
| `outOfProcessDebug` | Runtime runs as a service (multi-app) | Shell / multi-app testing |
| `outOfProcessRelease` | Service mode, optimized | Production |

For initial testing, use **inProcessDebug**.

## Step 4: Install on device

```bash
# Install the runtime APK
adb install -r src/xrt/targets/openxr_android/build/outputs/apk/inProcess/debug/*.apk

# Verify installation
adb shell pm list packages | grep monado
# Should show: package:org.freedesktop.monado.openxr_runtime.in_process
```

## Step 5: Test the runtime

### Quick smoke test — verify runtime loads

```bash
# Check that the OpenXR runtime service is registered
adb shell dumpsys package org.freedesktop.monado.openxr_runtime.in_process | grep -A5 "OpenXR"

# Check logcat for runtime initialization
adb logcat -s monado:V DisplayXR:V leia:V | head -50
```

### Test with a sample OpenXR app

Since there is no dedicated DisplayXR Android test app yet (see [#134](https://github.com/dfattal/openxr-3d-display/issues/134)), use one of these options:

#### Option A: Khronos hello_xr sample

Build the [OpenXR SDK](https://github.com/KhronosGroup/OpenXR-SDK-Source) `hello_xr` for Android:

```bash
git clone https://github.com/KhronosGroup/OpenXR-SDK-Source.git
cd OpenXR-SDK-Source
# Follow Android build instructions in the repo
# Build hello_xr with Vulkan graphics plugin
```

#### Option B: CNSDK sample app (if available)

Check the CNSDK samples for an OpenXR-compatible app:
```bash
ls /Users/david.fattal/Documents/GitHub/CNSDK/android/samplesProject/
```

#### Option C: Any existing OpenXR Android app

Any OpenXR app that uses the Khronos loader will discover the DisplayXR runtime automatically via the Android `OpenXRRuntimeService` intent.

### Verify interlacing

When an OpenXR app renders stereo views on the Nubia Pad 2:

1. **3D mode active**: The display should switch to lightfield mode (visible lenticular pattern)
2. **Head tracking**: Moving your head should shift the 3D perspective
3. **Logcat markers** to watch for:
   ```
   adb logcat -s monado:V | grep -E "(CNSDK|interlacer|display_processor|self_submitting)"
   ```
   - `Created CNSDK display processor` — factory succeeded
   - `leia_core_is_initialized` — CNSDK core ready
   - `leia_interlacer_vulkan_initialize` — interlacer created

## Troubleshooting

### Build fails: "CNSDK not found"

Verify the symlink/directory:
```bash
ls -la cnsdk/
cat cnsdk/VERSION.txt
```

### Build fails: missing face tracking AAR

Temporarily comment out the last line in `src/xrt/targets/openxr_android/build.gradle`:
```groovy
// addFileDependency(dependencies, "${project.cnsdkDir}/android/sdk-faceTrackingService-${project.cnsdkVersion}.aar")
```

### APK installs but runtime not discovered

Check that the runtime service is declared:
```bash
adb shell dumpsys package org.freedesktop.monado.openxr_runtime.in_process \
  | grep -A10 "org.khronos.openxr"
```

The output should show:
```
filter: org.khronos.openxr.OpenXRRuntimeService
meta-data:
  org.khronos.openxr.OpenXRRuntime.SoFilename=libopenxr_monado.so
  org.khronos.openxr.OpenXRRuntime.MajorVersion=1
```

### No 3D interlacing / black screen

1. Check that the Leia Display Service is running:
   ```bash
   adb shell dumpsys activity services | grep leia
   ```
2. Check CNSDK initialization in logcat:
   ```bash
   adb logcat | grep -i "leia\|cnsdk\|interlacer"
   ```
3. Verify the app is requesting stereo rendering (2 views, not 1)

### Device not found by ADB

```bash
# Restart ADB server
adb kill-server && adb start-server

# If using USB-C, try a different cable/port
# If wireless debugging: adb connect <device-ip>:5555
```

## Architecture on Android

```
OpenXR App (Vulkan)
       |
  OpenXR Loader (Android service discovery)
       |
  DisplayXR Runtime (libopenxr_monado.so)
       |
  Vulkan Native Compositor (VK_KHR_android_surface)
       |
  CNSDK Display Processor (self_submitting)
       |  
  leia_interlacer_vulkan_do_post_process()
       |
  Leia Display (interlaced lightfield output)
       |
  Leia Face Tracking Service (head position)
```

The runtime APK registers as an OpenXR runtime service. Any OpenXR app on the device automatically discovers it via the Android package manager. The CNSDK display processor handles interlacing and face tracking through the Leia system services pre-installed on the device.

## Related issues

- [#125](https://github.com/dfattal/openxr-3d-display/issues/125) — CNSDK Vulkan display processor
- [#127](https://github.com/dfattal/openxr-3d-display/issues/127) — Vulkan compositor Android support
- [#131](https://github.com/dfattal/openxr-3d-display/issues/131) — Android CI workflow
- [#133](https://github.com/dfattal/openxr-3d-display/issues/133) — Gradle build integration
- [#134](https://github.com/dfattal/openxr-3d-display/issues/134) — Android test app (cube_handle_vk_android)

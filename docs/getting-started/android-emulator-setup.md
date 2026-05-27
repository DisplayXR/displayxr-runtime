# Android emulator pre-bring-up

How far the Android emulator gets you on the way to a real Lume Pad
bring-up. Short version: **build + install + the full OpenXR
loader / broker / plug-in handshake all work — and
`xrCreateInstance` succeeds.** The wall is at the runtime's own
Vulkan-device creation, where the emulator's software-Vulkan is
missing extensions the runtime asks for. Useful for catching every
build / loader / linker bug before hardware.

## What works on the emulator

| Step | Works | Notes |
|---|---|---|
| Build runtime + plug-in + test app APKs | ✅ | All gradle + cmake paths the bring-up checklist documents. |
| `adb install` of arm64-v8a APKs | ✅ | Android-36 emulator's `cpu.abilist` includes `arm64-v8a` (binary translation via libnativebridge); no x86_64 rebuild required. |
| Test app launches as a NativeActivity | ✅ | `am start -n com.displayxr.cube_handle_vk_android/android.app.NativeActivity`. |
| `xrInitializeLoaderKHR` succeeds | ✅ | Khronos loader picks up the JavaVM + Activity refs. |
| Vulkan 1.0+ available | ✅ | `pm list features` reports `vulkan.level=1`, `vulkan.version=0x402080` (1.0.131). |
| Runtime broker ContentProvider | ✅ | `OpenXRRuntimeBroker.kt` in the runtime APK responds at `org.khronos.openxr.runtime_broker` (#332). |
| Loader → runtime DLL handshake | ✅ | `xrNegotiateLoaderRuntimeInterface` succeeds. |
| Plug-in `.so` dlopen + CNSDK transitive deps | ✅ | `preload_runtime_lib_dir` in `target_plugin_loader.c` (#333) brings sibling .so files into the namespace by absolute path so DT_NEEDED resolves. |
| Plug-in `xrtPluginNegotiate` + `probe` | ✅ | Returns `iface={id="leia-cnsdk", ...}`. |
| `xrCreateInstance` | ✅ | Full handshake completes. |
| `xrCreateVulkanInstanceKHR` | ✅ | Software Vulkan accepts the instance create. |

## What doesn't work on the emulator

| Step | Why | Where it works |
|---|---|---|
| `xrCreateVulkanDeviceKHR` | Software Vulkan returns `VK_ERROR_EXTENSION_NOT_PRESENT` for one of the device extensions the runtime requests. Emulator's swiftshader vulkan implements a minimal extension set. | Lume Pad (real arm Vulkan + Mali / Adreno drivers should have what's missing). |
| Anything past xrCreateSession | Blocked on the above. | Lume Pad. |
| CNSDK face-tracking init | Needs Leia hardware (camera + system service). | Lume Pad. |
| Display weave / calibration | No lenticular optics. | Lume Pad. |

For first-light hardware bring-up the emulator already validates everything except hardware Vulkan + CNSDK init — which is a meaningful slice of what can go wrong.

## Step-by-step emulator workflow

This walks the full happy path. Stop at any step that errors.

### 0. One-time AVD setup

```bash
# In Android Studio: Tools > Device Manager > Create Device > Phone >
# any device > Next > pick a recent x86_64 system image (Android 13+ is
# fine — Android 12+ has all the package-visibility hardening you want
# to catch).
#
# Or via cmdline:
ANDROID_SDK=$LOCALAPPDATA/Android/Sdk
$ANDROID_SDK/cmdline-tools/latest/bin/sdkmanager.bat \
    "system-images;android-36;google_apis_playstore;x86_64"
$ANDROID_SDK/cmdline-tools/latest/bin/avdmanager.bat create avd \
    -n DisplayXR_Emulator -k "system-images;android-36;google_apis_playstore;x86_64"
```

### 1. Start emulator + wait for boot

```bash
ANDROID_SDK=$LOCALAPPDATA/Android/Sdk
"$ANDROID_SDK/emulator/emulator.exe" -avd DisplayXR_Emulator \
    -no-snapshot-save -no-audio -gpu swiftshader_indirect -no-boot-anim &

ADB="$ANDROID_SDK/platform-tools/adb.exe"
until [ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" = "1" ]; do
    sleep 5
done

# Confirm Vulkan support is present
"$ADB" shell pm list features | grep vulkan
```

### 2. Build the three APKs

See [`android-build-guide.md`](android-build-guide.md) for the
prerequisites. The condensed build sequence — order matters:

```bash
# (A) Runtime APK first. This downloads and unpacks Eigen into
# the runtime's build/intermediates/ tree, which the plug-in
# CMake then consumes via Eigen3_DIR.
cd /c/openxr-3d-display
./gradlew.bat :src:xrt:targets:openxr_android:assembleInProcessDebug

# (B) Plug-in .so + CNSDK transitive .so deps, dropped into
# the runtime APK's jniLibs/<ABI>/ in one command. The
# `install-runtime-jnilibs` target builds libdxrp050_leia_cnsdk.so,
# extracts sdk-faceTrackingInApp-<ver>.aar + snpe-release.aar, and
# copies all 16 .so files (plug-in + 4 CNSDK + 11 SNPE) into the
# runtime's jniLibs. Without these the runtime fails at first
# xrCreateInstance with "dlopen libleiaSDK-faceTrackingInApp.so
# not found".
cd /c/displayxr-leia-plugin
scripts/build-android.sh install-runtime-jnilibs

# (C) Re-build runtime APK so it picks up the new jniLibs/
# contents. The `--rerun-tasks` flag is load-bearing — gradle's
# incremental builder can miss a newly-created jniLibs/<ABI>/
# directory and ship the runtime APK without the .so files.
cd /c/openxr-3d-display
./gradlew.bat :src:xrt:targets:openxr_android:assembleInProcessDebug --rerun-tasks

# (D) Test app.
./gradlew.bat :test_apps:cube_handle_vk_android:assembleDebug
```

**Sanity check:** `unzip -l <runtime.apk> | grep '\.so$'` should list 16 entries (1 runtime + 1 plug-in + 3 CNSDK + 11 SNPE). If it shows only 1 or 2, the install-runtime-jnilibs step didn't run or the runtime rebuild didn't pick up the new jniLibs.

### 3. Install on emulator

```bash
ADB="$LOCALAPPDATA/Android/Sdk/platform-tools/adb.exe"
"$ADB" install -r src/xrt/targets/openxr_android/build/outputs/apk/inProcess/debug/openxr_android-inProcess-debug.apk
"$ADB" install -r test_apps/cube_handle_vk_android/build/outputs/apk/debug/cube_handle_vk_android-debug.apk
```

### 4. Run + observe

```bash
# Clear log buffer + start the test app
"$ADB" logcat -c
"$ADB" shell am start -n com.displayxr.cube_handle_vk_android/android.app.NativeActivity

# Capture logcat (filter strings from android-bringup-logcat.md)
"$ADB" logcat -v threadtime -s cube_handle_vk_android:V OpenXR-Loader:V \
    target_plugin_loader:V leia_cnsdk:V DisplayXR-Broker:V
```

**Expected log on the emulator:**

```
xrInitializeLoaderKHR -> XR_SUCCESS
OpenXR-Loader: getActiveRuntimeCursor: Querying URI: content://org.khronos.openxr.runtime_broker/openxr/1/abi/arm64-v8a/runtimes/active
DisplayXR-Broker: Active runtime resolved: ...openxr_displayxr.so in /data/app/.../lib/arm64
OpenXR-Loader: RuntimeInterface::LoadRuntime succeeded
plugin loader: preloaded libblink.so
plugin loader: preloaded libleiaSDK-faceTrackingInApp.so
plugin loader: preloaded liblicense_utils.so
plugin loader: preloaded libSNPE.so
(... 11 more SNPE deps preloaded ...)
plugin loader: active plug-in: id=leia-cnsdk name='DisplayXR Leia CNSDK (Android)' ...
xrCreateInstance -> XR_SUCCESS
xrCreateVulkanInstanceKHR -> XR_SUCCESS
xrCreateVulkanDeviceKHR vk_result=-7  ← VK_ERROR_EXTENSION_NOT_PRESENT (emulator stops here)
```

This is the **expected emulator stopping point**. Everything up to xrCreateInstance is real validation; the device-create failure is the emulator's Vulkan limit, not our code. Lume Pad's hardware Vulkan should clear this and reach `xrCreateSession`.

## What this catches before Lume Pad

| Bug class | Caught? |
|---|---|
| APK build / `.so`-name / manifest mismatch | ✅ |
| `jniLibs/` plumbing (gradle's incremental-cache miss) | ✅ |
| Android-12+ `<queries>` regressions | ✅ |
| OpenXR loader binding | ✅ |
| Runtime broker discovery | ✅ |
| Plug-in `dlopen` + Android linker namespaces | ✅ |
| Plug-in `probe`/`negotiate` contract | ✅ |
| Vulkan-extension availability mismatches | Partial (only the ones swiftshader does support) |
| CNSDK init / display weave / calibration | ❌ — need hardware |
| `xrCreateVulkanDeviceKHR` extension set | ❌ — need hardware |

Pre-bring-up, the emulator covers everything except the last three. That's a real majority of what would otherwise burn the first Lume Pad session.

## Tear-down

```bash
"$ADB" shell am force-stop com.displayxr.cube_handle_vk_android
"$ADB" uninstall com.displayxr.cube_handle_vk_android
"$ADB" uninstall org.freedesktop.monado.openxr_runtime.in_process
"$ADB" emu kill
```

## Related docs

- [`android-build-guide.md`](android-build-guide.md) — full prerequisites + build commands.
- [`android-bringup-checklist.md`](android-bringup-checklist.md) — the post-emulator hardware test plan.
- [`android-bringup-logcat.md`](android-bringup-logcat.md) — pre-written logcat filters per test.

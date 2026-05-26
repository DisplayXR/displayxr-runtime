# Android emulator pre-bring-up

How far the Android emulator gets you on the way to a real Lume Pad
bring-up. Short version: **build + install + most of the OpenXR loader
discovery surface — but xrCreateInstance fails at the broker step.**
Useful for catching half the bring-up bugs early; not a full
replacement for hardware.

## What works on the emulator

| Step | Works | Notes |
|---|---|---|
| Build runtime + plug-in + test app APKs | ✅ | All gradle + cmake paths the bring-up checklist documents. |
| `adb install` of arm64-v8a APKs | ✅ | Android-36 emulator's `cpu.abilist` includes `arm64-v8a` (binary translation via libnativebridge); no x86_64 rebuild required. |
| Test app launches as a NativeActivity | ✅ | `am start -n com.displayxr.cube_handle_vk_android/android.app.NativeActivity`. |
| `xrInitializeLoaderKHR` succeeds | ✅ | Khronos loader picks up the JavaVM + Activity refs. |
| Vulkan 1.0+ available | ✅ | `pm list features` reports `vulkan.level=1`, `vulkan.version=4206592` (0x402080 = 1.0.131). |

## What doesn't work on the emulator

`xrCreateInstance` fails with `XR_ERROR_RUNTIME_UNAVAILABLE`. Logcat:

```
OpenXR-Loader: Error: RuntimeManifestFile::FindManifestFiles -
    failed to determine active runtime file path for this environment
OpenXR-Loader: Error: RuntimeInterface::LoadRuntimes - unknown error
OpenXR-Loader: Error: xrCreateInstance failed
cube_handle_vk_android: xrCreateInstance -> XR_ERROR_RUNTIME_UNAVAILABLE
```

The Khronos OpenXR loader on Android uses a **runtime broker** model:
the test app queries PackageManager for an installed
ContentProvider at the authority `org.khronos.openxr.runtime_broker`
(or `org.khronos.openxr.system_runtime_broker`), and that broker
tells the loader which `.so` to dlopen. The DisplayXR runtime APK
ships only the runtime `.so` + `RuntimeService` — not the broker
ContentProvider — so even with the APK installed, the loader has no
way to discover it.

Two related issues fall out of this:

1. **`<queries>` element**: Android 12+ blocks cross-package
   PackageManager queries by default. Fixed in PR #269 — the test
   app's manifest now declares `<queries><intent>... OpenXRRuntimeService ...</intent></queries>`.
   Without this, the loader's PackageManager query returns empty on
   any Android-12+ device, emulator or Lume Pad.

2. **Active runtime broker**: real hardware (Lume Pad) presumably
   ships a vendor broker APK pointing at the installed runtime,
   OR a system-level setting that the loader consults. On the
   stock Android emulator neither exists, so the runtime is
   invisible to the loader.

### What to do about the broker on the emulator

Three options if you really need to exercise `xrCreateInstance` →
`xrCreateSession` on the emulator:

| Option | Effort | Caveats |
|---|---|---|
| **Install Khronos's sample broker APK** | Medium — build from <https://github.com/KhronosGroup/OpenXR-SDK-Source/tree/main/specification/sources/scripts/openxr-android-broker> | Sample isn't published as an APK; need to build it yourself. |
| **Add a broker ContentProvider to the runtime APK** | Small (~50 lines Kotlin) | Probably the right long-term answer for the runtime — a broker is needed on any Android device where the runtime is the only one installed. Tracked as a follow-up. |
| **Set the active runtime via Settings provider** | OS-dependent | Lume Pad may expose this; stock emulator doesn't. |

For first-light bring-up on Lume Pad, **skip the emulator** and go
straight to hardware — Lume Pad's preinstalled OS likely handles the
broker. If you hit `XR_ERROR_RUNTIME_UNAVAILABLE` on the Lume Pad too,
swap to one of the three options above (likely option 2 — bake the
broker into the runtime APK).

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
build steps. The condensed version:

> **Pre-merge branch note:** until all Android PRs land on `main`,
> the runtime stack and the test app stack live on parallel branches.
> Make sure your runtime checkout has BOTH the runtime PR head
> (e.g. `feat/android-plugin-namespace-fix` — top of the runtime
> PR stack) AND the test-app PR head (e.g.
> `feat/cube-test-app-triangle-render`) merged into one local
> working branch. Otherwise the `:test_apps:cube_handle_vk_android`
> gradle task silently no-ops (the module doesn't exist) and
> step 3 fails with "file not found" on the test app APK.
> Post-merge to `main` this all just works from a single checkout.

```bash
# (A) Runtime APK first — required because the plug-in's CMake
# pulls in the runtime as a subdirectory and consumes the
# gradle-fetched Eigen from the runtime's build/ tree. This
# first run also materializes that Eigen on disk so step (B)
# can find it.
cd /c/openxr-3d-display
./gradlew.bat :src:xrt:targets:openxr_android:assembleInProcessDebug

# (B) Plug-in .so + drop into runtime APK's jniLibs/, with all
# CNSDK transitive .so deps. Using the `install-runtime-jnilibs`
# target does both in one command — basic `scripts/build-android.sh`
# (no target arg) would ship the plug-in .so alone and the runtime
# would later fail to dlopen its DT_NEEDED references at first
# xrCreateInstance ("library libleiaSDK-faceTrackingInApp.so not
# found"). 16 .so files total dropped: plug-in + 4 CNSDK + 11 SNPE.
cd /c/displayxr-leia-plugin && scripts/build-android.sh install-runtime-jnilibs

# (C) Re-build runtime APK so it picks up the freshly-installed
# jniLibs/<ABI>/ contents. The `--rerun-tasks` flag is load-bearing
# the first time — gradle's incremental builder can miss a
# newly-created `jniLibs/<ABI>/` directory and ship the runtime
# APK without those .so files.
cd /c/openxr-3d-display
./gradlew.bat :src:xrt:targets:openxr_android:assembleInProcessDebug --rerun-tasks

# (D) Test app APK.
./gradlew.bat :test_apps:cube_handle_vk_android:assembleDebug
```

Always sanity-check `unzip -l <runtime.apk> | grep '.so$'` shows
all 16 .so files (the runtime + plug-in + CNSDK transitive
deps). If it shows only 1 or 2, the install-runtime-jnilibs step
didn't run or the runtime rebuild didn't pick them up.

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
    target_plugin_loader:V leia_cnsdk:V
```

**Expected log on the emulator (without a broker):**

```
xrInitializeLoaderKHR -> XR_SUCCESS
OpenXR-Loader: Entering loader trampoline (xrCreateInstance)
OpenXR-Loader: Error: RuntimeManifestFile::FindManifestFiles - failed to determine active runtime file path
xrCreateInstance -> XR_ERROR_RUNTIME_UNAVAILABLE
```

If you see this exact sequence, the emulator confirms:
- Build pipeline produced a valid APK that installs.
- Manifest's `<queries>` block lets the loader's PackageManager calls run (no platform-level visibility errors).
- Vulkan is available.

The error at `xrCreateInstance` is the **expected emulator stopping
point** — the next step requires a broker.

## What this catches before Lume Pad

Half-dozen bug classes you can hit and fix without leaving the host:

| Bug class | Caught? | Example |
|---|---|---|
| APK build failures | ✅ | The `.so`-name / manifest-name mismatch fixed in PR #268. |
| `jniLibs/` plumbing | ✅ | Plug-in `.so` not bundled into the runtime APK (gradle incremental cache bug). |
| Android-12+ `<queries>` regressions | ✅ | PR #269's manifest fix. |
| OpenXR loader binding (instance create entry point) | ✅ | `xrInitializeLoaderKHR` succeeds. |
| Native-lib JNI plumbing (NativeActivity, ALooper, etc.) | ✅ | Test app boots into android_main without crashing. |
| Vulkan-extension availability mismatches | Partial | Emulator's Vulkan 1.0.131 is older than Lume Pad's; some extension queries we'd want to make won't exercise. |
| CNSDK init | ❌ | Needs Leia hardware (no lenticular optics, no face-tracking camera). |
| Display weave correctness | ❌ | Same — needs hardware. |
| Face-axis / view-mapping / UV-flip calibration | ❌ | Same. |
| Active-runtime broker resolution | ❌ | Needs a broker installed (deferred). |

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

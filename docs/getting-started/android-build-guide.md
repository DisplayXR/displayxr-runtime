# Android Build & Test Guide

> **Just want to install and run it?** You do not need this document. See
> [android-install-demos.md](android-install-demos.md) for the end-to-end path from published
> release artifacts to five running demos, no build environment required.


Build and deploy DisplayXR on an Android device with a Leia 3D display.

> **You probably do not need this page.** Since #1212 the runtime ships
> **released APKs**, so installing is no longer a build exercise — see
> [Installing from released artifacts](#installing-from-released-artifacts)
> immediately below. Everything after that is the from-source bring-up
> path, for people changing the runtime itself.

## Installing from released artifacts

Every `v*` release carries two Android APKs:

| asset | contents | use it when |
|---|---|---|
| `DisplayXR-Runtime-<ver>-android-arm64.apk` | sim-display only | no vendor display; hardware-free testing |
| `DisplayXR-Runtime-Leia-<ver>-android-arm64.apk` | + the Leia CNSDK plug-in | **a Leia device** |

Install the runtime **and** an app with:

```bash
./scripts/install-android.sh \
    DisplayXR-Runtime-Leia-2.13.5-android-arm64.apk \
    DisplayXRModelViewer-0.24.2.apk
```

Use the script rather than `adb install`. Two device-state requirements are
invisible, are dropped by every reinstall, and both fail with symptoms that
point somewhere else:

- **The runtime app must be launched once.** Uninstalling it deregisters its
  `OpenXRRuntimeBroker` ContentProvider, and Android's `FLAG_STOPPED` keeps
  that provider unresolvable by other packages until the app is opened. Every
  OpenXR app then dies at instance creation with
  `Failed to find provider info for org.khronos.openxr.runtime_broker` /
  `XR_ERROR_RUNTIME_UNAVAILABLE` — which reads as a broken runtime rather
  than "nobody opened it". There is no `BOOT_COMPLETED` receiver to clear it.
- **`SYSTEM_ALERT_WINDOW` must be re-granted.** It is an app-op, never granted
  at install, and dropped by uninstall+install. Without it see-through apps
  render on a **black background** while 3D and weaving keep working, so it
  looks like a content bug.

Then open the **DisplayXR** app on the device: its dashboard runs the same
self-test as `displayxr-cli selftest`. On a vendor display the active plug-in
must not be `sim-display`; a failing `vendor_dp` check means a vendor plug-in
was present but could not load, usually an ABI mismatch between the APK and
the plug-in inside it.

Which plug-in is inside a given APK is determined by `versions.json`'s
`leia_plugin` field at the runtime tag, so the whole stack is reproducible
from a version string. Why the vendor plug-in is bundled rather than
separately installed:
[ADR-038](../adr/ADR-038-android-vendor-plugin-ships-in-the-runtime-apk.md).

---

## Building from source

Everything below is the from-source path.

> **Plug-in split (post-#268):** the CNSDK display-processor plug-in
> now lives in [`displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin)
> and builds to `libdxrp050_leia_cnsdk.so`, which is dropped into the
> runtime APK's `jniLibs/<ABI>/` for the runtime's plug-in loader to
> discover at `xrCreateInstance`. That repo builds it in CI now
> (`build-android.yml`) and attaches it to its own `v*` releases, so a
> hand build is only needed when you are changing the plug-in.

Companion docs:
- [`android-bringup-checklist.md`](android-bringup-checklist.md) — A→B→C→D step-by-step test procedure once both APKs are built.
- [`displayxr-leia-plugin/docs/cnsdk-android-calibration.md`](https://github.com/DisplayXR/displayxr-leia-plugin/blob/main/docs/cnsdk-android-calibration.md) — symptom→fix table for the three CNSDK convention assumptions (face axes, view mapping, UV flip).

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

The plug-in's CMake build expects CNSDK as an extracted release tree; point `CNSDK_ROOT` at it. Minimum **0.10.54** — 0.7.28 predates the loader architecture the plug-in compiles against and fails at runtime with "Missing required service: InterlacingService".

### Fetch CNSDK

> The public `leiainc.github.io` copy is stuck at **0.7.28 and no longer
> works**. Fetch a current build from the private `LeiaInc/CNSDK` repo
> (needs LeiaInc org read access) — this is the same source CI uses:
```bash
gh release download v0.10.61 -R LeiaInc/CNSDK -p 'cnsdk-android-*.zip'
unzip cnsdk-android-*.zip -d cnsdk
```

Result: `cnsdk/` contains
```
cnsdk/
  VERSION.txt                                       # e.g. "0.10.61"
  android/
    sdk-<ver>.aar                                   # Java glue + the two transitive .so
  include/leia/{common,device,headTracking,sdk}/    # C headers
  lib/arm64-v8a/
    libleiaCore-loader.so                           # the shim the plug-in DT_NEEDEDs
    libleiaSDK-jni.so                               # Java<->native bridge
  share/cmake/CNSDK/                                # find_package(CNSDK CONFIG) target
```

The `.gitignore` already excludes `/cnsdk/`, so don't commit it.

**CNSDK is a build-time dependency, not a redistributable engine.**
`libleiaCore-loader.so` is a shim: at runtime it builds a `DexClassLoader`
over the on-device package `com.leialoft.display.config`, reads its
`nativeLibraryDir`, and `dlopen`s `libleiaCore-impl.so` **from there**. The
real core, face tracking and per-device calibration live on the device — the
same relationship Windows has with an installed `LeiaSR_runtime.dll`. What
must ship in the APK is only the shim plus the AAR's Java glue, because
Android has no system-wide search path for third-party natives. See
[ADR-038](../adr/ADR-038-android-vendor-plugin-ships-in-the-runtime-apk.md).

Never republish the CNSDK zip: Leia's Creator Toolkit licence permits
distribution "as incorporated into your Products" (§3) but forbids
distributing the materials standalone (§4b).

### Other CNSDK versions

The build reads `cnsdk/VERSION.txt` and substitutes it into the AAR path, and
`find_package(CNSDK 0.10.54 ...)` enforces the floor via the SDK's own
`CNSDKConfigVersion.cmake`. The runtime APK's gradle AAR lookup falls back
through `sdk-faceTrackingInApp-<ver>.aar` → `sdk-faceTrackingService-<ver>.aar`
→ `sdk-<ver>.aar`, so newer packagings work without code changes.

## Step 2: Configure `local.properties`

Create or edit `local.properties` in the repo root:
```properties
# Android SDK location
sdk.dir=C:/Users/<you>/AppData/Local/Android/Sdk          # Windows
# sdk.dir=/Users/<you>/Library/Android/sdk                # macOS
```

## Step 3: Build the runtime APK

```bash
./gradlew :src:xrt:targets:openxr_android:assembleDebug

# APK output:
# src/xrt/targets/openxr_android/build/outputs/apk/debug/openxr_android-debug.apk
```

### Build variants

There is **one** runtime APK (#1031). The `inProcess` / `outOfProcess` product
flavors are gone: the merged build carries both the in-process native compositor
and the service + satellite slots, and each app lands on one or the other at
`xrCreateInstance`. So the only axis left is debug vs release.

| Variant | Use case |
|---------|---------|
| `debug` | Everything — first-time bring-up, single app, multi-app, weave. **Use this.** |
| `release` | Performance testing / production |

### Which deployment does my app get?

**In-process by default** — the app's own process hosts the compositor and the
vendor display processor (ADR-036 D2, Architecture A). An app opts into the IPC
path, where the runtime service hands it a satellite compositor process (ADR-036
D3, Architecture C), by any one of:

| Opt-in | How | Notes |
|---|---|---|
| Env / sysprop | `XRT_FORCE_MODE=ipc`, or `adb shell setprop debug.dxr.force_ipc 1` | Overrides everything, both directions — `XRT_FORCE_MODE=native` forces back in-process. The sysprop is device-wide, so unset it when you are done. |
| Manifest | `<meta-data android:name="com.displayxr.force_ipc" android:value="true"/>` | The per-app switch. Pair with `com.displayxr.satellite_slot` to pin a slot. |
| Capability | enable `XR_DXR_weave` | Present-owners (the browser, `weave_client_vk_android`). Weave lives only in the service compositor, so this is automatic — no configuration. |
| Adopted socket | `ipc_client_connection_adopt_fd()` or `DXR_IPC_FD=<n>` | Embedders with no `Context` (Chromium's GPU process, #1056). |

### Migrating an existing app to the merged runtime

Before #1031 every app on the device was pushed out of process, because the
installed `outOfProcess` flavor left it no choice. In-process is now the
default, so an app that was never ported to Architecture A gets the in-process
path for the first time — and if it is not ready for it, that shows up as a
`xrCreateSession` failure rather than a graceful fallback.

The known failure shape, seen with the `native_app_glue` demos (`android_main`
render thread, no first-class Activity/surface binding): the vendor core loader
aborts under CheckJNI with a **null jobject in `GetObjectClass`**, inside
`libleiaCore-loader.so` reached from `leia_cnsdk_create` →
`leia_dp_factory_cnsdk` → `comp_vk_native_compositor_create` →
`oxr_session_populate_vk_native`. The app has no Activity-typed Context to hand
the vendor Java glue in its own process. This is the Architecture-A gap noted
in ADR-036 D2 Amendment 1, not a regression — those apps simply never ran
in-process before.

Until such an app is ported (real `XR_DXR_android_surface_binding`, an Activity
the runtime can reach — the `cube_handle_vk_android` shape), keep it on the IPC
path with one line in its own manifest:

```xml
<application>
    <meta-data android:name="com.displayxr.force_ipc" android:value="true" />
</application>
```

That is a per-app change in the app's repo; it needs nothing from the runtime
and does not affect any other app on the device.

Check which one an app got:

```bash
adb logcat -d | grep "Hybrid mode:"
# "using in-process native compositor"  -> Architecture A
# "... forcing IPC ..." / "using IPC/service compositor" -> Architecture C
adb shell ps -A -o PID,NAME | grep :dxr   # satellites, one per IPC client
```

## Step 4: Build the test app APK

```bash
# Switch to the test-app branch
git checkout feat/cube-handle-vk-android-frame-loop

./gradlew :test_apps:cube_handle_vk_android:assembleDebug

# APK output:
# test_apps/handle/cube_handle_vk_android/build/outputs/apk/debug/cube_handle_vk_android-debug.apk
```

The test app does loader init → `xrCreateInstance` → `xrGetSystem` → `xrCreateVulkanInstanceKHR` → `xrCreateVulkanDeviceKHR` → `xrCreateSession` → per-view swapchains → frame loop with red/blue clear color. It's the canonical OpenXR-runtime smoke test.

## Step 5: Install on device

```bash
adb uninstall org.freedesktop.monado.openxr_runtime.out_of_process 2>/dev/null
adb uninstall com.displayxr.cube_handle_vk_android 2>/dev/null

adb install -r src/xrt/targets/openxr_android/build/outputs/apk/debug/openxr_android-debug.apk
adb install -r test_apps/handle/cube_handle_vk_android/build/outputs/apk/debug/cube_handle_vk_android-debug.apk
```

Verify the runtime is registered:
```bash
adb shell pm list packages | grep monado
# package:org.freedesktop.monado.openxr_runtime.out_of_process

adb shell dumpsys package org.freedesktop.monado.openxr_runtime.out_of_process | grep -A3 OpenXR
# Should show org.khronos.openxr.OpenXRRuntimeService
# and SoFilename=libopenxr_displayxr.so
```

### Re-grant the overlay permission after EVERY reinstall

`SYSTEM_ALERT_WINDOW` ("display over other apps") is a **special app-op
permission**: it is never granted at install time, and an `adb uninstall` +
`adb install` **drops** any previous grant. The runtime needs it for **overlay
mode** (#558) — the service-owned `TYPE_APPLICATION_OVERLAY` that see-through
apps (`displayxr-demo-avatar`) weave into.

```bash
adb shell appops set org.freedesktop.monado.openxr_runtime.out_of_process SYSTEM_ALERT_WINDOW allow
# verify: should print "SYSTEM_ALERT_WINDOW: allow"
adb shell appops get org.freedesktop.monado.openxr_runtime.out_of_process SYSTEM_ALERT_WINDOW
```

`scripts/build-android.sh install` now does this for you; the manual command is
for the hand-installed / released-APK case. **Symptom when it's missing:** a
see-through app renders on a **BLACK** background and nothing else looks wrong —
the 3D weave keeps working, so it is easily mistaken for a transparency
regression in the runtime or the demo. Confirm with:

```bash
adb logcat | grep SURFACE_FMT
# lost grant : ... compositeAlpha=0x8 transparent=0 overlay=0
# granted    : ... compositeAlpha=0x8 transparent=1 overlay=1
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

## Multi-app testing (satellite compositor processes)

Two or more DisplayXR apps weave at the same time on Android either **in-process**
(each app hosts its own compositor and vendor core — the default since #1031) or
**out-of-process**, where the runtime hands an IPC client its own **satellite
compositor process** — `MonadoServiceSlot0..3`,
declared with `android:process=":dxr0"` … `":dxr3"`, assigned by a broker in the runtime's main
process (ADR-036 D3, #1031; mechanism in
[`service-architecture.md` §7a](../architecture/service-architecture.md)). Nothing needs
enabling: any out-of-process client gets a satellite, and the fifth concurrent app falls back to
the single main-process service. Since #1031 the two deployments coexist on one device — see
"Which deployment does my app get?" above for how an app lands on each.

**Build a second copy of the cube** (same source, different `applicationId`, so two clients can
be installed at once):
```bash
./gradlew :src:xrt:targets:openxr_android:assembleDebug
./gradlew :test_apps:cube_handle_vk_android:assembleDebug                      # A
./gradlew :test_apps:cube_handle_vk_android:assembleDebug -PdxrAppIdSuffix=b   # B → ....b
```
Both cubes run **in-process** by default. To put one of them on a satellite instead
(the mixed case), force just that package:
```bash
adb shell am start -n com.displayxr.cube_handle_vk_android.b/....MainActivity  # in-process
adb shell setprop debug.dxr.force_ipc 1   # next launch goes IPC; unset when done
```
(The two builds write the same APK path, so copy A aside before building B. On a host with a
system cJSON — `brew install cjson` — add `-PdxrForceVendoredCjson`; see #496.)

**Run them side by side:**
```bash
scripts/android-sidebyside.sh                 # freeform, left = A, right = B
scripts/android-sidebyside.sh --kill          # tear down apps + satellites first
scripts/android-sidebyside.sh --no-stage      # let the first app take the relayout bounce
```
The script SIGSTOPs each app until its task bounds are final, so no window is resized after its
OpenXR session is live, and prints one status line per app: task, window frame, satellite pid and
`:dxrN`, the broker's decision verbatim, whether that satellite is presenting, its content mode,
and its swapchain surface format.

**Checking slot assignment by hand:**
```bash
adb logcat -s dxr-slot-broker            # acquire/release decisions + occupancy
adb shell ps -A -o PID,NAME | grep :dxr  # one process per live client
adb shell setprop debug.dxr.slot 2       # dev pin: every client asks for slot 2
adb shell setprop debug.dxr.slot -1      # back to broker-assigned
```
An app can also pin itself with `<meta-data android:name="com.displayxr.satellite_slot"
android:value="N"/>`. Both are only *preferences* — the broker still decides, so two apps
pinning the same slot cannot collide.

**Raising the slot count.** `dxrSatelliteSlots` in `src/xrt/ipc/android/build.gradle` (default 4)
is the single source of truth: it generates the `MonadoServiceSlotN` classes. Add the matching
`<service android:name=".MonadoServiceSlotN" android:process=":dxrN"/>` entries to that module's
`AndroidManifest.xml` — `:src:xrt:ipc:checkSatelliteSlotManifest` fails the build if you forget.
Each extra live satellite costs ~30–60 MB and one GPU context.

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
adb shell dumpsys package org.freedesktop.monado.openxr_runtime.out_of_process \
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

### See-through app (demo-avatar) renders on a BLACK background

The weave is fine; overlay mode is off. Almost always the runtime package lost
`SYSTEM_ALERT_WINDOW` on a reinstall — see *Re-grant the overlay permission*
under Step 5. Check the gate in order:

```bash
# 1. the permission (the usual culprit)
adb shell appops get org.freedesktop.monado.openxr_runtime.out_of_process SYSTEM_ALERT_WINDOW
# 2. the app's opt-in: its manifest must carry
#    <meta-data android:name="com.displayxr.overlay_mode" android:value="true"/>
adb logcat | grep -E "canDrawOverOtherApps|creating service overlay"
# 3. what the compositor actually built
adb logcat | grep -E "SURFACE_FMT|set_transparent_background|alpha-gate"
```

A healthy overlay session logs `canDrawOverOtherApps (overlay mode) = true`,
`connect: overlay mode — creating service overlay`, `transparent=1 overlay=1`,
and `Leia CNSDK DP: alpha-gate pipeline ready`.

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
- [#130](https://github.com/DisplayXR/displayxr-runtime/issues/130) — `XR_DXR_android_surface_binding`
- [#131](https://github.com/DisplayXR/displayxr-runtime/issues/131) — Android CI workflow
- [#133](https://github.com/DisplayXR/displayxr-runtime/issues/133) — Gradle build integration
- [#134](https://github.com/DisplayXR/displayxr-runtime/issues/134) — Android test app (`cube_handle_vk_android`)

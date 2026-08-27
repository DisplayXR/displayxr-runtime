# Installing DisplayXR and the demo apps on Android

The end-to-end path from **published release artifacts** to five running demos on a
Leia-class Android device. No build environment required — just `adb` and the APKs.

Verified end-to-end on an **LPD-20W** phone (1080x2400, Android 13) on 2026-08-26. Every
step below was executed on hardware; the gotchas are the ones that actually occurred, not
anticipated ones.

For building from source instead, see [android-build-guide.md](android-build-guide.md).

---

## What you install

**One runtime APK, plus one APK per demo.** That is the whole list.

The vendor plug-in is **inside the runtime APK** ([ADR-038](../adr/ADR-038-the-android-vendor-plugin-ships-in-the-runtime-apk.md)) — there is no
separate plug-in install step on Android, unlike Windows and Linux. The
`displayxr-leia-cnsdk-*-android-arm64-v8a.tar.gz` asset on the plug-in release exists for CI to
consume; **do not hand it to a user**.

| Component | Asset | Where |
|---|---|---|
| Runtime + Leia plug-in | `DisplayXR-Runtime-Leia-<ver>-android-arm64.apk` | `displayxr-runtime` releases |
| Model Viewer | `DisplayXRModelViewer-<ver>.apk` | `displayxr-demo-modelviewer` |
| Media Player | `DisplayXRMediaPlayer-<ver>.apk` | `displayxr-demo-mediaplayer` |
| Gaussian Splat | `DisplayXRGaussianSplat-<ver>.apk` | `displayxr-demo-gaussiansplat` |
| EarthView | `DisplayXREarthView-<ver>-android.apk` | `displayxr-demo-earthview` |
| Avatar | `DisplayXRAvatar-<ver>.apk` | `displayxr-demo-avatar` |

> **Availability note.** The Android runtime APK is produced by the Android release path added
> in #1212. It appears on releases tagged **after** that path landed — `v2.13.5` and earlier
> carry only the Windows/macOS/Linux assets. Check the release's asset list before starting.

## Download

```bash
gh release download --repo DisplayXR/displayxr-runtime --pattern "*android-arm64.apk"
for r in modelviewer mediaplayer gaussiansplat earthview avatar; do
  gh release download --repo DisplayXR/displayxr-demo-$r --pattern "*.apk"
done
```

## Install

Use the script — **not** bare `adb install`. It encodes two device-state requirements that are
invisible, are dropped by every reinstall, and fail with symptoms that point somewhere else.

```bash
scripts/install-android.sh DisplayXR-Runtime-Leia-<ver>-android-arm64.apk \
    DisplayXRModelViewer-*.apk DisplayXRMediaPlayer-*.apk \
    DisplayXRGaussianSplat-*.apk DisplayXREarthView-*-android.apk \
    DisplayXRAvatar-*.apk
```

`DXR_DEVICE=<serial>` selects a device when more than one is attached. `--uninstall` removes
the runtime; `--force-reinstall` handles installing a released runtime over a dev build.

**Order matters: runtime first, then apps.** The script does this for you.

### If you are replacing dev builds

`--force-reinstall` covers the **runtime** only. A *demo* app installed from a local build has a
different signing key, and its install fails with:

```
INSTALL_FAILED_UPDATE_INCOMPATIBLE: Existing package … signatures do not match newer version
```

Uninstall those packages first — note the Gaussian Splat package is `gausssplat` (three `s`),
which does not match its repo or asset name:

```bash
for p in model_viewer mediaplayer gausssplat earthview avatar; do
  adb uninstall com.displayxr.${p}_vk_android
done
```

## Grant app permissions

The script grants `SYSTEM_ALERT_WINDOW` to the runtime. It does **not** grant the demos'
permissions, so Gaussian Splat and Avatar open on a permission dialog instead of content. Grant
them up front for an unattended setup:

```bash
for p in com.displayxr.gausssplat_vk_android com.displayxr.avatar_vk_android; do
  adb shell pm grant $p android.permission.CAMERA
  adb shell pm grant $p android.permission.POST_NOTIFICATIONS
done
# Avatar's float-over-desktop mode:
adb shell appops set com.displayxr.avatar_vk_android SYSTEM_ALERT_WINDOW allow
```

`CAMERA` is the head-tracking camera. Without it those two demos still launch, but sit behind a
dialog.

## Verify

Open the **DisplayXR** app. Its dashboard runs the same checks as `displayxr-cli selftest`.

- All checks **PASS**.
- The active plug-in is **not** `sim-display` on a vendor display. A failing `vendor_dp` check
  means a vendor plug-in was present but could not be loaded — usually an ABI mismatch between
  the runtime and plug-in versions.

Then launch a demo. From the shell, launch by package rather than by activity name (activity
names differ per demo):

```bash
adb shell monkey -p com.displayxr.model_viewer_vk_android -c android.intent.category.LAUNCHER 1
```

A working weave logs a geometry line per session:

```
HW_GEO: view=1800x810 (aspect 2.222) tiles=2x1 atlas=3600x810 target=1080x2400
```

> **Weaving is not the same as tracking.** With no face found, CNSDK falls back to **NoFaceMode**
> — a fixed sweet-spot weave that is real 3D and looks correct in a screenshot, but does not
> follow you. The check is the validity flag: `HW_FACE: listener=1(...)` is tracked,
> `listener=0(...)` is not, and the coordinates printed after a `0` may be stale and entirely
> plausible-looking.

## What to expect from each demo

| Demo | On first launch |
|---|---|
| Model Viewer | Renders immediately. |
| Media Player | Opens on its splash; needs a media file (Ctrl+O / file picker). |
| Gaussian Splat | Renders after the camera permission is granted. |
| EarthView | **Requires your own Google Map Tiles API key**, prompted on first run. Without it the app runs but shows no globe. |
| Avatar | Renders after camera permission. Floats over other apps when `SYSTEM_ALERT_WINDOW` is allowed — seeing another app behind it is correct. |

## Troubleshooting

**Every app fails with `XR_ERROR_RUNTIME_UNAVAILABLE`.** The runtime is installed but has never
been launched. Uninstalling deregisters the `OpenXRRuntimeBroker` ContentProvider, and Android's
`FLAG_STOPPED` keeps it unresolvable by other packages until the app is opened once. There is no
`BOOT_COMPLETED` receiver to clear it. Open the DisplayXR app, or:

```bash
adb shell am start -n org.freedesktop.monado.openxr_runtime.out_of_process/org.freedesktop.monado.openxr_runtime.DashboardActivity
```

This is the single most common Android failure, and nothing on screen explains it. Note the
broker can appear healthy to `content query` while the app is still in the stopped state.

**An app launches but is black, no error.** The runtime APK is missing the CNSDK Java glue. The
CNSDK loader resolves `com.leia.sdk.internal.Plugin` through the runtime APK's classloader; with
no glue it falls back to `dlopen`ing an impl that in-service CNSDK builds do not ship, and the
plug-in returns `-22` → no display processor → black. Native libraries being present is **not**
evidence the glue is: check for `com.leia.sdk.internal.Plugin` in the APK's `classes*.dex`. A
released APK always carries it; a hand-built one may not.

**A demo dies immediately with `SIGSEGV` in `vulkan.adreno.so`.** Plug-in build-configuration
issue, not an install problem — see
[displayxr-leia-plugin#195](https://github.com/DisplayXR/displayxr-leia-plugin/issues/195).
Released plug-ins from v2.6.1 onward are unaffected.

**Weave looks doubled in portrait but correct in landscape.** Fixed in plug-in **v2.6.1**
([#196](https://github.com/DisplayXR/displayxr-leia-plugin/pull/196)). If you see it, the runtime
APK predates that plug-in.

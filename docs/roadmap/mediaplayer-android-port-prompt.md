# New-session prompt — Android port of the media player

Paste the block below as the first message of a fresh session (the nubia NP02J
must be connected via adb). Context that produced it: the model-viewer Android
port (PR DisplayXR/displayxr-demo-modelviewer#26) established the pattern; the
media player is the next demo leg.

---

## Mission

Port the DisplayXR **stereo media player** to Android as a new `android/` leg in
its **own demo repo** `DisplayXR/displayxr-demo-mediaplayer` (beside the desktop
`src/`), **superseding runtime PR DisplayXR/displayxr-runtime#515** (which wrongly
put it in the runtime's `test_apps/` with an in-process CNSDK harness). When it
works on-device, close #515 with a pointer to the new PR.

It must be **vendor-neutral + out-of-process (ADR-025): NO CNSDK / SR SDK**. It
binds to the installed DisplayXR runtime via the Khronos OpenXR loader broker.

## The proven template — copy it

The model-viewer Android leg is the working structural template. It's cloned at
`~/Documents/GitHub/displayxr-demo-modelviewer-wt` on branch
`android/model-viewer-vk-android` (= PR #26). Mirror its layout exactly:

- **Gradle bootstrap** (the mediaplayer repo, like modelviewer, is CMake-only —
  add it): root `settings.gradle` (`include ':android'`), root `build.gradle`
  (plugins `com.android.application` 8.6.0 + `org.jetbrains.kotlin.android`
  2.0.0; `ext` with `kotlinVersion=2.0.0`, `buildToolsVersion=34.0.0`,
  `cmake_version=3.22.1`, `ndk_version=26.3.11579264`, `sharedCompileSdk=35`,
  `sharedTargetSdk=31`, `sharedMinSdk=29`), `gradle.properties`
  (`android.useAndroidX=true`), the `gradlew`+`gradle/wrapper/` (copy from the
  runtime repo, gradle 8.12), and a gitignored `local.properties`
  (`sdk.dir=/Users/david.fattal/Library/Android/sdk`). Add `local.properties`,
  `.gradle/`, `android/build/`, `android/.cxx/` to `.gitignore`.
- **`android/build.gradle`**: CNSDK-free. `namespace`/`applicationId`
  `com.displayxr.mediaplayer_vk_android`, `prefab true`, abiFilters
  `arm64-v8a`, `ANDROID_STL=c++_shared`, debug `cppFlags '-DXRT_DEBUG_ANDROID_VERBOSE'`,
  `externalNativeBuild.cmake.path "src/main/cpp/CMakeLists.txt"`. The ONLY
  dependency is `implementation 'org.khronos.openxr:openxr_loader_for_android:1.1.41'`.
- **`android/src/main/AndroidManifest.xml`**: the validated OOP `<queries>` block
  — `OpenXRRuntimeService` intent + `<provider android:authorities="org.khronos.openxr.runtime_broker;org.khronos.openxr.system_runtime_broker" tools:replace="android:authorities"/>` (the `tools:replace` + full authority list avoids the loader-AAR manifest-merge conflict) + both runtime packages (`...out_of_process`, `...in_process`) + the `org.freedesktop.monado.ipc.CONNECT` intent. `xmlns:tools`. `WAKE_LOCK` only (no CAMERA — OOP service owns tracking). `configChanges="orientation|keyboardHidden|screenSize"`, `launchMode="singleTask"`, `meta-data android.app.lib_name = mediaplayer_vk_android`.
- **`MainActivity.kt`**: copy modelviewer's — thin `NativeActivity` wrapper that
  resolves the installed runtime (prefer `out_of_process`), `wakeRuntime()`,
  pushes `Surface.rotation` via `nativeSetRotation`, forwards touch via
  `dispatchTouchEvent → nativeOnTouch`. Keep the JNI package
  `com.displayxr.mediaplayer_vk_android` consistent with the `Java_…` symbols in
  `main.cpp`.

## Source material — harvest from suki's #515

Runtime branch `android/mediaplayer-vk-android` (PR #515) has the device-verified
Android pieces. Fetch them via `gh api .../contents/...?ref=android/mediaplayer-vk-android`
under `test_apps/mediaplayer_vk_android/src/main/cpp/`:

- **`video_decoder.cpp/.h`** — AMediaExtractor + AMediaCodec (`libmediandk`) →
  planar YUV → triple buffer → `uploadYUV`. Genuinely Android-only (the desktop
  uses FFmpeg, which doesn't cross-compile). **Keep ~verbatim.**
- **`sbs_renderer.cpp/.h`** — minimal SBS blit (one UV half per eye; left
  offset(0,0) scale(0.5,1), right offset(0.5,0)). `sbs.frag` mode 0 = image,
  mode 1/2 = video YUV→RGB (BT.709) + per-eye downscale.
- **`main.cpp`** — the OpenXR-Android harness + frame loop. (audio_player.cpp /
  AAudio was Stage 3, NOT done in #515 — skip for v1.)
- bundled `assets/test_LR_2x1.png` (SBS test image). Test videos are device-side
  only (SAF / `externalDataPath`) — never bundle/commit clips.

## What to consume from the demo repo (shared, not forked)

The mediaplayer repo's `src/` is a full **desktop** app (ImGui HUD, FFmpeg,
SDL/Window) — NOT reusable wholesale on Android (unlike modelviewer's
`model_common`, which was byte-identical). The genuinely shared asset is the
**SBS shader**: reconcile suki's `sbs.frag`/`fullscreen.vert` against the demo's
`shaders/sbs.frag` + `shaders/fullscreen.vert` and reference those by relative
path from the android CMakeLists (`../../../../shaders/…`), compiling SPIR-V with
`glslangValidator` (same pattern as modelviewer's android CMakeLists). Don't fork
the shader if the demo's copy already matches.

## The rig — likely required, but simpler than the model viewer

On the OOP runtime, the **plain** `xrLocateViews` path returns `got_eyes=0`
(no valid view poses) → black panel; chaining **`XR_EXT_view_rig`**
(`XrDisplayRigEXT`) is what makes the runtime return valid views (this is why
the model viewer was black until the rig was wired — confirmed by the
`VIEW-RIG IPC client: … eyes=2` log). So the media player almost certainly needs
the rig chained too, **just to get valid views to submit a projection layer.**

BUT a flat SBS panel is far simpler than the model viewer: **the stereo lives in
the image (left/right halves), not in the geometry.** The SBS blit is a
fullscreen per-eye blit that does NOT use the view pose/projection for depth. So:
- Chain `XrDisplayRigEXT` minimally — identity-ish pose, a window-filling
  `virtualDisplayHeight`, factors = 1 — purely to make the locate return valid
  views so the projection layer submits.
- **Do NOT inherit the model viewer's vHeight/depth/convergence tuning saga** —
  that was about 3D *geometry* depth budget; flat SBS content has none.
- First, **verify on-device whether the rig is even needed**: does the SBS
  projection layer present with `got_eyes=0`, or does it need valid poses? If
  it presents black without the rig, wire the minimal rig.

Vendor the `XR_EXT_view_rig.h` header into the demo's `openxr_includes/openxr/`
(copy from the runtime's `src/external/openxr_includes/openxr/XR_EXT_view_rig.h`)
if you chain it. NOTE: the runtime's OOP per-rig math has an open
viewing-distance/depth question (deferred) — irrelevant for flat SBS.

## Build / deploy / verify (device specifics learned this session)

- `export JAVA_HOME=/Library/Java/JavaVirtualMachines/temurin-21.jdk/Contents/Home`
  then `cd <mediaplayer-clone> && ./gradlew :android:assembleDebug --no-daemon`.
  Gradle's `$?` can be masked by a trailing echo — capture it explicitly.
- `adb install -r -d android/build/outputs/apk/debug/android-debug.apk`.
- **Start the OOP runtime via its LAUNCHER, not `start-service`** (start-service
  alone does NOT reliably bring it up on this device):
  `adb shell monkey -p org.freedesktop.monado.openxr_runtime.out_of_process -c android.intent.category.LAUNCHER 1`, settle ~3 s, confirm
  `adb shell pidof org.freedesktop.monado.openxr_runtime.out_of_process` is non-empty.
- Launch the app: `adb shell monkey -p com.displayxr.mediaplayer_vk_android -c android.intent.category.LAUNCHER 1`.
- **Verification is by your eyes on the panel** — you CANNOT screencap the VK
  weave overlay. Read logcat for bring-up (`xrCreateSession`, session state →5,
  `VIEW-RIG IPC … eyes=2` if rig used, decoder frames, fps) and **ask the user to
  confirm the panel** (image weaves? video plays + weaves? in focus?).
- The OOP runtime/lifecycle is **flaky** — service can die, activities stop,
  results vary across rapid relaunches. Launch service fresh, settle, then test;
  don't over-thrash. Filter app logs by pid: `adb logcat -d | awk '$3==PID'`.

## Scope (mirror #515)

Stage 1 (image, `test_LR_2x1.png`) + Stage 2 (video, AMediaCodec, default
device-side clip + SAF picker). Audio (AAudio) + playback controls are Stage 3 —
out of scope for v1. CI compiles desktop only (no Android Gradle in CI), so the
leg isn't CI-exercised — verify on-device.

Read first: `docs/roadmap/android-demo-ports-plan.md` (this repo) for the full
decision record and the modelviewer precedent.

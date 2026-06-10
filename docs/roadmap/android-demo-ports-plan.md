# Android demo ports — modelviewer / mediaplayer / gauss

**Status:** in progress (modelviewer leg first). **Owner:** runtime hub. **Date:** 2026-06-10.

Tracking doc for porting the three Vulkan demos to Android. Driven from the
`displayxr-runtime` hub but the work lands in the **demo repos**, not here.

## TL;DR decision

`leaiss`/suki opened runtime PRs **#511** (`model_viewer_vk_android`) and **#515**
(`mediaplayer_vk_android`, stacked on #511) that drop full Android apps into the
runtime's `test_apps/`. A gauss equivalent lives on the runtime branch
`android/gausssplat-android-port` (never PR'd).

**These do not belong in `displayxr-runtime`.** Demos live in their own repos
(`displayxr-demo-{modelviewer,mediaplayer,gaussiansplat}`), structured `windows/`
+ `macos/` + shared `*_common/`. The Android work becomes an **`android/` leg**
in each demo repo, consuming the shared renderer. **Close #511/#515**; the gauss
branch never becomes a runtime PR.

The move is a **harvest, not a re-port** — suki's Android-specific work (harness,
JNI touch, AMediaCodec, the Adreno sort) is device-verified and kept; only the
*placement* and the *forked renderer copies* are wrong.

## Evidence (diffs, 2026-06-10)

| Demo | suki's android renderer vs shared `*_common/` | Implication |
|---|---|---|
| **modelviewer** | **byte-identical** (`model_renderer.cpp` 1701 lines, 0 diff; all 9 shaders 0 diff). Only `model_loader.cpp` differs (12 lines = gltf-only gating). | Reseat on `model_common/`; gate OBJ/STL/FBX/USD behind `#if !defined(__ANDROID__)`. No fork. |
| **gauss** | **materially diverged** (`gs_renderer.cpp` 153 lines; `sort/preprocess/hist/tile_boundary/render` shaders ~100 lines). | Mandatory Adreno port: Qualcomm has **no `shaderInt64`** → radix sort rewritten 64-bit → **32-bit packed keys** (`tile<<16 \| depth16`, 4 passes not 8, graceful `VK_NULL_HANDLE` fallback). Desktop 64-bit path *cannot run* on Adreno. |
| **mediaplayer** | n/a (decode is platform-specific) | `AMediaCodec` video decode is genuinely Android-only (desktop uses FFmpeg, doesn't cross-compile). Keep verbatim. |

**All three** copied the desktop Kooima view-math files (`display3d_view.c` /
`camera3d_view.c` / `view_params.h`) but left them **dead/unwired** — the Android
`main.cpp` consumes raw `xrLocateViews` `views[i].fov`. They neither chain
`XR_EXT_view_rig` (the `cube_handle_vk_*` runtime-side-Kooima path) nor run the
desktop app-side `display3d_view`. Per runtime#510 `15ee44e94`, the **OOP server
computes server-side Kooima** and returns it via `views[i].fov` regardless — so
the apps already render correctly; the rig is a *display-centric framing
enhancement*, not a correctness blocker.

## Architecture decisions

- **Out-of-process only (ADR-025).** The Android runtime + vendor DP run
  out-of-process; in-process is dev-only. A vendor-neutral demo therefore must be
  **CNSDK-free** and bind to the installed OOP runtime
  (`org.freedesktop.monado.openxr_runtime.out_of_process`).
- **OOP discovery is in the manifest `<queries>`** (broker provider authority +
  both runtime packages + `ipc CONNECT`), added in runtime `6c0f0941b`, validated
  on nubia NP02J. suki's branch predates this (in_process-only → black screen on
  OOP); use the **current cube manifest** as the template.
- **OpenXR loader** comes from the Khronos AAR
  (`org.khronos.openxr:openxr_loader_for_android`) via prefab →
  `find_package(OpenXR CONFIG)`. No runtime source tree needed in the demo repo.
- **Shared renderer by relative path, not copied.** The `android/` leg's
  `src/main/cpp/CMakeLists.txt` lists `../../../../<demo>_common/*.cpp` directly,
  so there is one renderer source, built by all platform legs.

## Gauss 32-bit decision (LOCKED)

**Unify `3dgs_common/` on 32-bit packed keys (Option A)** — one code path for all
platforms. Removes the `shaderInt64` dependency everywhere (a *Windows GPU-compat
upside* — runs on parts with weak/absent int64). **No runtime impact.**

- **Deferred gate:** validate **16-bit packed depth precision** on the *Windows*
  desktop demo when we get there. If it doesn't hold at desktop scene scale, fall
  back to **Option B** (compile-time 64-bit desktop / 32-bit Android via shader
  `#define` + CMake).
- **Issue #365 ("Support 32-bit win32 OpenXR runtime") is UNRELATED.** That is
  x86 *process architecture* for the runtime DLL so x86 client apps find a
  runtime. Gauss "32-bit" is compute-shader *integer key width*; the demo stays
  an x64 process and the runtime never sees the sort keys (only the composited
  image). #365 stays Low/future.

## Per-demo plan

> **On-device status (2026-06-10, nubia NP02J):** the `android/` leg is built and
> committed (`displayxr-demo-modelviewer` branch `android/model-viewer-vk-android`,
> not yet pushed). Verified on-device: shared `model_common` ModelRenderer compiles
> for the NDK, APK installs, creates an **OOP service-mode session**, the Leia DP
> comes up out-of-process (LeiaSR 3D mode active), the renderer initializes at
> 1920×1200/eye, loads `sample.glb`, and runs the frame loop to session state 5 —
> **behaviorally identical to the reference `cube_handle_vk_android`**. Presentation
> is blocked: `oxr_session_locate_views` returns `got_eyes=0 have_view_state=0`
> (no valid view poses) — the in-flight OOP server-side view-pose path (runtime#510,
> commits `15ee44e94` / `b342f63a4`), which **gates the reference cube identically**.
> Not a port issue; unblocks when that runtime path lands. (Note: `start-service`
> alone does NOT bring the OOP service up on this device — launch the runtime's
> launcher activity once.)

### 1. modelviewer → `displayxr-demo-modelviewer` (in progress)
- `android/` leg = harness only (Gradle bootstrap, `NativeActivity`,
  `MainActivity.kt`, `main.cpp`, JNI touch). CMake → shared `model_common/`.
- Gate OBJ/STL/FBX/USD in `model_common/model_loader.cpp` (`#if !defined(__ANDROID__)`).
- v1 ships on OOP server-side Kooima (suki's path, 60–76 fps on device).
- **Follow-up:** wire `XR_EXT_view_rig` (header vendored into `openxr_includes/`)
  for display-centric framing parity with desktop; delete dead `display3d_view`.
- Bundled model: the demo's existing `sample.glb` (multi-model + license
  attribution later).

### 2. mediaplayer → `displayxr-demo-mediaplayer`
- `android/` harness + keep `video_decoder.cpp` / `audio_player.cpp` (AMediaCodec)
  verbatim. Reconcile SBS shader against the shared `sbs.frag` / `fullscreen.vert`.
- Confirm framing intent: a flat SBS panel likely wants a window-filling/default
  rig, not an orbit rig.

### 3. gauss → `displayxr-demo-gaussiansplat`
- Land the Adreno 32-bit sort into `3dgs_common/` (Option A unified), `android/`
  harness over it. Carries the deferred depth-precision check.

**Sequencing:** modelviewer → mediaplayer → gauss (gauss last; only one mutating
shared `*_common/` + carrying the precision gate).

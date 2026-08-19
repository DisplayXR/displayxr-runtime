# weave_client_vk_android — the reference `XR_DXR_weave` present-owner on Android

What `displayxr-browser`'s GPU process will do, minus Chromium (#1036, epic #1031 /
ADR-036 D3). The app owns its `SurfaceView` and its own Vulkan swapchain, renders
pre-weave side-by-side stereo into an `AHardwareBuffer`, hands it plus its
window-relative rects to the runtime, gets a woven `AHardwareBuffer` back, blits
that into its own swapchain and presents. It never weaves (ADR-007 / ADR-019).

It is deliberately shader-free — the pre-weave content is painted by the CPU and
everything after that is copies and blits — so the file is about the weave
contract rather than about a renderer.

## What you should see

| Element | Left view | Right view |
|---|---|---|
| Field | red | blue |
| White bar | 30% of the rect width | 60% |
| Rect border | green, 2 px | green, 2 px |
| Liveness block | yellow, walks left→right once a second | same |

Two horizontal rect bands, with a **not woven** gap between them (the caller's own
pixels show through) — that gap is the `firstChunk` transparent-clear working.

## Build + run

```bash
./gradlew :test_apps:weave_client_vk_android:assembleDebug
adb install -r -d test_apps/weave/weave_client_vk_android/build/outputs/apk/debug/weave_client_vk_android-debug.apk
adb shell am start -n com.displayxr.weave_client_vk_android/.MainActivity
adb logcat -s weave_client_vk_android:V
```

Needs the **out-of-process** runtime flavor installed — the weave service lives in
the service compositor, and an in-process session reports
`XR_ERROR_FEATURE_UNSUPPORTED` by design.

## Verifying the weave without a display capture

`adb screencap` returns black for this app's layer on the NP02J, so the panel
cannot answer "is it woven?" from the host. Read the buffers back instead:

```bash
adb shell setprop debug.dxr.weave.dump 1     # dumps once, then clears itself
adb pull /sdcard/Android/data/com.displayxr.weave_client_vk_android/files/weave_out.ppm
```

`weave_in.ppm` is what the app handed the runtime, `weave_out.ppm` is what came
back. A woven row alternates per pixel between the two views' colours; a row that
is uniformly red is the DP's **no-face 2D fallback**, not a bug — CNSDK
`NoFaceMode` drops to a single view when the tracker sees nobody. Force the
light-field on to see the lattice:

```bash
adb shell setprop debug.dxr.overlay 1        # vendor force-3D; remember to clear it
```

## Debug knobs

| Property | Effect |
|---|---|
| `debug.dxr.weave.passthrough 1` | blit the app's own pre-weave input instead of the woven output — the A/B that separates "my present path is broken" from "the weave is broken" |
| `debug.dxr.weave.dump 1` | one-shot PPM dump of the input + the woven output |

## Known simplifications

- One submit per frame with two rects; the v6 `XrWeaveSubmitLayoutDXR` N-view
  atlas path is implemented in the runtime but not exercised here.
- No overlay atlas (v4) and no eye-driven off-axis re-render — the returned eye
  positions are logged, not consumed. A real present-owner re-renders its next
  pre-weave pair from them.

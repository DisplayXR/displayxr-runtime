# DisplayXR WebXR Bridge — Developer Guide

This is the start-here guide for building a **bridge-aware WebXR app** — a JS app that gets full DisplayXR features (display info, rendering modes, tracked eyes, window metadata, HUD, input forwarding) while still running as a normal WebXR site.

For the wire-level message schema see [`PROTOCOL.md`](PROTOCOL.md). For setup (build, install, launch) see [`README.md`](README.md). For a runnable copy-paste starter see [`examples/minimal.html`](examples/minimal.html). For the canonical full-featured reference see [`sample/sample.js`](sample/sample.js).

---

## What the bridge gives you

Plain WebXR doesn't know about 3D displays. Chrome's WebXR exposes generic stereo with compromise-scaled framebuffers and no concept of:

- **Display geometry** in physical units (meters) — needed for Kooima off-axis projection.
- **Rendering modes** — 2D, side-by-side stereo, lenticular layouts. Vendor-specific.
- **Eye tracking poses** in display-local coords — needed for parallax.
- **Window metadata** — size, position on display, view-tile dims. Needed for window-relative Kooima.
- **Hardware-state events** — display-mode changes (3D ↔ 2D backlight toggles).
- **App-initiated mode changes** — your app asking for stereo SBS rather than 2D.
- **HUD overlay** — debug text painted by the compositor on top of your render.
- **Input forwarding** — keyboard/mouse events delivered to your app even when the compositor window has focus.

The bridge fills all those gaps via a Chrome extension + a WebSocket-based companion process. Your page acquires `session.displayXR` on top of a normal `XRSession`.

```js
const session = await navigator.xr.requestSession('immersive-vr', {
  optionalFeatures: ['local'],
});
const displayXR = session.displayXR;            // present iff bridge + extension are installed
if (displayXR) {
  // bridge-aware path (full features)
} else {
  // standard WebXR fallback (Chrome's compromise-scaled stereo)
}
```

Browsers without the extension installed still work — `session.displayXR` is `undefined`, you take the standard-WebXR fallback. Plan for both.

## Prerequisites

Setup steps (build, install, run) are in [`README.md`](README.md). After install you should have:

- `displayxr-service.exe` running.
- `displayxr-webxr-bridge.exe` running (WebSocket on `127.0.0.1:9014`).
- Chrome extension loaded (path: `webxr-bridge/extension`).
- Page served over HTTP (e.g. `python -m http.server 8080` from this directory).

Status indicators in the sample (`webxr-bridge/sample/index.html`) give you live feedback on each.

## The `session.displayXR` surface

Returned by the extension's main-world shim ([`extension/src/main-world.js`](extension/src/main-world.js)). Every property is read-only and re-fetched on each `session.displayXR` access — so reading per-frame is fine.

The **first** access of `session.displayXR` signals the extension that the page wants the bridge. In `bridge=Auto` mode the service spawns `displayxr-webxr-bridge.exe` on demand — legacy WebXR pages (which never touch this getter) never cause a spawn. Because the bridge handshake is asynchronous, the returned surface is initially "pending": `ready` is a Promise, every other data field is `null` until it resolves.

Apps MUST `await session.displayXR.ready` before reading `displayInfo`, `renderingMode`, `windowInfo`, `eyePoses`, etc. Warm-case (bridge already running) the Promise resolves in ~10 ms; cold-case (bridge needs to spawn) it takes ~500 ms. The Promise rejects after 3 s — treat the rejection as "bridge unavailable, fall back to legacy WebXR."

| Field | Type | Meaning |
|---|---|---|
| `ready` | `Promise<void>` | Resolves when `displayInfo` is populated. Rejects on 3 s timeout. Await before reading any other field. |
| `displayInfo.displayPixelSize` | `[w, h]` | Physical display resolution. |
| `displayInfo.displaySizeMeters` | `[w, h]` | Physical display size. Drives Kooima. |
| `displayInfo.nominalViewerPosition` | `[x, y, z]` | Default seated viewer position (m). |
| `renderingMode` | object | Current mode (see below). |
| `renderingModes` | array | All available modes — for menus / capability discovery. |
| `windowInfo` | object \| null | Live window metadata (see below). |
| `eyePoses` | array | Streaming tracked eyes (call `configureEyePoses('raw')` first). |
| `eyeTracking.supportedModes` | `string[]` | Modes the DP advertises (subset of `['MANAGED', 'MANUAL']`). |
| `eyeTracking.defaultMode` | `'MANAGED'` \| `'MANUAL'` | Mode active if the app doesn't request one. |

### Session bootstrap

```js
const session = await navigator.xr.requestSession('immersive-vr', { optionalFeatures: ['local'] });

// First access triggers bridge-attach + (in Auto mode) spawn-on-demand.
// The returned surface is a *pending* snapshot — fields like displayInfo
// are null until ready resolves. Re-read the getter after the await to
// get a populated snapshot.
let displayXR = session.displayXR;
let useBridge = false;
if (displayXR && displayXR.ready) {
  try {
    await displayXR.ready;
    displayXR = session.displayXR;  // re-read for populated surface
    useBridge = true;
  } catch (e) {
    console.log('bridge not available:', e.message);
  }
}

if (useBridge) {
  // displayXR.displayInfo, .renderingMode, etc. are now populated
  displayXR.configureEyePoses('raw');
  // ... Kooima setup, HUD, etc.
} else {
  // Fall back to standard WebXR — XRView.projectionMatrix + XRView.transform.
}
```

### Rendering mode

Each mode object carries:

| Field | Meaning |
|---|---|
| `index` | Mode index (pass to `requestRenderingMode`). |
| `name` | Vendor name, e.g. `"LeiaSR"`, `"2D"`. |
| `tileColumns × tileRows` | Per-frame tile layout (e.g. `2×1` SBS). |
| `viewScale` | `[sx, sy]` — per-tile scale relative to display. |
| `viewCount` | Eyes per frame (1 mono, 2 stereo, etc.). |
| `hardware3D` | Whether the display is in 3D-emit mode for this mode. |

```js
// List modes (e.g. for a UI dropdown):
for (const m of displayXR.renderingModes) {
  console.log(`${m.index}: ${m.name} ${m.tileColumns}×${m.tileRows} hw3D=${m.hardware3D}`);
}
// Switch mode:
displayXR.requestRenderingMode(1);
```

### Window info

`displayXR.windowInfo` updates **live** when the compositor window moves or resizes. `null` until the bridge locates the window.

| Field | Meaning |
|---|---|
| `valid` | `true` once the bridge has the window. |
| `windowPixelSize` | `[w, h]` of the client area. |
| `windowSizeMeters` | `[w, h]` in physical meters. |
| `windowCenterOffsetMeters` | `[x, y]` from display center. + = right / up. |
| `viewWidth, viewHeight` | Per-tile pixel dims, bridge-computed = `windowPixelSize × viewScale`. **Use these for your viewport** — they match exactly what the compositor will crop. |

Subscribe to `windowinfochange` for live updates:

```js
session.addEventListener('windowinfochange', e => {
  const w = e.detail.windowInfo;
  // recompute Kooima frustum on next frame
});
```

### Eye poses

Bridge forwards **raw display-local DP-tracked eye positions** — origin = display center, X+ right, Y+ up, Z+ toward viewer. **No client-side calibration needed**. Pre-Phase-5 the sample subtracted an "originOffset" from incoming eyes; that's gone, and the runtime now returns display-local coords for bridge-relay sessions (handled in [`oxr_session.c`](../src/xrt/state_trackers/oxr/oxr_session.c) via `is_bridge_relay`).

```js
displayXR.configureEyePoses('raw');         // enable streaming (call once)
// in your frame loop:
const eyes = displayXR.eyePoses;             // [{ position: [x,y,z], orientation: [...], fov: {...} }, ...]
```

If `eyes.length === 0` (no tracker yet), use `displayInfo.nominalViewerPosition` as a default.

**Mode capability gating.** Don't assume both `MANAGED` and `MANUAL` are available — some DPs expose only one:

```js
const canToggle =
  displayXR.eyeTracking.supportedModes.includes('MANAGED') &&
  displayXR.eyeTracking.supportedModes.includes('MANUAL');

if (canToggle) displayXR.requestEyeTrackingMode(wantManual ? 1 : 0);
// Leia today reports ['MANAGED'] only — hide the toggle in your UI.
```

The bridge rejects unsupported mode requests with a log-only warning; this guard exists so your UI matches the device's actual state.

### HUD

Compositor-side overlay. Send up to 8 lines, label + text:

```js
displayXR.sendHudUpdate(true, [
  { label: 'mode',  text: displayXR.renderingMode.name },
  { label: 'eyes',  text: `${displayXR.eyePoses.length} tracked` },
  { label: 'fps',   text: '60' },
]);
```

To clear the HUD, call once with `visible=false` (lines ignored).

### Input forwarding

When the compositor window has focus, the bridge captures keyboard and mouse via low-level Win32 hooks and forwards them as `displayxrinput` session events:

```js
session.addEventListener('displayxrinput', e => {
  const m = e.detail;
  if (m.kind === 'key' && m.down && m.code === 32 /* SPACE */) resetView();
  if (m.kind === 'mouse' && m.event === 'move') /* ... */;
  if (m.kind === 'wheel') /* m.deltaY in WHEEL_DELTA units */;
});
```

The bridge auto-suppresses mouse events while the compositor window is in a modal size/move loop (it watches the compositor's `DXR_InSizeMove` HWND property). You don't need to filter title-bar drags out yourself.

## Events to subscribe to

| Event | Fires when | Typical handler |
|---|---|---|
| `renderingmodechange` | Mode switched (via your request OR vendor SDK). | Refresh tile layout; update UI selector. |
| `windowinfochange` | Window moved / resized. | Recompute Kooima screen dims + center offset. |
| `hardwarestatechange` | Display backlight toggled 3D ↔ 2D. | **Debounce** ≥ 600 ms before reacting (see Pitfalls). |
| `bridgestatus` | Bridge process connected / disconnected. | Update status UI. |
| `displayxrinput` | Key / mouse / wheel from compositor focus. | App input. |

## Kooima projection — primer

The bridge gives you the *ingredients* (eye position + screen geometry); you assemble the matrix. Kooima projection is an asymmetric off-axis perspective frustum where the near plane is the physical screen, sheared so the eye's actual position relative to the screen produces a geometrically correct view of geometry behind/in-front-of the screen plane.

The math is in [`test_apps/common/display3d_view.c`](../test_apps/common/display3d_view.c) and [`.h`](../test_apps/common/display3d_view.h) (canonical implementation). The sample has a JS port in [`sample/sample.js`](sample/sample.js) (`buildKooimaProjection`); the minimal example has a stripped-down version inline (`makeKooimaProjection`).

For **window-relative Kooima** (when the app window isn't fullscreen):

1. Use `windowInfo.windowSizeMeters` as the screen dimensions (not `displaySizeMeters`).
2. Subtract `windowInfo.windowCenterOffsetMeters` from each eye's `[x, y]` so the eye is window-centric.
3. Camera world position = `eye_window × es + rig.pos` (matches `display3d_view.c::display3d_compute_view`).

The cube stays centered in the window as the user drags it — same convention as the native reference app `cube_handle_d3d11_win`.

## Frame loop pattern

Assumes you've already run the *Session bootstrap* above and cached a local `displayXR` that is `null` on the fallback path and non-null on the bridge-aware path. Don't re-read `session.displayXR` on every frame inside the fallback branch — the getter always returns a pending surface when the extension is installed, which would defeat the `if (!displayXR)` guard.

```js
function onFrame(time, frame) {
  session.requestAnimationFrame(onFrame);
  if (!displayXR) { renderFallback(time, frame); return; }

  const wi = displayXR.windowInfo;
  const di = displayXR.displayInfo;
  const layout = displayXR.renderingMode;          // tileColumns/tileRows
  const eyes = displayXR.eyePoses;

  // Per-tile pixel dims — prefer bridge-pushed viewWidth/Height.
  const tileW = (wi && wi.viewWidth)  || Math.floor(layer.framebufferWidth  / layout.tileColumns);
  const tileH = (wi && wi.viewHeight) || Math.floor(layer.framebufferHeight / layout.tileRows);
  const screenWm = (wi && wi.valid) ? wi.windowSizeMeters[0] : di.displaySizeMeters[0];
  const screenHm = (wi && wi.valid) ? wi.windowSizeMeters[1] : di.displaySizeMeters[1];
  const winOff   = (wi && wi.valid) ? wi.windowCenterOffsetMeters : [0, 0];

  for (let i = 0; i < layout.tileColumns * layout.tileRows; i++) {
    const tx = i % layout.tileColumns, ty = Math.floor(i / layout.tileColumns);
    const vpY = layer.framebufferHeight - tileH - ty * tileH;  // GL→D3D Y-flip
    gl.viewport(tx * tileW, vpY, tileW, tileH);

    const e = eyes[i] ? eyes[i].position : di.nominalViewerPosition;
    const eyeWin = [e[0] - winOff[0], e[1] - winOff[1], e[2]];
    setProjection(makeKooimaProjection(eyeWin, screenWm, screenHm));
    setView(eyeWin);
    drawScene();
  }
}
```

The Y-flip on the viewport is needed because GL's viewport origin is bottom-left; ANGLE forwards to D3D11 which is top-left; the compositor reads from D3D11 row 0. If your renderer is native D3D, skip the flip.

## Cross-process protocol summary

The bridge talks to the compositor via Win32 HWND properties (named atoms). You don't usually need to know about these, but they're useful for debugging weird behavior:

| Property | Set by | Read by | Meaning |
|---|---|---|---|
| `DXR_BridgeViewW` / `DXR_BridgeViewH` | bridge | compositor | Per-tile content dims (= `windowPx × viewScale`). |
| `DXR_RequestMode` | bridge | compositor | App-requested rendering-mode index (encoded `index + 1`). |
| `DXR_InSizeMove` | compositor window | bridge | Set during `WM_ENTERSIZEMOVE` / cleared `WM_EXITSIZEMOVE`. Bridge gates input forwarding on this. |

See [`docs/roadmap/webxr-bridge-v2-phase5-status.md`](../docs/roadmap/webxr-bridge-v2-phase5-status.md) for the architectural rationale.

## Common pitfalls

- **Don't calibrate eye positions.** The bridge already returns raw display-local coords (origin at display center). The sample used to subtract an "originOffset" pre-Phase-5; that was masking a runtime bug now fixed.
- **Debounce `hardwarestatechange`.** The Leia driver flaps `3D → 2D → 3D` during fullscreen ↔ windowed transitions. Acting on each event triggers real mode changes that drop eye tracking for several seconds. Wait ≥ 600 ms; if state flaps back to current within that window, cancel the pending request.
- **Recompute Kooima per frame.** `windowInfo` updates live during window drag. Don't cache `screenWm/screenHm/winOff` between frames.
- **`renderingMode.index` is async.** When you call `requestRenderingMode(1)`, the `renderingMode.index` doesn't change until the next `renderingmodechange` event fires. Track your own `lastRequestedMode` if you need the value immediately.
- **HUD writes are best-effort.** They go through shared memory with no acknowledgement. Don't assume an update appeared on the compositor window before you continue.
- **`session.displayXR` exists immediately but fields are null until `ready` resolves.** The getter always returns a surface when the extension is installed. Fields like `displayInfo` and `renderingMode` are `null` until the bridge handshake completes — `await session.displayXR.ready` first (see *Session bootstrap* above). When the extension isn't installed, the getter is absent and `session.displayXR` is `undefined` — always have a standard-WebXR fallback path.
- **Don't spin on `session.displayXR` waiting for non-null fields.** The Promise is the single source of truth for readiness. If it rejects (3 s timeout in cold-start case where the service isn't running), treat that as "run legacy" and move on — don't retry synchronously.

## Reference map

| File | Purpose |
|---|---|
| [`README.md`](README.md) | Setup: build, install, launch the bridge. |
| [`PROTOCOL.md`](PROTOCOL.md) | Wire-level WebSocket message schema (v1). |
| [`examples/minimal.html`](examples/minimal.html) | ~250-line single-file copy-paste starter. No three.js. |
| [`sample/sample.js`](sample/sample.js) | Full-featured three.js reference with all features wired (mode UI, HUD, camera-centric Kooima, fallback). |
| [`extension/src/main-world.js`](extension/src/main-world.js) | The shim that exposes `session.displayXR`. |
| [`../test_apps/common/display3d_view.c`](../test_apps/common/display3d_view.c) | Canonical Kooima math (C). The JS ports mirror this. |
| [`../test_apps/cube_handle_d3d11_win/main.cpp`](../test_apps/cube_handle_d3d11_win/main.cpp) | Native equivalent of the WebXR sample. Same conventions. |
| [`../docs/specs/XR_EXT_display_info.md`](../docs/specs/XR_EXT_display_info.md) | Underlying OpenXR extension that the bridge surfaces over WS. |
| [`../docs/roadmap/webxr-bridge-v2-plan.md`](../docs/roadmap/webxr-bridge-v2-plan.md) | Architecture rationale: why the bridge exists. |

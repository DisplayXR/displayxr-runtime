# Agent prompt: WebXR Bridge v2 — Kooima 3D sample (#139)

Use this prompt to start a fresh Claude Code session on a Windows machine with a Leia SR display attached, to implement the proper 3D sample for WebXR Bridge v2 on branch `feature/webxr-bridge-v2`.

---

## Prompt

```
I'm implementing the proper 3D rendering sample for WebXR Bridge v2 (#139). Phase 2 shipped — the bridge host, MV3 extension, and bridge-relay compositor are all working end-to-end. The current sample at `webxr-bridge/sample/` draws colored rectangles with raw GL for mode verification. This task replaces it with a real 3D scene using Kooima display-centric projection from bridge-delivered RAW eye poses — matching how `cube_handle_d3d11_win` renders.

## Current state of the branch

You are on `feature/webxr-bridge-v2`. Relevant recent commits:

- `caaab165c` — WebXR Bridge v2 Phase 2: WS metadata server, MV3 extension, bridge-relay compositor (#139). Full Phase 2 implementation.
- `860244357` — Fix: restore ESC → close window for non-shell compositor mode (#139)
- `5c411cfdb` — Fix RENDERING_MODE_CHANGED fan-out to headless sessions (#142) (#139)
- `7c5f60ec3` — Scaffold WebXR Bridge v2 host (Phase 1) (#139)

Do NOT rebase off `main`. Commit messages must include `(#139)`.

## What already works

- **Bridge host** (`src/xrt/targets/webxr_bridge/main.cpp`): WebSocket on 127.0.0.1:9014, streams `display-info`, `mode-changed`, `hardware-state-changed`, `eye-poses` (RAW ~100Hz). Accepts `request-mode`, `configure` from client.
- **MV3 extension** (`webxr-bridge/extension/`): ISOLATED world WS relay + MAIN world `session.displayXR` surface with `displayInfo`, `renderingMode`, `eyePoses`, `computeFramebufferSize()`, `configureEyePoses()`, `requestRenderingMode()`. Dispatches `renderingmodechange` + `hardwarestatechange` events on the session.
- **Bridge-relay compositor**: auto-detects bridge presence via `is_bridge_relay` on `xrt_session_info`. When active, overrides Chrome's legacy-compromise `subImage.imageRect` with mode-native tile rects in the D3D11 service blit loop. No window created for headless bridge session.
- **Eye poses**: bridge polls `xrLocateViews` at ~100Hz, sends RAW positions + orientations + asymmetric FOV angles. With backpressure to prevent WS buffer overflow.
- **Mode switching**: V-key in compositor → `RENDERING_MODE_CHANGED` event → bridge → WS → extension → page's `renderingmodechange` event. Verified end-to-end.

## What this task delivers

Replace `webxr-bridge/sample/sample.js` (and update `index.html` if needed) with a proper 3D WebXR app that mirrors `cube_handle_d3d11_win`'s rendering approach:

### Rendering model

1. **Ignore Chrome's view poses and projection matrices.** Chrome's `view.transform` and `view.projectionMatrix` come from the legacy compromise path and have wrong aspect ratios for the active mode. The sample must build its own camera from bridge data.

2. **Use bridge-delivered RAW eye poses for Kooima projection.** Each frame, read `session.displayXR.eyePoses` (array of `{position, orientation, fov}`). Use the asymmetric FOV angles directly to build per-eye projection matrices — these are already Kooima-computed by the runtime.

3. **Per-mode tile layout.** Use `session.displayXR.renderingMode` to compute tile rects:
   - `tileW = displayPixelSize[0] * viewScale[0]`
   - `tileH = displayPixelSize[1] * viewScale[1]`
   - For each eye: `gl.viewport(tileX * tileW, tileY * tileH, tileW, tileH)` and `gl.scissor(...)`.
   - 2D mode (1×1, mono): 1 eye, full display.
   - 3D mode (2×1, stereo): 2 eyes, half-width tiles side by side.

4. **Display-centric projection (default).** Like `cube_handle_d3d11_win`'s display-centric mode:
   - The display is the "screen" in Kooima's model.
   - Eye positions from the bridge are physical eyes relative to the display center.
   - The projection matrix is an off-axis frustum that maps the 3D scene onto the physical screen plane.
   - The bridge already computes the Kooima projection and delivers it as asymmetric FOV angles — the sample just needs to build a `makePerspective(left, right, top, bottom, near, far)` from `fov.angleLeft/Right/Up/Down`.

5. **On `renderingmodechange`:** update tile layout (tileColumns, tileRows, viewScale), recompute viewports. No layer rebuild — just change what's drawn. The compositor's bridge-aware path reads mode-native tile rects.

### Scene

Match `cube_handle_d3d11_win`'s visual style:
- Colored cubes at different Z depths (for parallax / depth perception on the 3D display)
- Grid floor
- Ambient + directional lighting
- Cubes rotate slowly (animated)
- Use three.js from CDN via importmap (`three@0.160.0` from unpkg)

### Controls (from bridge, not Chrome)

For Phase 2: no input forwarding yet (that's Phase 3). The sample is view-only:
- Eye tracking moves the viewpoint (via bridge RAW eye poses)
- V-key in compositor switches modes (via bridge `renderingmodechange`)
- No WASD, no mouse look (Phase 3)

### What NOT to touch

- Do NOT modify the bridge host (`main.cpp`), extension (`main-world.js`, `isolated-world.js`), or any runtime/compositor code. This task is sample-only.
- Do NOT use Chrome's `view.transform` or `view.projectionMatrix` for rendering. Those are legacy-compromise values.
- Do NOT rebuild `XRWebGLLayer` on mode change. The framebuffer stays the same; only the viewports change.
- Do NOT use three.js's built-in WebXR support (`renderer.xr.enabled`). We manage the XR frame loop and GL state ourselves.

## Context to read in order

1. `CLAUDE.md` — project overview, build commands, test procedures.
2. `webxr-bridge/PROTOCOL.md` — JSON protocol v1 spec. Pay attention to `eye-poses` message format (position, orientation, fov).
3. `webxr-bridge/extension/src/main-world.js` — the `session.displayXR` surface API. Read how `displayInfo`, `renderingMode`, `eyePoses` are structured.
4. `webxr-bridge/sample/sample.js` — current placeholder sample (colored rectangles). You'll replace this.
5. `test_apps/cube_handle_d3d11_win/main.cpp` lines 300-670 — the rendering loop. Focus on:
   - Lines 302-312: mode info lookup (tileColumns, tileRows, viewScaleX/Y, monoMode)
   - Lines 337-338: maxTileW/H from swapchain
   - Lines 342-433: Kooima projection computation (display-centric mode at line 421-432)
   - Lines 596-653: per-eye viewport setup, render, and subImage.imageRect submission
6. `src/xrt/auxiliary/math/m_display3d_view.c` — `display3d_compute_views()` function. This is what the runtime calls for display-centric Kooima. Understanding it helps but the sample doesn't call it directly — the bridge already delivers the pre-computed FOV.
7. `docs/architecture/kooima-projection.md` — background on Kooima asymmetric frustum projection.
8. `docs/specs/legacy-app-support.md` — explains the legacy compromise and how bridge-aware rendering escapes it.

## Key insight: FOV from bridge = Kooima projection

The bridge streams `eye-poses` with per-eye `fov` (angleLeft/Right/Up/Down in radians). These are the Kooima-computed asymmetric frustum angles from `xrLocateViews` in RAW mode. The sample converts them directly to a projection matrix:

```js
const near = 0.01, far = 100.0;
const l = Math.tan(fov.angleLeft) * near;
const r = Math.tan(fov.angleRight) * near;
const t = Math.tan(fov.angleUp) * near;
const b = Math.tan(fov.angleDown) * near;
projMatrix.makePerspective(l, r, t, b, near, far);
```

And uses the eye's `position` + `orientation` to build the view matrix (camera at eye position, looking along the orientation). This matches exactly what `cube_handle_d3d11_win` does with its display-centric Kooima path.

## Build / test

Local Windows only. Do NOT use `/ci-monitor` or push without asking.

```bat
scripts\build_windows.bat build
scripts\build_windows.bat installer
```

Uninstall previous install, then install from `_package\`.

Test procedure:
1. Start service: `Start-Process "C:\Program Files\DisplayXR\Runtime\displayxr-service.exe"`
2. Start bridge: `Start-Process "C:\Program Files\DisplayXR\Runtime\displayxr-webxr-bridge.exe"`
3. Load extension: `chrome://extensions` → Load unpacked → `webxr-bridge\extension\`
4. Serve sample: `cd webxr-bridge\sample && python -m http.server 8080`
5. Open `http://localhost:8080/` → Enter XR
6. Visual check: cubes should have correct aspect ratio and depth separation. On a Leia SR 3D display, the stereo effect should be visible (left/right eye parallax).
7. Press V in compositor → mode switches, tile layout updates, scene re-renders at new mode's aspect ratio.

## Verification

- **2D mode**: 1 tile fills the display. Scene rendered with mono Kooima projection (center eye). Aspect ratio matches the physical display (no horizontal stretch/squeeze).
- **3D mode**: 2 tiles side by side. Each eye has its own Kooima projection with IPD offset. Cubes at different depths show correct parallax. Aspect ratio per tile matches the mode's native tile aspect.
- **Mode switch**: pressing V re-settles within 1-2 frames. No layer rebuild, no flash, just viewport + projection update.
- **Eye tracking**: if Leia SR eye tracking is active, moving your head should change the viewpoint (parallax shift on the cubes). The bridge delivers updated eye positions each frame.

## Commit plan

Single commit on `feature/webxr-bridge-v2`:

```
WebXR Bridge v2: Kooima 3D sample with display-centric projection (#139)

- webxr-bridge/sample: replace colored-rectangle placeholder with three.js
  scene using Kooima display-centric projection from bridge-delivered RAW
  eye poses. Per-mode tile layout, mono/stereo switching on renderingmodechange.
  Matches cube_handle_d3d11_win's rendering approach.
```
```

---

## Notes for the person kicking this off

- The bridge, extension, and compositor changes are all committed and working. This task is purely sample-side — no runtime, bridge, or extension changes.
- The critical path is: `session.displayXR.eyePoses[i].fov` → `makePerspective()` → per-tile `gl.viewport`. If the FOV data isn't arriving (bridge not running, extension not loaded), the sample should fall back to a reasonable default (e.g., symmetric 60° FOV).
- three.js's `WebGLRenderer.render(scene, camera)` resets GL state internally. The sample must re-bind the XR framebuffer and re-set viewport/scissor AFTER `renderer.render()`, or use `renderer.setViewport()` and `renderer.setScissorTest()` which three.js respects. Alternatively, use `renderer.autoClear = false` and `renderer.setRenderTarget(null)` to prevent three.js from rebinding its default framebuffer.
- The previous attempt with three.js failed because `new XRWebGLLayer(proxySession, gl)` rejected the Proxy wrapper. This was fixed by using `Object.defineProperty` instead of Proxy. The current extension code works — `session.displayXR` is a getter on the real session object.
- `displayPixelSize` is reported correctly as `[3840, 2160]` when the bridge runs in IPC mode (verified in the last test session).
- The sample should NOT use `renderer.xr.enabled = true` (three.js's built-in WebXR). We manage the XR loop ourselves because we need custom tile viewports and bridge-derived projection matrices.

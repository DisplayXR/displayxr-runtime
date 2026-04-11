# WebXR Bridge v2 Plan — Metadata Sideband for Native WebXR

**Branch:** `feature/webxr-bridge-v2`
**Tracking issue:** #139
**Target platform:** Windows, D3D11 service compositor (macOS and shell hosting are deferred)
**Supersedes:** everything currently under `webxr-bridge/` (old bridge is fully scrapped)

## Overview

WebXR on Windows already works against DisplayXR today without any bridge or extension. Chromium's built-in WebXR implementation speaks standard OpenXR, goes through the OpenXR loader into `displayxr-service`, and renders via the D3D11 service compositor. Frames are standard OpenXR swapchain images on shared DXGI handles — zero-copy, no WebSocket.

That path has four limitations:

1. **Legacy compromise framebuffer size.** Chrome does not enable `XR_EXT_display_info`, so the runtime falls into a compromise branch at `src/xrt/state_trackers/oxr/oxr_system.c:155-183` that hardcodes a `0.5 × 1.0` recommended view scale for 2-view SBS modes. It cannot adapt when the DP render mode changes mid-session.
2. **No display info in JS.** The page cannot see physical display dimensions, eye position, or the catalogue of rendering modes.
3. **No mode-change events in JS.** `XrEventDataRenderingModeChangedEXT` fires inside the runtime but Chrome's WebXR API does not surface OpenXR events to the page.
4. **No custom input / qwerty suppression.** The service-hosted window forwards its keys into the qwerty driver (moving the head pose), and there is no path for mouse / keyboard / gamepad events to reach the WebXR page as DOM events.

**Goal.** Leave the WebXR frame path completely untouched. Add a new Chrome MV3 extension plus a small native bridge process that act as a **metadata and control sideband**. A displayXR-aware WebXR page then behaves semantically like a handle app (display-info aware, mode-event aware, dynamic render texture sizing, custom input, qwerty suppressed) while being rendered inside the runtime-hosted window created by the D3D11 service compositor in the normal (non-shell) path.

Vendor-agnostic — the bridge talks to the service compositor, not to any DP.

## Architecture

```
+-------------------------------+     WebSocket (loopback)
|  Chrome MV3 extension         |<------------------------->+------------------------------+
|   - MAIN world: navigator.xr  |    JSON metadata + input  |  displayxr-webxr-bridge.exe  |
|     wrapper, session.displayXR|    (no frames on this     |  (tiny OpenXR client, own    |
|   - ISOLATED world: WS client |     channel, ever)        |   XrInstance / XrSession)    |
+---------------+---------------+                           +---------------+--------------+
                ^                                                           |
                | session.displayXR,                                        | OpenXR events,
                | renderingmodechange,                                      | XR_EXT_display_info,
                | DOM input events                                          | XR_EXT_app_owned_input
                v                                                           v
        +-------+----------+                                   +------------+---------------+
        | WebXR page       |                                   |                            |
        | (three.js sample)|                                   |    displayxr-service.exe   |
        +-------+----------+                                   |   (D3D11 svc compositor,   |
                |                                              |    creates hosted window,  |
                | navigator.xr / WebXR                         |    runs DP)                |
                | (standard browser-native path,               |                            |
                |  frames on shared DXGI handles)              |                            |
                v                                              ^
        +-------+-------------------------------------------+  |
        |      Chromium built-in WebXR (OpenXR client)      |--+
        +---------------------------------------------------+
```

Two **separate OpenXR sessions** talk to the same service at the same time:

- **Chrome's WebXR session** — does the actual rendering. Does not know about `XR_EXT_display_info`. Goes through the legacy compromise path today. Produces frames, submits them via the standard IPC client compositor. Not touched by this plan.
- **Bridge's metadata session** — a second OpenXR client in `displayxr-webxr-bridge.exe`. Enables `XR_EXT_display_info` and (phase 3) `XR_EXT_app_owned_input`. Does not render anything, no swapchain, no composition layer. Only purpose: query display info, poll for mode-change events, poll hosted-window input events, and relay everything to the browser over WebSocket.

Both sessions coexist in the same service (service is multi-client capable since #116 was fixed).

### What does NOT change

- The WebXR frame pipeline. Chrome native WebXR → OpenXR loader → DisplayXR IPC → service compositor → DP is unchanged. Frames stay on shared DXGI handles, zero-copy.
- The legacy compromise branch in `oxr_system_fill_in()`. It remains as the fallback for WebXR apps that do not install the extension. Not touched.
- `cube_hosted_d3d11_win` and every other existing IPC app. The new `XR_EXT_app_owned_input` is strictly opt-in.

## Prior art and existing infrastructure

| Component | Location | What exists |
|-----------|----------|-------------|
| `XR_EXT_display_info` | `src/xrt/state_trackers/oxr/` + `docs/specs/XR_EXT_display_info.md` | Fully shipped; display dims, eye pos, supported rendering modes. |
| Rendering mode events | `src/xrt/state_trackers/oxr/oxr_session.c:855-885` | `XrEventDataRenderingModeChangedEXT` already pushed to IPC client sessions on DP mode changes. |
| Legacy compromise branch | `src/xrt/state_trackers/oxr/oxr_system.c:155-183` | Hardcodes 0.5 × 1.0 for clients without `XR_EXT_display_info`. This is the branch Chrome's WebXR currently hits. |
| Window-relative Kooima | `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp:8943-8969` | `comp_d3d11_service_get_client_window_metrics()` — Chrome's `xrLocateViews` already returns Kooima-correct projection matrices. |
| Qwerty disable helper | `src/xrt/drivers/qwerty/qwerty_interface.h:230` | `qwerty_set_process_keys()` — already used by external-window handle apps via `oxr_session.c:2604-2615`. |
| IPC multi-client | service, post-#116 | Multiple OpenXR sessions against the same service are supported. |
| Reference minimal IPC app | `test_apps/cube_hosted_d3d11_win/` | Shape to model the new bridge host after (minus the graphics binding). |
| Old bridge (to delete) | `webxr-bridge/` (native-host/, extension/, package.json, build.sh) | macOS-only, WebSocket-based frame transport, IWER polyfill. Scrap entirely. |

## Phase 1 — Bridge host skeleton

No extension, no WebSocket, no runtime changes. Just the OpenXR client that reads display info and logs events, and deletion of the old bridge.

### Tasks

| Task | Size | Description |
|------|------|-------------|
| 1.1 Delete old `webxr-bridge/` | Small | Remove `webxr-bridge/native-host/*`, `webxr-bridge/extension/*`, `webxr-bridge/package.json`, `webxr-bridge/package-lock.json`, `webxr-bridge/build.sh`. Keep the `webxr-bridge/` directory itself — it will be repopulated in Phase 2. Commit the deletion in the same PR as the new scaffold so `main` never has a broken intermediate state. |
| 1.2 New target directory | Small | Create `src/xrt/targets/webxr_bridge/` with `main.cpp` and `CMakeLists.txt`. Model the CMake after `test_apps/cube_hosted_d3d11_win/CMakeLists.txt` but drop all D3D11 / graphics dependencies. Only link the OpenXR loader and standard C/C++. |
| 1.3 Minimal OpenXR client | Medium | In `main.cpp`: create `XrInstance` with `XR_EXT_display_info` enabled, create `XrSystemId`, query system properties + `XR_EXT_display_info` chain, log everything. Create an `XrSession` **without** any graphics binding (use `XR_KHR_headless` if supported; otherwise use a tiny D3D11 device as a no-op binding — verify which applies to this runtime). No swapchain, no composition layer, no frame loop. |
| 1.4 Event pump | Medium | Add an `xrPollEvent` loop on a background thread. Log every event. On `XrEventDataRenderingModeChangedEXT`, re-query `xrEnumerateViewConfigurationViews` for the new per-view dims and log them. |
| 1.5 CMake integration | Small | Add the new target to the Windows build in `scripts\build_windows.bat`. Output `_package\bin\displayxr-webxr-bridge.exe`. |
| 1.6 Coexistence verification | Medium | Run `displayxr-service.exe`, then in parallel start `displayxr-webxr-bridge.exe` and open any WebXR demo in Chrome (e.g. `immersive-web.github.io/webxr-samples/`). Confirm both sessions stay alive, Chrome still renders, the bridge logs display info and mode events. |

### Open question in Phase 1

- **Can an OpenXR session exist without a graphics binding against DisplayXR today?** OpenXR 1.0 requires a graphics binding unless `XR_MND_headless` or equivalent is enabled. Check `src/xrt/state_trackers/oxr/oxr_instance.c` for what extensions are exposed. If headless is not available, the bridge will need a no-op D3D11 device binding — still trivial, no actual swapchain or rendering needed.

### Phase 1 verification

```
scripts\build_windows.bat build
_package\bin\displayxr-service.exe
_package\bin\displayxr-webxr-bridge.exe
# In another window: open a WebXR demo page in Chrome, click Enter XR.
```

Expected: bridge logs a complete `display-info` dump. Chrome WebXR renders normally in the service-hosted window. Cycling render modes via whatever service hotkey is active logs a `mode-changed` event in the bridge. No crashes, no lock contention.

---

## Phase 2 — WebSocket metadata server, MV3 extension, three.js sample

No runtime changes. Only bridge + extension + sample.

### Tasks

| Task | Size | Description |
|------|------|-------------|
| 2.1 Embed a loopback WebSocket server | Medium | Use a small header-only C++ library (suggest [uWebSockets](https://github.com/uNetworking/uWebSockets) or a minimal hand-rolled implementation, since libwebsockets is already used by the old bridge). Bind `127.0.0.1:9014`. Reject any non-localhost origin. Single-client for now; multi-client later if needed. |
| 2.2 JSON protocol | Small | Define message shapes. Messages in (extension → bridge): `hello` (handshake with options). Messages out (bridge → extension): `display-info` (once on connect), `mode-changed` (on event), `window-resized` (phase 2.5 decides if needed). Keep a single stable schema documented in `webxr-bridge/PROTOCOL.md`. |
| 2.3 **Framebuffer-size override smoke test** | Medium | **Do this before building the full sample.** Write a minimal WebXR page that calls `new XRWebGLLayer(session, gl, { framebufferScaleFactor: 4.0 })` or `XRWebGLBinding.createProjectionLayer({ textureWidth: LARGE, textureHeight: LARGE })` and inspect `layer.framebufferWidth/Height`. Confirm Chrome actually honours a size larger than its own recommendation (which today is the 0.5 × 1.0 legacy compromise). If Chrome clamps, document the clamp behaviour and fall through to the runtime-side mitigation described below. |
| 2.4 New MV3 extension — manifest | Small | New `webxr-bridge/extension/manifest.json`. Name `DisplayXR WebXR Bridge`. No `nativeMessaging` permission (dropped entirely). Two content scripts, MAIN and ISOLATED worlds, document_start. |
| 2.5 MAIN-world `navigator.xr` wrapper | Large | `webxr-bridge/extension/src/main-world.js`: keep a reference to the original `navigator.xr`. Replace `requestSession` with a wrapper that awaits the real session, then wraps it in a `Proxy` exposing an extra `session.displayXR` property. The proxy is a pass-through — frame callbacks, view poses, projection matrices, `updateRenderState`, everything else forwards unchanged. Emits `renderingmodechange`, `windowresize`, `input` as events on the session object. |
| 2.6 `session.displayXR` surface | Medium | Properties: `displayInfo`, `renderingMode` (current mode + per-view dims + atlas rects). Helper: `session.displayXR.computeFramebufferSize()` returns the pixel dims the page should pass to `XRWebGLLayer` / `XRProjectionLayer` for the current mode. No magic constants; all values come from the bridge over the WebSocket. |
| 2.7 ISOLATED-world WebSocket relay | Medium | `webxr-bridge/extension/src/isolated-world.js`: WebSocket client to `ws://127.0.0.1:9014`. Relays messages between MAIN and ISOLATED via `window.postMessage` with strict origin checks. Reconnects on disconnect. |
| 2.8 three.js sample | Medium | `webxr-bridge/sample/index.html` + `sample.js`. Scene: textured cube + skybox + pointer-locked first-person camera. Heavily commented. Demonstrates: feature detection, reading `displayInfo`, `computeFramebufferSize()`, `renderingmodechange` handling, rebuilding `XRWebGLLayer` via `updateRenderState({ baseLayer })`. Uses three.js from a CDN (no build step). |
| 2.9 Top-level README | Small | `webxr-bridge/README.md` walking through: start service → start bridge → load extension → open sample → press Enter XR. |

### If Chrome clamps framebuffer size (task 2.3 fallback)

If Chrome refuses to honour a framebuffer larger than the legacy 0.5 × 1.0 recommendation, we can make the recommendation bigger without breaking anything else:

- In `src/xrt/state_trackers/oxr/oxr_system.c` at the legacy branch (line 155-183), widen `info->legacy_view_width_pixels` / `info->legacy_view_height_pixels` to the maximum across all supported rendering modes instead of the compromise scale. The sample app then passes a `framebufferScaleFactor` ≤ 1 computed dynamically from `session.displayXR.computeFramebufferSize()` to reach the correct per-mode dims.
- This affects all existing legacy WebXR apps but only by giving them more framebuffer than they need — they will render in the top-left corner and the rest is ignored. Verify that the existing legacy compromise scaling in the compositor handles this gracefully. If not, fall back to gating the widened recommendation on an environment variable so existing users are unaffected.

### Phase 2 verification

```
# 1. Build, start service + bridge (same as Phase 1).
# 2. chrome://extensions → Load unpacked → webxr-bridge/extension/
# 3. Open webxr-bridge/sample/index.html in Chrome.
# 4. Click Enter XR.
```

Expected:
- three.js cube renders in the service-hosted window at the **correct** per-view dims for the current mode. Visual check: pick a mode whose `view_scale_x * display_width ≠ 0.5 * display_width` and verify the rendering is not stretched.
- Changing the DP render mode mid-session: `renderingmodechange` fires in the sample, `XRWebGLLayer` is rebuilt, the scene re-settles correctly without a page reload.
- `session.displayXR.displayInfo.displaySizeMeters` matches the physical panel.

---

## Phase 3 — `XR_EXT_app_owned_input` and input forwarding

Runtime changes start here. Small, isolated, all gated behind a new opt-in OpenXR extension.

### Tasks

| Task | Size | Description |
|------|------|-------------|
| 3.1 Extension header | Small | `src/xrt/include/openxr/xr_ext_app_owned_input.h` — define `XR_EXT_APP_OWNED_INPUT_EXTENSION_NAME`, new enum values in the reserved range, any new struct types (input event layout). Match the format of existing `XR_EXT_*` headers in the repo. |
| 3.2 Register extension | Small | `src/xrt/state_trackers/oxr/oxr_instance.c` — add the new extension to the exposed list (behind a build flag if needed), same way `XR_EXT_display_info` is registered. Plumb an `inst->extensions.EXT_app_owned_input` flag. |
| 3.3 Qwerty auto-disable widening | Small | `src/xrt/state_trackers/oxr/oxr_session.c` around line 2604-2615 — the current condition auto-calls `qwerty_set_process_keys(false)` when the session has an external window handle. Widen: also fire when `inst->extensions.EXT_app_owned_input` is set. Restore `true` on session destroy. |
| 3.4 IPC RPC: `ipc_call_poll_window_input` | Medium | Add to `src/xrt/ipc/shared/proto.per` (+ regenerate sibling files). Signature: `(session_id) -> input_event[]`. Server stub in `src/xrt/ipc/server/`, client wrapper in `src/xrt/ipc/client/`. |
| 3.5 Server-side input ring buffer | Medium | `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — in the existing WndProc that feeds qwerty, also push raw events `{kind, timestamp, key/button/x/y, modifiers}` into a per-session ring buffer when any client has the extension enabled. Add a one-per-compositor-frame XInput poll for gamepad state deltas. Ring size 256 events; overflow drops oldest. |
| 3.6 Bridge polls + forwards | Medium | Bridge host calls `ipc_call_poll_window_input` once per frame (or every ~16 ms on its event thread). Each event forwarded as an `input` JSON message over the WebSocket. |
| 3.7 Extension synthesizes DOM events | Medium | `webxr-bridge/extension/src/main-world.js` — on each `input` message, synthesize a `MouseEvent` / `KeyboardEvent` on `document` (via `document.dispatchEvent`), and mirror gamepad state into a shim that `navigator.getGamepads()` reads. Standard three.js / pointer-lock / gamepad code works unmodified. |
| 3.8 Sample demonstrates | Small | Update `webxr-bridge/sample/sample.js` to drive the three.js camera from pointer-lock mouse look + WASD + XInput. Add a comment section explaining how to opt out of qwerty disable if you want both WASD-driven camera *and* qwerty head pose simultaneously (unusual, but possible). |
| 3.9 Spec doc | Small | `docs/specs/XR_EXT_app_owned_input.md` — match the format of `XR_EXT_display_info.md`. |

### Phase 3 verification

- Sample page in XR, focused in the service-hosted window: pressing WASD moves the three.js camera and does **not** move the runtime head pose (qwerty disabled). Mouse look via pointer lock works. An XInput controller appears in `navigator.getGamepads()`.
- Extension disabled: same sample page falls back to the Phase 2 behaviour (qwerty still moves the head pose).
- **Regression**: `cube_hosted_d3d11_win` against the same service (no bridge, no extension) behaves identically to pre-Phase 3. Qwerty still moves its head pose. No new RPC invoked on its session.

### Qwerty-disable scope note

Calling `qwerty_set_process_keys(false)` from the bridge's session affects the whole service, including any other concurrent client. That is fine for the single-user / single-WebXR-app scenario but would need rethinking for multi-app shell mode. Out of scope for this plan; document it in the spec.

---

## Files & critical paths

### New
- `src/xrt/targets/webxr_bridge/` — bridge host sources + CMake (Phase 1).
- `src/xrt/include/openxr/xr_ext_app_owned_input.h` — new extension header (Phase 3).
- `src/xrt/state_trackers/oxr/oxr_api_app_owned_input.c` — extension API glue if any beyond enable (Phase 3).
- `webxr-bridge/extension/manifest.json` + `src/main-world.js` + `src/isolated-world.js` (Phase 2).
- `webxr-bridge/sample/index.html` + `sample.js` (Phase 2).
- `webxr-bridge/README.md` (Phase 2).
- `webxr-bridge/PROTOCOL.md` — WebSocket JSON protocol spec (Phase 2).
- `docs/specs/XR_EXT_app_owned_input.md` (Phase 3).

### Modified
- `src/xrt/state_trackers/oxr/oxr_session.c` — qwerty auto-disable widening (Phase 3).
- `src/xrt/state_trackers/oxr/oxr_instance.c` — register new extension (Phase 3).
- `src/xrt/ipc/shared/proto.per` (+ generated siblings) — new RPC (Phase 3).
- `src/xrt/ipc/server/` — per-session ring buffer + RPC server (Phase 3).
- `src/xrt/ipc/client/` — RPC client wrapper (Phase 3).
- `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` — WndProc event push + XInput poll (Phase 3).
- `scripts/build_windows.bat` — add new target (Phase 1).

### Deleted (in the Phase 1 commit)
- `webxr-bridge/native-host/openxr_bridge_host.cpp` (~1916 lines)
- `webxr-bridge/native-host/bridge_window_macos.mm`
- `webxr-bridge/native-host/CMakeLists.txt`
- `webxr-bridge/native-host/com.openxr.bridge.json`
- `webxr-bridge/extension/src/index.js` (old IWER-based polyfill)
- `webxr-bridge/extension/webxr-polyfill-bundle.js` (bundled IWER, ~11k lines)
- `webxr-bridge/extension/background.js`, `content-script.js`
- `webxr-bridge/package.json`, `package-lock.json`, `build.sh`

### Not touched
- `src/xrt/state_trackers/oxr/oxr_system.c:155-183` — legacy compromise branch stays (may be widened defensively in Phase 2 if Chrome clamps; see task 2.3 fallback).
- WebXR frame pipeline.
- `cube_hosted_d3d11_win` and every other existing IPC app.

---

## Risks & open questions

1. **Chrome `XRWebGLLayer` framebuffer-size flexibility.** Verified in Phase 2 task 2.3 before building the full sample. Mitigation: widen the legacy recommendation in `oxr_system.c` so page-side scale factor stays ≤ 1.
2. **Headless OpenXR session support.** Verified in Phase 1 task 1.3. Fallback: use a no-op D3D11 binding.
3. **Qwerty disable scope.** Whole-service, fine for single-user single-WebXR scenario. Multi-app shell will revisit.
4. **Service multi-client stability.** Service is multi-client capable post-#116. Phase 1 task 1.6 is the first real coexistence test in production-like conditions.
5. **Frame perf is not in scope.** Worth repeating: nothing in this plan touches the WebXR frame pipeline. Any perf concern about Chrome frame submission is outside scope.

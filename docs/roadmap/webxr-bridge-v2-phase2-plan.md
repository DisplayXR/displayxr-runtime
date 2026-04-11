# WebXR Bridge v2 — Phase 2 Plan: WebSocket metadata server, MV3 extension, three.js sample

**Tracking issue:** #139
**Previous phase:** Phase 1 (#139, commit `7c5f60ec3`) — bridge host skeleton
**Blocking fix (now resolved):** #142 (commit `5c411cfdb`) — `RENDERING_MODE_CHANGED_EXT` fan-out to headless sessions
**Target platform:** Windows only. macOS and shell hosting deferred.
**Authoritative architecture doc:** `docs/roadmap/webxr-bridge-v2-plan.md` (phases 2.1–2.9 listed there are canonical; this doc expands them into a step-by-step implementation plan)

## Problem recap

DisplayXR-aware WebXR pages need a metadata / control sideband so they can:

1. See the physical display's `displayPixelSize`, `displaySizeMeters`, eye positions, and the catalogue of rendering modes.
2. React to runtime-initiated rendering-mode changes (user presses V or 1/2/3 in the service compositor window) by rebuilding their `XRWebGLLayer` with the correct per-view dims for the new mode.
3. Bypass Chrome's legacy compromise framebuffer size (`oxr_system.c:155-183`) which hardcodes 0.5 × 1.0 view scale, so the page can render at the actual per-view atlas dimensions.

Phase 1 built the native half: `displayxr-webxr-bridge.exe` is a headless OpenXR client that enables `XR_EXT_display_info` + `XR_MND_headless`, enumerates modes, and logs events to stdout. With #142 committed, it now actually receives `RENDERING_MODE_CHANGED_EXT` and `HARDWARE_DISPLAY_STATE_CHANGED_EXT` when any other session (Chrome, cube_hosted, etc.) triggers a mode change.

Phase 2 builds the browser-facing half: a loopback WebSocket server inside the bridge, a Chrome MV3 extension that wraps `navigator.xr`, and a three.js sample that exercises the `session.displayXR` surface. **No runtime changes** — this phase is bridge + extension + sample only.

## Goals and non-goals

**Goals.**
- Loopback-only WebSocket server in the bridge (reject non-localhost).
- Stable JSON protocol: `display-info` on connect, `mode-changed` on event, optional `window-resized`.
- MV3 extension with MAIN-world `navigator.xr` Proxy adding a `session.displayXR` namespace.
- three.js sample that reads `session.displayXR.displayInfo`, uses `computeFramebufferSize()` to size `XRWebGLLayer` correctly, and rebuilds the layer on `renderingmodechange`.
- **Verify first (task 2.3):** determine whether Chrome honours an oversized `XRWebGLLayer` framebuffer or clamps to the legacy compromise. This gates the rest of the phase.
- A top-level `webxr-bridge/README.md` + `webxr-bridge/PROTOCOL.md`.

**Non-goals.**
- Input forwarding, qwerty disable, or `XR_EXT_app_owned_input` — those are Phase 3.
- Frame transport. Frames continue to flow through Chrome's native WebXR → OpenXR → service compositor path, zero-copy, untouched.
- macOS. Phase 2 is Windows-only.
- Multi-client WebSocket. Single extension instance at a time; multi-client is a possible follow-up but not required.

## Design summary

### Data flow

```
Chrome MV3 extension ──────── WebSocket 127.0.0.1:9014 ───────── displayxr-webxr-bridge.exe
  MAIN world:                    JSON messages                      (Phase 1 bridge host
    navigator.xr Proxy           (display-info,                      + new WS server)
    session.displayXR            mode-changed, ...)                            │
  ISOLATED world:                                                              │ xrPollEvent
    WebSocket client                                                           │
                                                                               ▼
Chrome WebXR session ─── OpenXR loader ─── DisplayXR IPC ──── displayxr-service.exe
(frames, unchanged)                                           (D3D11 service compositor)
```

Two OpenXR sessions against the same service, post-#116 multi-client safe:
- **Chrome's session** drives frames via the standard WebXR path. Does not see `session.displayXR` at the OpenXR level — the extension wraps it client-side.
- **Bridge's session** holds `XR_EXT_display_info`, polls events, relays to WebSocket.

The extension pairs the two sessions *in the browser*, not at the OpenXR level. The page only ever talks to Chrome's WebXR session; `session.displayXR` is a JS-side façade fed by the WebSocket.

### JSON protocol (v1)

All messages are single-line JSON over a text WebSocket frame. No framing, no length prefix.

**Extension → bridge:**
```json
{ "type": "hello", "version": 1, "origin": "chrome-extension://<id>" }
```

**Bridge → extension (on connect, after `hello`):**
```json
{
  "type": "display-info",
  "version": 1,
  "displayPixelSize":   [3840, 2160],
  "displaySizeMeters":  [0.344, 0.194],
  "recommendedViewScale": [0.5, 0.5],
  "nominalViewerPosition": [0.0, 0.1, 0.6],
  "renderingModes": [
    { "index": 0, "name": "2D",     "viewCount": 1, "tileColumns": 1, "tileRows": 1, "viewScale": [1.0, 1.0], "hardware3D": false },
    { "index": 1, "name": "LeiaSR", "viewCount": 2, "tileColumns": 2, "tileRows": 1, "viewScale": [0.5, 0.5], "hardware3D": true  }
  ],
  "currentModeIndex": 1,
  "views": [
    { "index": 0, "recommendedImageRectWidth": 1920, "recommendedImageRectHeight": 1080, "maxImageRectWidth": 3840, "maxImageRectHeight": 2160 },
    { "index": 1, "recommendedImageRectWidth": 1920, "recommendedImageRectHeight": 1080, "maxImageRectWidth": 3840, "maxImageRectHeight": 2160 }
  ]
}
```

**Bridge → extension (on every `XrEventDataRenderingModeChangedEXT`):**
```json
{
  "type": "mode-changed",
  "version": 1,
  "previousModeIndex": 1,
  "currentModeIndex": 0,
  "hardware3D": false,
  "views": [ ... ]
}
```

**Bridge → extension (on `XrEventDataHardwareDisplayStateChangedEXT`):**
```json
{
  "type": "hardware-state-changed",
  "version": 1,
  "hardware3D": false
}
```

Document this schema in `webxr-bridge/PROTOCOL.md`. Bridge and extension must both check `version == 1` on every message. Future schema changes bump the integer.

### Extension structure

Two content scripts, both at `document_start`:

- **MAIN world** (`main-world.js`): wraps `navigator.xr.requestSession`. Awaits the real session, then returns a `Proxy` that forwards every property/method to the underlying session except for the added `session.displayXR` property. The proxy also dispatches `renderingmodechange`, `hardwarestatechange` events via the session's existing `EventTarget` interface (both `XRSession` extends `EventTarget`).
- **ISOLATED world** (`isolated-world.js`): opens the WebSocket, forwards messages to/from MAIN world via `window.postMessage` with strict origin checks (`window.location.origin`). Reconnects on disconnect with exponential backoff.

No `nativeMessaging`, no background page, no bundler. The extension loads directly via `chrome://extensions → Load unpacked`.

### `session.displayXR` surface

```ts
interface SessionDisplayXR {
  // Current display info snapshot. Updated on every `mode-changed` message.
  readonly displayInfo: {
    displayPixelSize:      [number, number];
    displaySizeMeters:     [number, number];
    nominalViewerPosition: [number, number, number];
  };

  // Current rendering mode info.
  readonly renderingMode: {
    index: number;
    name: string;
    viewCount: number;
    tileColumns: number;
    tileRows: number;
    viewScale: [number, number];
    hardware3D: boolean;
    views: Array<{
      index: number;
      recommendedImageRectWidth:  number;
      recommendedImageRectHeight: number;
      maxImageRectWidth:  number;
      maxImageRectHeight: number;
    }>;
  };

  // Helper: returns the per-eye framebuffer dims the page should pass to
  // XRWebGLLayer / XRProjectionLayer for the current mode. This is the
  // "escape the legacy 0.5 × 1.0" call. Implementation reads renderingMode.views.
  computeFramebufferSize(): { width: number; height: number };
}
```

Plus events dispatched on the session object itself:
- `renderingmodechange` — `event.detail = { previousModeIndex, currentModeIndex, renderingMode }`
- `hardwarestatechange` — `event.detail = { hardware3D }`

### Framebuffer-size escape hatch

The whole point of `computeFramebufferSize()` is to produce pixel dims that match the current `renderingMode.views[i].recommendedImageRectWidth/Height`, which are the correct per-view atlas slots from the runtime. The page then passes those via:

```js
const dims = session.displayXR.computeFramebufferSize();
const layer = new XRWebGLLayer(session, gl, {
    framebufferScaleFactor: dims.width / defaultRecommended.width
});
```

or, if `framebufferScaleFactor` clamps (see risk), via `XRProjectionLayer`:

```js
const glBinding = new XRWebGLBinding(session, gl);
const layer = glBinding.createProjectionLayer({
    textureWidth:  dims.width,
    textureHeight: dims.height,
    // ...
});
```

**Which API Chrome actually honours is the single biggest unknown in Phase 2 and must be settled by task 2.3 (framebuffer-size smoke test) before building the full sample.**

## Implementation order

Do the steps in this order. Steps 1–2 are the only ones that can block everything else; do them first and stop at each checkpoint.

### Step 1 — Framebuffer-size smoke test (task 2.3 in main plan)

**This is the riskiest verification in the phase. Do it first. If it fails, the fallback runtime change in step 1b is needed before the sample is worth building.**

Write a minimal 50–80 line WebXR test page (no extension, no bridge) that:

1. Requests an immersive session.
2. Tries **two** paths for oversized framebuffer:
   a. `new XRWebGLLayer(session, gl, { framebufferScaleFactor: 4.0 })` and logs `layer.framebufferWidth`/`framebufferHeight`.
   b. `new XRWebGLBinding(session, gl).createProjectionLayer({ textureWidth: 3840, textureHeight: 2160 })` and logs the resulting texture size.
3. Compares the returned dims to the input. If the returned dims are capped below the request, **Chrome is clamping** (the legacy 0.5 × 1.0 recommendation via `oxr_system.c:155-183` is a hard ceiling in practice).

Expected outcome — one of:

- **Best case:** `XRProjectionLayer` honours the requested `textureWidth/Height` and produces an arbitrary-size framebuffer. Use this API in the sample, no runtime change needed.
- **Middle case:** `XRWebGLLayer` honours `framebufferScaleFactor` up to some multiple of the recommendation. Still usable as long as the multiple covers the atlas dims. Use this API in the sample.
- **Worst case:** Both APIs clamp to the legacy 0.5 × 1.0 recommendation. Implement step 1b.

**Commit this smoke test under `webxr-bridge/tools/framebuffer-size-check/` so it's reproducible later.** A simple standalone HTML file is fine.

### Step 1b — Runtime legacy-recommendation widening (only if step 1 clamps)

In `src/xrt/state_trackers/oxr/oxr_system.c`, the legacy compromise branch at lines 155-183 fills `info->legacy_view_width_pixels` / `legacy_view_height_pixels`. Widen both to the **maximum** across all rendering modes (not the 0.5 × 1.0 compromise). This gives Chrome a recommendation big enough that its scale factor lands on the correct atlas dims.

Risk: every existing legacy WebXR app will suddenly get a bigger framebuffer than it expects. Most will render to the top-left corner and waste pixels — harmless. But verify the compositor's compromise scaling still crops correctly (`docs/architecture/extension-vs-legacy.md` covers this). If the visual result regresses for existing legacy apps, gate the widening behind an environment variable `XRT_WIDEN_LEGACY_VIEWS=1` so default behaviour is unchanged.

**Stop and ask the user before committing a runtime change in Phase 2** — the whole point of this phase was "no runtime changes." If Chrome clamps, the user may choose the env var gate or may decide to accept a lower-res WebXR rendering for non-extension-aware pages.

### Step 2 — WebSocket server inside the bridge (task 2.1)

**Library choice needs user sign-off before coding.** Options:

| Option | Pros | Cons |
|---|---|---|
| Hand-rolled minimal WS (single file, ~400 LOC) | Zero dependencies, full control | Non-trivial to get right; RFC 6455 framing + mask + UTF-8 validation |
| [uWebSockets](https://github.com/uNetworking/uWebSockets) (header-only-ish) | Fast, well-tested | Adds a dependency and a few thousand LOC to the target |
| libwebsockets (already in the repo tree somewhere?) | Reuses existing dependency if present | Historically the old bridge used it; check if it's still pulled in |

Recommendation: **start with hand-rolled minimal WS**, single-client, synchronous handshake + simple read loop on a dedicated thread. We don't need broadcast, compression, or sub-protocols. About 300-500 lines of C++. If it turns out to be brittle, swap to uWebSockets later.

Whichever you pick, the WS server:
- Binds `127.0.0.1:9014` only — not `0.0.0.0`. Reject any `Origin:` header whose host is not `localhost`/`127.0.0.1` or `chrome-extension://`.
- One client at a time. If a second connects, close it with 1008 (policy violation).
- Runs on its own thread. The OpenXR event pump stays on the main thread (from Phase 1). Inter-thread comms via a lock-protected std::queue of outgoing messages + an atomic "client-connected" flag.

### Step 3 — JSON serialization (task 2.2)

Use a header-only JSON library. `nlohmann/json` is the usual choice and is already present in many Monado-derived trees. Check `vcpkg/ports/` or `build/vcpkg_installed/` to see if it's available; otherwise fetch via vcpkg. Hand-rolling JSON for this tiny schema is also fine (5 message types, flat shapes) — maybe 200 LOC.

Serialize from the Phase 1 `Bridge` struct. On connect send `display-info` immediately. On every bridge-side `RENDERING_MODE_CHANGED`, push a `mode-changed` message into the outgoing queue. Same for hardware state change.

### Step 4 — Bridge integration

In `src/xrt/targets/webxr_bridge/main.cpp`:

- Spin up the WS server thread in `main()` after `get_system_and_display_info` has succeeded.
- Add an outgoing message queue + mutex + condvar.
- In `handle_event()`, push a `mode-changed` or `hardware-state-changed` JSON string into the queue. Do **not** block the OpenXR event pump.
- On `XR_SESSION_STATE_EXITING` and on Ctrl+C, shut down the WS server cleanly (close client, join thread).

Logging: keep stdout logging exactly as in Phase 1. Add one-line info logs on WS connect/disconnect/send. No per-frame logs.

### Step 5 — MV3 extension scaffolding (tasks 2.4, 2.7)

Create `webxr-bridge/` at repo root (Phase 1 deleted it; this is the repopulation):

```
webxr-bridge/
├── README.md              # walkthrough: service → bridge → extension → sample
├── PROTOCOL.md            # JSON schema spec, version 1
├── extension/
│   ├── manifest.json      # MV3, no nativeMessaging, two content scripts
│   ├── icons/             # 16/48/128 placeholder
│   └── src/
│       ├── main-world.js      # navigator.xr Proxy
│       └── isolated-world.js  # WebSocket client + message relay
├── sample/
│   ├── index.html
│   └── sample.js          # three.js from CDN
└── tools/
    └── framebuffer-size-check/ # step 1 smoke test, standalone HTML
```

`manifest.json` essentials:
```json
{
  "manifest_version": 3,
  "name": "DisplayXR WebXR Bridge",
  "version": "0.2.0",
  "description": "Metadata/control sideband for DisplayXR WebXR",
  "content_scripts": [
    {
      "matches": ["http://*/*", "https://*/*", "file:///*"],
      "js": ["src/isolated-world.js"],
      "run_at": "document_start",
      "world": "ISOLATED",
      "all_frames": false
    },
    {
      "matches": ["http://*/*", "https://*/*", "file:///*"],
      "js": ["src/main-world.js"],
      "run_at": "document_start",
      "world": "MAIN",
      "all_frames": false
    }
  ],
  "permissions": []
}
```

No host permissions, no `activeTab`, no `nativeMessaging`. WebSocket to localhost does **not** require host permissions in MV3 when initiated from content scripts in ISOLATED world — document this in `PROTOCOL.md`.

### Step 6 — Two-script message relay (task 2.7)

`isolated-world.js`:
- Opens `new WebSocket("ws://127.0.0.1:9014")`.
- On message, relays to MAIN world via `window.postMessage({ source: "displayxr-bridge", payload }, window.location.origin)`.
- Listens for `window.postMessage` with `source: "displayxr-bridge-req"` from MAIN world, forwards to WebSocket.
- Reconnects on close with exponential backoff (500ms, 1s, 2s, 4s, capped at 8s).
- Strict origin check on every message — drop anything not from `window.location.origin`.

`main-world.js`:
- Captures the original `navigator.xr`.
- Listens for `window.postMessage` with `source: "displayxr-bridge"`.
- Maintains a module-level `latestDisplayInfo` and `latestRenderingMode` state, updated on `display-info` and `mode-changed` messages.
- Wraps `navigator.xr.requestSession` — awaits the real session, wraps it in a `Proxy`, and returns the proxy. The `Proxy`:
  - Forwards every `get` to the underlying session, except for `displayXR` which returns the `SessionDisplayXR` object.
  - Forwards `addEventListener`/`removeEventListener`/`dispatchEvent` to the underlying session's EventTarget methods (via `session.addEventListener(...)` — we can't re-use the Proxy for these because they need to bind to the real object).

On `mode-changed`, dispatch a `renderingmodechange` event on the underlying session so the page receives it via `session.addEventListener('renderingmodechange', ...)`.

### Step 7 — `session.displayXR` implementation (task 2.6)

Inside `main-world.js`, the `SessionDisplayXR` instance is a plain object (not a class) — simpler to serialize. Rebuild it whenever `display-info` or `mode-changed` arrives. `computeFramebufferSize()` returns:

```js
// For single-view 2D mode (viewCount=1):
//   width  = renderingMode.views[0].recommendedImageRectWidth
//   height = renderingMode.views[0].recommendedImageRectHeight
//
// For multi-view modes (viewCount=2, 4, ...):
//   Assume the page draws into a single framebuffer sized as the atlas:
//   width  = view.recommendedImageRectWidth * tileColumns
//   height = view.recommendedImageRectHeight * tileRows
```

Document the tile layout assumption in `PROTOCOL.md`. It matches how the compositor already composes the atlas in `compositor_layer_commit`.

### Step 8 — three.js sample (task 2.8)

`sample/index.html` pulls `three` from a CDN:

```html
<script type="importmap">
{ "imports": { "three": "https://unpkg.com/three@0.160.0/build/three.module.js" } }
</script>
```

`sample.js`:
- Feature-detects `navigator.xr` and the `session.displayXR` surface.
- On `Enter XR`:
  1. Request an immersive session.
  2. Read `session.displayXR.displayInfo` and log it.
  3. Call `session.displayXR.computeFramebufferSize()` and set up either `XRWebGLLayer` (with `framebufferScaleFactor`) or `XRProjectionLayer` depending on task 2.3 outcome.
  4. Start the frame loop.
- On `renderingmodechange`: recompute framebuffer size, rebuild the layer, call `session.updateRenderState({ baseLayer: newLayer })`.
- Minimal three.js scene: textured cube + skybox + fixed camera. No input yet (Phase 3 adds input).
- Heavy comments on every line that touches `session.displayXR`.

### Step 9 — README + protocol doc (tasks 2.2, 2.9)

`webxr-bridge/README.md`: step-by-step walkthrough. Exactly these steps, no prose:

```
1. Build the runtime + bridge:  scripts\build_windows.bat build installer
2. Uninstall any previous DisplayXR install, then install the new MSI.
3. Start the service: Start-Process "C:\Program Files\DisplayXR\Runtime\displayxr-service.exe"
4. Start the bridge:  Start-Process "C:\Program Files\DisplayXR\Runtime\displayxr-webxr-bridge.exe"
5. Load extension:    chrome://extensions → Developer mode ON → Load unpacked → <repo>\webxr-bridge\extension\
6. Open sample:       chrome://<...>/webxr-bridge/sample/index.html (via file:// or a local HTTP server)
7. Click Enter XR.
```

`webxr-bridge/PROTOCOL.md`: the JSON schema above, plus version semantics and origin-check rules.

## Critical files to create / modify

| File | Change |
|---|---|
| `src/xrt/targets/webxr_bridge/main.cpp` | Add WS server thread + outgoing queue + JSON serialization. ~200 new LOC on top of the Phase 1 415-line skeleton. |
| `src/xrt/targets/webxr_bridge/CMakeLists.txt` | Link whichever JSON + WS libs you pick. Probably no changes if you hand-roll. |
| `webxr-bridge/README.md` | New, walkthrough. |
| `webxr-bridge/PROTOCOL.md` | New, JSON schema v1. |
| `webxr-bridge/extension/manifest.json` | New. MV3, no nativeMessaging. |
| `webxr-bridge/extension/src/main-world.js` | New. `navigator.xr` Proxy + `session.displayXR`. |
| `webxr-bridge/extension/src/isolated-world.js` | New. WebSocket client + postMessage relay. |
| `webxr-bridge/sample/index.html` + `sample.js` | New. three.js demo via CDN. |
| `webxr-bridge/tools/framebuffer-size-check/` | New. Step 1 smoke test. |
| *Maybe* `src/xrt/state_trackers/oxr/oxr_system.c` | **Only if step 1 clamps.** Widen `legacy_view_width_pixels/height_pixels` behind `XRT_WIDEN_LEGACY_VIEWS`. Ask user first. |

## Critical files to read while implementing

- `docs/roadmap/webxr-bridge-v2-plan.md` — canonical Phase 2 task list (2.1–2.9).
- `src/xrt/targets/webxr_bridge/main.cpp` — Phase 1 bridge host. Extends in step 4.
- `src/xrt/state_trackers/oxr/oxr_system.c:155-183` — legacy compromise branch. Only touched in step 1b.
- `docs/architecture/extension-vs-legacy.md` — background on the legacy compromise.
- `docs/specs/XR_EXT_display_info.md` — what the bridge's OpenXR session sees.
- `docs/specs/swapchain-model.md` — canvas concept (informational for `computeFramebufferSize()`).

## Verification

Four-step matrix, all must pass before commit:

1. **Smoke test on its own page (no extension, no bridge).** Load `webxr-bridge/tools/framebuffer-size-check/index.html`. Click Enter XR. Read the reported framebuffer dims. **Outcome determines sample architecture** (XRWebGLLayer vs XRProjectionLayer) and whether step 1b is needed.

2. **Bridge serves display-info on WS connect.** Start service + bridge. Open a WS client (any; `wscat` or a two-line browser console snippet) to `ws://127.0.0.1:9014`. Send `{"type":"hello","version":1,"origin":"test"}`. Confirm a `display-info` message comes back with the correct `displayPixelSize` (3840 × 2160 on a Leia SR) and both rendering modes.

3. **three.js sample renders at correct per-view dims.** Start service + bridge. Load the extension + sample page. Click Enter XR. Visual check: the scene is **not** horizontally squeezed in the 3D mode (the current pre-fix failure mode). Compare a screenshot of the service-hosted window against a reference screenshot of `cube_hosted_d3d11_win` running in the same mode. Geometry should look correct.

4. **Mode change mid-session re-settles cleanly.** Same session as (3). Focus the service compositor window. Press `V` or `1`/`2`. Sample receives `renderingmodechange`, rebuilds its layer, frame loop re-settles within 1-2 frames. No visible glitch longer than ~30ms. Verify via:
   - Bridge stdout logs `RENDERING_MODE_CHANGED previous=X current=Y` (this already works post-#142, verified).
   - Sample page console logs `renderingmodechange` event receipt.
   - Visual check: scene renders correctly in the new mode.

If all four pass, the Phase 2 goals are met and the commit is ready.

## Out of scope (do not touch)

- Input forwarding, qwerty disable, `XR_EXT_app_owned_input`. Phase 3.
- WebXR frame pipeline (Chromium native WebXR → OpenXR → service compositor → DP). Untouched.
- `cube_hosted_d3d11_win` and every other existing IPC app. Untouched.
- macOS and Metal compositor.
- Shell mode integration. The bridge runs against non-shell service today.
- Multi-client WebSocket. Single client.
- Any DisplayXR runtime change **unless** step 1 clamps (then step 1b kicks in, and only with user sign-off).

## Things to confirm with the user before editing

- **WebSocket library choice.** Hand-rolled minimal WS vs uWebSockets vs libwebsockets. Recommendation: hand-rolled for Phase 2, re-evaluate for Phase 3.
- **JSON library choice.** `nlohmann/json` vs hand-rolled. Recommendation: hand-rolled — the schema is tiny.
- **Port 9014.** Verify it is not already in use on the dev machine. (`netstat -ano | findstr :9014`)
- **`XRT_WIDEN_LEGACY_VIEWS` env-var gate name.** If step 1 clamps, this is the cleanest no-regression path; confirm the env-var name.
- **Extension ID stability.** MV3 extensions loaded unpacked get a random ID. The bridge's origin check has to allow `chrome-extension://*` broadly, or the README has to document "copy the extension ID and set `DISPLAYXR_EXTENSION_ID` env var" before launching the bridge. Recommendation: allow any `chrome-extension://` origin for Phase 2, tighten later.

## Commit plan

One commit at the end of the phase, on `feature/webxr-bridge-v2`:

```
WebXR Bridge v2 Phase 2: WS metadata server, MV3 extension, three.js sample (#139)

- webxr_bridge: add loopback WS server on 127.0.0.1:9014, JSON protocol v1
- webxr-bridge/extension: MV3 with MAIN-world navigator.xr Proxy +
  ISOLATED-world WebSocket relay. No nativeMessaging. session.displayXR
  surface with displayInfo, renderingMode, computeFramebufferSize().
- webxr-bridge/sample: three.js scene that sizes XRWebGLLayer/XRProjectionLayer
  from session.displayXR.computeFramebufferSize() and rebuilds on
  renderingmodechange
- webxr-bridge/PROTOCOL.md + README.md
- Framebuffer-size smoke test under webxr-bridge/tools/ documenting
  Chrome's clamping behaviour

Phase 2 of docs/roadmap/webxr-bridge-v2-plan.md. Builds on Phase 1 (#139)
and #142 mode-change-event fan-out. No runtime changes.
```

If step 1b was necessary (runtime widening), split into two commits so the runtime change stays reviewable on its own:

```
1. WebXR Bridge v2 Phase 2 (part 1): widen legacy view recommendation under
   XRT_WIDEN_LEGACY_VIEWS (#139)
2. WebXR Bridge v2 Phase 2 (part 2): WS server, MV3 extension, sample (#139)
```

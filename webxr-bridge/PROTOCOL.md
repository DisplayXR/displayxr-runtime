# DisplayXR WebXR Bridge v2 — JSON Protocol v1

WebSocket transport over `ws://127.0.0.1:9014`. Single-line JSON text frames.
Both sides check `"version": 1` on every message. Future schema changes bump the integer.

## Extension → Bridge

### `hello` (required, must be first message)

```json
{ "type": "hello", "version": 1, "origin": "chrome-extension://abc123" }
```

Bridge validates the origin header. Allowed origins: `http://localhost*`, `http://127.0.0.1*`, `https://localhost*`, `https://127.0.0.1*`, `file://`, `chrome-extension://*`.

### `configure`

```json
{ "type": "configure", "version": 1, "eyePoseFormat": "raw" }
```

`eyePoseFormat`:
- `"raw"` — start streaming RAW eye poses (physical eyes, no qwerty transform). The page builds its own Kooima projection from these.
- `"none"` — stop eye pose streaming. Page uses Chrome's `view.transform` (RENDER_READY) instead.

### `request-mode`

```json
{ "type": "request-mode", "version": 1, "modeIndex": 0 }
```

Bridge calls `xrRequestDisplayRenderingModeEXT(session, modeIndex)`. Runtime processes the mode switch; both sessions receive `RENDERING_MODE_CHANGED` events. A `mode-changed` message follows asynchronously.

## Bridge → Extension

### `display-info` (sent once after `hello`)

```json
{
  "type": "display-info",
  "version": 1,
  "displayPixelSize": [3840, 2160],
  "displaySizeMeters": [0.344, 0.194],
  "recommendedViewScale": [0.5, 0.5],
  "nominalViewerPosition": [0.0, 0.1, 0.6],
  "renderingModes": [
    { "index": 0, "name": "2D", "viewCount": 1, "tileColumns": 1, "tileRows": 1, "viewScale": [1.0, 1.0], "hardware3D": false },
    { "index": 1, "name": "LeiaSR", "viewCount": 2, "tileColumns": 2, "tileRows": 1, "viewScale": [0.5, 0.5], "hardware3D": true }
  ],
  "currentModeIndex": 1,
  "views": [
    { "index": 0, "recommendedImageRectWidth": 1920, "recommendedImageRectHeight": 1080, "maxImageRectWidth": 3840, "maxImageRectHeight": 2160 },
    { "index": 1, "recommendedImageRectWidth": 1920, "recommendedImageRectHeight": 1080, "maxImageRectWidth": 3840, "maxImageRectHeight": 2160 }
  ],
  "windowInfo": {
    "valid": true,
    "windowPixelSize": [1920, 1080],
    "windowSizeMeters": [0.172, 0.097],
    "windowCenterOffsetMeters": [0.05, -0.02],
    "viewWidth": 960,
    "viewHeight": 540
  }
}
```

`windowInfo` is the live state of the service compositor window (Win32 client area) the bridge located via `FindWindowW("DisplayXRD3D11", ...)`. `valid` is `false` when the window can't be found (e.g. before a WebXR session is active). `windowCenterOffsetMeters` is the window center's physical offset from the display center (`+x` right, `+y` up). Pages doing window-relative Kooima should use `windowSizeMeters` as the screen and subtract `windowCenterOffsetMeters` from each eye's XY position before computing the asymmetric frustum (matches `test_apps/cube_handle_d3d11_win/main.cpp:342-433`).

`viewWidth` and `viewHeight` are the compositor's actual per-view tile dimensions in pixels, read from the compositor's HWND properties. These are deferred-resize-aware — they don't update mid-drag, only after the compositor finishes resizing its atlas. Pages should use these for per-tile viewport sizing instead of computing from `displayPixelSize × viewScale` or dividing the framebuffer by the tile grid. Present when the compositor has published its view dims; absent on older bridges.

### `window-info`

```json
{
  "type": "window-info",
  "version": 1,
  "windowInfo": { "valid": true, "windowPixelSize": [1920, 1080], "windowSizeMeters": [0.172, 0.097], "windowCenterOffsetMeters": [0.05, -0.02], "viewWidth": 960, "viewHeight": 540 }
}
```

Pushed by the bridge when the polled window metrics change (resize, move, or window appearing/disappearing). Polled at ~4 Hz from the event loop. The extension dispatches a `windowinfochange` event on the session with `event.detail.windowInfo`.

### `input`

Raw Win32 input event captured from the compositor window via system-wide low-level hooks (`WH_KEYBOARD_LL` / `WH_MOUSE_LL`). Only forwarded when the compositor window has focus. Compositor-side qwerty processing is disabled when the bridge is active so there's no double-handling.

```json
{ "type": "input", "version": 1, "kind": "key",
  "down": true, "code": 87, "repeat": false,
  "modifiers": { "ctrl": false, "shift": false, "alt": false } }

{ "type": "input", "version": 1, "kind": "mouse",
  "event": "down" | "up" | "move",
  "button": 0,
  "x": 123, "y": 456,
  "buttons": 1,
  "modifiers": {...} }

{ "type": "input", "version": 1, "kind": "wheel",
  "deltaY": 120,
  "x": 123, "y": 456,
  "modifiers": {...} }
```

`code` is the Win32 virtual-key code (`'A' = 65`, `VK_LEFT = 0x25`, etc.). `button` is `0`=left, `1`=right, `2`=middle. `buttons` is the held-button bitmask (`1`=L, `2`=R, `4`=M). `x`/`y` are client-area pixels in the compositor window's coordinate space (DPI-physical, matches `windowInfo.windowPixelSize`). `deltaY` is in `WHEEL_DELTA` units (+120 = one notch up).

The extension dispatches a `displayxrinput` event on the session with `event.detail` carrying the message. Pages should treat the data as the raw equivalent of cube_handle_d3d11_win's WndProc input — semantics (WASD movement, mouse look, etc.) are entirely up to the page.

### `mode-changed`

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

Sent on every `XrEventDataRenderingModeChangedEXT`. The `views` array contains refreshed view configuration dims for the new mode.

### `hardware-state-changed`

```json
{
  "type": "hardware-state-changed",
  "version": 1,
  "hardware3D": false
}
```

Sent on `XrEventDataHardwareDisplayStateChangedEXT` (physical display's 3D backlight state changed).

### `eye-poses`

```json
{
  "type": "eye-poses",
  "version": 1,
  "format": "raw",
  "eyes": [
    {
      "position": [-0.0315, 0.1, 0.6],
      "orientation": [0, 0, 0, 1],
      "fov": { "angleLeft": -0.51, "angleRight": 0.53, "angleUp": 0.27, "angleDown": -0.36 }
    },
    {
      "position": [0.0315, 0.1, 0.6],
      "orientation": [0, 0, 0, 1],
      "fov": { "angleLeft": -0.53, "angleRight": 0.51, "angleUp": 0.27, "angleDown": -0.36 }
    }
  ]
}
```

Streamed at ~100 Hz when `eyePoseFormat` is `"raw"`. Contains per-eye position, orientation (quaternion xyzw), and asymmetric FOV angles (radians) from `xrLocateViews` in RAW mode (no qwerty transform).

## Tile layout model

**Per-tile viewport sizing:** When `windowInfo.viewWidth` and `viewHeight` are available, use them directly as the per-tile viewport dimensions — they are the compositor's actual deferred-resize-aware view dims. Fall back to `displayPixelSize × viewScale` only when the fields are absent (older bridges).

**Atlas framebuffer size:** The page uses `displayPixelSize × viewScale × tileColumns/tileRows` to compute the atlas framebuffer size. For a mode with `tileColumns=2, tileRows=1, viewScale=[0.5, 0.5]` on a 3840×2160 display:

- Per-tile render rect: `viewWidth × viewHeight` (e.g. `1920 × 1080` at fullscreen)
- Atlas: `tileColumns × viewWidth` wide, `tileRows × viewHeight` tall
- Tile 0 (left eye): viewport `(0, 0, viewWidth, viewHeight)`
- Tile 1 (right eye): viewport `(viewWidth, 0, viewWidth, viewHeight)`

The page sets `gl.viewport` and `gl.scissor` per tile and renders with projection matrices derived from the bridge's eye pose FOV data. The OpenXR swapchain is larger than the atlas (fixed max allocation); the compositor crops from the top-left corner.

## MV3 WebSocket permissions

WebSocket to `127.0.0.1` from an ISOLATED world content script does not require `host_permissions` in MV3. The extension has zero permissions beyond two content scripts.

## Version semantics

Protocol version is `1`. Both bridge and extension check `version` on every incoming message. On mismatch, the message is ignored and a warning is logged. Future breaking changes bump the version integer.

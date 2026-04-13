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
  ]
}
```

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

The page uses `displayPixelSize × viewScale × tileColumns/tileRows` to compute the atlas framebuffer size and per-tile render rects. For a mode with `tileColumns=2, tileRows=1, viewScale=[0.5, 0.5]` on a 3840×2160 display:

- Per-tile render rect: `3840 × 0.5 = 1920` wide, `2160 × 0.5 = 1080` tall
- Atlas: `2 × 1920 = 3840` wide, `1 × 1080 = 1080` tall
- Tile 0 (left eye): viewport `(0, 0, 1920, 1080)`
- Tile 1 (right eye): viewport `(1920, 0, 1920, 1080)`

The page sets `gl.viewport` and `gl.scissor` per tile and renders with projection matrices derived from the bridge's eye pose FOV data.

## MV3 WebSocket permissions

WebSocket to `127.0.0.1` from an ISOLATED world content script does not require `host_permissions` in MV3. The extension has zero permissions beyond two content scripts.

## Version semantics

Protocol version is `1`. Both bridge and extension check `version` on every incoming message. On mismatch, the message is ignored and a warning is logged. Future breaking changes bump the version integer.

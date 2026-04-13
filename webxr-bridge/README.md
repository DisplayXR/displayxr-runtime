# DisplayXR WebXR Bridge v2

Metadata and control sideband for DisplayXR-aware WebXR pages. Gives browser-based WebXR apps access to display info, rendering modes, eye poses, and mode-change events — the same capabilities that native handle apps get via `XR_EXT_display_info`.

## Quick Start

### 1. Build the runtime + bridge

```bat
scripts\build_windows.bat build installer
```

### 2. Install

Uninstall any previous DisplayXR install, then run `_package\DisplayXRSetup-*.exe`.

### 3. Start the service

```powershell
Start-Process "C:\Program Files\DisplayXR\Runtime\displayxr-service.exe"
```

### 4. Start the bridge

```powershell
Start-Process "C:\Program Files\DisplayXR\Runtime\displayxr-webxr-bridge.exe"
```

The bridge connects to the service via IPC, enables `XR_EXT_display_info` + `XR_MND_headless`, and starts a WebSocket server on `127.0.0.1:9014`.

### 5. Load the Chrome extension

1. Open `chrome://extensions`
2. Enable **Developer mode** (top right)
3. Click **Load unpacked**
4. Select `<repo>/webxr-bridge/extension/`

### 6. Open the sample

Serve the sample directory via a local HTTP server:

```bat
cd webxr-bridge\sample
python -m http.server 8080
```

Open `http://localhost:8080/` in Chrome.

### 7. Click Enter XR

The sample reads `session.displayXR` for display info and rendering mode, streams RAW eye poses from the bridge, and renders a three.js scene using Kooima-style projection into per-mode tile viewports.

Press **V** in the service compositor window to cycle rendering modes. The sample receives a `renderingmodechange` event and updates its tile layout.

## Architecture

```
Chrome MV3 extension ──── WebSocket 127.0.0.1:9014 ──── displayxr-webxr-bridge.exe
  MAIN world:                JSON messages                  (headless OpenXR client
    navigator.xr Proxy       (display-info,                  + WS server)
    session.displayXR         mode-changed,                         │
  ISOLATED world:             eye-poses, ...)                       │ xrLocateViews
    WebSocket client                                                │ xrPollEvent
                                                                    ▼
Chrome WebXR session ─── OpenXR loader ─── IPC ──── displayxr-service.exe
(frames, unchanged)                                 (D3D11 service compositor)
```

Two OpenXR sessions against the same service:
- **Chrome's session** drives frames via the standard WebXR path. Does not see `session.displayXR` at the OpenXR level — the extension wraps it client-side.
- **Bridge's session** holds `XR_EXT_display_info`, polls events and eye poses, relays to WebSocket.

## Protocol

See [PROTOCOL.md](PROTOCOL.md) for the JSON message schema (v1).

## Files

```
webxr-bridge/
├── README.md              # this file
├── PROTOCOL.md            # JSON schema v1
├── extension/
│   ├── manifest.json      # MV3, no permissions, two content scripts
│   ├── icons/             # placeholder icons
│   └── src/
│       ├── main-world.js      # navigator.xr Proxy + session.displayXR
│       └── isolated-world.js  # WebSocket client + postMessage relay
├── sample/
│   ├── index.html
│   └── sample.js          # three.js via CDN
└── tools/
    └── framebuffer-size-check/
        └── index.html     # framebuffer-size smoke test
```

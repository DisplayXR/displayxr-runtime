# OpenXR Extension Design for 3D Light Field Displays

## Executive Summary

This document describes the architectural approach for implementing an OpenXR runtime for 3D light field displays (e.g., Leia displays).

**Key insights:**
1. OpenXR already supports UI composition via Quad/Cylinder layers - no special extension needed for UI
2. Monado already supports multiple sessions (up to 64 clients) with layer compositing
3. The runtime can composite all layers and write directly to framebuffer - no intermediate texture needed
4. The main extension needed is **per-session window targeting** for multi-app support
5. Locomotion uses standard OpenXR APIs once input routing is solved

---

## Problem Statement

### The 3D Display Use Case

Unlike traditional VR headsets, a 3D light field display:
- Is a **desktop monitor** with autostereoscopic 3D capability
- **Tracks the user's eyes** relative to the physical display
- Uses **keyboard/mouse/gamepad** as primary input (not motion controllers)
- Requires **interlacing** of stereo views for the 3D effect
- May run **multiple apps simultaneously** in different windows

### The Two-Pose Problem

A first-person application (e.g., FPS game) needs to combine two independent poses:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           Game World                                         │
│                                                                             │
│                    Player Character                                          │
│                    ┌─────────────────┐                                      │
│                    │                 │                                      │
│                    │  Display Origin │◄─── Keyboard/mouse/gamepad           │
│                    │  (LOCAL space)  │     moves this through the world     │
│                    │       │         │     (locomotion)                     │
│                    │       │         │                                      │
│                    │       ▼         │                                      │
│                    │   ┌───────┐     │                                      │
│                    │   │ HEAD  │◄────┼──── Eye tracking moves this          │
│                    │   │(VIEW) │     │     relative to display origin       │
│                    │   └───────┘     │     (head pose)                      │
│                    │                 │                                      │
│                    └─────────────────┘                                      │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘

Final Render Pose = LOCAL offset (locomotion) × VIEW pose (eye tracking)
```

### The Window Focus Problem

Current OpenXR runtimes create their own window, which steals input focus:

```
┌────────────────────────────────────────────────────────────────────┐
│                         Desktop                                     │
│                                                                    │
│  ┌─────────────────────┐     ┌─────────────────────────────────┐  │
│  │ Application Window  │     │ OpenXR Runtime Window           │  │
│  │                     │     │ (created by Monado)             │  │
│  │ - Game logic        │     │                                 │  │
│  │ - Wants input! ✗    │     │ - Has focus                     │  │
│  │                     │     │ - Captures keyboard/mouse ✓    │  │
│  └─────────────────────┘     └─────────────────────────────────┘  │
│                                                                    │
│  PROBLEM: App doesn't receive keyboard/mouse/gamepad input        │
└────────────────────────────────────────────────────────────────────┘
```

---

## What OpenXR/Monado Already Provides

### Multi-Session Support (Already Implemented)

Monado's multi-compositor system already supports multiple simultaneous clients:

```c
// From comp_multi_private.h
#define MULTI_MAX_CLIENTS 64  // Up to 64 simultaneous sessions!
#define MULTI_MAX_LAYERS XRT_MAX_LAYERS  // Multiple layers per client

struct multi_system_compositor {
    struct xrt_compositor_native *xcn;           // Native compositor
    struct multi_compositor *clients[MULTI_MAX_CLIENTS];  // Active sessions
};
```

### Composition Layers for UI (Already Implemented)

OpenXR supports multiple layer types that apps (Unity, WebXR, etc.) can use for UI:

```c
enum xrt_layer_type {
    XRT_LAYER_STEREO_PROJECTION,       // Main 3D scene
    XRT_LAYER_STEREO_PROJECTION_DEPTH, // With depth buffer
    XRT_LAYER_QUAD,                    // 2D UI panels in 3D space ✓
    XRT_LAYER_CYLINDER,                // Curved UI surfaces ✓
    XRT_LAYER_CUBE,                    // Cubemap layers
    XRT_LAYER_EQUIRECT1,               // Equirectangular
    XRT_LAYER_EQUIRECT2,               // Equirectangular v2
};
```

**Unity/WebXR can submit:**
- Projection layer for 3D scene
- Quad layers for floating UI panels
- Cylinder layers for curved menus

The runtime composites all layers together with proper z-ordering.

### Current Limitation: Single Output Target

```
Current Architecture (VR-centric):

┌──────────────┐  ┌──────────────┐  ┌──────────────┐
│ App 1        │  │ App 2        │  │ App 3        │
│ + UI layers  │  │ + UI layers  │  │ + UI layers  │
└──────┬───────┘  └──────┬───────┘  └──────┬───────┘
       │                 │                 │
       └────────────────┼─────────────────┘
                        ▼
         ┌──────────────────────────────┐
         │ Multi System Compositor      │
         │ (merges all layers)          │
         └──────────────┬───────────────┘
                        ▼
         ┌──────────────────────────────┐
         │ Native Compositor            │
         │ (SINGLE output target)       │  ◄── LIMITATION
         └──────────────┬───────────────┘
                        ▼
                 [Single HMD Display]
```

---

## Proposed Solution: Per-Session Window Targeting

### Architecture Overview

Extend Monado to support per-session output targets (windows):

```
Proposed Architecture (Multi-Window):

┌──────────────────┐    ┌──────────────────┐    ┌──────────────────┐
│ App 1 (Unity)    │    │ App 2 (WebXR)    │    │ App 3 (Native)   │
│                  │    │                  │    │                  │
│ ┌──────────────┐ │    │ ┌──────────────┐ │    │ ┌──────────────┐ │
│ │ Projection   │ │    │ │ Projection   │ │    │ │ Projection   │ │
│ │ layer        │ │    │ │ layer        │ │    │ │ layer        │ │
│ ├──────────────┤ │    │ ├──────────────┤ │    │ └──────────────┘ │
│ │ Quad UI      │ │    │ │ Cylinder UI  │ │    │                  │
│ │ layers       │ │    │ │ layers       │ │    │                  │
│ └──────────────┘ │    │ └──────────────┘ │    │                  │
│        │         │    │        │         │    │        │         │
│        ▼         │    │        ▼         │    │        ▼         │
│ ┌──────────────┐ │    │ ┌──────────────┐ │    │ ┌──────────────┐ │
│ │ Session 1    │ │    │ │ Session 2    │ │    │ │ Session 3    │ │
│ │ Compositor   │ │    │ │ Compositor   │ │    │ │ Compositor   │ │
│ │ + Interlacer │ │    │ │ + Interlacer │ │    │ │ + Interlacer │ │
│ └──────┬───────┘ │    │ └──────┬───────┘ │    │ └──────┬───────┘ │
│        │         │    │        │         │    │        │         │
│        ▼         │    │        ▼         │    │        ▼         │
│   [Window 1]     │    │   [Window 2]     │    │   [Window 3]     │
│   (App owns)     │    │   (Browser owns) │    │   (App owns)     │
└──────────────────┘    └──────────────────┘    └──────────────────┘
        │                       │                       │
        └───────────────────────┴───────────────────────┘
                                │
                    App receives input directly!
                    (keyboard/mouse/gamepad)
```

### Key Benefits

1. **App owns window** → App receives all input directly
2. **Runtime composites layers** → UI overlay works via standard OpenXR layers
3. **Runtime writes to framebuffer** → No intermediate texture, minimal latency
4. **Multiple sessions** → Each app gets its own window
5. **Standard OpenXR for UI** → Unity/WebXR Quad/Cylinder layers work as-is

---

## Extension Definition

### XR_EXT_session_target

Allow apps to specify their output target (window) when creating a session:

```c
#define XR_EXT_session_target 1

typedef struct XrSessionTargetCreateInfoEXT {
    XrStructureType    type;
    const void*        next;

#ifdef _WIN32
    HWND               windowHandle;
    HDC                deviceContext;    // Optional
#endif
#ifdef __linux__
    xcb_connection_t*  xcbConnection;
    xcb_window_t       xcbWindow;
    // Or for Wayland:
    struct wl_display* waylandDisplay;
    struct wl_surface* waylandSurface;
#endif
#ifdef __ANDROID__
    ANativeWindow*     nativeWindow;
#endif
} XrSessionTargetCreateInfoEXT;

// Chain to XrSessionCreateInfo
XrSessionCreateInfo sessionInfo = {
    .type = XR_TYPE_SESSION_CREATE_INFO,
    .next = &targetInfo,  // XrSessionTargetCreateInfoEXT
    ...
};
```

### Usage Example (Native App)

```c
// App creates its own window
HWND myWindow = CreateWindowEx(..., "My XR App", ...);

// App creates OpenXR session with its window as target
XrSessionTargetCreateInfoEXT targetInfo = {
    .type = XR_TYPE_SESSION_TARGET_CREATE_INFO_EXT,
    .windowHandle = myWindow,
};

XrSessionCreateInfo sessionInfo = {
    .type = XR_TYPE_SESSION_CREATE_INFO,
    .next = &targetInfo,
    .systemId = systemId,
};

xrCreateSession(instance, &sessionInfo, &session);

// App receives input from its window
while (running) {
    // Input comes directly to app's message pump
    while (PeekMessage(&msg, myWindow, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_KEYDOWN) {
            handleGameInput(msg);  // Locomotion, actions, etc.
        }
    }

    // Standard OpenXR frame loop
    xrWaitFrame(session, &frameWaitInfo, &frameState);
    xrBeginFrame(session, &frameBeginInfo);

    // Submit layers (projection + UI)
    XrCompositionLayerProjection projectionLayer = {...};
    XrCompositionLayerQuad uiLayer = {...};  // Standard OpenXR UI layer

    const XrCompositionLayerBaseHeader* layers[] = {
        (XrCompositionLayerBaseHeader*)&projectionLayer,
        (XrCompositionLayerBaseHeader*)&uiLayer,
    };

    XrFrameEndInfo frameEndInfo = {
        .layerCount = 2,
        .layers = layers,
    };

    // Runtime composites layers, interlaces, writes to app's window
    xrEndFrame(session, &frameEndInfo);
}
```

### WebXR Integration

For WebXR, the architecture is straightforward:

```
┌─────────────────────────────────────────────────────────────────────┐
│                        Browser Window                                │
│                     (Browser owns this)                              │
│                                                                     │
│  ┌───────────────────────────────────────────────────────────────┐  │
│  │                                                               │  │
│  │                      <canvas> element                         │  │
│  │                                                               │  │
│  │            OpenXR runtime renders HERE directly               │  │
│  │                   (with CNSDK interlacing)                    │  │
│  │                                                               │  │
│  └───────────────────────────────────────────────────────────────┘  │
│                                                                     │
│  ✓ Input: Browser owns window → DOM events work normally           │
│  ✓ Rendering: OpenXR writes to canvas via XR_EXT_session_target    │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

**Two problems, two solutions:**

| Problem | Solution | Status |
|---------|----------|--------|
| App needs keyboard/mouse/gamepad | Browser owns window → DOM events work | ✓ Automatic |
| OpenXR renders to browser canvas | Browser passes canvas handle to OpenXR | Requires browser mod |

### What Browser Vendors Need To Do

The browser's WebXR backend needs to use `XR_EXT_session_target`:

```c
// Current browser WebXR implementation (simplified):
requestSession() {
    // Browser tells OpenXR to create its own window
    xrCreateSession(instance, &sessionInfo, &session);
    // Problem: OpenXR owns window, steals input focus
}

// Required modification:
requestSession() {
    // Browser gets native handle for its canvas
    nativeHandle = canvas.getBackingSurface();  // Platform-specific

    // Browser passes canvas to OpenXR via our extension
    XrSessionTargetCreateInfoEXT targetInfo = {
        .type = XR_TYPE_SESSION_TARGET_CREATE_INFO_EXT,
        .windowHandle = nativeHandle,  // Render here!
    };

    XrSessionCreateInfo sessionInfo = {
        .next = &targetInfo,  // Chain the extension
        // ...
    };

    xrCreateSession(instance, &sessionInfo, &session);
    // Now: OpenXR renders to browser's canvas, browser keeps input
}
```

### WebXR Developer Experience

Once browser support exists, WebXR apps work unchanged:

```javascript
// Standard WebXR code - no modifications needed
const session = await navigator.xr.requestSession('immersive-vr');

// Input works via normal DOM events (browser owns window)
document.addEventListener('keydown', (e) => {
    if (e.key === 'w') movePlayer(0, 0, -1);
    if (e.key === 's') movePlayer(0, 0, 1);
});

// Standard WebXR frame loop
function onXRFrame(time, frame) {
    // Locomotion via offset reference space
    const transform = new XRRigidTransform(playerPos, playerRot);
    const movedSpace = localSpace.getOffsetReferenceSpace(transform);

    // Eye tracking combined with locomotion
    const pose = frame.getViewerPose(movedSpace);

    // Render scene and UI layers
    renderScene(projectionLayer, pose);
    renderUI(quadLayer);  // Standard OpenXR quad layer for UI
}
```

### Prototype Option: Electron

For prototyping before browser vendor support, use Electron:

```javascript
// main.js (Electron main process)
const { app, BrowserWindow } = require('electron');
const { initLeiaOpenXR } = require('leia-openxr-binding');

app.whenReady().then(() => {
    const win = new BrowserWindow({ width: 1920, height: 1080 });

    // Pass window handle to OpenXR (we control this)
    initLeiaOpenXR(win.getNativeWindowHandle());

    win.loadFile('index.html');  // Load WebXR app
});
```

**Electron trade-offs:**
- ✓ Works today without browser vendor cooperation
- ✓ Full native performance
- ✗ Requires packaging app (not pure web)
- ✗ ~100MB download (bundles Chromium)

---

## Implementation in Monado

### Changes Required

| Component | Current State | Change Needed |
|-----------|---------------|---------------|
| Multi-session support | ✓ Up to 64 clients | No change |
| Layer compositing | ✓ Quad, Cylinder, etc. | No change |
| CNSDK interlacing | ✓ In compositor | No change |
| Output target | Single `comp_target` | **Per-session targets** |
| Session creation | No target param | **Accept target in create info** |

### Modified comp_target Architecture

```c
// Current: Single target for all sessions
struct comp_compositor {
    struct comp_target *target;  // One for everyone
};

// Proposed: Per-session targets
struct multi_compositor {
    struct xrt_compositor_native base;
    struct multi_system_compositor *msc;

    // NEW: This session's output target
    struct comp_target *session_target;

    // Existing layer slots
    struct multi_layer_slot progress;
    struct multi_layer_slot scheduled;
    struct multi_layer_slot delivered;
};
```

### Render Loop Changes

```c
// Current: All sessions render to single target
void multi_system_compositor_render(struct multi_system_compositor *msc) {
    for (int i = 0; i < MULTI_MAX_CLIENTS; i++) {
        if (msc->clients[i]) {
            composite_client_layers(msc->xcn, msc->clients[i]);
        }
    }
    present_to_single_target(msc->xcn);
}

// Proposed: Each session renders to its own target
void multi_system_compositor_render(struct multi_system_compositor *msc) {
    for (int i = 0; i < MULTI_MAX_CLIENTS; i++) {
        struct multi_compositor *mc = msc->clients[i];
        if (mc && mc->session_target) {
            composite_client_layers(mc);      // Composite this session's layers
            interlace_with_cnsdk(mc);         // Apply 3D effect
            present_to_target(mc->session_target);  // Output to session's window
        }
    }
}
```

---

## Data Flow Summary

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              Application                                     │
│                                                                             │
│  1. App creates window, passes to xrCreateSession                          │
│  2. App handles input from its window (keyboard/mouse/gamepad)             │
│  3. App moves LOCAL reference space based on input (locomotion)            │
│  4. App submits layers:                                                    │
│     - Projection layer (3D scene)                                          │
│     - Quad/Cylinder layers (UI)                                            │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                         OpenXR Runtime (Monado)                             │
│                                                                             │
│  1. Receive layers from app                                                │
│  2. Get VIEW pose from eye tracking (CNSDK)                                │
│  3. Composite all layers with correct z-ordering                           │
│  4. Apply CNSDK interlacing for 3D effect                                  │
│  5. Write final output to app's window                                     │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           App's Window                                       │
│                                                                             │
│  - Displays interlaced 3D content + composited UI                          │
│  - Receives all input events (app's message pump)                          │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Comparison with Previous Approach

| Aspect | Previous (Shared Texture) | Current (Direct Framebuffer) |
|--------|---------------------------|------------------------------|
| **UI Overlay** | App composites after runtime | Runtime composites via layers |
| **Latency** | +1 blit (texture copy) | Direct write, minimal latency |
| **Memory** | Extra shared texture | No intermediate buffer |
| **Complexity** | App imports/composites texture | Standard OpenXR layer submission |
| **Multi-window** | Each app manages its texture | Runtime manages per-session targets |
| **Input routing** | ✓ App owns window | ✓ App owns window |
| **WebXR compat** | Good (texture sharing) | Better (matches layer model) |
| **Unity compat** | Good | Better (native layer support) |

---

## Implementation Roadmap

### Phase 1: Per-Session Targets
1. Define `XR_EXT_session_target` extension
2. Modify `comp_target` to be per-session instead of global
3. Update session creation to accept target info
4. Test with single native app

### Phase 2: Multi-Window Support
1. Enable multiple simultaneous sessions with different targets
2. Each session gets independent compositor pipeline
3. Shared CNSDK resources where possible
4. Test with multiple native apps

### Phase 3: Platform Integration
1. Windows: Win32 HWND support
2. Linux: XCB and Wayland surface support
3. Test with native OpenXR apps (C++, Rust)

### Phase 4: Framework Integration
1. Unity XR Plugin modifications (pass window handle from Unity)
2. Electron wrapper for WebXR prototyping
3. Documentation and samples

### Phase 5: WebXR Browser Support
1. Engage with browser vendors (Google, Mozilla)
2. Propose `XR_EXT_session_target` for WebXR backend
3. Browser passes canvas handle → OpenXR renders to it

---

## Conclusion

The 3D light field display use case is well-served by extending Monado's existing architecture:

| Requirement | Solution |
|-------------|----------|
| Eye tracking → VIEW pose | ✓ CNSDK integration (exists) |
| Interlacing for 3D | ✓ CNSDK interlacer (exists) |
| UI overlay | ✓ Standard OpenXR Quad/Cylinder layers (exists) |
| Layer compositing | ✓ Multi-compositor (exists) |
| Multiple sessions | ✓ Up to 64 clients (exists) |
| Input to app | **Per-session window targeting (new)** |
| Multiple windows | **Per-session output targets (new)** |
| Locomotion | ✓ Standard OpenXR offset spaces (exists) |

**Primary extension needed:** `XR_EXT_session_target` to allow apps to specify their output window.

### Native Apps (Unity, Unreal, Custom)

Works with the extension directly:
- App creates window, passes handle via `XR_EXT_session_target`
- App receives input from its window (keyboard/mouse/gamepad)
- App submits Projection + Quad/Cylinder layers
- Runtime composites, interlaces, writes to app's window

### WebXR Apps

Same architecture, browser passes canvas handle:
- Browser owns window → input works via DOM events (automatic)
- Browser passes canvas handle to OpenXR via `XR_EXT_session_target`
- Runtime renders directly to browser's canvas
- **Requires:** Browser vendor to modify WebXR backend

**Prototype path:** Electron wrapper while waiting for browser support

### Priority Order

1. **Native app support first** - Direct path, proves architecture
2. **Unity integration** - Large ecosystem
3. **Electron WebXR** - Enables web content without browser changes
4. **Browser WebXR** - Requires browser vendor cooperation (Google, Mozilla)

# OpenXR Extension Design for 3D Light Field Displays

## Executive Summary

This document describes the architectural approach for implementing an OpenXR runtime for 3D light field displays (e.g., Leia displays) using **SR Runtime** (Simulated Reality SDK) for eye tracking and interlacing.

**Key insights:**
1. OpenXR already supports UI composition via Quad/Cylinder layers - no special extension needed for UI
2. Monado already supports multiple sessions (up to 64 clients) with layer compositing
3. SR Runtime integration already exists in Monado (PR #4 merged) for weaving/interlacing
4. The main extension needed is **per-session window targeting** for multi-app support
5. Locomotion uses standard OpenXR APIs once input routing is solved

**Technology Stack:**
- **OpenXR Runtime:** Monado (modified)
- **3D Interlacing:** SR Runtime (`SR::IVulkanWeaver1`)
- **Eye Tracking:** SR Runtime (`SR::SRContext`)
- **Graphics API:** Vulkan

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

### Architecture Overview (Simplified by SR Windowed Mode)

SR Runtime's **windowed mode** (runtime >= 1.34) makes this straightforward:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              Application                                     │
│                                                                             │
│   1. App creates its own window (HWND)                                      │
│   2. App passes HWND to xrCreateSession via XR_EXT_session_target           │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
                                     │
                                     ▼ HWND
┌─────────────────────────────────────────────────────────────────────────────┐
│                         OpenXR Runtime (Monado)                             │
│                                                                             │
│   HWND ──┬──► comp_target (VkSwapchain from HWND)                          │
│          │                                                                  │
│          └──► SR Weaver (CreateSRWeaver with HWND = windowed mode)         │
│                                                                             │
│   Render Pipeline:                                                          │
│   ┌─────────────┐    ┌─────────────┐    ┌─────────────┐    ┌────────────┐  │
│   │ App submits │ ─► │ Compositor  │ ─► │ SR Weaver   │ ─► │ Present to │  │
│   │ layers      │    │ composites  │    │ interlaces  │    │ app window │  │
│   └─────────────┘    └─────────────┘    └─────────────┘    └────────────┘  │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
                                     │
                                     ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           App's Window                                       │
│                                                                             │
│   ✓ Displays interlaced 3D content                                         │
│   ✓ App receives all input (owns window)                                   │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Multi-App Support

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              Desktop                                         │
│                                                                             │
│   ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐        │
│   │ App 1 Window    │    │ App 2 Window    │    │ Other Windows   │        │
│   │ (Unity game)    │    │ (WebXR browser) │    │ (non-XR apps)   │        │
│   │                 │    │                 │    │                 │        │
│   │ Session 1       │    │ Session 2       │    │                 │        │
│   │ SR Weaver 1     │    │ SR Weaver 2     │    │                 │        │
│   │ (windowed)      │    │ (windowed)      │    │                 │        │
│   │                 │    │                 │    │                 │        │
│   └─────────────────┘    └─────────────────┘    └─────────────────┘        │
│         │                       │                                           │
│         ▼                       ▼                                           │
│   App 1 gets input        App 2 gets input                                 │
│   (keyboard/mouse)        (DOM events)                                     │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Key Benefits

1. **App owns window** → App receives all input directly (keyboard/mouse/gamepad)
2. **SR windowed mode** → Each session has its own SR weaver bound to its window
3. **No fullscreen takeover** → Multiple XR apps can run alongside regular apps
4. **Standard OpenXR layers** → UI overlay works via Quad/Cylinder layers
5. **Minimal changes** → Just pass HWND to SR weaver and Vulkan surface

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
│  │                   (with SR Runtime interlacing)               │  │
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
| SR Runtime interlacing | ✓ `leiasr_weave()` in compositor | Pass window handle to weaver |
| Output target | Single `comp_target` | **Per-session targets** |
| Session creation | No target param | **Accept target in create info** |

### SR Runtime Integration (Already Merged - PR #4)

The SR Runtime weaver is already integrated:

```c
// src/xrt/drivers/leiasr/leiasr.cpp

// Current: Window handle passed as NULL (fullscreen mode)
CreateSRWeaver(sr->context, device, physicalDevice, graphicsQueue,
               commandPool, NULL, sr);  // <-- NULL = fullscreen mode
                           ^^^^

// Weaving function accepts framebuffer as parameter
void leiasr_weave(struct leiasr* leiasr,
                  VkCommandBuffer commandBuffer,
                  VkImageView leftImageView,
                  VkImageView rightImageView,
                  VkRect2D viewport,
                  int imageWidth, int imageHeight, VkFormat imageFormat,
                  VkFramebuffer framebuffer,      // <-- Output target
                  int framebufferWidth, int framebufferHeight,
                  VkFormat framebufferFormat);
```

### SR Runtime Windowed Mode (Key Enabler)

**Critical capability:** SR Runtime supports **windowed mode** weaving (runtime >= 1.34), not just fullscreen:

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           Desktop                                            │
│                                                                             │
│   ┌─────────────────┐    ┌─────────────────┐    ┌─────────────────┐        │
│   │ App 1 Window    │    │ App 2 Window    │    │ Other Windows   │        │
│   │                 │    │                 │    │ (non-XR)        │        │
│   │ SR Weaver 1     │    │ SR Weaver 2     │    │                 │        │
│   │ (windowed)      │    │ (windowed)      │    │                 │        │
│   │                 │    │                 │    │                 │        │
│   │ 3D interlaced   │    │ 3D interlaced   │    │ 2D normal       │        │
│   └─────────────────┘    └─────────────────┘    └─────────────────┘        │
│                                                                             │
└─────────────────────────────────────────────────────────────────────────────┘
```

| Feature | Fullscreen (HWND=NULL) | Windowed (HWND=window) |
|---------|------------------------|------------------------|
| Multiple XR apps | ✗ One at a time | ✓ Multiple simultaneous |
| Input routing | Runtime captures | ✓ App receives directly |
| Desktop integration | ✗ Exclusive | ✓ Works with other windows |

**This solves our core problems:**
1. **Input routing** - App owns window → app receives keyboard/mouse/gamepad
2. **Multi-app** - Each app has its own SR weaver instance in windowed mode
3. **No complex extension** - Just pass HWND to `CreateSRWeaver()`

**What we need to change:**
1. Pass the window handle during weaver creation (currently NULL)
2. Create `comp_target` from external window handle (for Vulkan swapchain)

### comp_target: Accept External Window

The `comp_target` abstraction already exists - we just need to add a path for external windows:

```c
// comp_window_mswin.c - Add external window support

struct comp_window_mswin {
    struct comp_target_swapchain base;
    HWND window;
    bool owns_window;         // NEW: false if external
    // ...
};

// New function to create from external HWND
bool comp_window_mswin_create_from_external(
    struct comp_compositor *c,
    HWND external_hwnd,
    struct comp_target **out_ct)
{
    struct comp_window_mswin *cwm = U_TYPED_CALLOC(struct comp_window_mswin);
    cwm->window = external_hwnd;
    cwm->owns_window = false;  // Don't destroy on cleanup

    // Create VkSurfaceKHR from HWND (standard Vulkan)
    VkWin32SurfaceCreateInfoKHR info = {
        .sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR,
        .hwnd = external_hwnd,
        .hinstance = GetModuleHandle(NULL),
    };
    vkCreateWin32SurfaceKHR(instance, &info, NULL, &surface);

    // Rest of swapchain setup is unchanged
    // ...
}
```

**Key insight:** The Vulkan swapchain creation doesn't care who created the window - it just needs a valid HWND. We're not changing Vulkan, just where the HWND comes from.

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
│  2. Get VIEW pose from eye tracking (SR Runtime: SR::SRContext)            │
│  3. Composite all layers with correct z-ordering                           │
│  4. Apply SR Runtime interlacing (SR::IVulkanWeaver1::weave())             │
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

### Phase 1: Single App with External Window (Windows) - IMPLEMENTED

**Status:** ✅ Complete

**Goal:** Accept external HWND, SR weaver in windowed mode

#### Implementation Summary

The following changes were made to implement XR_EXT_session_target support:

**1. Extension Definition** (`src/external/openxr_includes/openxr/XR_EXT_session_target.h`):
```c
#define XR_EXT_session_target 1
#define XR_TYPE_SESSION_TARGET_CREATE_INFO_EXT ((XrStructureType)1000999001)

typedef struct XrSessionTargetCreateInfoEXT {
    XrStructureType             type;
    const void* XR_MAY_ALIAS    next;
    void*                       windowHandle;  // HWND on Windows
} XrSessionTargetCreateInfoEXT;
```

**2. Core Interface** (`src/xrt/include/xrt/xrt_compositor.h`):
```c
struct xrt_session_info {
    bool is_overlay;
    uint64_t flags;
    uint32_t z_order;
    void *external_window_handle;  // NEW: External window from extension
};
```

**3. Leiasr Driver** (`src/xrt/drivers/leiasr/leiasr.h`, `leiasr.cpp`):
```c
// Added windowHandle parameter - NULL = fullscreen, valid HWND = windowed
xrt_result_t leiasr_create(..., void *windowHandle, struct leiasr **out);

// In leiasr.cpp: passes windowHandle to CreateSRWeaver
CreateSRWeaver(sr->context, ..., (HWND)windowHandle, sr);
```

**4. Compositor Window** (`src/xrt/compositor/main/comp_window_mswin.c`):
```c
// New function to create comp_target from external HWND
bool comp_window_mswin_create_from_external(
    struct comp_compositor *c,
    void *external_hwnd,
    struct comp_target **out_ct);

// Added owns_window flag to struct - false prevents window destruction on cleanup
```

**5. Compositor** (`src/xrt/compositor/main/comp_compositor.h`, `comp_compositor.c`):
- Added `external_window_handle` field to `comp_compositor` struct
- Modified `compositor_begin_session()` to use external HWND when available
- Added `compositor_init_window_from_external()` helper function

**6. Multi-Compositor Bridge** (`src/xrt/compositor/multi/comp_multi_private.h`, `comp_multi_compositor.c`):
- Added `external_window_handle` to `multi_system_compositor` struct
- Modified `multi_compositor_begin_session()` to pass HWND from session info to native compositor

**7. Session Creation** (`src/xrt/state_trackers/oxr/oxr_session.c`):
```c
// Parse XR_EXT_session_target extension
const XrSessionTargetCreateInfoEXT *target_info = OXR_GET_INPUT_FROM_CHAIN(
    createInfo, XR_TYPE_SESSION_TARGET_CREATE_INFO_EXT, XrSessionTargetCreateInfoEXT);
if (target_info && target_info->windowHandle) {
    xsi.external_window_handle = (void *)target_info->windowHandle;
}
```

**8. Renderer** (`src/xrt/compositor/main/comp_renderer.c`):
```c
// Pass window handle to leiasr_create (NULL = fullscreen, HWND = windowed)
void *window_handle = r->c->external_window_handle;
leiasr_create(..., window_handle, &r->leiasr);
```

#### Internal Data Flow (Verified)

```
┌──────────────────────────────────────────────────────────────────────┐
│  OpenXR App                                                          │
│    xrCreateSession(XrSessionTargetCreateInfoEXT { hwnd })            │
└───────────────────────────────────┬──────────────────────────────────┘
                                    │
                                    ▼
┌──────────────────────────────────────────────────────────────────────┐
│  oxr_session.c:oxr_session_create()                                  │
│    - Parses XrSessionTargetCreateInfoEXT from extension chain        │
│    - Populates xrt_session_info.external_window_handle               │
└───────────────────────────────────┬──────────────────────────────────┘
                                    │
                                    ▼
┌──────────────────────────────────────────────────────────────────────┐
│  comp_multi_compositor.c:multi_compositor_create(xsi)                │
│    - Creates per-session multi_compositor                            │
│    - Stores mc->xsi = *xsi (including external_window_handle)        │
└───────────────────────────────────┬──────────────────────────────────┘
                                    │
                                    ▼
┌──────────────────────────────────────────────────────────────────────┐
│  App calls xrBeginSession()                                          │
│    - multi_compositor_begin_session()                                │
│    - Detects first session with external HWND                        │
│    - Passes HWND to comp_compositor.external_window_handle           │
└───────────────────────────────────┬──────────────────────────────────┘
                                    │
                                    ▼
┌──────────────────────────────────────────────────────────────────────┐
│  comp_compositor.c:compositor_begin_session() (deferred surface)     │
│    - If external_window_handle is set:                               │
│      - compositor_init_window_from_external(HWND)                    │
│      - Creates comp_target via comp_window_mswin_create_from_external│
│      - compositor_init_renderer() → leiasr_create(HWND)              │
└──────────────────────────────────────────────────────────────────────┘
```

**Files Modified:**
| File | Change |
|------|--------|
| `src/external/openxr_includes/openxr/XR_EXT_session_target.h` | NEW: Extension header |
| `src/xrt/include/xrt/xrt_openxr_includes.h` | Include new extension header |
| `src/xrt/include/xrt/xrt_compositor.h` | Add external_window_handle to xrt_session_info |
| `src/xrt/drivers/leiasr/leiasr.h` | Add windowHandle parameter |
| `src/xrt/drivers/leiasr/leiasr.cpp` | Pass windowHandle to CreateSRWeaver |
| `src/xrt/compositor/main/comp_window.h` | Declare create_from_external function |
| `src/xrt/compositor/main/comp_window_mswin.c` | Add owns_window flag, external window creation |
| `src/xrt/compositor/main/comp_compositor.h` | Add external_window_handle field |
| `src/xrt/compositor/main/comp_compositor.c` | Handle external HWND in begin_session |
| `src/xrt/compositor/main/comp_renderer.c` | Pass window handle to leiasr_create |
| `src/xrt/compositor/multi/comp_multi_private.h` | Add external_window_handle to msc |
| `src/xrt/compositor/multi/comp_multi_compositor.c` | Bridge HWND from session to native compositor |
| `src/xrt/state_trackers/oxr/oxr_session.c` | Parse XR_EXT_session_target extension |

**Test:** Native Win32 app creates window, passes to OpenXR, receives input, sees 3D

---

### Phase 2: Per-Session SR Weaver
**Goal:** Multiple apps, each with own window and SR weaver

**Move SR weaver to per-session:**
```c
// comp_multi_private.h
struct multi_compositor {
    struct xrt_compositor_native base;
    struct multi_system_compositor *msc;

    // NEW: Per-session resources
    struct comp_target *session_target;
    struct leiasr *session_weaver;  // SR weaver for this session's window

    // Existing
    struct multi_layer_slot progress, scheduled, delivered;
};
```

**Update render loop:**
```c
// Each session renders to its own target with its own weaver
for (int i = 0; i < MULTI_MAX_CLIENTS; i++) {
    struct multi_compositor *mc = msc->clients[i];
    if (mc && mc->session_target) {
        composite_client_layers(mc);
        leiasr_weave(mc->session_weaver, ...);  // Per-session weaver
        present_to_target(mc->session_target);
    }
}
```

**Test:** Two native apps running simultaneously, each with 3D in their window

---

### Phase 3: Framework Integration
**Goal:** Unity and Electron support

**Unity XR Plugin:**
- Modify OpenXR plugin to expose window handle
- Pass Unity's HWND via `XR_EXT_session_target`
- Test with Unity sample project

**Electron wrapper:**
```javascript
// leia-openxr-binding npm package
const { BrowserWindow } = require('electron');
const win = new BrowserWindow({...});
const hwnd = win.getNativeWindowHandle();

// Pass to OpenXR session
initLeiaOpenXR(hwnd);
```

**Test:** Unity game and Electron WebXR app both working

---

### Phase 4: Linux Platform Support (Optional)
**Goal:** XCB/Wayland external window support

- Extend `comp_window_xcb.c` to accept external window
- Note: SR Runtime may have different Linux support - verify first

---

### Phase 5: WebXR Browser Support (Future)
**Goal:** Browser vendor cooperation

- Propose `XR_EXT_session_target` to Khronos/browser vendors
- Browser passes canvas backing surface as window handle
- Same architecture, just different source of HWND equivalent

---

## Conclusion

### The Key Insight: SR Windowed Mode

SR Runtime's **windowed mode** (passing HWND to `CreateSRWeaver`) is the key enabler:

| Without Windowed Mode | With Windowed Mode |
|-----------------------|-------------------|
| SR weaver takes fullscreen | SR weaver renders to specific window |
| One XR app at a time | Multiple XR apps simultaneously |
| Runtime owns display | App owns its window |
| Complex input routing needed | App receives input directly |

### What's Already Done

| Component | Status |
|-----------|--------|
| SR Runtime integration | ✓ Merged (PR #4) |
| Vulkan weaver (`IVulkanWeaver1`) | ✓ Working |
| Eye tracking (`SR::SRContext`) | ✓ Working |
| Layer compositing | ✓ Monado multi-compositor |
| OpenXR layers (Quad/Cylinder) | ✓ Standard OpenXR |

### What Needs Implementation

| Change | Effort | Files |
|--------|--------|-------|
| Add HWND param to `leiasr_create()` | Small | `leiasr.h`, `leiasr.cpp` |
| Accept external HWND in `comp_window_mswin` | Medium | `comp_window_mswin.c` |
| Parse `XR_EXT_session_target` extension | Medium | `oxr_session.c` |
| Per-session SR weaver (Phase 2) | Medium | `comp_multi_private.h`, render loop |

### Phase 1 Deliverable

**Single native app with external window:**
```
App creates HWND → passes to xrCreateSession → SR weaver in windowed mode → 3D output
     ↑                                                                           │
     └─────────────── App receives keyboard/mouse input ←────────────────────────┘
```

### Architecture Summary

```
┌─────────────────────────────────────────────────────────────────────┐
│ App creates window (HWND)                                           │
│         │                                                           │
│         ▼                                                           │
│ xrCreateSession + XR_EXT_session_target(HWND)                       │
│         │                                                           │
│         ├──► comp_target (VkSwapchain from HWND)                    │
│         │                                                           │
│         └──► SR Weaver (windowed mode, bound to HWND)               │
│                    │                                                │
│                    ▼                                                │
│         Interlaced 3D output to app's window                        │
│                                                                     │
│ App receives input ◄─── Window message pump (app owns window)       │
└─────────────────────────────────────────────────────────────────────┘
```

**This is a minimal, clean solution** that leverages SR Runtime's existing windowed mode capability rather than fighting against a fullscreen-only architecture.

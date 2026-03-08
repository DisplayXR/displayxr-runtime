# XR_EXT_win32_window_binding

| Property | Value |
|----------|-------|
| Extension Name | `XR_EXT_win32_window_binding` |
| Spec Version | 1 |
| Type Values | `XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT` (1000999001), `XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT` (1000999002) |
| Author | Leia Inc. |
| Platform | Windows (Win32). Linux/Android reserved for future use. |

---

## 1. Overview

`XR_EXT_win32_window_binding` allows an OpenXR application to provide its own window handle (HWND on Windows) to the runtime when creating a session. When present, the runtime renders into the application's window instead of creating its own, and the application retains control of input, lifecycle, and the message pump. The extension also defines `XrCompositionLayerWindowSpaceEXT`, a composition layer type positioned in fractional window coordinates for HUD/UI overlays that scale automatically with the window.

The target use case is **desktop 3D light field displays** (autostereoscopic monitors) where the user interacts via keyboard, mouse, and gamepad rather than VR controllers.

---

## 2. Motivation

### 2.1 Desktop 3D Display Use Case

Unlike a VR headset, a 3D light field display is a desktop monitor with autostereoscopic capability. It:

- **Tracks the user's eyes** relative to the physical display panel
- Uses **keyboard, mouse, and gamepad** as primary input
- Requires **interlacing** (weaving) of stereo views for the lenticular 3D effect
- May run **multiple apps simultaneously** in separate windows on the desktop

Standard OpenXR was designed for VR headsets, where the runtime owns the display and input is routed through the XR system. None of those assumptions hold for a desktop 3D monitor.

### 2.2 The Window Focus Problem

When an OpenXR runtime creates its own window, that window receives input focus. The application's window, which needs keyboard/mouse input for gameplay or UI, loses focus:

```
┌─────────────────────────────────────────────────────────────────┐
│                          Desktop                                  │
│                                                                 │
│  ┌──────────────────────┐     ┌──────────────────────────────┐  │
│  │ Application Window   │     │ OpenXR Runtime Window        │  │
│  │                      │     │ (created by Monado)          │  │
│  │ - Game logic         │     │                              │  │
│  │ - Wants input        │     │ - Has focus                  │  │
│  │ - Does NOT get it    │     │ - Captures keyboard/mouse    │  │
│  └──────────────────────┘     └──────────────────────────────┘  │
│                                                                 │
│  PROBLEM: App cannot receive keyboard/mouse/gamepad input       │
└─────────────────────────────────────────────────────────────────┘
```

By letting the app provide its own window, focus stays with the app and input routing works naturally through the normal Win32 message pump.

### 2.3 The Two-Pose Problem

A first-person application on a 3D display needs to combine two independent poses:

```
┌──────────────────────────────────────────────────────────────────┐
│                        Game World                                  │
│                                                                  │
│                 Player Character                                   │
│                 ┌──────────────────┐                               │
│                 │                  │                               │
│                 │  Display Origin  │<── Keyboard/mouse/gamepad     │
│                 │  (LOCAL space)   │    moves this through world   │
│                 │       |          │    (locomotion)               │
│                 │       v          │                               │
│                 │   ┌────────┐    │                               │
│                 │   │  HEAD  │<───┼── Eye tracking moves this     │
│                 │   │ (VIEW) │    │   relative to display origin   │
│                 │   └────────┘    │   (head pose)                 │
│                 │                  │                               │
│                 └──────────────────┘                               │
│                                                                  │
│  Final Render Pose = LOCAL offset (locomotion) x VIEW (tracking)  │
└──────────────────────────────────────────────────────────────────┘
```

**Locomotion** (moving through the world) is driven by app input. **Eye tracking** (stereoscopic parallax) is driven by the runtime's SR SDK. Both must compose correctly, which only works when the app receives input — which requires the app to own the window.

### 2.4 Why Window-Space Layers Require Session Targeting

In standard OpenXR, every composition layer is positioned in 3D space relative to a reference space. "Window coordinates" have no meaning because the runtime may not even have a window.

When a session is created with `XR_EXT_win32_window_binding`, a contract is established: a window exists, it has pixel dimensions, and those dimensions are known to the runtime. This makes fractional window coordinates meaningful and allows `XrCompositionLayerWindowSpaceEXT` to position a HUD overlay as "30% from the left edge, 70% of the window width" — coordinates that automatically adapt when the window is resized.

The two concepts are inseparable: window-space layers only make sense when there is a window, and session targeting is what guarantees there is one.

---

## 3. API Reference

### 3.1 Defines

```c
#define XR_EXT_win32_window_binding                          1
#define XR_EXT_win32_window_binding_SPEC_VERSION             1
#define XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME           "XR_EXT_win32_window_binding"
#define XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT         ((XrStructureType)1000999001)
#define XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT     ((XrStructureType)1000999002)
```

### 3.2 XrWin32WindowBindingCreateInfoEXT

```c
typedef struct XrWin32WindowBindingCreateInfoEXT {
    XrStructureType             type;           // Must be XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT
    const void* XR_MAY_ALIAS    next;           // Pointer to next structure in chain
    void*                       windowHandle;   // HWND of the target window (Windows)
} XrWin32WindowBindingCreateInfoEXT;
```

**Chaining:** This structure is placed in the `next` chain of `XrSessionCreateInfo`.

**Fields:**

| Field | Description |
|-------|-------------|
| `type` | Must be `XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT` |
| `next` | `NULL` or pointer to next structure in chain |
| `windowHandle` | Platform window handle. On Windows, cast from `HWND`. Must be a valid window that remains alive for the session's lifetime. |

**Semantics:**

- When present and `windowHandle` is non-NULL, the runtime renders into the specified window. The application owns the window and is responsible for its lifecycle, message pump, and input handling.
- The runtime creates its graphics resources (swapchain, weaver) bound to this window.
- The application must not destroy the window before calling `xrDestroySession`.

**Fallback when absent:**

When this structure is not in the chain (or `windowHandle` is NULL), the runtime falls back to its default behavior: creating its own window in fullscreen mode. Existing applications work without modification.

### 3.3 XrCompositionLayerWindowSpaceEXT

```c
typedef struct XrCompositionLayerWindowSpaceEXT {
    XrStructureType             type;       // Must be XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT
    const void* XR_MAY_ALIAS    next;       // Pointer to next structure in chain
    XrCompositionLayerFlags     layerFlags; // e.g. XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT
    XrSwapchainSubImage         subImage;   // Source swapchain + rect
    float                       x;          // Left edge, fraction of window width  [0..1]
    float                       y;          // Top edge, fraction of window height   [0..1]
    float                       width;      // Fraction of window width  [0..1]
    float                       height;     // Fraction of window height [0..1]
    float                       disparity;  // Horizontal shift, fraction of window width
} XrCompositionLayerWindowSpaceEXT;
```

**Fields:**

| Field | Description |
|-------|-------------|
| `type` | Must be `XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT` |
| `next` | `NULL` or pointer to next structure in chain |
| `layerFlags` | Standard composition layer flags. Use `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` for alpha-blended overlays. |
| `subImage` | Identifies the source swapchain, image index, and sub-rectangle to sample from |
| `x`, `y` | Position of the layer's top-left corner as a fraction of the window dimensions (0.0 = left/top edge, 1.0 = right/bottom edge) |
| `width`, `height` | Size of the layer as a fraction of the window dimensions |
| `disparity` | Horizontal pixel shift between left and right eye views, expressed as a fraction of window width. `0.0` = at screen depth, negative values push toward the viewer |

**Validity requirement:** The session must have been created with `XrWin32WindowBindingCreateInfoEXT` providing a valid window handle. Submitting this layer type on a session without a target window is undefined behavior.

**Rendering behavior:**

- The layer is rendered **pre-interlace** — it passes through the SR weaver like any other layer, so it participates correctly in the lenticular 3D effect
- The same texture is composited into both eye views with the per-eye horizontal shift controlled by `disparity`
- When `layerFlags` includes `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`, the layer is alpha-blended over whatever is behind it (typically the projection layer)
- Coordinates are fractional, so the layer automatically scales when the window is resized

**Use cases:** HUD overlays, debug text, UI panels, health bars, crosshairs — anything that should be anchored to the window rather than to 3D space.

### 3.4 Internal Types

These are internal runtime types that implement the extension. They are not part of the OpenXR API surface but are documented here for runtime developers.

**`XRT_LAYER_WINDOW_SPACE`** — Enum value in `enum xrt_layer_type` (`src/xrt/include/xrt/xrt_compositor.h:86`) that identifies a window-space layer in the internal compositor pipeline.

**`xrt_layer_window_space_data`** — Internal struct (`src/xrt/include/xrt/xrt_compositor.h:395`) carrying the fractional position, size, and disparity:

```c
struct xrt_layer_window_space_data
{
    struct xrt_sub_image sub;
    float x;         // Left edge, fraction of window width [0..1]
    float y;         // Top edge, fraction of window height [0..1]
    float width;     // Fraction of window width [0..1]
    float height;    // Fraction of window height [0..1]
    float disparity; // Horizontal shift, fraction of window width
};
```

**`xrt_comp_layer_window_space()`** — Inline helper (`src/xrt/include/xrt/xrt_compositor.h:1865`) that dispatches a window-space layer through the compositor's vtable.

---

## 4. Usage Examples

### 4.1 Basic Session Targeting

```c
// 1. Create a Win32 window
HWND myWindow = CreateWindowEx(
    0, L"MyAppClass", L"My XR App",
    WS_OVERLAPPEDWINDOW,
    CW_USEDEFAULT, CW_USEDEFAULT, 1280, 720,
    NULL, NULL, hInstance, NULL);
ShowWindow(myWindow, SW_SHOW);

// 2. Chain XrWin32WindowBindingCreateInfoEXT to xrCreateSession
XrWin32WindowBindingCreateInfoEXT targetInfo = {
    .type = XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT,
    .next = NULL,
    .windowHandle = (void*)myWindow,
};

XrSessionCreateInfo sessionInfo = {
    .type = XR_TYPE_SESSION_CREATE_INFO,
    .next = &targetInfo,
    .systemId = systemId,
};

XrSession session;
xrCreateSession(instance, &sessionInfo, &session);

// 3. Run frame loop — app receives input from its own window
while (running) {
    MSG msg;
    while (PeekMessage(&msg, myWindow, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
        if (msg.message == WM_KEYDOWN) {
            handleGameInput(msg);  // Locomotion, actions, etc.
        }
    }

    XrFrameWaitInfo frameWaitInfo = {.type = XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState frameState = {.type = XR_TYPE_FRAME_STATE};
    xrWaitFrame(session, &frameWaitInfo, &frameState);
    xrBeginFrame(session, NULL);

    // Submit projection layer
    XrCompositionLayerProjection projLayer = { /* ... */ };
    const XrCompositionLayerBaseHeader *layers[] = {
        (XrCompositionLayerBaseHeader*)&projLayer,
    };

    XrFrameEndInfo endInfo = {
        .type = XR_TYPE_FRAME_END_INFO,
        .displayTime = frameState.predictedDisplayTime,
        .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
        .layerCount = 1,
        .layers = layers,
    };

    xrEndFrame(session, &endInfo);
}
```

### 4.2 Submitting a Window-Space HUD

```c
// Create a swapchain for the HUD texture
XrSwapchainCreateInfo hudSwapchainInfo = {
    .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
    .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT |
                  XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
    .format = DXGI_FORMAT_R8G8B8A8_UNORM,
    .width = 512,
    .height = 128,
    .sampleCount = 1,
    .faceCount = 1,
    .arraySize = 1,
    .mipCount = 1,
};
XrSwapchain hudSwapchain;
xrCreateSwapchain(session, &hudSwapchainInfo, &hudSwapchain);

// Each frame: render text into HUD swapchain, then submit both layers
XrCompositionLayerProjection projLayer = { /* ... 3D scene ... */ };

XrCompositionLayerWindowSpaceEXT hudLayer = {
    .type = XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT,
    .layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT,
    .subImage = {
        .swapchain = hudSwapchain,
        .imageRect = { .offset = {0, 0}, .extent = {512, 128} },
        .imageArrayIndex = 0,
    },
    .x = 0.02f,        // 2% from left edge
    .y = 0.90f,        // 90% from top (near bottom)
    .width = 0.40f,    // 40% of window width
    .height = 0.08f,   // 8% of window height
    .disparity = 0.0f, // At screen depth
};

const XrCompositionLayerBaseHeader *layers[] = {
    (XrCompositionLayerBaseHeader*)&projLayer,
    (XrCompositionLayerBaseHeader*)&hudLayer,
};

XrFrameEndInfo endInfo = {
    .type = XR_TYPE_FRAME_END_INFO,
    .layerCount = 2,
    .layers = layers,
    /* ... */
};
xrEndFrame(session, &endInfo);
```

### 4.3 Window Drag Handling (WM_PAINT Trick)

When the user drags or resizes a Win32 window, Windows enters a modal message loop inside `DefWindowProc` that blocks the application's normal frame loop. To keep rendering during drag, applications should use the same pattern as the SR SDK's native examples:

```cpp
static bool g_isMoving = false;

LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg) {
    case WM_ENTERSIZEMOVE:
        g_isMoving = true;
        InvalidateRect(hWnd, nullptr, false);  // Force WM_PAINT
        return 0;

    case WM_EXITSIZEMOVE:
        g_isMoving = false;
        return 0;

    case WM_PAINT:
        if (g_isMoving) {
            // Run one OpenXR frame inside the modal drag loop
            run_one_openxr_frame();
            // Don't call BeginPaint/EndPaint — window stays invalidated
            // Windows keeps sending WM_PAINT — continuous rendering
            return 0;
        }
        break;  // Fall through to DefWindowProc when not dragging

    case WM_GETMINMAXINFO: {
        MINMAXINFO *mmi = (MINMAXINFO *)lParam;
        mmi->ptMinTrackSize.x = 100;
        mmi->ptMinTrackSize.y = 100;
        return 0;
    }
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}
```

**Why not use a dedicated render thread?** The D3D11 immediate device context is not thread-safe. Both the render thread (calling `weave()`) and the main thread (via `DefWindowProc` and DXGI housekeeping) would touch the same context concurrently, causing crashes — particularly on mouse-up events where `ReleaseCapture()` triggers synchronous messages that re-enter the D3D11 context. Even with `ID3D11Multithread::SetMultithreadProtected(TRUE)`, the SR SDK's internal weaver state has no synchronization. The single-threaded WM_PAINT approach eliminates all data races by design.

### 4.4 Fallback Behavior

When the `XrWin32WindowBindingCreateInfoEXT` structure is absent from the `next` chain (or `windowHandle` is NULL), the runtime uses its pre-existing behavior:

| Condition | Render Path |
|-----------|-------------|
| HWND provided | Per-session: `render_per_session_clients_locked()` in `comp_multi_system.c` |
| HWND is NULL | Shared: `transfer_layers_locked()` then `xrt_comp_layer_commit()` |

This ensures full backward compatibility. Legacy VR applications, fullscreen use cases, and applications that don't need window control all work without modification.

---

## 5. Implementation Architecture

### 5.1 Data Flow

End-to-end path from the application to the display:

```
┌──────────────────────────────────────────────────────────────────────┐
│  Application                                                          │
│    xrCreateSession(XrWin32WindowBindingCreateInfoEXT { windowHandle })     │
│    xrEndFrame(projectionLayer + optional windowSpaceLayer)            │
└────────────────────────────────┬─────────────────────────────────────┘
                                 |
                                 v
┌──────────────────────────────────────────────────────────────────────┐
│  oxr_session.c : oxr_session_create()                                 │
│    - Parses XrWin32WindowBindingCreateInfoEXT from next chain              │
│    - Stores HWND in xrt_session_info.external_window_handle           │
└────────────────────────────────┬─────────────────────────────────────┘
                                 |
                                 v
┌──────────────────────────────────────────────────────────────────────┐
│  comp_multi_compositor.c : multi_compositor_create(xsi)               │
│    - Creates per-session multi_compositor                             │
│    - Stores session info including external_window_handle             │
│    - On first frame: multi_compositor_init_session_render()           │
│      - Uses comp_target_service to create comp_target from HWND       │
│      - Creates per-session SR weaver bound to HWND                    │
└────────────────────────────────┬─────────────────────────────────────┘
                                 |
                                 v
┌──────────────────────────────────────────────────────────────────────┐
│  comp_multi_system.c : render_per_session_clients_locked()            │
│    - Extracts stereo views from projection layer                      │
│    - Acquires swapchain image from per-session comp_target            │
│    - Calls leiasr_weave() with left/right views                       │
│    - Renders window-space layers (if any)                             │
│    - Presents to app window                                           │
└────────────────────────────────┬─────────────────────────────────────┘
                                 |
                                 v
                          App's Window (HWND)
                     Interlaced 3D + composited HUD
```

For the D3D11 native compositor path:

```
oxr_session.c  -->  comp_d3d11_compositor.cpp  -->  comp_d3d11_renderer.cpp
                                                         |
                                                         v
                                                    leiasr_d3d11.cpp (SR weaver)
                                                         |
                                                         v
                                                    App's Window (HWND)
```

### 5.2 Per-Session Rendering Pipeline

The multi-compositor system (`comp_multi_system.c`) splits the render pipeline based on whether a session has an external window:

**Shared path** (no HWND): Layers are dispatched via `transfer_layers_locked()` to the shared native compositor, which renders to a single runtime-owned display. Sessions without an external window handle are processed here.

**Per-session path** (has HWND): Sessions with an external window are skipped in the shared dispatch. After the shared commit, `render_per_session_clients_locked()` iterates each per-session client and renders independently to its own `comp_target`:

1. Extract stereo views from the first projection layer
2. Acquire a swapchain image from the per-session `comp_target`
3. Call `leiasr_weave()` using the per-session SR weaver
4. Render any window-space layers
5. Present to the session's window
6. Retire the delivered frame

**Window-space layer rendering** is implemented in both compositor paths:

- **D3D11 native compositor:** `comp_d3d11_renderer.cpp` function `render_window_space_layer` (line 827)
- **Vulkan per-session path:** `comp_multi_system.c` (lines 949-1000)
- The Vulkan shared/main compositor has no window-space support, which is correct — on the shared path, no app window exists and window coordinates have no meaning.

### 5.3 Target Provider Service Pattern

Creating per-session resources from within `comp_multi` requires calling functions defined in `comp_main` (e.g., `comp_window_mswin_create_from_external`). But `comp_main` already links to `comp_multi`, so a direct dependency would be circular.

The solution is a **service interface** defined in the shared `comp_util` layer:

```
┌───────────────────────────────────────────────────────────────┐
│  comp_util  (shared layer)                                      │
│    comp_target_service.h  — interface definition                │
└───────────────────────────────────────────────────────────────┘
                 ^                              ^
       implements|                              |uses
                 |                              |
┌────────────────┴──────────┐    ┌─────────────┴────────────────┐
│  comp_main                 │    │  comp_multi                   │
│    Creates service object  │--->│    Receives service at init   │
│    Implements callbacks    │    │    Calls service.create()     │
└────────────────────────────┘    └───────────────────────────────┘
```

The service provides three operations:
- `create_from_window(HWND)` — creates a `comp_target` with a VkSwapchain bound to the window
- `destroy_target()` — destroys a target created by the service
- `get_vk()` — returns the `vk_bundle` for Vulkan GPU operations

At compositor creation time, `comp_main` builds a `comp_target_service` struct with function pointers and passes it to `comp_multi`. When a session with an external window begins its first frame, `comp_multi` calls through the service to create the per-session target without any direct link to `comp_main`.

### 5.4 Eye Tracking via LookaroundFilter

Each per-session SR weaver provides predicted eye positions through its internal `LookaroundFilter`:

```
weaver->getPredictedEyePositions(leftEye, rightEye)
    |
    v
oxr_session_locate_views()
    - Converts mm -> meters (divide by 1000)
    - Computes eye_relation = right_eye - left_eye
    - Passes to xrt_device_get_view_poses() for FOV calculation
```

The LookaroundFilter adapts its latency prediction to the application's actual update rate and monitor refresh rate. This is preferred over the deprecated `EyeTracker::openEyePairStream()` which cannot adapt to per-application latency.

**Fallback:** When no per-session weaver is available (no external window), the runtime falls back to static IPD (interpupillary distance).

### 5.5 Window-Adaptive FOV (Kooima Projection)

The runtime uses the **Kooima generalized perspective projection** to compute asymmetric FOV based on eye position relative to the display. When the window is smaller than the full display, a viewport scale formula (derived from SRHydra's `Session.cpp`) adjusts the projection:

```
PixelSize      = DisplayPhysicalSize / DisplayPixelResolution
WindowPhysSize = WindowPixelSize * PixelSize
viewportScale  = min(DisplayPhysSize) / min(WindowPhysSize)
ScreenSize     = WindowPhysSize * viewportScale
```

Additionally, eye positions are offset to account for windows that are not centered on the display:

```
offset_x =  (window_center_px - display_center_px) * pixel_size_x
offset_y = -((window_center_py - display_center_py) * pixel_size_y)
             ^-- negated: screen coords Y-down, eye coords Y-up

adjusted_eye.x -= offset_x
adjusted_eye.y -= offset_y
```

**Fullscreen identity:** When the window fills the display, `viewportScale` = 1.0 and offsets = (0, 0), so all adjustments become no-ops and the FOV is identical to the non-windowed case.

### 5.6 Proportional Render Texture Resize

**Two-swapchain path (legacy):** When an application submits two separate swapchains (one per eye), the runtime composites them into an internal SBS stereo texture. When the window is smaller than the full display, this stereo texture is scaled proportionally to avoid wasting GPU on pixels that will be downscaled:

```
ratio = min(window_width / display_px_width, window_height / display_px_height)
ratio = clamp(ratio, 0.0, 1.0)

new_view_width  = SR_recommended_width  * ratio
new_view_height = SR_recommended_height * ratio
```

Only size-dependent resources (stereo texture, SRV, RTV, depth texture, DSV) are recreated. Shaders, samplers, blend state, and rasterizer state are preserved. The minimum render texture size is clamped to 64x64.

| Window Size | Ratio | GPU Savings |
|-------------|-------|-------------|
| Full display | 1.0 | 0% (baseline) |
| Half display | 0.5 | ~75% pixels |
| Quarter display | 0.25 | ~94% pixels |
| Minimum (64x64) | Clamped | Maximum |

**Single-swapchain path (ext apps):** The ext test apps create one swapchain at native display pixel resolution (`displayPixelWidth` x `displayPixelHeight` from `XR_EXT_display_info`) and render both stereo views side-by-side into it using viewport offsets. Both `projectionViews[0]` and `projectionViews[1]` reference the same swapchain with different `imageRect` offsets. When the runtime detects that both views reference the same swapchain and image index, it skips the internal SBS blit and passes the application's texture directly to the weaver. This eliminates one full-resolution GPU copy per frame.

### 5.7 Lenticular Phase-Aligned Window Snapping

On a lenticular display, each subpixel projects light at a specific angle determined by its absolute position relative to the lens array. When a window is dragged to an arbitrary pixel position, the phase relationship between content and lenses can break, causing crosstalk (left/right eye images bleeding into each other).

**This is handled automatically by the SR weaver.** During initialization, the weaver **subclasses the application's window procedure** via `SetWindowLongPtr`. The subclassed `WndProc` intercepts:

| Message | Action |
|---------|--------|
| `WM_ENTERSIZEMOVE` | Records initial window position |
| `WM_WINDOWPOSCHANGING` | Snaps proposed position to nearest phase-aligned coordinate |
| `WM_EXITSIZEMOVE` | Clears drag state |

The window moves in small discrete steps (typically 1-2 pixels) that preserve the lenticular phase, so the 3D effect remains stable throughout a drag while motion feels smooth to the user.

No runtime or application code is needed — any application using a weaver-bound window gets phase-aligned snapping automatically.

---

## 6. D3D11 Hybrid Mode (Windows)

### 6.1 Problem

On **Intel Iris integrated GPUs**, Vulkan's `VK_KHR_external_memory` extensions fail to import D3D11 textures created by applications. This manifests as black screens in WebXR, failed swapchain imports in service mode, and silent rendering failures.

Additionally, **WebXR (Chrome/Edge)** and **UWP apps** run inside a Windows AppContainer sandbox and cannot access the GPU driver directly for in-process composition — they must communicate via IPC with an out-of-process service.

### 6.2 Architecture

Hybrid mode provides two execution paths, selected automatically based on the application's sandbox state:

```
                    OpenXR Application
                           |
                           v
             ┌──────────────────────────────┐
             │   xrt_instance_create()      │
             │   u_sandbox_should_use_ipc() │
             │         |                    │
             │   ┌─────┴─────┐              │
             │   v           v              │
             │ false       true             │
             └───┬───────────┬──────────────┘
                 |           |
                 v           v
   ┌──────────────────┐   ┌───────────────────────┐
   │ In-Process Mode  │   │ IPC/Service Mode      │
   │ D3D11 native     │   │                       │
   │ compositor       │   │ monado-service with   │
   │ (in-process)     │   │ D3D11 service         │
   └────────┬─────────┘   │ compositor            │
            |              │ - Own D3D11 device    │
            |              │ - Import via DXGI     │
            |              │ - KeyedMutex sync     │
            |              └───────────┬───────────┘
            |                          |
            v                          v
   ┌──────────────────────────────────────────────┐
   │             Leia SR Weaver                    │
   │        (Light field interlacing)              │
   └──────────────────────────────────────────────┘
```

**In-process mode:** For native Win32/D3D11 apps. The D3D11 native compositor runs in the app's process, avoiding IPC overhead.

**IPC/Service mode:** For sandboxed apps (WebXR, UWP). The app communicates over a named pipe to `monado-service`, which has its own D3D11 device. Textures are shared via DXGI shared handles with `IDXGIKeyedMutex` synchronization.

Both paths avoid Vulkan-D3D11 interop entirely.

### 6.3 Environment Variables

| Variable | Values | Default | Effect |
|----------|--------|---------|--------|
| `XRT_FORCE_MODE` | `native`, `ipc` | (unset = auto) | Override sandbox detection |
| `XRT_SERVICE_USE_D3D11` | `0`, `1` | `1` (true) | D3D11 vs Vulkan service compositor |
| `OXR_ENABLE_D3D11_NATIVE_COMPOSITOR` | `0`, `1` | `1` (enabled) | Enable D3D11 native compositor for in-process mode |

### 6.4 Build Configuration

```cmake
option(XRT_FEATURE_HYBRID_MODE "Enable hybrid in-process/IPC mode for Windows" OFF)
```

Requires: `WIN32`, `XRT_MODULE_IPC`, `XRT_FEATURE_OPENXR`, `XRT_HAVE_D3D11`.

When enabled, the build produces:
- `openxr_monado.dll` — Runtime with hybrid entry point
- `monado-service.exe` — Service with D3D11 compositor

Key compile definitions:
- `XRT_FEATURE_HYBRID_MODE` — Renames `xrt_instance_create` to `native_instance_create` in hybrid builds
- `XRT_USE_D3D11_SERVICE_COMPOSITOR` — Enables D3D11 compositor selection logic

---

## 7. Known Issues

### 7.1 SR SDK WndProcDispatcher Race Condition (Resolved)

Previous versions of the SR SDK had a **use-after-free race condition** in `WeaverBaseImpl.ipp` where `WndProcDispatcher` released the global map lock before calling `instance->weaverWndProc()`, allowing another thread to destroy the weaver between the lookup and the call.

**Status: Fixed in SR SDK** (commit `54410d9f`). The fix introduces a per-instance `SRWLOCK` (`instanceLock`) inside a `WindowObjectData` struct. The dispatcher now acquires a shared instance lock before releasing the map lock and holds it for the entire `weaverWndProc` call. `restoreOriginalWindowProc` acquires the instance lock exclusively after removing the entry from the map, which blocks until all in-flight dispatcher calls complete. Re-entrancy is handled via an `inDispatcherCount` counter, and same-thread destruction (from within a WndProc callback) skips the exclusive-lock wait to avoid deadlock.

The workaround in Monado's `leiasr_destroy()` (message pumping and delays) is no longer needed but remains as a defensive measure.

### 7.2 D3D11 Thread Safety

The D3D11 immediate device context is **single-threaded by design**. Applications must not use a dedicated render thread alongside the message pump when the SR weaver is active. Use the WM_PAINT trick (Section 4.3) for continuous rendering during window drag/resize.

---

## 8. Files Reference

### Extension Definition

| File | Purpose |
|------|---------|
| `src/external/openxr_includes/openxr/XR_EXT_win32_window_binding.h` | Extension header: struct definitions, type constants, defines |

### Core Interfaces

| File | Purpose |
|------|---------|
| `src/xrt/include/xrt/xrt_compositor.h` | `xrt_session_info.external_window_handle`, `XRT_LAYER_WINDOW_SPACE` enum, `xrt_layer_window_space_data` struct, `xrt_comp_layer_window_space()` |
| `src/xrt/include/xrt/xrt_openxr_includes.h` | Includes the extension header |

### State Tracker (OpenXR API Layer)

| File | Purpose |
|------|---------|
| `src/xrt/state_trackers/oxr/oxr_session.c` | Parses `XrWin32WindowBindingCreateInfoEXT`, window-adaptive Kooima FOV, eye tracking integration |
| `src/xrt/state_trackers/oxr/oxr_session_frame_end.c` | Handles `XrCompositionLayerWindowSpaceEXT` submission |
| `src/xrt/state_trackers/oxr/oxr_session_gfx_d3d11_native.c` | D3D11 native session creation path |
| `src/xrt/state_trackers/oxr/oxr_extension_support.h` | Extension registration |

### Multi-Compositor

| File | Purpose |
|------|---------|
| `src/xrt/compositor/multi/comp_multi_system.c` | Per-session render pipeline, window-space layer rendering (Vulkan path), `render_per_session_clients_locked()` |
| `src/xrt/compositor/multi/comp_multi_private.h` | `session_render` struct (target, weaver, window handle), `target_service` pointer |
| `src/xrt/compositor/multi/comp_multi_compositor.c` | `multi_compositor_init_session_render()`, per-session resource creation/cleanup |

### Native Compositor (Vulkan)

| File | Purpose |
|------|---------|
| `src/xrt/compositor/main/comp_compositor.h` | `external_window_handle` field, target service |
| `src/xrt/compositor/main/comp_compositor.c` | Service callback implementation, external window handling in `compositor_begin_session()` |
| `src/xrt/compositor/main/comp_renderer.c` | Passes window handle to `leiasr_create()` |
| `src/xrt/compositor/main/comp_window_mswin.c` | `comp_window_mswin_create_from_external()`, `owns_window` flag |

### Native Compositor (D3D11)

| File | Purpose |
|------|---------|
| `src/xrt/compositor/d3d11/comp_d3d11_compositor.h` | D3D11 compositor interface, window metrics |
| `src/xrt/compositor/d3d11/comp_d3d11_compositor.cpp` | Window metrics computation, proportional render texture resize in `begin_frame` |
| `src/xrt/compositor/d3d11/comp_d3d11_renderer.h` | Renderer interface, `comp_d3d11_renderer_resize()` |
| `src/xrt/compositor/d3d11/comp_d3d11_renderer.cpp` | `render_window_space_layer()`, stereo texture resize |
| `src/xrt/compositor/d3d11/comp_d3d11_window.h` | D3D11 window/swapchain management |
| `src/xrt/compositor/d3d11/comp_d3d11_window.cpp` | D3D11 window/swapchain implementation |

### Target Provider Service

| File | Purpose |
|------|---------|
| `src/xrt/compositor/util/comp_target_service.h` | Service interface breaking `comp_main`/`comp_multi` circular dependency |

### SR Driver

| File | Purpose |
|------|---------|
| `src/xrt/drivers/leiasr/leiasr.h` | Vulkan weaver interface: `leiasr_create()`, `leiasr_weave()`, `leiasr_get_predicted_eye_positions()` |
| `src/xrt/drivers/leiasr/leiasr.cpp` | Vulkan weaver implementation, `getPredictedEyePositions()` wrapper |
| `src/xrt/drivers/leiasr/leiasr_d3d11.h` | D3D11 weaver interface, display pixel info |
| `src/xrt/drivers/leiasr/leiasr_d3d11.cpp` | D3D11 weaver implementation |
| `src/xrt/drivers/leiasr/leiasr_types.h` | `leiasr_window_metrics`, `leiasr_eye_position`, `leiasr_eye_pair` |

### Hybrid Mode

| File | Purpose |
|------|---------|
| `src/xrt/targets/openxr/target.c` | Hybrid entry point: `xrt_instance_create()` with sandbox detection |
| `src/xrt/targets/common/target_instance.c` | `native_instance_create()`, D3D11 service compositor selection |
| `src/xrt/auxiliary/util/u_sandbox.h` | `u_sandbox_should_use_ipc()` API |
| `src/xrt/auxiliary/util/u_sandbox.c` | Windows AppContainer detection |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.h` | D3D11 service compositor interface |
| `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` | D3D11 service compositor: own device, DXGI import, KeyedMutex sync |

### IPC Client

| File | Purpose |
|------|---------|
| `src/xrt/ipc/client/ipc_client_interface.h` | `ipc_instance_create()` for service mode |

### Test Applications

| Directory | Description |
|-----------|-------------|
| `test_apps/cube_ext_d3d11_win/` | D3D11 test app using `XR_EXT_win32_window_binding` with WM_PAINT drag handling |
| `test_apps/cube_ext_vk_win/` | Vulkan variant |
| `test_apps/cube_ext_gl_win/` | OpenGL variant |
| `test_apps/cube_ext_d3d12_win/` | D3D12 variant |
| `test_apps/cube_rt_d3d11_win/` | Standard OpenXR test app (no window binding, uses shared compositor) |
| `test_apps/common/` | Shared utilities: window manager, D3D11 renderer, HUD renderer, text overlay, input handler |

---

## 9. Future Directions

- **Multi-app simultaneous testing** — Validate two per-session apps rendering to different windows at the same time
- **Async weaving** — Replace `vkQueueWaitIdle()` with fence-based synchronization for better throughput
- **WebXR / browser integration** — Browser passes canvas backing surface as window handle via `XR_EXT_win32_window_binding`; requires browser vendor cooperation
- **Linux / Android platform support** — Extend `windowHandle` to accept XCB/Wayland/ANativeWindow handles
- **Khronos standardization** — Propose `XR_EXT_win32_window_binding` for inclusion in the OpenXR specification

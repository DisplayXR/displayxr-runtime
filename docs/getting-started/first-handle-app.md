# Tutorial: Your First Handle App (D3D11 on Windows)

This tutorial walks through building a `_handle` app — an app that creates its own window and passes it to DisplayXR for stereo rendering. We annotate the [`cube_handle_d3d11_win`](../../test_apps/cube_handle_d3d11_win/) test app as a working reference.

> **Reference code:** `test_apps/cube_handle_d3d11_win/` — a complete, buildable D3D11 handle app.

## Prerequisites

- DisplayXR built and working (see [Building](building.md))
- Visual Studio 2022 with C++ workload
- Familiarity with Win32 windowing and D3D11 basics
- Familiarity with OpenXR concepts (instances, sessions, swapchains)

## Architecture

A handle app owns the window. The runtime renders *into* it via the native compositor — no intermediate window, no Vulkan conversion.

```
Your App                          DisplayXR Runtime
  HWND ──────────────────────────> D3D11 native compositor
  D3D11 device ──────────────────> (renders into your HWND)
  OpenXR calls ──────────────────> display processor → display
```

Three extensions make this work:
- `XR_KHR_D3D11_enable` — standard OpenXR D3D11 binding
- `XR_EXT_win32_window_binding` — pass your HWND to the runtime
- `XR_EXT_display_info` — query physical display properties, enumerate rendering modes

---

## Step 1: Create Your Window

The app creates the window **before** initializing OpenXR. The HWND is passed to the runtime at session creation.

```cpp
// main.cpp:675-676
HWND hwnd = CreateAppWindow(hInstance, g_windowWidth, g_windowHeight);
```

This is a standard Win32 `CreateWindowEx` call — nothing DisplayXR-specific. The key point: **you own the window**. The runtime will composite into it, not create its own.

> **Source:** `main.cpp:169-201` (window class registration + creation)

---

## Step 2: Create the OpenXR Instance with Extensions

Enable all three extensions when creating the instance.

```cpp
// xr_session.cpp:100-108 — build extension list
std::vector<const char*> enabledExtensions;
enabledExtensions.push_back(XR_KHR_D3D11_ENABLE_EXTENSION_NAME);
if (xr.hasWin32WindowBindingExt) {
    enabledExtensions.push_back(XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME);
}
if (xr.hasDisplayInfoExt) {
    enabledExtensions.push_back(XR_EXT_DISPLAY_INFO_EXTENSION_NAME);
}

// xr_session.cpp:117-131 — create instance
XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
strcpy_s(createInfo.applicationInfo.applicationName, "MyApp");
createInfo.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;
createInfo.enabledExtensionCount = (uint32_t)enabledExtensions.size();
createInfo.enabledExtensionNames = enabledExtensions.data();
xrCreateInstance(&createInfo, &xr.instance);
```

After instance creation, get the system ID:

```cpp
// xr_session.cpp:136-138
XrSystemGetInfo systemInfo = {XR_TYPE_SYSTEM_GET_INFO};
systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
xrGetSystem(xr.instance, &systemInfo, &xr.systemId);
```

> **Note:** `XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY` is used because OpenXR doesn't yet have a form factor for 3D displays. DisplayXR maps it to the connected 3D display.

---

## Step 3: Query Display Info

With `XR_EXT_display_info` enabled, chain `XrDisplayInfoEXT` into `xrGetSystemProperties` to get physical display dimensions, pixel resolution, and eye tracking capabilities.

```cpp
// xr_session.cpp:150-177
XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
XrDisplayInfoEXT displayInfo = {(XrStructureType)XR_TYPE_DISPLAY_INFO_EXT};
XrEyeTrackingModeCapabilitiesEXT eyeCaps = {
    (XrStructureType)XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_EXT};
displayInfo.next = &eyeCaps;
sysProps.next = &displayInfo;

xrGetSystemProperties(xr.instance, xr.systemId, &sysProps);

// Now available:
// displayInfo.displaySizeMeters.width/height  — physical screen size in meters
// displayInfo.displayPixelWidth/Height        — native resolution
// displayInfo.recommendedViewScaleX/Y         — viewport scaling factors
// displayInfo.nominalViewerPositionInDisplaySpace — default eye position
// eyeCaps.supportedModes / eyeCaps.defaultMode — eye tracking capabilities
```

These physical dimensions are essential for [Kooima projection](../architecture/kooima-projection.md) — computing asymmetric frustums that match the viewer's actual position relative to the display.

Also load the extension function pointers for mode control:

```cpp
// xr_session.cpp:196-200
xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeEXT",
    (PFN_xrVoidFunction*)&xr.pfnRequestDisplayRenderingModeEXT);
xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesEXT",
    (PFN_xrVoidFunction*)&xr.pfnEnumerateDisplayRenderingModesEXT);
```

---

## Step 4: Initialize D3D11 on the Correct Adapter

The runtime specifies which GPU to use via an adapter LUID. You **must** create your D3D11 device on this adapter.

```cpp
// main.cpp:718-730
LUID adapterLuid;
GetD3D11GraphicsRequirements(xr, &adapterLuid);      // queries runtime
InitializeD3D11WithLUID(renderer, adapterLuid);        // creates device on that GPU
```

> **Why?** The runtime's compositor needs to share textures with your D3D11 device. They must be on the same GPU.

---

## Step 5: Create Session with Window Binding

This is the key step — chain your HWND into the session creation call.

```cpp
// xr_session.cpp:231-255
// 1. D3D11 graphics binding (standard OpenXR)
XrGraphicsBindingD3D11KHR d3d11Binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
d3d11Binding.device = d3d11Device;

// 2. Window binding (DisplayXR extension) — THIS IS THE KEY PART
XrWin32WindowBindingCreateInfoEXT windowBinding = {
    XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT};
windowBinding.windowHandle = hwnd;

// 3. Chain them: sessionInfo → d3d11Binding → windowBinding
d3d11Binding.next = &windowBinding;

// 4. Create session
XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
sessionInfo.next = &d3d11Binding;
sessionInfo.systemId = xr.systemId;
xrCreateSession(xr.instance, &sessionInfo, &xr.session);
```

After session creation, enumerate available rendering modes:

```cpp
// xr_session.cpp:259-282
uint32_t modeCount = 0;
xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, 0, &modeCount, nullptr);
std::vector<XrDisplayRenderingModeInfoEXT> modes(modeCount);
xr.pfnEnumerateDisplayRenderingModesEXT(xr.session, modeCount, &modeCount, modes.data());

// Each mode provides:
// modes[i].modeName       — "2D", "Stereo", "Quad", etc.
// modes[i].viewCount      — 1, 2, 4, ...
// modes[i].tileColumns    — atlas grid columns
// modes[i].tileRows       — atlas grid rows
// modes[i].viewScaleX/Y   — viewport scaling for this mode
// modes[i].hardwareDisplay3D — whether this mode uses the 3D display hardware
```

---

## Step 6: Create Swapchain

Create a single swapchain sized to the native display resolution. The app renders a tiled atlas of views into this swapchain.

```cpp
// main.cpp:767-784 — create swapchain, then enumerate D3D11 images
CreateSwapchain(xr);  // sizes to displayPixelWidth × displayPixelHeight

std::vector<XrSwapchainImageD3D11KHR> swapchainImages(xr.swapchain.imageCount,
    {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR});
xrEnumerateSwapchainImages(xr.swapchain.swapchain, count, &count,
    (XrSwapchainImageBaseHeader*)swapchainImages.data());
// Each image has: swapchainImages[i].texture (ID3D11Texture2D*)
```

The swapchain is worst-case sized — large enough for any rendering mode's atlas. Individual modes may use only a portion. See [Swapchain Model](../specs/swapchain-model.md) for details.

---

## Step 7: Render Loop

The render loop follows the standard OpenXR frame cadence with multiview atlas tiling.

### 7a. Begin Frame

```cpp
// BeginFrame() calls xrWaitFrame (blocks until display time) then xrBeginFrame
XrFrameState frameState;
BeginFrame(xr, frameState);
// frameState.predictedDisplayTime — when this frame will be displayed
// frameState.shouldRender         — false if session not focused
```

### 7b. Get View Poses

```cpp
// LocateViews() calls xrLocateViews, applies eye factors (IPD, parallax),
// applies player locomotion transform, fills xr.viewMatrices[] / projMatrices[]
LocateViews(xr, frameState.predictedDisplayTime,
    cameraPosX, cameraPosY, cameraPosZ, yaw, pitch, viewParams);
```

### 7c. Query Current Mode's Tile Layout

```cpp
// main.cpp:302-312 — read the active rendering mode's parameters
uint32_t modeViewCount = xr.renderingModeViewCounts[currentMode];
uint32_t tileColumns   = xr.renderingModeTileColumns[currentMode];
uint32_t tileRows      = xr.renderingModeTileRows[currentMode];
bool     monoMode      = !xr.renderingModeDisplay3D[currentMode];
float    scaleX        = xr.renderingModeScaleX[currentMode];
float    scaleY        = xr.renderingModeScaleY[currentMode];
```

### 7d. Render Each View into Its Atlas Tile

Acquire the swapchain once, render all views into their respective tiles, then release.

```cpp
// main.cpp:560-638 (simplified)
uint32_t imageIndex;
AcquireSwapchainImage(xr, imageIndex);
ID3D11Texture2D* texture = swapchainImages[imageIndex].texture;

// Compute per-tile render dimensions
uint32_t renderW = (uint32_t)(windowWidth * scaleX);
uint32_t renderH = (uint32_t)(windowHeight * scaleY);

for (int eye = 0; eye < eyeCount; eye++) {
    // Calculate tile position in atlas grid
    uint32_t tileX = eye % tileColumns;
    uint32_t tileY = eye / tileColumns;

    // Set viewport to this tile's region
    D3D11_VIEWPORT vp = {};
    vp.TopLeftX = (FLOAT)(tileX * renderW);
    vp.TopLeftY = (FLOAT)(tileY * renderH);
    vp.Width    = (FLOAT)renderW;
    vp.Height   = (FLOAT)renderH;
    renderer.context->RSSetViewports(1, &vp);

    // Render scene with this eye's view/projection
    RenderScene(renderer, rtv, dsv, renderW, renderH,
        viewMatrices[eye], projMatrices[eye], ...);

    // Fill submission struct for this view
    projectionViews[eye].subImage.swapchain = xr.swapchain.swapchain;
    projectionViews[eye].subImage.imageRect.offset = {
        (int32_t)(tileX * renderW), (int32_t)(tileY * renderH)};
    projectionViews[eye].subImage.imageRect.extent = {
        (int32_t)renderW, (int32_t)renderH};
    projectionViews[eye].pose = rawViews[eye].pose;
    projectionViews[eye].fov  = rawViews[eye].fov;
}

ReleaseSwapchainImage(xr);
```

### 7e. Submit Frame

```cpp
// main.cpp:653
EndFrame(xr, frameState.predictedDisplayTime, projectionViews.data(), eyeCount);
```

The runtime's compositor crops the atlas, passes it to the display processor for interlacing/weaving, and presents to your window.

---

## Step 8: Handle Mode Switching

Users switch rendering modes with V (cycle) or 1-8 (direct select). The app requests a mode change, and the runtime responds asynchronously with an event.

### Request a Mode Change

```cpp
// main.cpp:272-276
if (modeChangeRequested) {
    xr.pfnRequestDisplayRenderingModeEXT(xr.session, newModeIndex);
}
```

### Handle the Event

```cpp
// xr_session_common.cpp:311-316 (inside PollEvents)
case XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_EXT: {
    auto* modeEvent = (XrEventDataRenderingModeChangedEXT*)&eventBuffer;
    xr.currentModeIndex = modeEvent->currentModeIndex;
    // Next frame automatically adapts: new viewCount, tileLayout, scaling
    break;
}
```

The next render frame reads the updated mode parameters (step 7c) and adjusts the atlas layout, view count, and scaling accordingly. No swapchain recreation needed — the worst-case sizing handles all modes.

---

## Putting It All Together

The full startup sequence in `main.cpp:662-868`:

```
1. CreateAppWindow()              — HWND created
2. InitializeOpenXR()             — instance + extensions + display info
3. GetD3D11GraphicsRequirements() — get adapter LUID from runtime
4. InitializeD3D11WithLUID()      — D3D11 device on correct GPU
5. CreateSession(device, hwnd)    — session with window + D3D11 binding
6. CreateSpaces()                 — LOCAL + VIEW reference spaces
7. CreateSwapchain()              — worst-case atlas swapchain
8. Main loop:
   ├── PollEvents()               — session state, mode changes
   ├── BeginFrame()               — wait + begin
   ├── LocateViews()              — eye poses + player transform
   ├── AcquireSwapchainImage()
   ├── for each view: set viewport, render, fill projectionView
   ├── ReleaseSwapchainImage()
   └── EndFrame()                 — submit to runtime
9. CleanupOpenXR() + CleanupD3D11()
```

## Other Platforms and APIs

The OpenXR integration is identical across APIs — only the graphics binding and swapchain image types change:

| Platform | Window binding | Graphics binding | Swapchain image type |
|----------|---------------|-----------------|---------------------|
| **D3D11 / Windows** | `XrWin32WindowBindingCreateInfoEXT` | `XrGraphicsBindingD3D11KHR` | `XrSwapchainImageD3D11KHR` |
| **D3D12 / Windows** | `XrWin32WindowBindingCreateInfoEXT` | `XrGraphicsBindingD3D12KHR` | `XrSwapchainImageD3D12KHR` |
| **Metal / macOS** | `XrCocoaWindowBindingCreateInfoEXT` | `XrGraphicsBindingMetalEXT` | `XrSwapchainImageMetalEXT` |
| **Vulkan / any** | `XrWin32WindowBindingCreateInfoEXT` or `XrCocoaWindowBindingCreateInfoEXT` | `XrGraphicsBindingVulkanKHR` | `XrSwapchainImageVulkanKHR` |
| **OpenGL / any** | `XrWin32WindowBindingCreateInfoEXT` or `XrCocoaWindowBindingCreateInfoEXT` | `XrGraphicsBindingOpenGLWin32KHR` | `XrSwapchainImageOpenGLKHR` |

See the corresponding test apps for each API: `cube_handle_metal_macos`, `cube_handle_vk_win`, `cube_handle_gl_win`, etc.

## Further Reading

- [App Classes](app-classes.md) — understand all four integration modes
- [Swapchain Model](../specs/swapchain-model.md) — two-swapchain architecture and canvas concept
- [Multiview Tiling](../specs/multiview-tiling.md) — atlas layout algorithm
- [Kooima Projection](../architecture/kooima-projection.md) — stereo math for display-centric rendering
- [XR_EXT_display_info](../specs/XR_EXT_display_info.md) — full extension specification
- [XR_EXT_win32_window_binding](../specs/XR_EXT_win32_window_binding.md) — window binding specification

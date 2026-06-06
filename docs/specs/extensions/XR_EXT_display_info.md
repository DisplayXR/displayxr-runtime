# OpenXR Extensions for Tracked 3D Displays

## Formal Proposal for XR_EXT_win32_window_binding, XR_EXT_android_surface_binding, XR_EXT_cocoa_window_binding, and XR_EXT_display_info

| Field | Value |
|---|---|
| **Proposal Title** | OpenXR Extensions for Tracked 3D (Autostereoscopic) Displays |
| **Authors** | David Fattal (Leia Inc.) |
| **Date** | 2026-02-13 |
| **Revision** | 0.2 |
| **Status** | Draft — proposed for Khronos OpenXR Working Group review |
| **Extension Names** | `XR_EXT_win32_window_binding`, `XR_EXT_android_surface_binding`, `XR_EXT_cocoa_window_binding`, `XR_EXT_display_info` |
| **OpenXR Version** | 1.0 |
| **Extension Type** | Instance extensions |
| **Dependencies** | OpenXR 1.0 core |
| **Contributors** | David Fattal (Leia Inc.) |

---

## 1. Motivation — Stereo Tracked 3D Displays Are Coming of Age

### Industry Context

Autostereoscopic and light field displays are reaching consumer maturity. These devices
present glasses-free 3D imagery by directing different views to different eye positions,
using lenticular optics, diffractive backlight, or multi-layer panel stacks. When combined
with real-time face and eye tracking, they produce dynamic, perspective-correct 3D that
responds to viewer motion — the same fundamental experience that head-mounted displays
deliver, but without worn hardware.

Major laptop and monitor OEMs are shipping tracked 3D displays today. Game engines,
creative tools, medical imaging, and industrial design workflows all benefit from a
standard programming interface for these devices.

### The Gap in OpenXR

The current OpenXR 1.0 specification assumes a head-mounted display (HMD) worn by the
user. The runtime creates and owns the display surface, manages per-eye rendering targets,
and controls the view-projection pipeline. This model does not map cleanly to tracked 3D
displays because:

1. **The display is a shared desktop monitor**, not a private headset. The application may
   need to render into its own window, possibly alongside 2D UI, possibly in windowed mode.
   The runtime cannot simply take over the screen.

2. **The display has fixed physical geometry**. Unlike an HMD whose optics define FOV and
   IPD, a tracked 3D display has a known physical rectangle at a known distance. The
   application or engine may need this geometry to build its own camera model (e.g.,
   Kooima off-axis projection) rather than consuming pre-built view/projection matrices.

3. **Stereo rendering is view-dependent, not head-locked**. An HMD's views are rigidly
   attached to the head. A tracked 3D display's views are defined by the viewer's eye
   positions relative to a fixed screen. The runtime must expose tracked eye positions in
   display-centric coordinates.

### Why OpenXR Matters

Without a standard API, every tracked 3D display vendor ships a proprietary SDK with
incompatible data types, coordinate conventions, and rendering models. Application
developers must write separate codepaths for each vendor. This fragments the ecosystem and
slows adoption.

OpenXR can prevent this fragmentation — but only if it provides the missing primitives:

- **Window/surface binding**: let the application own the rendering surface.
- **Display info**: let the application know the display's physical geometry, its nominal
  viewing conditions, the recommended render quality, and whether the display supports
  switching between 2D and 3D modes.
- **Display mode control**: let the application request 2D or 3D mode on displays that
  support switchable optics or backlights.

### What These Extensions Solve

This proposal introduces four independent but complementary extensions:

| Extension | Purpose |
|---|---|
| `XR_EXT_win32_window_binding` | App provides a Win32 HWND for runtime rendering; enables windowed mode, multi-app, app-controlled input, and window-space overlay layers. |
| `XR_EXT_android_surface_binding` | App provides an Android `ANativeWindow` for runtime rendering; the Android counterpart to the Win32 window binding. |
| `XR_EXT_cocoa_window_binding` | App provides a Cocoa `NSView*` (with `CAMetalLayer` backing) for runtime rendering on macOS. |
| `XR_EXT_display_info` | Runtime exposes physical display geometry, nominal viewer position, recommended render scale, and display mode switching capability. Provides `xrRequestDisplayModeEXT` for 2D/3D mode control, `xrRequestEyeTrackingModeEXT` for managed/manual eye tracking selection, and `xrRequestDisplayRenderingModeEXT` for vendor-specific rendering mode switching. In RAW mode, `xrLocateViews` returns screen-centered eye positions regardless of the reference space parameter. |

Together they form a minimal, complete interface for tracked 3D display rendering through
OpenXR across desktop, mobile, and macOS platforms.

**Future direction — multiview and passive displays**: while this proposal focuses on
stereo (2-view) tracked displays, the architecture is designed to extend naturally to
multiview displays with more than two views and to passive (non-tracked) autostereoscopic
displays. See [OPEN 4](#open-issues) in the Issues section.

---

## 2. Overview

### Architecture

```
    Application
        │
        ├── xrCreateInstance()
        │       enable "XR_EXT_win32_window_binding"   (Win32)
        │         — or "XR_EXT_android_surface_binding" (Android)
        │         — or "XR_EXT_cocoa_window_binding"   (macOS)
        │       enable "XR_EXT_display_info"
        │
        ├── xrGetSystemProperties()
        │       ◄── XrDisplayInfoEXT (chained)
        │           • displaySizeMeters
        │           • nominalViewerPositionInDisplaySpace
        │           • recommendedViewScaleX / Y
        │           • hardwareDisplay3D
        │
        ├── xrCreateSession()
        │       XrSessionCreateInfo
        │        └── next: XrGraphicsBindingD3D11KHR          (Win32)
        │        │            └── next: XrWin32WindowBindingCreateInfoEXT
        │        │                       • windowHandle = app HWND
        │        └── next: XrGraphicsBindingVulkanKHR          (Android)
        │        │            └── next: XrAndroidSurfaceBindingCreateInfoEXT
        │        │                       • nativeWindow = app ANativeWindow*
        │        │                       • surface = Java Surface jobject
        │        │                       • screenOffsetX/Y = position on display
        │        └── next: XrGraphicsBindingVulkanKHR          (macOS)
        │                    └── next: XrCocoaWindowBindingCreateInfoEXT
        │                               • viewHandle = app NSView* (CAMetalLayer)
        │
        │   ┌── Session READY → runtime auto-requests 3D mode (if supported)
        │
        ├── xrRequestDisplayModeEXT(session, mode)     [optional override]
        │       XR_DISPLAY_MODE_3D_EXT  /  XR_DISPLAY_MODE_2D_EXT
        │
        ├── xrRequestDisplayRenderingModeEXT(session, modeIndex)  [optional]
        │       mode 0 = standard, 1+ = vendor-defined
        │
        ├── xrLocateViews(space = LOCAL)
        │       ◄── per-eye positions in screen-centered coordinates (RAW mode)
        │
        ├── App computes Kooima projection from eye positions + display geometry
        │
        ├── xrEndFrame()
        │       submit XrCompositionLayerProjection (in LOCAL space)
        │       submit XrCompositionLayerWindowSpaceEXT (HUD overlay)
        │
        │   ┌── Session STOPPING → runtime auto-requests 2D mode (if supported)
        │
        └── Runtime interlaces stereo content onto tracked 3D display
```

### Canonical Display Pyramid

The physical display and nominal viewer position together define a **canonical display
pyramid** (frustum):

- **Base**: the physical display rectangle (known real-world size in meters).
- **Apex**: the nominal viewer position (`nominalViewerPositionInDisplaySpace`).
- **Edges**: rays from the apex through each corner of the display rectangle.

This pyramid represents the *intended single-view camera* for the display. It anchors
zero-parallax depth, stereo comfort, and content framing. Stereo rendering is then
**sampling this same pyramid from two nearby eye positions** — the tracked physical eyes.

### View Modes: RAW vs RENDER_READY

| Mode | Behavior | Camera Model Owned By |
|---|---|---|
| **RENDER_READY** | Runtime returns converged, comfortable stereo view poses and FOV angles. The application still builds its own projection matrix from the FOV. | Runtime |
| **RAW** | Runtime returns raw tracked eye positions in screen-centered coordinates; `orientation` is identity; `fov` is advisory. | Application |

**Ownership rules:**

| Condition | Default Mode |
|---|---|
| `XR_EXT_display_info` not enabled | RENDER_READY |
| `XR_EXT_display_info` enabled | RAW |

In RAW mode the application builds its own projection (typically Kooima off-axis frustum)
from the eye positions and display geometry. This gives engines full control over the
camera model, matching how Unity and Unreal handle 3D display integration. In
RENDER_READY mode the runtime provides pre-built view poses and FOV angles with
convergence and comfort adjustments applied; the application still constructs the
projection matrix from the returned `XrFovf`, but the camera model is runtime-owned.
This mode is suitable for legacy apps and WebXR.

### Relationship Between the Extensions

The three extensions are **independent**:

- An application can use `XR_EXT_win32_window_binding` (or its Android counterpart) alone
  to render into its own window/surface with RENDER_READY views (no display geometry needed).
- An application can use `XR_EXT_display_info` alone to get display geometry, RAW eye
  positions, and display mode control while letting the runtime manage the display surface.
- Using a window/surface binding together with `XR_EXT_display_info` gives full control:
  app-owned rendering surface + app-owned camera model + display mode switching.
- `XR_EXT_win32_window_binding` and `XR_EXT_android_surface_binding` are **mutually
  exclusive** platform variants — an application uses one or the other, never both.

---

## 3. Extension 1: XR_EXT_win32_window_binding

### IP Status

No known IP claims.

### Name Strings

- Extension name: `XR_EXT_win32_window_binding`
- Spec version: 1
- Extension name define: `XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME`

### Overview

This extension allows an OpenXR application to provide its own Win32 window handle (HWND)
to the runtime via the session creation chain. When provided, the runtime renders into the
application's window instead of creating its own display surface.

**Use cases:**
- **Windowed mode rendering**: 3D content in a resizable desktop window.
- **Multi-application scenarios**: multiple OpenXR apps sharing one display.
- **Application-controlled input**: the application owns the window message pump and
  handles keyboard, mouse, and touch input directly.
- **Hybrid 2D/3D UI**: 3D content composited alongside traditional Win32 UI elements.
- **Window-space overlays**: HUD and status overlays positioned in fractional window
  coordinates, automatically adapting to window resize.

### New Enum Constants

```c
#define XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT  ((XrStructureType)1000999001)
#define XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT    ((XrStructureType)1000999002)
```

> **Note**: These values use the vendor extension range. They would be replaced with
> officially assigned values upon standardization.

### New Structures

#### XrWin32WindowBindingCreateInfoEXT

Chained to `XrSessionCreateInfo` (via the graphics binding's `next` pointer) to provide
an external window handle for session rendering.

| Member | Type | Description |
|---|---|---|
| `type` | `XrStructureType` | Must be `XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT`. |
| `next` | `const void*` | Pointer to next structure in the chain, or `NULL`. |
| `windowHandle` | `void*` | The Win32 `HWND` of the target window. Must be a valid, visible window handle. |

```c
typedef struct XrWin32WindowBindingCreateInfoEXT {
    XrStructureType             type;
    const void* XR_MAY_ALIAS    next;
    void*                       windowHandle;
} XrWin32WindowBindingCreateInfoEXT;
```

**Valid Usage:**
- `type` **must** be `XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT`.
- `windowHandle` **must** be a valid Win32 `HWND` that the application owns.
- The window **must** remain valid for the lifetime of the `XrSession`.
- The application **must** service the window's message pump on the thread that created
  the window. Failure to pump messages will stall rendering.
- The application **must not** destroy the window before calling `xrDestroySession`.

#### XrCompositionLayerWindowSpaceEXT

A composition layer type for content positioned in fractional window coordinates. The
layer is composited into both eye views with an optional per-eye horizontal disparity
shift.

| Member | Type | Description |
|---|---|---|
| `type` | `XrStructureType` | Must be `XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT`. |
| `next` | `const void*` | Pointer to next structure in the chain, or `NULL`. |
| `layerFlags` | `XrCompositionLayerFlags` | Composition flags (e.g., `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT`). |
| `subImage` | `XrSwapchainSubImage` | Source swapchain and image rectangle. |
| `x` | `float` | Left edge position as a fraction of window width, in `[0, 1]`. |
| `y` | `float` | Top edge position as a fraction of window height, in `[0, 1]`. |
| `width` | `float` | Layer width as a fraction of window width, in `(0, 1]`. |
| `height` | `float` | Layer height as a fraction of window height, in `(0, 1]`. |
| `disparity` | `float` | Horizontal shift per eye as a fraction of window width. `0` = screen depth (zero parallax); negative = toward viewer. See [Disparity Guidance](#disparity-guidance). |

```c
typedef struct XrCompositionLayerWindowSpaceEXT {
    XrStructureType             type;
    const void* XR_MAY_ALIAS    next;
    XrCompositionLayerFlags     layerFlags;
    XrSwapchainSubImage         subImage;
    float                       x;
    float                       y;
    float                       width;
    float                       height;
    float                       disparity;
} XrCompositionLayerWindowSpaceEXT;
```

**Valid Usage:**
- `type` **must** be `XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT`.
- The session **must** have been created with `XrWin32WindowBindingCreateInfoEXT`.
- `subImage.swapchain` **must** be a valid `XrSwapchain`.
- `x`, `y`, `width`, `height` **should** define a region within `[0, 1]` window-space
  coordinates. The runtime **may** clamp values outside this range.

#### Disparity Guidance

The `disparity` field controls the perceived depth of the window-space layer. It is
specified as a fraction of window width and is an **artistic/UX choice** made by the
application — there is no single correct value.

**Sign convention:**
- `disparity = 0.0` — layer appears at screen depth (zero parallax). Recommended default
  for most HUD and UI overlays.
- `disparity < 0` — layer appears to float in front of the screen (toward the viewer).
- `disparity > 0` — layer appears behind the screen surface.

**Recommended ranges:**
| Use Case | Typical Disparity | Perceived Effect |
|---|---|---|
| Flat HUD / status overlay | `0.0` | Pinned to screen surface. No stereo conflict. |
| Subtle pop-out | `-0.005` to `-0.01` | Gentle float above content. Comfortable for extended viewing. |
| Strong pop-out (alerts) | `-0.02` to `-0.05` | Noticeable depth separation. Use sparingly. |
| Behind-screen elements | `+0.005` to `+0.02` | Recessed into the scene. |

**Guidelines:**
- Values beyond `+/- 0.10` (10% of window width) produce extreme parallax and may cause
  discomfort. Avoid for sustained UI elements.
- For text-heavy overlays, `0.0` is strongly recommended to avoid stereo rivalry on fine
  detail.
- The optimal disparity is resolution-independent because it is expressed as a fraction of
  window width, not in pixels.

### New Functions

None. This extension operates entirely through structure chaining:
- `XrWin32WindowBindingCreateInfoEXT` chains to `XrSessionCreateInfo`.
- `XrCompositionLayerWindowSpaceEXT` is submitted as a composition layer in `xrEndFrame`.

### Interactions

- **Requires** a Win32 platform binding extension (`XR_KHR_D3D11_enable` or
  `XR_KHR_opengl_enable`) for the graphics binding in the session creation chain.
- **Does not require** `XR_EXT_display_info`, but they are complementary.
- When the window is resized, the runtime **must** adjust its rendering surface
  accordingly. The application should recompute render resolution using the display info
  scale factors if available.

### Example Code: Session Creation with D3D11

```cpp
// 1. Enable the extension at instance creation
std::vector<const char*> extensions = {
    XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
    XR_EXT_WIN32_WINDOW_BINDING_EXTENSION_NAME,
};

XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
createInfo.enabledExtensionCount = (uint32_t)extensions.size();
createInfo.enabledExtensionNames = extensions.data();
// ... fill in applicationInfo ...
xrCreateInstance(&createInfo, &instance);

// 2. Create session with HWND binding chained to graphics binding
XrGraphicsBindingD3D11KHR d3d11Binding = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
d3d11Binding.device = d3d11Device;

XrWin32WindowBindingCreateInfoEXT windowBinding = {
    XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT};
windowBinding.windowHandle = hwnd;  // Application-owned HWND

// Chain: sessionInfo -> d3d11Binding -> windowBinding
d3d11Binding.next = &windowBinding;

XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
sessionInfo.next = &d3d11Binding;
sessionInfo.systemId = systemId;

xrCreateSession(instance, &sessionInfo, &session);
```

### Example Code: Session Creation with OpenGL

```cpp
XrGraphicsBindingOpenGLWin32KHR glBinding = {
    XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR};
glBinding.hDC = hDC;
glBinding.hGLRC = hGLRC;

XrWin32WindowBindingCreateInfoEXT windowBinding = {
    XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT};
windowBinding.windowHandle = hwnd;

// Chain: sessionInfo -> glBinding -> windowBinding
glBinding.next = &windowBinding;

XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
sessionInfo.next = &glBinding;
sessionInfo.systemId = systemId;

xrCreateSession(instance, &sessionInfo, &session);
```

### Example Code: Submitting a Window-Space HUD Layer

```cpp
// Render HUD content to a dedicated swapchain, then submit it as a
// window-space layer alongside the main projection layer.

XrCompositionLayerWindowSpaceEXT hudLayer = {};
hudLayer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT;
hudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
hudLayer.subImage.swapchain = hudSwapchain;
hudLayer.subImage.imageRect.offset = {0, 0};
hudLayer.subImage.imageRect.extent = {(int32_t)hudWidth, (int32_t)hudHeight};
hudLayer.subImage.imageArrayIndex = 0;
hudLayer.x        = 0.0f;   // top-left corner
hudLayer.y        = 0.0f;
hudLayer.width    = 0.30f;  // 30% of window width
hudLayer.height   = 0.25f;  // 25% of window height
hudLayer.disparity = 0.0f;  // at screen depth (zero parallax)

const XrCompositionLayerBaseHeader* layers[] = {
    (XrCompositionLayerBaseHeader*)&projectionLayer,
    (XrCompositionLayerBaseHeader*)&hudLayer,
};

XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
endInfo.displayTime = predictedDisplayTime;
endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
endInfo.layerCount = 2;
endInfo.layers = layers;

xrEndFrame(session, &endInfo);
```

---

## 4. Extension 2: XR_EXT_android_surface_binding

> **Status: Planned — not yet implemented.** The extension header exists but no
> compositor or platform code implements Android surface binding yet.

### IP Status

No known IP claims.

### Name Strings

- Extension name: `XR_EXT_android_surface_binding`
- Spec version: 1
- Extension name define: `XR_EXT_ANDROID_SURFACE_BINDING_EXTENSION_NAME`

### Overview

This extension is the Android counterpart to `XR_EXT_win32_window_binding`. It allows an
OpenXR application to provide its own Android rendering surface to the runtime via the
session creation chain. When provided, the runtime renders into the application's surface
instead of managing its own display output.

On Android, applications inherently own their Activity and rendering surface. This
extension formalizes the handoff: the application passes an `ANativeWindow*` (obtained
from a `SurfaceView` or `SurfaceHolder` via the NDK) to the runtime, which uses it as the
compositing and interlacing target. The application also provides the Java `Surface`
`jobject`, which the runtime may need for platform SDK integration (some vendor Android
SDKs require the Java Surface for interlacer initialization). Additionally, the application must provide
the surface's screen-space position, since neither `ANativeWindow` nor `Surface` exposes
where the surface is located on the physical display — information the runtime needs for
correct light field interlacing.

**Use cases:**
- **Application-owned rendering surface**: the application controls the `SurfaceView`
  lifecycle, visibility, and z-order within the Android view hierarchy.
- **Hybrid 2D/3D UI**: 3D content composited alongside Android UI elements (toolbars,
  overlays, system UI).
- **Multi-surface scenarios**: the application may have multiple surfaces and dedicate one
  to OpenXR stereo content.
- **Window-space overlays**: HUD and status overlays positioned in fractional surface
  coordinates, automatically adapting to surface resize (using
  `XrCompositionLayerWindowSpaceEXT`).

### New Enum Constants

```c
#define XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_EXT  ((XrStructureType)1000999005)
```

> **Note**: This value uses the vendor extension range. It would be replaced with an
> officially assigned value upon standardization.

### New Structures

#### XrAndroidSurfaceBindingCreateInfoEXT

Chained to `XrSessionCreateInfo` (via the graphics binding's `next` pointer) to provide
an external native window for session rendering.

| Member | Type | Description |
|---|---|---|
| `type` | `XrStructureType` | Must be `XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_EXT`. |
| `next` | `const void*` | Pointer to next structure in the chain, or `NULL`. |
| `nativeWindow` | `ANativeWindow*` | The Android native window to render into. Must be a valid, active native window. |
| `surface` | `jobject` | The Java `android.view.Surface` associated with the native window. The runtime may need this for platform SDK initialization (e.g., a vendor's interlacer). May be `NULL` if the runtime does not require it. |
| `screenOffsetX` | `int32_t` | Horizontal offset of the surface's left edge on the physical display, in display pixels. |
| `screenOffsetY` | `int32_t` | Vertical offset of the surface's top edge on the physical display, in display pixels. |

```c
typedef struct XrAndroidSurfaceBindingCreateInfoEXT {
    XrStructureType             type;
    const void* XR_MAY_ALIAS    next;
    ANativeWindow*              nativeWindow;
    jobject                     surface;
    int32_t                     screenOffsetX;
    int32_t                     screenOffsetY;
} XrAndroidSurfaceBindingCreateInfoEXT;
```

**Valid Usage:**
- `type` **must** be `XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_EXT`.
- `nativeWindow` **must** be a valid `ANativeWindow*` obtained via
  `ANativeWindow_fromSurface()` or `ASurfaceHolder_getNativeWindow()`.
- `surface` **should** be a valid global reference to the Java `android.view.Surface`
  associated with `nativeWindow`. It **may** be `NULL` if the runtime does not require it
  for platform SDK integration, but runtimes **should** document whether they need it.
- `screenOffsetX` and `screenOffsetY` **must** be the surface's top-left corner position
  in physical display pixel coordinates. The application obtains these by calling
  `View.getLocationOnScreen()` on the `SurfaceView`. For a fullscreen surface on the
  primary display, both values are typically `0`.
- The native window **must** remain valid for the lifetime of the `XrSession`.
- The application **must not** release the native window (via `ANativeWindow_release()`)
  before calling `xrDestroySession`.
- The application **should** acquire a reference (via `ANativeWindow_acquire()`) to ensure
  the window outlives the session.
- If the surface moves or is resized (e.g., multi-window mode, orientation change), the
  application **should** destroy and recreate the session with updated offsets, or the
  runtime **may** provide a mechanism to update offsets dynamically in a future revision.

### New Functions

None. This extension operates entirely through structure chaining:
- `XrAndroidSurfaceBindingCreateInfoEXT` chains to `XrSessionCreateInfo`.
- `XrCompositionLayerWindowSpaceEXT` (defined in `XR_EXT_win32_window_binding`) is
  platform-independent and works with Android surface bindings as well.

### Interactions

- **Requires** an Android-compatible graphics binding extension (`XR_KHR_vulkan_enable` or
  `XR_KHR_opengl_es_enable`) for the graphics binding in the session creation chain.
- **Does not require** `XR_EXT_display_info`, but they are complementary.
- **Mutually exclusive** with `XR_EXT_win32_window_binding` — an application uses one or
  the other based on platform.
- When the surface is resized (e.g., orientation change), the runtime **must** adjust its
  rendering surface accordingly.

### Example Code: Session Creation with Vulkan on Android

```cpp
// 1. Enable the extension at instance creation
std::vector<const char*> extensions = {
    XR_KHR_VULKAN_ENABLE_EXTENSION_NAME,
    XR_EXT_ANDROID_SURFACE_BINDING_EXTENSION_NAME,
};

XrInstanceCreateInfo createInfo = {XR_TYPE_INSTANCE_CREATE_INFO};
createInfo.enabledExtensionCount = (uint32_t)extensions.size();
createInfo.enabledExtensionNames = extensions.data();
// ... fill in applicationInfo ...
xrCreateInstance(&createInfo, &instance);

// 2. Obtain ANativeWindow and screen position from a SurfaceView
ANativeWindow* nativeWindow = ANativeWindow_fromSurface(env, javaSurface);
ANativeWindow_acquire(nativeWindow);  // ensure lifetime

// Get screen position: call SurfaceView.getLocationOnScreen() from Java
jint location[2];
// ... JNI call to getLocationOnScreen(location) ...
int32_t screenX = location[0];
int32_t screenY = location[1];

// 3. Create session with surface binding chained to graphics binding
XrGraphicsBindingVulkanKHR vkBinding = {XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
vkBinding.instance = vkInstance;
vkBinding.physicalDevice = vkPhysicalDevice;
vkBinding.device = vkDevice;
vkBinding.queueFamilyIndex = graphicsQueueFamily;
vkBinding.queueIndex = 0;

XrAndroidSurfaceBindingCreateInfoEXT surfaceBinding = {
    (XrStructureType)XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_EXT};
surfaceBinding.nativeWindow = nativeWindow;
surfaceBinding.surface = javaSurface;       // Java Surface jobject
surfaceBinding.screenOffsetX = screenX;     // position on physical display
surfaceBinding.screenOffsetY = screenY;

// Chain: sessionInfo -> vkBinding -> surfaceBinding
vkBinding.next = &surfaceBinding;

XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
sessionInfo.next = &vkBinding;
sessionInfo.systemId = systemId;

xrCreateSession(instance, &sessionInfo, &session);
```

### Example Code: Session Creation with OpenGL ES on Android

```cpp
XrGraphicsBindingOpenGLESAndroidKHR glesBinding = {
    XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
glesBinding.display = eglDisplay;
glesBinding.config = eglConfig;
glesBinding.context = eglContext;

XrAndroidSurfaceBindingCreateInfoEXT surfaceBinding = {
    (XrStructureType)XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_EXT};
surfaceBinding.nativeWindow = nativeWindow;
surfaceBinding.surface = javaSurface;
surfaceBinding.screenOffsetX = screenX;
surfaceBinding.screenOffsetY = screenY;

// Chain: sessionInfo -> glesBinding -> surfaceBinding
glesBinding.next = &surfaceBinding;

XrSessionCreateInfo sessionInfo = {XR_TYPE_SESSION_CREATE_INFO};
sessionInfo.next = &glesBinding;
sessionInfo.systemId = systemId;

xrCreateSession(instance, &sessionInfo, &session);
```

---

## 5. Extension 3: XR_EXT_display_info

### IP Status

No known IP claims.

### Name Strings

- Extension name: `XR_EXT_display_info`
- Spec version: 14
- Extension name define: `XR_EXT_DISPLAY_INFO_EXTENSION_NAME`

### Overview

This extension exposes the physical properties of a tracked 3D display to the application:
the display's physical dimensions, its nominal viewer position, and recommended render
resolution scale factors. It also lets the application **enumerate the available display
rendering modes** (mono / stereo / multi-view tiled layouts and vendor-specific variations)
and request a mode at runtime, and exposes eye-tracking mode control.

With this information the application can:
- Build its own camera model (Kooima off-axis projection) from raw tracked eye positions.
- Compute render resolution dynamically as the window/surface resizes.
- Locate views and submit layers using LOCAL space (RAW mode returns screen-relative positions).
- Enumerate rendering modes (`xrEnumerateDisplayRenderingModesEXT`) — including each mode's view
  count, tile layout, view scale, hardware-3D state, and (per session) which mode is active and
  which are requestable — and request a mode via `xrRequestDisplayRenderingModeEXT`.
- Choose between managed and manual eye tracking.

This extension is **platform-independent**. It works on any platform that supports OpenXR,
regardless of the graphics API or windowing system in use.

### New Enum Constants

```c
#define XR_TYPE_DISPLAY_INFO_EXT              ((XrStructureType)1000999003)
#define XR_DISPLAY_MODE_2D_EXT                0
#define XR_DISPLAY_MODE_3D_EXT                1
```

> **Note**: These values use the vendor extension range. They would be replaced with
> officially assigned values upon standardization.

### New Structures

#### XrDisplayInfoEXT

Chained to `XrSystemProperties` to return physical display information.

| Member | Type | Description |
|---|---|---|
| `type` | `XrStructureType` | Must be `XR_TYPE_DISPLAY_INFO_EXT`. |
| `next` | `void*` | Pointer to next structure in the chain, or `NULL`. |
| `displaySizeMeters` | `XrExtent2Df` | Physical display rectangle size in meters (`width`, `height`). |
| `nominalViewerPositionInDisplaySpace` | `XrVector3f` | Design-time expected viewer position relative to display center (meters). Defines the apex of the canonical display pyramid. See [Nominal Viewer Position](#nominal-viewer-position). |
| `recommendedViewScaleX` | `float` | Horizontal render resolution scale factor. See [Recommended View Scale](#recommended-view-scale). |
| `recommendedViewScaleY` | `float` | Vertical render resolution scale factor. See [Recommended View Scale](#recommended-view-scale). |
| `displayPixelWidth` | `uint32_t` | Native display panel width in pixels (0 if unknown). |
| `displayPixelHeight` | `uint32_t` | Native display panel height in pixels (0 if unknown). |

> **Note:** `hardwareDisplay3D` was removed from `XrDisplayInfoEXT` in v12. It is now a per-mode
> field on `XrDisplayRenderingModeInfoEXT` (and is also reported by the
> `XrEventDataHardwareDisplayStateChangedEXT` event). Query it via
> `xrEnumerateDisplayRenderingModesEXT` — see
> [Enumerating Rendering Modes](#enumerating-rendering-modes-v8--xrdisplayrenderingmodeinfoext--xrenumeratedisplayrenderingmodesext).
> Likewise, the modern way to learn the view count, tile layout, per-view scale, active mode, and
> which modes are requestable is the same enumeration API — not fields on `XrDisplayInfoEXT`.

```c
typedef struct XrDisplayInfoEXT {
    XrStructureType             type;
    void* XR_MAY_ALIAS          next;
    XrExtent2Df                 displaySizeMeters;
    XrVector3f                  nominalViewerPositionInDisplaySpace;
    float                       recommendedViewScaleX;
    float                       recommendedViewScaleY;
    uint32_t                    displayPixelWidth;
    uint32_t                    displayPixelHeight;
} XrDisplayInfoEXT;
```

**Valid Usage:**
- `type` **must** be `XR_TYPE_DISPLAY_INFO_EXT`.
- The application **must** have enabled the `XR_EXT_display_info` extension at instance
  creation.
- The runtime fills in all fields when `xrGetSystemProperties` is called with this
  structure chained to `XrSystemProperties`.
- All returned values are static display properties that do not change during the runtime's
  lifetime.

### New Enums

#### XrDisplayModeEXT

> **Deprecated in v10.** `XrDisplayModeEXT` is deprecated in favor of `xrRequestDisplayRenderingModeEXT`, which provides a unified rendering mode API covering both 2D/3D switching and vendor-specific rendering variations. New applications should use `xrRequestDisplayRenderingModeEXT`. `XrDisplayModeEXT` and `xrRequestDisplayModeEXT` remain supported for backward compatibility.

```c
typedef enum XrDisplayModeEXT {
    XR_DISPLAY_MODE_2D_EXT = 0,
    XR_DISPLAY_MODE_3D_EXT = 1,
} XrDisplayModeEXT;
```

| Value | Meaning |
|---|---|
| `XR_DISPLAY_MODE_2D_EXT` | Standard 2D display mode. Switchable optics or backlight are disabled; the display behaves as a conventional flat panel. |
| `XR_DISPLAY_MODE_3D_EXT` | Tracked 3D display mode. Switchable optics or backlight are enabled; the display produces glasses-free 3D imagery via light field interlacing. |

### New Functions

This extension's current (v14) function set (v14 adds no functions — only the chained
`XrDisplayRenderingModeTrackingInfoEXT` struct and the
`XrEventDataEyeTrackingStateChangedEXT` event; see
[Per-Mode Tracking Capability + Tracking-State Event (v14)](#7c-per-mode-tracking-capability--tracking-state-event-v14)):

| Function | Added | Purpose |
|---|---|---|
| [`xrEnumerateDisplayRenderingModesEXT`](#enumerating-rendering-modes-v8--xrdisplayrenderingmodeinfoext--xrenumeratedisplayrenderingmodesext) | v8 | Enumerate available rendering modes (`XrDisplayRenderingModeInfoEXT`). |
| [`xrRequestDisplayRenderingModeEXT`](#7-display-rendering-mode-control-v7) | v7 | Request a rendering mode by index (the unified mode-control API). |
| [`xrRequestEyeTrackingModeEXT`](#new-function-xrrequesteyetrackingmodeext) | v6 | Switch between managed and manual eye tracking. |
| `xrRequestDisplayModeEXT` | v4 (**deprecated** v10) | Legacy 2D/3D toggle — thin wrapper over `xrRequestDisplayRenderingModeEXT`. |

#### xrRequestDisplayModeEXT

> **Deprecated in v10.** Use `xrRequestDisplayRenderingModeEXT` instead. See [Display Rendering Mode Control (v7)](#7-display-rendering-mode-control-v7) for the unified replacement (and [Enumerating Rendering Modes](#enumerating-rendering-modes-v8--xrdisplayrenderingmodeinfoext--xrenumeratedisplayrenderingmodesext) to discover modes). `xrRequestDisplayModeEXT` remains supported for backward compatibility as a thin wrapper that finds the first mode matching the requested hardware-3D state and delegates.

Requests that the runtime switch the display between 2D and 3D modes.

```c
XrResult xrRequestDisplayModeEXT(
    XrSession           session,
    XrDisplayModeEXT    mode);
```

**Parameters:**

| Parameter | Description |
|---|---|
| `session` | A valid `XrSession` handle. |
| `mode` | The requested display mode (`XR_DISPLAY_MODE_2D_EXT` or `XR_DISPLAY_MODE_3D_EXT`). |

**Return Codes:**

| Code | Meaning |
|---|---|
| `XR_SUCCESS` | The mode request was accepted by the runtime. |
| `XR_ERROR_FEATURE_UNSUPPORTED` | The display does not support mode switching (no enumerated rendering mode reports `hardwareDisplay3D == XR_TRUE`). |
| `XR_ERROR_SESSION_NOT_RUNNING` | The session is not in a running state. |
| `XR_ERROR_HANDLE_INVALID` | The session handle is invalid. |

**Semantics:**

- This function is a **request**, not a guarantee. The runtime forwards the request to the
  underlying platform SDK via the display processor (some vendors implement it as a
  preference-based hint, others as direct backlight control). The platform may aggregate
  requests from multiple applications or defer the switch.
- The function returns `XR_SUCCESS` when the request has been accepted, regardless of
  whether the display has physically completed the mode change.
- The application **may** call this function at any time while the session is running.
- If no enumerated rendering mode reports `hardwareDisplay3D == XR_TRUE` (the display has no 3D
  mode), this function returns `XR_ERROR_FEATURE_UNSUPPORTED` and has no effect. (`hardwareDisplay3D`
  is a per-mode field on `XrDisplayRenderingModeInfoEXT`; it is no longer on `XrDisplayInfoEXT`.)

### Display Mode Switching

#### Automatic Lifecycle Behavior

The runtime manages display mode transitions automatically as part of the session
lifecycle:

- When the session transitions to the **READY** state, the runtime **automatically
  requests** `XR_DISPLAY_MODE_3D_EXT` on displays that support mode switching.
- When the session transitions to the **STOPPING** state (or is destroyed), the runtime
  **automatically requests** `XR_DISPLAY_MODE_2D_EXT`.

This ensures correct behavior for applications that never call `xrRequestDisplayModeEXT`:
the display enters 3D mode when the XR session starts and returns to 2D mode when it ends.

#### Explicit Override

The application **may** call `xrRequestDisplayModeEXT` at any time during the session to
override the automatic behavior. Use cases include:

- Temporarily switching to 2D mode for a settings menu or 2D video playback.
- Deferring 3D mode activation until the application has finished loading.
- Implementing a user-facing 2D/3D toggle.

#### Mono Submission in 2D Mode

When the display is in 2D mode, the application **may** submit a single-view projection
layer (`viewCount == 1`) to `xrEndFrame`, even though the session view configuration is
`XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO`. This enables the application to render a
single full-resolution view instead of two reduced-resolution stereo views, yielding a
significant quality improvement for 2D content.

**Runtime behavior:**
- `xrLocateViews` is called with an `XRT_MAX_VIEWS` (8)-sized buffer and returns the active
  mode's views; the application uses the returned eye positions to compute a center-eye camera
  position (or any position it chooses). (See the multiview model — the render/submit loop is
  bounded by the active mode's `viewCount`, not a hardcoded 2.)
- `xrEndFrame` accepts `viewCount == 1` projection layers when the active mode is a 2D/mono mode.
  In a 3D mode, `viewCount` must equal the active rendering mode's `viewCount` (2 for SBS stereo,
  4 for quad, etc.).
- The compositor renders the single view to fill the full display output and skips light
  field interlacing (weaving), since 2D mode does not use the lenticular optics.

**Application responsibilities:**
- Detect 2D mode (via a prior call to `xrRequestDisplayModeEXT` or an application toggle).
- Create or reuse a swapchain at full window/display resolution for the mono view.
- Render a single view using a center-eye camera position and full-resolution viewport.
- Submit `viewCount == 1` with the single projection view to `xrEndFrame`.
- When switching back to a 3D mode, resume submitting the active mode's `viewCount` (2 for SBS
  stereo, 4 for quad) with per-view content at the recommended scaled resolution.

**Backward compatibility:**
- Applications that always submit the stereo view count continue to work in 2D and stereo 3D
  modes. The runtime blits the content as before. (Multi-view modes such as quad require the app
  to honor the active mode's `viewCount`.)

#### Implementation Notes

The runtime translates `xrRequestDisplayModeEXT` into the appropriate vendor SDK call via the
display processor's `set_property` method. Vendor implementations vary — a preference-based
hint aggregated across applications, or direct hardware (backlight/optics) control. For the
concrete per-platform mapping of the first vendor integration, see
[Leia display mode switching](../../vendors/leia/display-mode-switching.md).

This abstraction also operates through:
- `XrDisplayInfoEXT` chaining to `xrGetSystemProperties`.
- `xrRequestDisplayModeEXT` for explicit display mode control.

### New Event Types (v10)

Two new event types are delivered via `xrPollEvent` to notify applications of asynchronous state changes:

- **`XrEventDataRenderingModeChangedEXT`** — fired when the active display rendering mode changes (e.g., after a call to `xrRequestDisplayRenderingModeEXT` completes, or when the runtime changes modes autonomously). Applications that need to react to rendering mode transitions (e.g., reconfigure swapchains or shaders) should poll for this event.

- **`XrEventDataHardwareDisplayStateChangedEXT`** — fired when the hardware 3D display state changes (e.g., the switchable lenses or backlight are enabled or disabled). Applications can use this event to update their UI or rendering pipeline in response to system-level display state changes.

### Example Code: Querying Display Mode Support and Requesting 2D

> ⚠️ **These inline examples predate the multiview model and the v13 header — read with care.**
> Three things below are stale (corrected inline where safe, flagged where a rewrite would need
> unverified API). For authoritative, working code use the shipping test apps
> (`test_apps/cube_handle_*`, `test_apps/common/xr_session_common.cpp`) and
> [`docs/guides/displayxr-app-rules.md`](../../guides/displayxr-app-rules.md):
> 1. **View arrays must be `XRT_MAX_VIEWS` (8)-sized, not `[2]`**, and render loops must be
>    bounded by the **active mode's `viewCount`**, not a hardcoded 2 (quad modes have 4 views).
> 2. **`hardwareDisplay3D` is no longer a field on `XrDisplayInfoEXT`** (v13). It lives per-mode on
>    `XrDisplayRenderingModeInfoEXT` and on the `XrEventDataHardwareDisplayStateChangedEXT` event.
> 3. **`xrRequestDisplayModeEXT` / `XR_DISPLAY_MODE_*_EXT` are deprecated** in favor of
>    `xrRequestDisplayRenderingModeEXT(session, modeIndex)` driven by mode enumeration; mode changes
>    are *requests* — update local state only on `XrEventDataRenderingModeChangedEXT`.

```cpp
// Query display capabilities
XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
XrDisplayInfoEXT displayInfo = {(XrStructureType)XR_TYPE_DISPLAY_INFO_EXT};
sysProps.next = &displayInfo;
xrGetSystemProperties(instance, systemId, &sysProps);

// NOTE (v13): hardwareDisplay3D is NOT on XrDisplayInfoEXT anymore — enumerate
// rendering modes (xrEnumerateDisplayRenderingModesEXT) and read it per-mode on
// XrDisplayRenderingModeInfoEXT. The pseudo-code below uses the DEPRECATED
// xrRequestDisplayModeEXT API; the current path is xrRequestDisplayRenderingModeEXT
// (request a mode index, then react to XrEventDataRenderingModeChangedEXT).
if (/* a 3D-capable rendering mode exists */ false) {
    // Example (deprecated API shown for historical context only):
    //   xrRequestDisplayModeEXT(session, XR_DISPLAY_MODE_2D_EXT);   // -> request a 2D mode index
    //   ... user interacts with 2D menu ...
    //   xrRequestDisplayModeEXT(session, XR_DISPLAY_MODE_3D_EXT);   // -> request a 3D mode index
}

// Example: mono submission in 2D mode
if (!displayMode3D) {
    // xrLocateViews fills an 8-wide buffer; in a 2D mode viewCount comes back 1,
    // but locate into XRT_MAX_VIEWS regardless and compute the center eye from the
    // valid entries.
    XrView views[8];
    for (uint32_t i = 0; i < 8; i++) views[i] = {XR_TYPE_VIEW};
    uint32_t viewCount = 8;
    // ... xrLocateViews(session, ..., 8, &viewCount, views) ...
    XrVector3f centerEye = {0, 0, 0};
    for (uint32_t i = 0; i < viewCount; i++) {
        centerEye.x += views[i].pose.position.x / viewCount;
        centerEye.y += views[i].pose.position.y / viewCount;
        centerEye.z += views[i].pose.position.z / viewCount;
    }

    // Render 1 view at full window resolution (no stereo scale factors)
    // ... render to monoSwapchain at windowWidth x windowHeight ...

    // Submit single projection view
    XrCompositionLayerProjectionView monoView = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    monoView.subImage.swapchain = monoSwapchain;
    monoView.subImage.imageRect = {{0, 0}, {(int32_t)windowWidth, (int32_t)windowHeight}};
    monoView.pose.position = centerEye;
    monoView.fov = /* center-eye FOV */;

    XrCompositionLayerProjection projLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projLayer.viewCount = 1;  // Mono submission — accepted in 2D mode
    projLayer.views = &monoView;
    // ... xrEndFrame with projLayer ...
}
```

### Canonical Display Pyramid

The canonical display pyramid is a formal geometric concept central to this extension:

```
                    Nominal Viewer (apex)
                         /    \
                        /      \
                       /        \
                      /   view   \
                     /   frustum  \
                    /              \
         ┌─────────────────────────────┐
         │                             │
         │     Physical Display        │
         │     (displaySizeMeters)     │
         │                             │
         └─────────────────────────────┘
                    (base)
```

- The **base** is the physical display rectangle, with dimensions
  `displaySizeMeters.width` x `displaySizeMeters.height`.
- The **apex** is the nominal viewer position
  (`nominalViewerPositionInDisplaySpace`), typically 0.5–0.7 meters in front of the
  display center along the +Z axis.
- The **frustum edges** are the rays from the apex through each corner of the display.

This pyramid defines the natural mono viewing frustum. Stereo rendering is then sampling
this pyramid from two nearby eye positions (the tracked physical eyes).

### Nominal Viewer Position

`nominalViewerPositionInDisplaySpace` is a **static, non-tracked, design-time expectation**
of where the viewer should be relative to the display. It is **not** the actual tracked
viewer position.

**Interpretation:**
- Actual tracked eyes are expected to *vary around* this position during use.
- At the nominal position: parallax is neutral, depth perception feels natural, and the
  canonical display pyramid is perfectly aligned.
- The nominal viewer position anchors stereo geometry and first-person camera alignment.
- It defines the apex of the canonical display pyramid.

**Typical value:**
- `{0.0, 0.0, 0.65}` in display space (65 cm directly in front of screen center).

### Recommended View Scale

The `recommendedViewScaleX` and `recommendedViewScaleY` fields are a **single pair shared
by both eyes**. They specify how to compute the optimal render resolution for each eye's
texture from the current window size:

```
renderWidth  = (uint32_t)(windowWidth  * recommendedViewScaleX)
renderHeight = (uint32_t)(windowHeight * recommendedViewScaleY)
```

**Semantics:**
- The scale factors are **static display properties**: they encode the ratio of the
  display's optimal internal render resolution to its native pixel resolution (e.g.,
  `sr_recommended_width / display_pixel_width`).
- They do **not** change with window resize. The formula above naturally produces the
  correct render resolution for any window size.
- **Both eyes use the same scale factors.** The scale encodes a static display property
  (ratio of optimal render resolution to native pixels), which is identical for left and
  right eyes.
- **Anisotropic scaling is intentional and supported**: `scaleX` may differ from `scaleY`
  because the optimal horizontal and vertical resolutions may have different ratios to
  native pixels (e.g., light field displays often need higher horizontal resolution for
  multi-view interlacing).
- Scale factors represent **quality scaling only**. Aspect ratio is controlled by the
  window viewport and projection.

**Example:**
- Display native resolution: 3840 x 2160.
- SR recommended render resolution: 1920 x 2160.
- Scale factors: `scaleX = 0.5`, `scaleY = 1.0`.
- If the window is 1920 x 1080: render at 960 x 1080 per eye.
- If the window is 3840 x 2160 (fullscreen): render at 1920 x 2160 per eye.

### RAW Mode

When `XR_EXT_display_info` is enabled, `xrLocateViews()` returns views in **RAW mode**:

- `XrView.pose.position` — the physical eye center in screen-centered coordinates
  (origin at display center, +X right, +Y up, +Z toward viewer).
- `XrView.pose.orientation` — identity quaternion `{0, 0, 0, 1}`.
- `XrView.fov` — advisory only. The application should compute its own FOV from the eye
  position and display geometry.

The runtime returns screen-relative eye positions **regardless of the reference space
parameter** passed to `xrLocateViews`. Applications should pass LOCAL space. The runtime
applies **no convergence adjustment or camera policy** to RAW views. The application is
fully responsible for its camera model.

**Kooima projection from RAW views:**

The Kooima off-axis projection algorithm computes an asymmetric frustum where the near
plane maps to the physical screen edges as seen from the eye position:

```
left   = nearZ * (-halfWidth  - eyeX) / eyeZ
right  = nearZ * (+halfWidth  - eyeX) / eyeZ
bottom = nearZ * (-halfHeight - eyeY) / eyeZ
top    = nearZ * (+halfHeight - eyeY) / eyeZ
```

Where `halfWidth = displaySizeMeters.width / 2`, `halfHeight = displaySizeMeters.height / 2`,
and `(eyeX, eyeY, eyeZ)` is the eye position from `XrView.pose.position` (screen-centered).

### RENDER_READY Mode

When `XR_EXT_display_info` is **not** enabled (or when explicitly overridden), the runtime
returns views in **RENDER_READY mode**:

- `XrView.pose` — view pose with convergence and comfort adjustments applied by the
  runtime.
- `XrView.fov` — field-of-view angles derived from the runtime's internal camera model.
  The application constructs a standard symmetric or asymmetric projection matrix from
  these angles (the runtime does **not** return a projection matrix directly — only FOV
  angles, per the core OpenXR `XrView` structure).

This mode is suitable for legacy OpenXR applications and WebXR, which expect the runtime
to own the camera model. The application still builds its own projection matrix from the
returned `XrFovf` using the standard OpenXR projection formula.

### Example Code: Querying Display Info

```cpp
// Chain XrDisplayInfoEXT to XrSystemProperties
XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
XrDisplayInfoEXT displayInfo = {(XrStructureType)XR_TYPE_DISPLAY_INFO_EXT};
sysProps.next = &displayInfo;

xrGetSystemProperties(instance, systemId, &sysProps);

// Now displayInfo is filled by the runtime:
float displayWidthM  = displayInfo.displaySizeMeters.width;   // e.g. 0.344
float displayHeightM = displayInfo.displaySizeMeters.height;  // e.g. 0.194
float scaleX = displayInfo.recommendedViewScaleX;             // e.g. 0.5
float scaleY = displayInfo.recommendedViewScaleY;             // e.g. 1.0
XrVector3f nominalPos = displayInfo.nominalViewerPositionInDisplaySpace;
// nominalPos ~ {0.0, 0.0, 0.65}
// NOTE (v13): hardwareDisplay3D is NOT on XrDisplayInfoEXT — read it per-mode on
// XrDisplayRenderingModeInfoEXT (via xrEnumerateDisplayRenderingModesEXT) or from the
// XrEventDataHardwareDisplayStateChangedEXT event.
```

### Example Code: Locating Views in RAW Mode

```cpp
// In RAW mode (XR_EXT_display_info enabled), xrLocateViews returns
// screen-centered eye positions regardless of the space parameter.
// Use LOCAL space for both view location and layer submission.
XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
locateInfo.displayTime = predictedDisplayTime;
locateInfo.space = localSpace;

XrViewState viewState = {XR_TYPE_VIEW_STATE};
// Locate into an XRT_MAX_VIEWS (8)-wide buffer — modes range from 1 (mono) to 4
// (quad) views. Only the first viewCount entries are valid after the call.
uint32_t viewCount = 8;
XrView views[8];
for (uint32_t i = 0; i < 8; i++) views[i] = {XR_TYPE_VIEW};
xrLocateViews(session, &locateInfo, &viewState, 8, &viewCount, views);
// views[i].pose.position = screen-relative eye position (identity orientation)
```

### Example Code: Kooima Asymmetric Frustum Projection

> **Note — Reference Implementation, Not Normative API**
>
> The Kooima projection code below is provided as **example/reference code** for
> implementers, not as a shipped API or header-only library. Engine plugins (Unity,
> Unreal) will implement their own versions. Native application developers are expected
> to copy, adapt, or rewrite this math to suit their needs.
>
> This is intentional: the extension provides raw eye positions and display geometry as
> **primitives**. How the application converts these into a projection matrix is entirely
> up to the application. Developers may wish to modify perspective, baseline, parallax
> budget, or near/far planes in ways that a fixed runtime function cannot anticipate.

This function computes the off-axis projection matrix from a tracked eye position and the
physical display geometry, both obtained from `XR_EXT_display_info`:

```cpp
Matrix4x4 ComputeKooimaProjection(
    const XrVector3f& eyePos,
    float screenWidthM, float screenHeightM,
    float nearZ, float farZ)
{
    // Screen half-extents (display is centered at origin in screen-centered coords)
    float halfW = screenWidthM / 2.0f;
    float halfH = screenHeightM / 2.0f;

    float ez = eyePos.z;
    if (ez <= 0.001f) ez = 0.65f;  // fallback: nominal distance

    float ex = eyePos.x;
    float ey = eyePos.y;

    // Project screen edges onto near plane (similar triangles)
    float left   = nearZ * (-halfW - ex) / ez;
    float right  = nearZ * ( halfW - ex) / ez;
    float bottom = nearZ * (-halfH - ey) / ez;
    float top    = nearZ * ( halfH - ey) / ez;

    // Build asymmetric frustum projection matrix (OpenGL convention)
    float w = right - left;
    float h = top - bottom;

    Matrix4x4 proj = {};
    proj[0][0] = 2.0f * nearZ / w;
    proj[1][1] = 2.0f * nearZ / h;
    proj[2][0] = (right + left) / w;
    proj[2][1] = (top + bottom) / h;
    proj[2][2] = -(farZ + nearZ) / (farZ - nearZ);
    proj[2][3] = -1.0f;
    proj[3][2] = -2.0f * farZ * nearZ / (farZ - nearZ);

    return proj;
}

// Companion: compute XrFovf from the same inputs (for projectionView submission)
XrFovf ComputeKooimaFov(
    const XrVector3f& eyePos,
    float screenWidthM, float screenHeightM)
{
    float ez = eyePos.z;
    if (ez <= 0.001f) ez = 0.65f;

    float halfW = screenWidthM / 2.0f;
    float halfH = screenHeightM / 2.0f;

    XrFovf fov;
    fov.angleLeft  = atanf((-halfW - eyePos.x) / ez);
    fov.angleRight = atanf(( halfW - eyePos.x) / ez);
    fov.angleDown  = atanf((-halfH - eyePos.y) / ez);
    fov.angleUp    = atanf(( halfH - eyePos.y) / ez);

    return fov;
}
```

### Example Code: Complete Per-Frame Rendering Loop

This example shows a complete frame with RAW mode Kooima projection, dynamic render
resolution, and window-space HUD submission:

> ⚠️ **Stale render loop — illustrative, not copy-paste.** This example hardcodes 2 views and a
> per-eye swapchain array. The shipping model is N-view (1–4) into a **single tiled atlas
> swapchain**, with the loop bounded by the **active mode's `viewCount`**. The view-count fixes
> are applied below; for the full correct loop (tile layout, canvas×scale sizing, mode-change
> handling) see `test_apps/cube_handle_*` and
> [`docs/guides/displayxr-app-rules.md`](../../guides/displayxr-app-rules.md) §3–§4.

```cpp
// --- Frame begin ---
XrFrameState frameState = {XR_TYPE_FRAME_STATE};
XrFrameWaitInfo waitInfo = {XR_TYPE_FRAME_WAIT_INFO};
xrWaitFrame(session, &waitInfo, &frameState);

XrFrameBeginInfo beginInfo = {XR_TYPE_FRAME_BEGIN_INFO};
xrBeginFrame(session, &beginInfo);

if (frameState.shouldRender) {

    // --- Locate views in DISPLAY space (RAW mode) ---
    XrViewLocateInfo locateInfo = {XR_TYPE_VIEW_LOCATE_INFO};
    locateInfo.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locateInfo.displayTime = frameState.predictedDisplayTime;
    locateInfo.space = localSpace;

    XrViewState viewState = {XR_TYPE_VIEW_STATE};
    // Locate into an XRT_MAX_VIEWS (8)-wide buffer; only [0, viewCount) are valid.
    uint32_t viewCount = 8;
    XrView views[8];
    for (uint32_t i = 0; i < 8; i++) views[i] = {XR_TYPE_VIEW};
    xrLocateViews(session, &locateInfo, &viewState, 8, &viewCount, views);
    // Number of views to actually render/submit = the ACTIVE mode's viewCount
    // (from the enumerated XrDisplayRenderingModeInfoEXT), not necessarily viewCount.
    uint32_t renderViewCount = activeMode.viewCount;  // 1 (mono) / 2 (SBS) / 4 (quad)

    // --- Compute dynamic render resolution ---
    uint32_t renderW = (uint32_t)(windowWidth  * recommendedViewScaleX);
    uint32_t renderH = (uint32_t)(windowHeight * recommendedViewScaleY);
    // Clamp to swapchain maximum
    renderW = min(renderW, swapchainWidth);
    renderH = min(renderH, swapchainHeight);

    // --- Compute Kooima projection for each eye ---
    // Scale display physical size to match rendered fraction of swapchain
    float screenW = displayWidthM  * (float)renderW / (float)swapchainWidth;
    float screenH = displayHeightM * (float)renderH / (float)swapchainHeight;

    XrCompositionLayerProjectionView projViews[8] = {};

    for (uint32_t eye = 0; eye < renderViewCount; eye++) {
        // Acquire swapchain image
        uint32_t imageIndex;
        XrSwapchainImageAcquireInfo acqInfo = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        xrAcquireSwapchainImage(swapchains[eye], &acqInfo, &imageIndex);
        XrSwapchainImageWaitInfo swWait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        swWait.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(swapchains[eye], &swWait);

        // Build Kooima projection from raw eye position + display geometry
        Matrix4x4 proj = ComputeKooimaProjection(
            views[eye].pose.position, screenW, screenH, 0.01f, 100.0f);
        XrFovf fov = ComputeKooimaFov(
            views[eye].pose.position, screenW, screenH);

        // Build view matrix from eye pose
        Matrix4x4 view = PoseToViewMatrix(views[eye].pose);

        // Set viewport to rendered area
        SetViewport(0, 0, renderW, renderH);

        // Render scene content
        RenderScene(view, proj);

        // Release swapchain image
        XrSwapchainImageReleaseInfo relInfo = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(swapchains[eye], &relInfo);

        // Fill projection view for submission
        projViews[eye].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        projViews[eye].pose = views[eye].pose;
        projViews[eye].fov = fov;  // App-computed Kooima FOV
        projViews[eye].subImage.swapchain = swapchains[eye];
        projViews[eye].subImage.imageRect.offset = {0, 0};
        projViews[eye].subImage.imageRect.extent = {
            (int32_t)renderW, (int32_t)renderH};
        projViews[eye].subImage.imageArrayIndex = 0;
    }

    // --- Submit projection layer + HUD layer ---
    XrCompositionLayerProjection projLayer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    projLayer.space = localSpace;
    projLayer.viewCount = renderViewCount;
    projLayer.views = projViews;

    XrCompositionLayerWindowSpaceEXT hudLayer = {};
    hudLayer.type = (XrStructureType)XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT;
    hudLayer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
    hudLayer.subImage.swapchain = hudSwapchain;
    hudLayer.subImage.imageRect = {{0,0}, {(int32_t)hudW, (int32_t)hudH}};
    hudLayer.x = 0.0f;  hudLayer.y = 0.0f;
    hudLayer.width = 0.3f;  hudLayer.height = 0.25f;
    hudLayer.disparity = 0.0f;

    const XrCompositionLayerBaseHeader* layers[] = {
        (XrCompositionLayerBaseHeader*)&projLayer,
        (XrCompositionLayerBaseHeader*)&hudLayer,
    };

    XrFrameEndInfo endInfo = {XR_TYPE_FRAME_END_INFO};
    endInfo.displayTime = frameState.predictedDisplayTime;
    endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    endInfo.layerCount = 2;
    endInfo.layers = layers;

    xrEndFrame(session, &endInfo);
}
```

---

## 6. Interactions and External Dependencies

### OpenXR 1.0 Core

All extensions require OpenXR 1.0 and depend on core concepts: `XrInstance`, `XrSession`,
`XrSpace`, `XrSwapchain`, `xrLocateViews`, `xrEndFrame`, `xrGetSystemProperties`,
`xrCreateReferenceSpace`.

### Platform Dependencies

| Extension | Platform Requirement |
|---|---|
| `XR_EXT_win32_window_binding` | **Win32 only**. Requires a Win32 platform graphics binding (`XR_KHR_D3D11_enable` or `XR_KHR_opengl_enable`). |
| `XR_EXT_android_surface_binding` | **Android only**. Requires an Android-compatible graphics binding (`XR_KHR_vulkan_enable` or `XR_KHR_opengl_es_enable`). |
| `XR_EXT_display_info` | **Platform-independent**. Works on any platform with a tracked 3D display. |

### Graphics API Interactions

**Win32:**
- **`XR_KHR_D3D11_enable`**: `XrWin32WindowBindingCreateInfoEXT` chains to
  `XrGraphicsBindingD3D11KHR.next`. The runtime creates a D3D11 swap chain on the
  provided HWND.
- **`XR_KHR_opengl_enable`**: `XrWin32WindowBindingCreateInfoEXT` chains to
  `XrGraphicsBindingOpenGLWin32KHR.next`. The runtime renders using the provided OpenGL
  context and the window's device context.

**Android:**
- **`XR_KHR_vulkan_enable`**: `XrAndroidSurfaceBindingCreateInfoEXT` chains to
  `XrGraphicsBindingVulkanKHR.next`. The runtime creates a Vulkan swap chain on the
  provided `ANativeWindow`.
- **`XR_KHR_opengl_es_enable`**: `XrAndroidSurfaceBindingCreateInfoEXT` chains to
  `XrGraphicsBindingOpenGLESAndroidKHR.next`. The runtime renders using the provided
  EGL context and the native window.

### Cross-Extension Interaction

The window/surface binding extensions and `XR_EXT_display_info` are **independent**:

- No extension requires any other.
- Enabling a window/surface binding together with `XR_EXT_display_info` gives the
  application full control: app-owned rendering surface + app-owned camera model +
  display mode switching.
- Enabling only a window/surface binding allows app-owned rendering surface with
  runtime-owned stereo views (RENDER_READY).
- Enabling only `XR_EXT_display_info` allows display geometry queries, RAW eye
  positions, and display mode control while the runtime manages the display surface.
- `XR_EXT_win32_window_binding` and `XR_EXT_android_surface_binding` are **mutually
  exclusive** platform variants.

### Interaction with XR_KHR_composition_layer_quad

`XrCompositionLayerWindowSpaceEXT` and `XrCompositionLayerQuad` serve different purposes:

- `XrCompositionLayerQuad` positions content in 3D space (meters, in an XrSpace).
- `XrCompositionLayerWindowSpaceEXT` positions content in fractional window/surface
  coordinates, automatically adapting to resize. It is the natural choice for HUD overlays
  on tracked 3D displays. This layer type works with both Win32 and Android surface
  bindings.

### Vendor SDK Deployment Model

These extensions are designed for a multi-vendor ecosystem where each 3D display vendor
ships their own system-level runtime and public SDK, following the same model as CUDA,
OpenCL, and DirectX:

- **Vendor Runtime** (OEM-installed): Contains the vendor's IP — interlacing algorithms,
  eye tracking models, calibration data, and display-specific hardware control. Installed
  on the device by the display OEM (Samsung, Acer, ZTE, etc.) or a vendor installer.

- **Public SDK** (freely available): Contains only C/C++ headers and import stubs — no IP.
  Developers and CI systems download the SDK to build against. If the vendor runtime is not
  installed on the target device, SDK calls fail gracefully.

- **OpenXR Driver** (this repository): Open-source glue code under
  `src/xrt/drivers/<vendor>/` that translates vendor SDK types into the runtime's
  vendor-neutral `xrt_*` interfaces. Contains no vendor IP.

**Version Compatibility Contract**: Vendor runtimes and SDKs follow a forward-compatible
runtime, backward-compatible SDK rule:

- **New runtime + old SDK**: Works. The runtime exports a superset of all previous API
  versions, so old applications continue to work.
- **Old runtime + new SDK**: May not work. The new SDK may call functions the old runtime
  doesn't implement.

The OpenXR driver should build against the **latest** vendor SDK but **check API
availability at runtime** and degrade gracefully when features are not supported by the
installed runtime. The `XR_EXT_display_info` extension's capability reporting
(`XrDisplayInfoEXT`, `XrEyeTrackingModeCapabilitiesEXT`) reflects only what the device
actually supports, not the full SDK API surface.

See the archived **Vendor Integration Guide** (`docs/archive/vendor-integration-historical.md` §13)
for the complete deployment model, multi-vendor build strategy, and driver implementation
guidelines.

---

## 7. Issues (Design Decisions)

### Resolved Issues

**RESOLVED 1: Scale factors vs. absolute pixel sizes for render resolution.**

*Problem*: The application needs to know the optimal per-eye render resolution. Should the
extension provide absolute pixel counts or scale factors?

*Resolution*: **Scale factors**. Absolute pixel sizes break when the window is resized,
requiring the runtime to push dynamic events. Scale factors are static display properties
that compose naturally with any window size via simple multiplication:
`renderWidth = windowWidth * scaleX`. This eliminates the need for resize notifications
and makes the API simpler.

*Alternative considered*: Let the runtime handle render resolution entirely via
`XrViewConfigurationView.recommendedImageRectWidth/Height` (the standard OpenXR mechanism).
The runtime already knows the window size and controls swapchain dimensions. However,
exposing scale factors to the application provides better separation of concern in windowed
mode: the application knows its own window dimensions (it owns the window via
`XR_EXT_win32_window_binding`), so a simple multiply gives the optimal texture size without
the runtime needing to track window resize events, recompute the ratio of window size to
monitor native resolution, and re-scale the vendor SDK's recommended render size. The app-side
formula `windowWidth * scaleX` is trivial, deterministic, and requires no runtime round-trip.
This also lets the application intentionally deviate from the recommended resolution for
performance scaling.

---

**RESOLVED 2: Display coordinate frame — no dedicated reference space needed.**

*Problem*: How should the display's coordinate frame be exposed? Options: (a) a new
reference space type, or (b) implicit via RAW mode behavior.

*Resolution*: **No new reference space**. In RAW mode (`XR_EXT_display_info` enabled),
`xrLocateViews` returns eye positions in screen-centered coordinates regardless of the
reference space parameter. The display coordinate frame (origin at display center, +X right,
+Y up, +Z toward viewer) is implicit in the returned positions. Applications pass LOCAL
space for both `xrLocateViews` and layer submission. This avoids adding a new reference
space type and the associated mapping/validation code, while providing the same data to
applications.

---

**RESOLVED 3: Single query struct vs. separate function for display info.**

*Problem*: Should display info be queried via a new function or by chaining a struct to
an existing query?

*Resolution*: **Chaining to `xrGetSystemProperties`**. This follows the established OpenXR
pattern for system-level queries (see `XrSystemHandTrackingPropertiesEXT` and similar). It
avoids adding a new API entry point and leverages the existing `next` chain mechanism.

---

**RESOLVED 4: Where to chain the window binding struct.**

*Problem*: `XrWin32WindowBindingCreateInfoEXT` could chain directly to
`XrSessionCreateInfo.next` or to the graphics binding's `next` pointer.

*Resolution*: **Chain to the graphics binding**. The window handle is logically associated
with the graphics context (the runtime needs both the GPU device and the target window).
Chaining `SessionCreateInfo → GraphicsBinding → WindowBinding` keeps related information
together and mirrors how multi-part session configuration works in OpenXR.

---

**RESOLVED 5: App-side vs. runtime-side projection matrix calculation.**

*Problem*: Should the runtime compute the Kooima projection matrix internally and return
it to the application (more future-proof — tweaking the runtime adjusts all apps without
recompilation), or should the application compute projection from raw eye positions and
display geometry (more flexible — apps control their own camera model)?

*Resolution*: **App-side projection in RAW mode; runtime-side FOV in RENDER_READY mode.**
Both approaches coexist:

- **RAW mode** (extension enabled): the runtime provides raw eye positions and display
  geometry as primitives. The application (or engine plugin) builds its own projection
  matrix. This gives engines and native apps full control over perspective, baseline,
  parallax budget, convergence tuning, and near/far planes — parameters that vary across
  applications and that a fixed runtime function cannot anticipate.

- **RENDER_READY mode** (extension not enabled): the runtime computes view poses and FOV
  angles internally using its own Kooima implementation. Legacy apps and WebXR get
  functional stereo without implementing any projection math. Runtime updates can improve
  the camera model for all RENDER_READY consumers without app recompilation.

This split follows the principle that **extensions empower, defaults protect**: enabling
the extension opts into full app-side control; not enabling it keeps the runtime in charge.

---

**RESOLVED 6: Kooima projection delivery mechanism.**

*Problem*: Should the Kooima projection math be shipped as a header-only library, a
runtime API function, or example code?

*Resolution*: **Example/reference code in the specification and test apps.** Engine plugins
(Unity, Unreal) will implement their own optimized versions. Native app developers copy and
adapt the reference code. This is intentional: the extension provides geometric primitives
(eye positions, display dimensions), and the math to convert them into a projection matrix
is straightforward (5 lines of similar-triangle arithmetic). Shipping it as a runtime API
would prevent applications from modifying perspective, baseline, or parallax behavior —
exactly the flexibility that motivated the RAW mode design.

---

**RESOLVED 7: Platform variants for window/surface binding.**

*Problem*: `XR_EXT_win32_window_binding` is Win32-specific. How should other
platforms provide equivalent functionality?

*Resolution*: **Per-platform surface binding extensions.** Each platform extension
follows the same structural pattern — chain a platform-specific surface handle to the
graphics binding at session creation. The `XrCompositionLayerWindowSpaceEXT` layer type
is platform-independent and works with all surface binding variants.

Implemented:
- `XR_EXT_android_surface_binding` (with `ANativeWindow*` + Java `Surface`)
- `XR_EXT_cocoa_window_binding` (with `NSView*` backed by `CAMetalLayer`)

Future platforms would follow the same pattern:
- `XR_EXT_wayland_surface_binding` (with `wl_surface*`)
- `XR_EXT_xlib_window_binding` (with X11 `Window`)

---

**RESOLVED 8: Display mode switching (2D/3D toggle) API design.**

*Problem*: Many tracked 3D displays support switching between 2D and 3D modes (via
switchable lenses, controllable backlights, or similar mechanisms). Should OpenXR expose
this capability, and if so, how?

*Resolution*: **Capability flag + request function with automatic lifecycle.**

- `XrDisplayInfoEXT` includes `hardwareDisplay3D` (`XrBool32`) so the application
  can discover whether the display is a hardware 3D display capable of mode switching. Not all tracked 3D displays support
  switching — some are always-3D — so the capability must be queryable.
- `xrRequestDisplayModeEXT(session, mode)` allows explicit mode control. The function is
  a **request** (not a hard set) because the underlying platform may aggregate preferences
  across applications (some vendors implement it as a preference-based hint) or may defer the switch.
- The runtime automatically requests 3D mode when the session becomes READY and 2D mode
  when the session is destroyed, providing correct default behavior for applications that
  never call the function explicitly.

*Alternatives considered*:
- **Struct-only (no function)**: rejected because mode switching is a dynamic action, not
  a static property. A function call is the natural API shape.
- **Event-based notification of mode changes**: deferred to a future revision. For v1,
  the application requests and trusts the runtime. The runtime handles graceful
  degradation (e.g., showing the left eye view) if forced to 2D by another application
  or system policy.
- **Vendor-specific weaver settings (contrast, anti-crosstalk)**: intentionally excluded.
  These are implementation details that vary across vendors and do not belong in a
  cross-vendor OpenXR extension.

---

### Open Issues

**OPEN 1: Multi-display scenarios.**

The current design assumes a single tracked 3D display per system. Multi-display scenarios
(e.g., multiple tracked monitors in a workstation) would require:
- A way to enumerate multiple displays per system.
- Per-display `XrDisplayInfoEXT` queries.
- Possibly per-display DISPLAY spaces.

This is out of scope for the initial extension but should be considered in future revisions.

---

**OPEN 2: Android surface screen position — static vs. dynamic.**

The current design requires `screenOffsetX/Y` at session creation, making the surface
position static. This works for fullscreen and fixed-layout scenarios but does not handle:
- Android multi-window (split-screen) mode where the app window can be repositioned.
- Freeform windowing on tablets/ChromeOS where the window is draggable.
- Orientation changes that alter the surface's screen position.

Possible solutions for dynamic position updates:
- A new function `xrUpdateSurfaceOffsetEXT(session, offsetX, offsetY)` that the
  application calls when its surface moves.
- Having the runtime query position via JNI using the `surface` `jobject` (if the platform
  SDK can resolve screen coordinates from a Surface).
- Requiring session recreation on layout changes (simplest, but disruptive).

For the initial version, static offsets at session creation are sufficient — most tracked
3D display Android devices run apps fullscreen. Dynamic updates should be addressed in a
future revision if windowed 3D display use cases emerge.

---

**OPEN 3: Interaction with XR_EXT_local_floor and other space extensions.**

For tracked 3D displays, LOCAL space serves as the primary coordinate frame. In RAW mode,
eye positions are returned in screen-centered coordinates regardless of the space parameter.
Future work should clarify how this interacts with spatial anchor extensions and
mixed-reality scenarios where tracked displays coexist with HMDs.

---

**OPEN 4: Multiview displays and passive (non-tracked) displays.**

The current design assumes stereo (2-view) rendering. However, the single-swapchain
architecture — where all views are written as sub-regions of a single swapchain sized to
the native display — naturally extends to N-view multiview displays (e.g., light field
displays with 4, 8, or more views) and to passive autostereoscopic displays without eye
tracking.

**Multiview rendering.** The key enabler is a new `XrViewConfigurationType` (e.g.,
`PRIMARY_MULTI_VIEW_EXT`). Most of the OpenXR machinery already supports arbitrary view
counts:

- `xrEnumerateViewConfigurationViews` would report N views with per-view recommended
  dimensions.
- `xrLocateViews` already uses the `viewCapacityInput` / `viewCountOutput` idiom and can
  return N poses. For a tracked display, these would be N synthetic viewpoints spread across
  the viewing zone. For a passive display, N fixed positions at the nominal viewing
  distance.
- `XrCompositionLayerProjection` already accepts an arbitrary `viewCount`, each
  `projectionView` referencing a `subImage.imageRect` tile within the shared swapchain.
- The compositor's same-swapchain optimization passes the raw texture directly to the
  display processor without interpreting the view layout.
- The display processor (vendor-specific) is the only component that needs to understand the
  multiview tiling order and perform the appropriate interlacing.

A future revision would need to define:
- The new view configuration type and how the runtime advertises the view count.
- A tiling convention (row-major, column-major, or vendor-specified) so applications know
  where to render each view within the single swapchain.

The compositor and OpenXR state tracker layers would need essentially zero changes.

#### Tiling Convention

The runtime's internal rendering modes now require `tile_columns` and `tile_rows` to be specified by the driver. The near-square formula has been removed in favor of explicit driver-specified tiling:

- **Single-swapchain tiling**: The driver specifies `tile_columns × tile_rows`. Views are placed at positions `(v % cols) * view_w, (v / cols) * view_h` within the atlas.
- **Zero-copy passthrough**: When the app's swapchain already matches the DP's expected tiled layout (same dimensions, correct tile positions, single swapchain for all views), the compositor skips the atlas copy entirely and passes the swapchain texture directly to the display processor.
- **DP contract**: The display processor always receives an exactly-sized tiled texture: `tile_columns × view_width` by `tile_rows × view_height`, with views packed in row-major order.

**Passive (non-tracked) displays.** Not all multiview 3D displays have eye tracking.
Passive autostereoscopic displays (lenticular, parallax barrier) present fixed-viewpoint 3D
without tracking the viewer. A dedicated `hasTracking` flag is **not needed** because the
existing OpenXR contract already handles this transparently:

- `xrLocateViews` always returns poses regardless of whether tracking hardware is present.
  For a non-tracked display, the runtime returns static default poses (e.g., standard 63 mm
  IPD at the nominal viewer position, which is already part of `XrDisplayInfoEXT`).
- `XrViewState` flags (`XR_VIEW_STATE_POSITION_TRACKED_BIT`,
  `XR_VIEW_STATE_ORIENTATION_TRACKED_BIT`) already signal whether the returned poses are
  actively tracked or fallback values. Applications that care can check these flags.
- Application code is identical for tracked and non-tracked displays: call `xrLocateViews`,
  render the returned views, submit. The only difference is that the poses don't change
  with viewer motion on a passive display.

---

## 7b. Eye Tracking Mode Control (v6)

### Problem Statement

A vendor SDK may have internal grace/resume periods that smoothly collapse
eye positions to a rest position when tracking is lost. This is convenient for most apps,
but some developers want raw tracking values plus an explicit `isTracking` flag to handle
tracking loss themselves (e.g., custom transition animations, UI overlays).

### New Types

#### `XrEyeTrackingModeEXT` — Mode Enum

```c
typedef enum XrEyeTrackingModeEXT {
    XR_EYE_TRACKING_MODE_MANAGED_EXT  = 0,  // default: SDK handles smoothing
    XR_EYE_TRACKING_MODE_MANUAL_EXT   = 1,  // app handles tracking loss
    XR_EYE_TRACKING_MODE_MAX_ENUM_EXT = 0x7FFFFFFF
} XrEyeTrackingModeEXT;
```

`MANAGED = 0` ensures zero-init defaults to current behavior for legacy apps.

#### `XrEyeTrackingModeCapabilityFlagsEXT` — Capability Bitmask

```c
typedef XrFlags64 XrEyeTrackingModeCapabilityFlagsEXT;
XR_EYE_TRACKING_MODE_CAPABILITY_NONE_EXT       = 0            // no tracking
XR_EYE_TRACKING_MODE_CAPABILITY_MANAGED_BIT_EXT = 0x00000001
XR_EYE_TRACKING_MODE_CAPABILITY_MANUAL_BIT_EXT  = 0x00000002
```

A runtime that supports both modes sets `supportedModes = MANAGED_BIT | MANUAL_BIT`.
A display with no eye tracking sets `supportedModes = 0`.

#### `XrEyeTrackingModeCapabilitiesEXT` — Extends `XrSystemProperties`

```c
typedef struct XrEyeTrackingModeCapabilitiesEXT {
    XrStructureType                        type;           // XR_TYPE_EYE_TRACKING_MODE_CAPABILITIES_EXT
    void* XR_MAY_ALIAS                     next;
    XrEyeTrackingModeCapabilityFlagsEXT    supportedModes; // bitmask
    XrEyeTrackingModeEXT                   defaultMode;    // mode used if app never requests one
} XrEyeTrackingModeCapabilitiesEXT;
```

Chained to `XrSystemProperties` alongside `XrDisplayInfoEXT`. If `supportedModes == 0`,
the display has no eye tracking; `defaultMode` is undefined and
`xrRequestEyeTrackingModeEXT` returns `XR_ERROR_FEATURE_UNSUPPORTED` for any mode.

#### `XrViewEyeTrackingStateEXT` — Extends `XrViewState`

```c
typedef struct XrViewEyeTrackingStateEXT {
    XrStructureType           type;       // XR_TYPE_VIEW_EYE_TRACKING_STATE_EXT
    void* XR_MAY_ALIAS        next;
    XrBool32                  isTracking; // XR_TRUE if eyes are actively tracked
    XrEyeTrackingModeEXT     activeMode; // currently active mode
} XrViewEyeTrackingStateEXT;
```

Opt-in: only populated if the app chains this struct to `XrViewState` in `xrLocateViews`.

### New Function: `xrRequestEyeTrackingModeEXT`

```c
XrResult xrRequestEyeTrackingModeEXT(XrSession session, XrEyeTrackingModeEXT mode);
```

**Return codes:**
- `XR_SUCCESS` — mode switched
- `XR_ERROR_FEATURE_UNSUPPORTED` — requested mode not in `supportedModes`
- `XR_ERROR_VALIDATION_FAILURE` — invalid mode enum value

### Behavioral Contract

1. `xrLocateViews` **always** returns fully populated views (positions, FOVs) regardless
   of tracking capability or `isTracking` state.
2. When `isTracking == XR_FALSE`, the vendor SDK MUST continue providing valid, usable eye
   positions — never zeros or uninitialized values. If the tracker is still following the
   viewer out-of-zone, the vendor MAY report those actual positions; if the tracker lost
   the viewer entirely, the vendor MUST report the last known valid position. The runtime
   passes vendor values through unchanged.
3. `isTracking` is orthogonal to `XrViewState.viewStateFlags` (`POSITION_TRACKED_BIT` etc.)
   — those flags reflect head pose tracking, not eye tracking.
4. `isTracking == XR_FALSE` means the viewer is outside the supported 3D view zone — it
   does **not** necessarily mean the physical tracker has lost lock on the viewer.

### Mode Semantics

**Managed mode** (default):
- Vendor SDK handles grace period, collapse/revival animations, and auto 2D/3D switching
- Eye positions during grace period are **vendor-animated** (e.g., collapsing toward
  nominal viewer position), not the raw tracked or last-known values
- Vendor MAY also apply shader-side animation on weaved frames during the grace period
- `isTracking` reflects vendor heuristic; SHOULD stay `true` during grace period and
  flip to `false` when the vendor switches the display to 2D

**Manual mode**:
- `isTracking` flips immediately on tracking loss — no grace period hiding
- Vendor continues returning valid eye positions without animation: actual tracked
  positions if the tracker still sees the viewer, or last known position if not
- No automatic 2D/3D switching — app calls `xrRequestDisplayRenderingModeEXT` explicitly
- App uses `isTracking` + the still-valid eye positions to design its own transitions

See `docs/specs/vendor/eye-tracking-modes.md` for the full MANAGED/MANUAL contract.

### Backward Compatibility

- Apps compiled against v5 are unaffected: new structs/function are opt-in
- `MANAGED = 0` means apps that never call `xrRequestEyeTrackingModeEXT` get unchanged behavior
- `XrViewEyeTrackingStateEXT` is only populated if chained by the app

### Vendor Examples

| Vendor | `supportedModes` | `defaultMode` | `isTracking` source |
|--------|-------------------|---------------|---------------------|
| Hardware DP (typical) | `MANAGED_BIT` | `MANAGED` | Eye-distance heuristic (collapsed = not tracking) |
| Hardware DP (future) | `MANAGED_BIT \| MANUAL_BIT` | `MANAGED` | SDK native flag |
| Sim display | `0` (NONE) — no real tracker (v14; was `MANUAL_BIT`) | undefined | Always `XR_FALSE` (dev toggle `SIM_DISPLAY_FAKE_TRACKING=1` re-enables `MANUAL_BIT` for hardware-free testing) |
| No-tracker display | `0` (NONE) | undefined | Always `XR_FALSE` |

---

## 7c. Per-Mode Tracking Capability + Tracking-State Event (v14)

### Motivation

Tracking capability is not uniform across a display's rendering modes. A vendor may offer
a **"2D tracked"** mode (content presented in 2D while the viewer remains eye-tracked, e.g.
for head-coupled UI) alongside the default tracked 3D mode and fully untracked export modes
(side-by-side, anaglyph). v13 and earlier could only express tracking capability
system-wide via `XrEyeTrackingModeCapabilitiesEXT.supportedModes`.

Separately, MANUAL-mode apps had to poll `XrViewEyeTrackingStateEXT.isTracking` every
`xrLocateViews` to detect tracking loss; an edge-triggered event is the right primitive.

### `XrDisplayRenderingModeTrackingInfoEXT` — chained per enumerated mode

```c
#define XR_TYPE_DISPLAY_RENDERING_MODE_TRACKING_INFO_EXT ((XrStructureType)1000999012)

typedef struct XrDisplayRenderingModeTrackingInfoEXT {
    XrStructureType    type;        // XR_TYPE_DISPLAY_RENDERING_MODE_TRACKING_INFO_EXT
    void* XR_MAY_ALIAS next;
    XrBool32           hasTracking; // mode consumes live eye tracking
} XrDisplayRenderingModeTrackingInfoEXT;
```

The application chains one instance to the `next` pointer of **each**
`XrDisplayRenderingModeInfoEXT` element it wants the capability for, before calling
`xrEnumerateDisplayRenderingModesEXT`. The runtime fills `hasTracking` for every element
that carries the chain; elements without it are filled exactly as in v13.

> **Why a chained struct, not a field append — layout-freeze policy.** The runtime's
> enumerate fill writes array elements with its own compiled struct stride. Appending
> fields to `XrDisplayRenderingModeInfoEXT` (as v12/v13 did) silently corrupts memory in
> application binaries compiled against an older header — there is no version handshake on
> the app ABI to reject them cleanly. **`XrDisplayRenderingModeInfoEXT` is therefore frozen
> at its v13 layout; all future per-mode fields MUST be added as chained structs.** This is
> the canonical OpenXR pattern for extending enumerated output structs.

**Semantics:**

- `hasTracking == XR_FALSE` on the active mode forces
  `XrViewEyeTrackingStateEXT.isTracking = XR_FALSE`, regardless of tracker state. The
  runtime derives: `isTracking = activeMode.hasTracking && dp.is_tracking`.
- `xrLocateViews` still ALWAYS returns fully populated views in every mode — in untracked
  modes positions derive from the nominal viewer (or whatever the vendor chooses).
- Consistency rule: `XrEyeTrackingModeCapabilitiesEXT.supportedModes != 0` ⇔ at least one
  rendering mode reports `hasTracking == XR_TRUE`.
- `xrRequestEyeTrackingModeEXT` remains a session-level preference validated against
  `supportedModes` only; requesting MANAGED/MANUAL while an untracked mode is active is
  valid and latent.

### `XrEventDataEyeTrackingStateChangedEXT` — tracking-state edge event

```c
#define XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_EXT ((XrStructureType)1000999013)

typedef struct XrEventDataEyeTrackingStateChangedEXT {
    XrStructureType          type;       // XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_EXT
    const void* XR_MAY_ALIAS next;
    XrBool32                 isTracking; // new state
    XrEyeTrackingModeEXT     activeMode; // session's MANAGED/MANUAL preference at edge time
} XrEventDataEyeTrackingStateChangedEXT;
```

Queued on **every edge of the derived `isTracking` value** — DP-reported tracking
loss/recovery AND rendering-mode switches into/out of untracked modes. This is the primary
tracking-loss notification for MANUAL mode (detect `isTracking == XR_FALSE`, run your own
transition, request a 2D mode when ready); it also fires in MANAGED mode, where apps may
ignore it entirely. Edge detection runs in the runtime's `xrLocateViews` path, so apps that
never locate views receive no events.

### Backward Compatibility

- v13-and-earlier binaries: `XrDisplayRenderingModeInfoEXT` layout unchanged; apps that
  don't chain see exactly v13 behavior. The new event type is never delivered to apps that
  don't recognize it any differently than other unhandled events (apps skip unknown
  `xrPollEvent` results by design).
- sim_display behavior change: `isTracking` is now honestly `XR_FALSE` (previously
  inconsistently `XR_TRUE`/`XR_FALSE` depending on code path). Only test code asserting
  `XR_TRUE` on sim would notice; use `SIM_DISPLAY_FAKE_TRACKING=1` there.

### Vendor requirements

Vendors set `XRT_RENDERING_MODE_FLAG_HAS_TRACKING` in `xrt_rendering_mode.mode_flags` for
each tracked mode (plug-in ABI v3 — see `docs/specs/vendor/eye-tracking-modes.md` and
`docs/guides/vendor-plugin-onboarding.md`). Zero-init = untracked = safe default.

---

## 7. Display Rendering Mode Control (v7)

### Motivation

Different 3D display vendors support multiple rendering variations — for example,
side-by-side stereo, anaglyph, lenticular, or holographic. The runtime needs a
vendor-neutral way for applications to switch between these modes at runtime.

### New Function: `xrRequestDisplayRenderingModeEXT`

```c
XrResult xrRequestDisplayRenderingModeEXT(XrSession session, uint32_t modeIndex);
```

Switches the active display rendering mode. Mode indices are vendor-defined:

| Index | Meaning |
|-------|---------|
| 0 | Standard rendering (always available) |
| 1+ | Vendor-specific variations |

A mode change is a **request**, not an immediate state change. The runtime fires
`XrEventDataRenderingModeChangedEXT` when the active mode actually changes (and
`XrEventDataHardwareDisplayStateChangedEXT` if the hardware-3D state flips). Applications
**must** update their local mode state from that event, not optimistically at call time. When
running under a workspace controller, app requests may be dropped — see `isRequestable` below.

### Enumerating Rendering Modes (v8) — `XrDisplayRenderingModeInfoEXT` + `xrEnumerateDisplayRenderingModesEXT`

Before requesting a mode, the application enumerates the modes the runtime exposes. Each mode
describes its view count, atlas tile layout, per-view scale, hardware-3D state, and (per session)
whether it is currently active and whether this session is allowed to request it.

```c
#define XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT ((XrStructureType)1000999008)

typedef struct XrDisplayRenderingModeInfoEXT {
    XrStructureType    type;             // XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT
    void*              next;
    uint32_t           modeIndex;        // pass to xrRequestDisplayRenderingModeEXT
    char               modeName[XR_MAX_SYSTEM_NAME_SIZE]; // human-readable
    uint32_t           viewCount;        // 1 = mono, 2 = stereo, 4 = quad, ...
    float              viewScaleX;       // per-view horizontal scale (vendor-provided)
    float              viewScaleY;       // per-view vertical scale (vendor-provided)
    XrBool32           hardwareDisplay3D; // whether the display hardware is in 3D for this mode
    uint32_t           tileColumns;      // (v12) atlas tile columns
    uint32_t           tileRows;         // (v12) atlas tile rows
    uint32_t           viewWidthPixels;  // (v12) per-view width in pixels
    uint32_t           viewHeightPixels; // (v12) per-view height in pixels
    XrBool32           isActive;         // (v13) this mode is the session's active mode
    XrBool32           isRequestable;    // (v13) this session may request this mode
} XrDisplayRenderingModeInfoEXT;
```

| Member | Description |
|---|---|
| `modeIndex` | Vendor-defined index to pass to `xrRequestDisplayRenderingModeEXT`. |
| `modeName` | Human-readable label (e.g. `"2D"`, `"Stereo SBS"`, `"Quad"`). |
| `viewCount` | Number of views this mode renders. Drives the `xrLocateViews` render/submit loop bound (1=mono … 4=quad). |
| `viewScaleX/Y` | Per-view render-resolution scale. The app sizes each tile as `windowSize × viewScale` (see [Recommended View Scale](#recommended-view-scale) and the multiview tiling spec). |
| `hardwareDisplay3D` | `XR_TRUE` if the display's light-field hardware is active in this mode. **This is the authoritative location** — `hardwareDisplay3D` is *not* a field on `XrDisplayInfoEXT` (removed in v12). |
| `tileColumns`, `tileRows` | Atlas tile layout for the mode's views (e.g. `2×1` SBS, `2×2` quad). |
| `viewWidthPixels`, `viewHeightPixels` | Per-view pixel dimensions for the mode. |
| `isActive` (v13) | `XR_TRUE` for the mode currently active for this session. Read it on the **first** enumerate after `xrCreateSession` to learn the active mode without waiting for an `XrEventDataRenderingModeChangedEXT` — useful when starting under a workspace that already chose a mode. Re-enumerating after a change reflects the new active mode. |
| `isRequestable` (v13) | `XR_TRUE` iff this session may request this mode. **False for non-controller sessions running under a workspace** — there the workspace controller is the sole mode authority and app requests are dropped. Apps should gate their mode-toggle UI on this (e.g. disable the V toggle, show "mode locked by workspace"). Always `XR_TRUE` for standalone sessions and workspace-controller sessions. |

```c
XrResult xrEnumerateDisplayRenderingModesEXT(
    XrSession                       session,
    uint32_t                        modeCapacityInput,   // 0 to query count
    uint32_t*                       modeCountOutput,
    XrDisplayRenderingModeInfoEXT*  modes);
```

Standard OpenXR two-call enumerate: call with `modeCapacityInput = 0` to read
`modeCountOutput`, allocate, then call again. Returns `XR_ERROR_SIZE_INSUFFICIENT` if the
supplied capacity is too small.

```c
// Two-call enumerate; struct type must be set on every element before the second call.
uint32_t modeCount = 0;
xrEnumerateDisplayRenderingModesEXT(session, 0, &modeCount, NULL);
XrDisplayRenderingModeInfoEXT modes[8];
for (uint32_t i = 0; i < modeCount && i < 8; i++)
    modes[i] = (XrDisplayRenderingModeInfoEXT){ .type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_EXT };
xrEnumerateDisplayRenderingModesEXT(session, modeCount, &modeCount, modes);

// Learn the active mode at startup without waiting for an event:
uint32_t activeIndex = 0;
for (uint32_t i = 0; i < modeCount; i++)
    if (modes[i].isActive) activeIndex = modes[i].modeIndex;

// Request a different mode only if allowed, then wait for the event to update local state:
if (modes[target].isRequestable)
    xrRequestDisplayRenderingModeEXT(session, modes[target].modeIndex);
```

### Internal Dispatch

The runtime dispatches the request through the existing device property mechanism:

```c
xrt_device_set_property(head, XRT_DEVICE_PROPERTY_OUTPUT_MODE, modeIndex);
```

This requires **no new vtable or interface**. Vendor drivers that support multiple
rendering modes handle `XRT_DEVICE_PROPERTY_OUTPUT_MODE` in their `set_property`
implementation. Drivers that do not implement `set_property` (or do not recognize
the property) silently ignore the call — graceful degradation.

### Vendor Examples

| Vendor | Mode 0 | Mode 1 | Mode 2 |
|--------|--------|--------|--------|
| sim_display | SBS stereo | Anaglyph | Blend |
| Hardware DP | Standard | (future: vendor-specific) | — |

### Backward Compatibility

- Apps compiled against v6 are unaffected: the new function is opt-in
- The runtime never calls `xrRequestDisplayRenderingModeEXT` automatically
- Drivers that do not handle the property are unaffected

---

## 8. Version History

### XR_EXT_win32_window_binding

| Revision | Date | Author | Description |
|---|---|---|---|
| 1 | 2025-01-15 | David Fattal | Initial version. Window handle binding and window-space composition layer. |

### XR_EXT_android_surface_binding

| Revision | Date | Author | Description |
|---|---|---|---|
| 1 | 2026-02-13 | David Fattal | Initial version. Android `ANativeWindow` surface binding as platform counterpart to Win32 window binding. |

### XR_EXT_display_info

| Revision | Date | Author | Description |
|---|---|---|---|
| 1 | 2025-01-15 | David Fattal | Initial version with absolute recommended view sizes. |
| 2 | 2025-03-01 | David Fattal | Replaced absolute sizes with `recommendedViewScaleX/Y` scale factors. Added `XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT`. Added nominal viewer pose. |
| 3 | 2025-06-01 | David Fattal | Changed `nominalViewerPoseInDisplaySpace` from `XrPosef` to `XrVector3f nominalViewerPositionInDisplaySpace`. Orientation was always identity; position is now populated from the vendor SDK's default-viewing-position query. |
| 4 | 2026-02-13 | David Fattal | Added `supportsDisplayModeSwitch` capability flag, `XrDisplayModeEXT` enum, and `xrRequestDisplayModeEXT` function for 2D/3D mode control. Added automatic lifecycle behavior (3D on session READY, 2D on session STOPPING). |
| 5 | 2026-02-20 | David Fattal | Added `displayPixelWidth` / `displayPixelHeight` to `XrDisplayInfoEXT`. |
| 6 | 2026-02-27 | David Fattal | Eye tracking mode control: `XrEyeTrackingModeEXT` enum, `XrEyeTrackingModeCapabilitiesEXT` (chained to `XrSystemProperties`), `XrViewEyeTrackingStateEXT` (chained to `XrViewState`), and `xrRequestEyeTrackingModeEXT` function. Allows apps to choose between managed (SDK-filtered) and manual eye tracking, with explicit `isTracking` flag. |
| 7 | 2026-03-04 | David Fattal | Vendor-specific display rendering mode control: `xrRequestDisplayRenderingModeEXT(session, modeIndex)` for switching between vendor-defined rendering variations (e.g., SBS stereo, anaglyph, lenticular). Mode 0 = standard (always available), mode 1+ = vendor-defined. Dispatches through `xrt_device_set_property`; no-op if driver doesn't support it. |
| 8 | 2026-03-06 | David Fattal | Removed `XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT`. In RAW mode, `xrLocateViews` returns screen-centered eye positions regardless of the reference space parameter — a dedicated DISPLAY space is unnecessary. Applications use LOCAL space for both view location and layer submission. |
| 10 | 2026-03-12 | David Fattal | Removed `supportsDisplayModeSwitch` (derivable from mode enumeration), renamed `display3D` to `hardwareDisplay3D`, deprecated `xrRequestDisplayModeEXT` in favor of unified `xrRequestDisplayRenderingModeEXT`, added rendering mode and hardware display state change events (`XrEventDataRenderingModeChangedEXT`, `XrEventDataHardwareDisplayStateChangedEXT`). |
| 11 | 2026-03-20 | David Fattal | Added per-mode tile layout fields to `XrDisplayRenderingModeInfoEXT`: `tileColumns`, `tileRows`, `viewWidthPixels`, `viewHeightPixels`. Enables runtime to describe atlas layout for multi-view rendering modes (e.g., 2x2 quad). |
| 12 | 2026-03-28 | David Fattal | Removed `hardwareDisplay3D` from `XrDisplayInfoEXT`. It remains available **per-mode** on `XrDisplayRenderingModeInfoEXT` (via `xrEnumerateDisplayRenderingModesEXT`) and is also reported by the `XrEventDataHardwareDisplayStateChangedEXT` event. Also moved `xrSetSharedTextureOutputRectEXT` to the window-binding extension headers. |
| 13 | 2026-05-18 | David Fattal | Added per-session `isActive` and `isRequestable` fields to `XrDisplayRenderingModeInfoEXT` (#234). `isActive` lets an app learn the current mode from the first enumerate without waiting for an event; `isRequestable` tells a session whether it may request a mode (false for non-controller sessions under a workspace). **Current header version (`XR_EXT_display_info_SPEC_VERSION == 13`).** |

> The `XR_EXT_display_info_SPEC_VERSION` define in the header is the authoritative current
> revision. Earlier revision numbers in this table reflect the proposal's editing history and do
> not all map one-to-one onto the header's inline `// ---- vN ----` section comments.

---

## Appendix A: Reference Implementation

A complete reference implementation is available in the [DisplayXR runtime](https://github.com/DisplayXR/displayxr-runtime):

| Component | Location |
|---|---|
| Extension headers | `src/external/openxr_includes/openxr/XR_EXT_display_info.h` |
| | `src/external/openxr_includes/openxr/XR_EXT_win32_window_binding.h` |
| | `src/external/openxr_includes/openxr/XR_EXT_cocoa_window_binding.h` |
| Runtime: display info query | `src/xrt/state_trackers/oxr/oxr_system.c` |
| Runtime: session + view locate | `src/xrt/state_trackers/oxr/oxr_session.c` |
| Runtime: events | `src/xrt/state_trackers/oxr/oxr_event.c` |
| Runtime: Kooima math | `src/xrt/auxiliary/math/m_display3d_view.h` |
| Display processor interface | `src/xrt/include/xrt/xrt_display_processor.h` |
| Vendor integration (plug-in DLL, ADR-019) | [`displayxr-leia-plugin`](https://github.com/DisplayXR/displayxr-leia-plugin) |
| Simulator driver | `src/xrt/drivers/sim_display/` |
| D3D11 test application (Win32) | `test_apps/cube_handle_d3d11_win/` |
| Metal test application (macOS) | `test_apps/cube_handle_metal_macos/` |
| Common Kooima projection | `test_apps/common/xr_session_common.cpp` |

The runtime is based on Monado (open-source OpenXR runtime) with native compositors per graphics API (D3D11, D3D12, Metal, OpenGL, Vulkan). Vendor display processing is abstracted via the `xrt_display_processor` interface — see `docs/guides/vendor-plugin-onboarding.md`.

## Appendix B: Glossary

| Term | Definition |
|---|---|
| **Autostereoscopic display** | A display that presents different images to each eye without requiring glasses or a headset. |
| **Light field display** | A display that emits light in controlled directions, creating a glasses-free 3D effect with support for multiple viewpoints. |
| **Kooima projection** | An off-axis asymmetric frustum projection algorithm where the near plane is aligned to a physical screen and the eye position is offset from screen center. Named after Robert Kooima's 2009 paper. |
| **Canonical display pyramid** | The geometric frustum defined by the display rectangle (base) and nominal viewer position (apex). Anchors zero-parallax and stereo comfort. |
| **RAW mode** | View mode where the runtime returns raw tracked eye positions and identity orientation, leaving camera model construction to the application. |
| **RENDER_READY mode** | View mode where the runtime returns view poses and FOV angles with convergence and comfort adjustments applied. The application still builds its own projection matrix from the FOV angles. |
| **Window-space coordinates** | Fractional coordinates in `[0, 1]` relative to the target window/surface dimensions. Used by `XrCompositionLayerWindowSpaceEXT`. |
| **Screen-centered coordinates** | The coordinate frame used by RAW mode eye positions: origin at the physical display center, +X right, +Y up, +Z toward the viewer. Implicit in the returned `XrView.pose.position` — no dedicated reference space needed. |
| **Nominal viewer position** | A static, design-time expectation of the viewer's position relative to the display. Not tracked; defines the apex of the canonical display pyramid. |
| **Disparity** | Horizontal shift between left and right eye images, measured as a fraction of window width. Controls perceived depth of window-space layers. |
| **Display mode** | The operational mode of a tracked 3D display: 2D (standard flat panel) or 3D (light field interlacing active). Historically controlled via `xrRequestDisplayModeEXT` (deprecated in v10). New apps use `xrRequestDisplayRenderingModeEXT` which provides unified control over both 2D/3D switching and vendor-specific rendering variations. |
| **Display rendering mode** | A vendor-specific rendering variation within 3D mode (e.g., SBS stereo, anaglyph, lenticular). Controlled via `xrRequestDisplayRenderingModeEXT`. Mode 0 is standard; higher indices are vendor-defined. |
| **Surface binding** | A platform-specific mechanism for the application to provide its own rendering surface to the runtime (`HWND` on Win32; `ANativeWindow*` + Java `Surface` + screen position on Android). |

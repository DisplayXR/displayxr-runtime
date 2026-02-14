# OpenXR Extensions for Tracked 3D Displays

## Formal Proposal for XR_EXT_win32_window_binding, XR_EXT_android_surface_binding, and XR_EXT_display_info

| Field | Value |
|---|---|
| **Proposal Title** | OpenXR Extensions for Tracked 3D (Autostereoscopic) Displays |
| **Authors** | David Fattal (Leia Inc.) |
| **Date** | 2026-02-13 |
| **Revision** | 2.0 |
| **Status** | Draft — proposed for Khronos OpenXR Working Group review |
| **Extension Names** | `XR_EXT_win32_window_binding`, `XR_EXT_android_surface_binding`, `XR_EXT_display_info` |
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

This proposal introduces three independent but complementary extensions:

| Extension | Purpose |
|---|---|
| `XR_EXT_win32_window_binding` | App provides a Win32 HWND for runtime rendering; enables windowed mode, multi-app, app-controlled input, and window-space overlay layers. |
| `XR_EXT_android_surface_binding` | App provides an Android `ANativeWindow` for runtime rendering; the Android counterpart to the Win32 window binding. |
| `XR_EXT_display_info` | Runtime exposes physical display geometry, nominal viewer position, recommended render scale, display mode switching capability, and a DISPLAY reference space anchored to the physical screen. Provides `xrRequestDisplayModeEXT` for 2D/3D mode control. |

Together they form a minimal, complete interface for tracked 3D display rendering through
OpenXR across desktop and mobile platforms.

---

## 2. Overview

### Architecture

```
    Application
        │
        ├── xrCreateInstance()
        │       enable "XR_EXT_win32_window_binding"   (Win32)
        │         — or "XR_EXT_android_surface_binding" (Android)
        │       enable "XR_EXT_display_info"
        │
        ├── xrGetSystemProperties()
        │       ◄── XrDisplayInfoEXT (chained)
        │           • displaySizeMeters
        │           • nominalViewerPositionInDisplaySpace
        │           • recommendedViewScaleX / Y
        │           • supportsDisplayModeSwitch
        │
        ├── xrCreateSession()
        │       XrSessionCreateInfo
        │        └── next: XrGraphicsBindingD3D11KHR          (Win32)
        │        │            └── next: XrWin32WindowBindingCreateInfoEXT
        │        │                       • windowHandle = app HWND
        │        └── next: XrGraphicsBindingVulkanKHR          (Android)
        │                    └── next: XrAndroidSurfaceBindingCreateInfoEXT
        │                               • nativeWindow = app ANativeWindow*
        │
        │   ┌── Session READY → runtime auto-requests 3D mode (if supported)
        │
        ├── xrRequestDisplayModeEXT(session, mode)     [optional override]
        │       XR_DISPLAY_MODE_3D_EXT  /  XR_DISPLAY_MODE_2D_EXT
        │
        ├── xrCreateReferenceSpace(DISPLAY)
        │       origin = display center, +X right, +Y up, +Z toward viewer
        │
        ├── xrLocateViews(space = DISPLAY)
        │       ◄── per-eye positions in display space (RAW mode)
        │
        ├── App computes Kooima projection from eye positions + display geometry
        │
        ├── xrEndFrame()
        │       submit XrCompositionLayerProjection (in DISPLAY space)
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
| **RAW** | Runtime returns raw tracked eye positions in display space; `orientation` is identity; `fov` is advisory. | Application |

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

### IP Status

No known IP claims.

### Name Strings

- Extension name: `XR_EXT_android_surface_binding`
- Spec version: 1
- Extension name define: `XR_EXT_ANDROID_SURFACE_BINDING_EXTENSION_NAME`

### Overview

This extension is the Android counterpart to `XR_EXT_win32_window_binding`. It allows an
OpenXR application to provide its own Android native window to the runtime via the session
creation chain. When provided, the runtime renders into the application's surface instead
of managing its own display output.

On Android, applications inherently own their Activity and rendering surface. This
extension formalizes the handoff: the application passes an `ANativeWindow*` (obtained
from a `SurfaceView` or `SurfaceHolder` via the NDK) to the runtime, which uses it as the
compositing and interlacing target.

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

```c
typedef struct XrAndroidSurfaceBindingCreateInfoEXT {
    XrStructureType             type;
    const void* XR_MAY_ALIAS    next;
    ANativeWindow*              nativeWindow;
} XrAndroidSurfaceBindingCreateInfoEXT;
```

**Valid Usage:**
- `type` **must** be `XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_EXT`.
- `nativeWindow` **must** be a valid `ANativeWindow*` obtained via
  `ANativeWindow_fromSurface()` or `ASurfaceHolder_getNativeWindow()`.
- The native window **must** remain valid for the lifetime of the `XrSession`.
- The application **must not** release the native window (via `ANativeWindow_release()`)
  before calling `xrDestroySession`.
- The application **should** acquire a reference (via `ANativeWindow_acquire()`) to ensure
  the window outlives the session.

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

// 2. Obtain ANativeWindow from a SurfaceView
ANativeWindow* nativeWindow = ANativeWindow_fromSurface(env, javaSurface);
ANativeWindow_acquire(nativeWindow);  // ensure lifetime

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
- Spec version: 4
- Extension name define: `XR_EXT_DISPLAY_INFO_EXTENSION_NAME`

### Overview

This extension exposes the physical properties of a tracked 3D display to the application:
the display's physical dimensions, its nominal viewer position, recommended render
resolution scale factors, and whether the display supports switching between 2D and 3D
modes. It also introduces a DISPLAY reference space anchored to the physical screen and
a function to request display mode changes.

With this information the application can:
- Build its own camera model (Kooima off-axis projection) from raw tracked eye positions.
- Compute render resolution dynamically as the window/surface resizes.
- Locate views and submit layers in display-anchored coordinates.
- Query whether the display supports 2D/3D mode switching and request mode changes.

This extension is **platform-independent**. It works on any platform that supports OpenXR,
regardless of the graphics API or windowing system in use.

### New Enum Constants

```c
#define XR_TYPE_DISPLAY_INFO_EXT              ((XrStructureType)1000999003)
#define XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT   ((XrReferenceSpaceType)1000999004)
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
| `supportsDisplayModeSwitch` | `XrBool32` | `XR_TRUE` if the display supports switching between 2D and 3D modes via `xrRequestDisplayModeEXT`. See [Display Mode Switching](#display-mode-switching). |

```c
typedef struct XrDisplayInfoEXT {
    XrStructureType             type;
    void* XR_MAY_ALIAS          next;
    XrExtent2Df                 displaySizeMeters;
    XrVector3f                  nominalViewerPositionInDisplaySpace;
    float                       recommendedViewScaleX;
    float                       recommendedViewScaleY;
    XrBool32                    supportsDisplayModeSwitch;
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

### New Reference Space Type: XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT

A reference space rigidly anchored to the physical display.

**Definition:**
- **Origin**: center of the physical display plane.
- **+X axis**: rightward along the display surface.
- **+Y axis**: upward along the display surface.
- **+Z axis**: toward the viewer (outward from the screen, following the right-hand rule).

**Semantics:**
- The DISPLAY space is **physically anchored** to the hardware. It is **not affected** by
  `xrRecenterSpace()` or any recentering of LOCAL/STAGE spaces.
- View positions returned by `xrLocateViews()` in DISPLAY space represent the viewer's
  tracked eye positions relative to the physical screen center.
- Layers submitted in DISPLAY space remain locked to the physical display regardless of
  any space recentering.

**Creation:**
```c
XrReferenceSpaceCreateInfo displaySpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
displaySpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT;
displaySpaceInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};  // identity
displaySpaceInfo.poseInReferenceSpace.position = {0, 0, 0};

XrSpace displaySpace;
xrCreateReferenceSpace(session, &displaySpaceInfo, &displaySpace);
```

### New Enums

#### XrDisplayModeEXT

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

#### xrRequestDisplayModeEXT

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
| `XR_ERROR_FEATURE_UNSUPPORTED` | The display does not support mode switching (`supportsDisplayModeSwitch` is `XR_FALSE`). |
| `XR_ERROR_SESSION_NOT_RUNNING` | The session is not in a running state. |
| `XR_ERROR_HANDLE_INVALID` | The session handle is invalid. |

**Semantics:**

- This function is a **request**, not a guarantee. The runtime forwards the request to the
  underlying platform SDK (e.g., SR SDK's `SwitchableLensHint`, CNSDK's backlight control).
  The platform may aggregate requests from multiple applications or defer the switch.
- The function returns `XR_SUCCESS` when the request has been accepted, regardless of
  whether the display has physically completed the mode change.
- The application **may** call this function at any time while the session is running.
- If `supportsDisplayModeSwitch` in `XrDisplayInfoEXT` is `XR_FALSE`, this function
  returns `XR_ERROR_FEATURE_UNSUPPORTED` and has no effect.

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

#### Implementation Notes

The runtime translates `xrRequestDisplayModeEXT` into the appropriate vendor SDK call:

| Platform | Runtime Implementation |
|---|---|
| Windows (SR SDK) | `SwitchableLensHint::enable()` / `SwitchableLensHint::disable()` — preference-based, aggregated across applications. |
| Android (CNSDK) | `leia_core_set_backlight(core, true)` / `leia_core_set_backlight(core, false)` — direct backlight control. |

This abstraction also operates through:
- `XrDisplayInfoEXT` chaining to `xrGetSystemProperties`.
- `XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT` via `xrCreateReferenceSpace`.
- `xrRequestDisplayModeEXT` for explicit display mode control.

### Example Code: Querying Display Mode Support and Requesting 2D

```cpp
// Query display capabilities
XrSystemProperties sysProps = {XR_TYPE_SYSTEM_PROPERTIES};
XrDisplayInfoEXT displayInfo = {(XrStructureType)XR_TYPE_DISPLAY_INFO_EXT};
sysProps.next = &displayInfo;
xrGetSystemProperties(instance, systemId, &sysProps);

if (displayInfo.supportsDisplayModeSwitch) {
    // Display supports 2D/3D switching.
    // The runtime automatically requests 3D mode when the session becomes READY.

    // Example: temporarily switch to 2D for a settings menu
    xrRequestDisplayModeEXT(session, XR_DISPLAY_MODE_2D_EXT);

    // ... user interacts with 2D menu ...

    // Switch back to 3D when menu closes
    xrRequestDisplayModeEXT(session, XR_DISPLAY_MODE_3D_EXT);
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

- `XrView.pose.position` — the physical eye center in DISPLAY space coordinates.
- `XrView.pose.orientation` — identity quaternion `{0, 0, 0, 1}`.
- `XrView.fov` — advisory only. The application should compute its own FOV from the eye
  position and display geometry.

The runtime applies **no convergence adjustment or camera policy** to RAW views. The
application is fully responsible for its camera model.

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
and `(eyeX, eyeY, eyeZ)` is the eye position in DISPLAY space from `XrView.pose.position`.

### RENDER_READY Mode

When `XR_EXT_display_info` is **not** enabled (or when explicitly overridden), the runtime
returns views in **RENDER_READY mode**:

- `XrView.pose` — view pose with convergence and comfort adjustments applied by the
  runtime. Includes runtime-side debug/qwerty input controls for scene navigation.
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
XrBool32 canSwitch = displayInfo.supportsDisplayModeSwitch;   // XR_TRUE or XR_FALSE
```

### Example Code: Creating DISPLAY Reference Space

```cpp
XrReferenceSpaceCreateInfo displaySpaceInfo = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
displaySpaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT;
displaySpaceInfo.poseInReferenceSpace.orientation = {0, 0, 0, 1};
displaySpaceInfo.poseInReferenceSpace.position = {0, 0, 0};

XrSpace displaySpace;
XrResult result = xrCreateReferenceSpace(session, &displaySpaceInfo, &displaySpace);
if (XR_SUCCEEDED(result)) {
    // DISPLAY space ready — use for xrLocateViews and layer submission
}
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
    // Screen half-extents (display is centered at origin in DISPLAY space)
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
    locateInfo.space = displaySpace;  // DISPLAY reference space

    XrViewState viewState = {XR_TYPE_VIEW_STATE};
    uint32_t viewCount = 2;
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    xrLocateViews(session, &locateInfo, &viewState, 2, &viewCount, views);

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

    XrCompositionLayerProjectionView projViews[2] = {};

    for (int eye = 0; eye < 2; eye++) {
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
    projLayer.space = displaySpace;  // Submit in DISPLAY space
    projLayer.viewCount = 2;
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
monitor native resolution, and re-scale the SR SDK's recommended render size. The app-side
formula `windowWidth * scaleX` is trivial, deterministic, and requires no runtime round-trip.
This also lets the application intentionally deviate from the recommended resolution for
performance scaling.

---

**RESOLVED 2: XrSpace vs. explicit struct for the display coordinate frame.**

*Problem*: How should the display's coordinate frame be exposed? Options: (a) a new
reference space type, or (b) an explicit transform struct returned alongside display info.

*Resolution*: **XrSpace (new reference space type)**. Using `XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT`
integrates naturally with `xrLocateViews(space=DISPLAY)` and layer submission in DISPLAY
space. It reuses existing OpenXR infrastructure rather than introducing parallel mechanisms.
The display space is physically anchored and not affected by `xrRecenterSpace()`.

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

**RESOLVED 7: Android platform variant for window/surface binding.**

*Problem*: `XR_EXT_win32_window_binding` is Win32-specific. How should Android (and other
platforms) provide equivalent functionality?

*Resolution*: **Per-platform surface binding extensions.** `XR_EXT_android_surface_binding`
follows the same structural pattern — chain a platform-specific surface handle to the
graphics binding at session creation — using `ANativeWindow*` instead of `HWND`. The
`XrCompositionLayerWindowSpaceEXT` layer type is platform-independent and works with all
surface binding variants. Future platforms (Wayland, X11, macOS/Cocoa) would follow the
same pattern:
- `XR_EXT_wayland_surface_binding` (with `wl_surface*`)
- `XR_EXT_xlib_window_binding` (with X11 `Window`)
- `XR_EXT_cocoa_window_binding` (with `NSWindow*`)

---

**RESOLVED 8: Display mode switching (2D/3D toggle) API design.**

*Problem*: Many tracked 3D displays support switching between 2D and 3D modes (via
switchable lenses, controllable backlights, or similar mechanisms). Should OpenXR expose
this capability, and if so, how?

*Resolution*: **Capability flag + request function with automatic lifecycle.**

- `XrDisplayInfoEXT` includes `supportsDisplayModeSwitch` (`XrBool32`) so the application
  can discover whether mode switching is available. Not all tracked 3D displays support
  switching — some are always-3D — so the capability must be queryable.
- `xrRequestDisplayModeEXT(session, mode)` allows explicit mode control. The function is
  a **request** (not a hard set) because the underlying platform may aggregate preferences
  across applications (e.g., SR SDK's `SwitchableLensHint`) or may defer the switch.
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

**OPEN 2: Interaction with XR_EXT_local_floor and other space extensions.**

DISPLAY space is semantically distinct from LOCAL, STAGE, and LOCAL_FLOOR. For tracked 3D
displays, DISPLAY space is the primary coordinate frame. Future work should clarify how
DISPLAY space interacts with spatial anchor extensions and mixed-reality scenarios where
tracked displays coexist with HMDs.

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
| 3 | 2025-06-01 | David Fattal | Changed `nominalViewerPoseInDisplaySpace` from `XrPosef` to `XrVector3f nominalViewerPositionInDisplaySpace`. Orientation was always identity; position is now populated from the SR SDK's `getDefaultViewingPosition()`. |
| 4 | 2026-02-13 | David Fattal | Added `supportsDisplayModeSwitch` capability flag, `XrDisplayModeEXT` enum, and `xrRequestDisplayModeEXT` function for 2D/3D mode control. Added automatic lifecycle behavior (3D on session READY, 2D on session STOPPING). |

---

## Appendix A: Reference Implementation

A complete reference implementation is available in the CNSDK-OpenXR repository:

| Component | Location |
|---|---|
| Extension headers | `src/external/openxr_includes/openxr/XR_EXT_win32_window_binding.h` |
| | `src/external/openxr_includes/openxr/XR_EXT_display_info.h` |
| Runtime: display info query | `src/xrt/state_trackers/oxr/oxr_system.c` |
| Runtime: session creation | `src/xrt/state_trackers/oxr/oxr_session.c` |
| Runtime: CNSDK integration | `src/xrt/compositor/main/comp_renderer.c` |
| Runtime: LeiaSR integration | `src/xrt/drivers/leiasr/` |
| D3D11 test application (Win32) | `test_apps/sr_cube_openxr_ext/` |
| OpenGL test application (Win32) | `test_apps/sr_cube_openxr_ext_gl/` |
| Common Kooima projection | `test_apps/common/xr_session_common.cpp` |

The runtime is based on Monado (open-source OpenXR runtime) with platform-specific SDK
integration:
- **Windows**: LeiaSR (Simulated Reality) SDK for eye tracking, light field interlacing,
  and switchable lens control.
- **Android**: CNSDK (Leia SDK) for eye tracking, Vulkan interlacing, and backlight
  control.

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
| **DISPLAY space** | A reference space anchored to the physical display center, with +X right, +Y up, +Z toward the viewer. Not affected by recentering. |
| **Nominal viewer position** | A static, design-time expectation of the viewer's position relative to the display. Not tracked; defines the apex of the canonical display pyramid. |
| **Disparity** | Horizontal shift between left and right eye images, measured as a fraction of window width. Controls perceived depth of window-space layers. |
| **Display mode** | The operational mode of a tracked 3D display: 2D (standard flat panel) or 3D (light field interlacing active). Controlled via `xrRequestDisplayModeEXT`. |
| **Surface binding** | A platform-specific mechanism for the application to provide its own rendering surface to the runtime (`HWND` on Win32, `ANativeWindow*` on Android). |

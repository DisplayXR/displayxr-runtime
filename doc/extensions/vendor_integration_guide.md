# Vendor Integration Guide: Adding a 3D Display to the OpenXR Runtime

This guide tells a new 3D display vendor exactly what to implement to connect
their hardware to this OpenXR runtime.  It covers every integration point:
device description, eye tracking, display processing (weaving/interlacing),
OpenXR extensions, and build system registration.

**Reference implementations** are cited throughout:
- **Leia** (`src/xrt/drivers/leia/`) — full hardware integration with SR SDK
- **sim_display** (`src/xrt/drivers/sim_display/`) — minimal software-only
  simulation (SBS, anaglyph, blend) that runs on any GPU

---

## 1. Architecture Overview

```
 ┌──────────────────────────────────────────────────────────────┐
 │  APPLICATION          OpenXR Application                     │
 │  xrLocateViews  xrEndFrame  xrRequestDisplayModeEXT         │
 └──────────┬──────────┬─────────────────┬──────────────────────┘
 ═══════════╪══════════╪═════════════════╪═══  boundary: OpenXR API
 ┌──────────▼──────────▼─────────────────▼──────────────────────┐
 │  RUNTIME             OpenXR State Tracker (oxr_session.c)    │
 │   Eye tracking → Kooima FOV → view poses & projection       │
 │   Uses: xrt_eye_position, xrt_window_metrics (vendor-neutral)│
 └──────────┬──────────┬─────────────────┬──────────────────────┘
            │          │                 │
 ┌──────────▼──────────▼─────────────────▼──────────────────────┐
 │  RUNTIME             Compositor                              │
 │  Vulkan path: multi_compositor → comp_renderer               │
 │  D3D11 path:  comp_d3d11_compositor                          │
 └────────────────────────┬─────────────────────────────────────┘
 ══════════════════════════╪════════  boundary: xrt_display_processor vtable
 ┌────────────────────────▼─────────────────────────────────────┐
 │  VENDOR DRIVER        Display Processor                      │
 │  YOUR CODE: vendor-specific stereo → display conversion      │
 │  (interlacing, SBS, anaglyph, etc.)                          │
 │                                                              │
 │  Vulkan:  xrt_display_processor         (process_views)      │
 │  D3D11:   xrt_display_processor_d3d11   (process_stereo)     │
 └────────────────────────┬─────────────────────────────────────┘
                          │
 ┌────────────────────────▼─────────────────────────────────────┐
 │                     Physical Display                         │
 │  Light field panel, lenticular screen, etc.                  │
 └──────────────────────────────────────────────────────────────┘
```

The vendor plugs in at the **Display Processor** layer.  The compositor calls
the display processor generically each frame; the vendor converts the rendered
stereo pair into whatever format the display needs.  Zero changes to compositor
code are required.

### 1.1 Architectural Boundaries

The codebase enforces strict boundaries between three layers.  Understanding
these boundaries is essential — vendor-specific code must **never** leak into
the runtime layer, and the runtime must **never** reference a specific vendor's
SDK types.

```
┌─────────────────────────────────────────────────────────────────────┐
│                      LAYER 1: APPLICATION                           │
│                                                                     │
│  Types used:  OpenXR API types only                                 │
│  - XrView, XrFovf, XrPosef, XrDisplayInfoEXT                       │
│  - XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT                              │
│  - XrWin32WindowBindingCreateInfoEXT / XrMacOSWindowBindingCreateInfoEXT │
│                                                                     │
│  The app never sees runtime or vendor internals.                    │
├─────────────────────────────────────────────────────────────────────┤
│                      LAYER 2: OPENXR RUNTIME                        │
│                (state tracker + compositor core)                     │
│                                                                     │
│  Types used:  Vendor-NEUTRAL xrt_* types only                       │
│  - struct xrt_eye_position, xrt_eye_pair   (xrt_display_metrics.h)  │
│  - struct xrt_window_metrics               (xrt_display_metrics.h)  │
│  - struct xrt_system_compositor_info       (xrt_compositor.h)       │
│  - struct xrt_display_processor            (xrt_display_processor.h)│
│  - struct xrt_display_processor_d3d11      (xrt_display_processor_d3d11.h) │
│  - struct xrt_device, xrt_hmd_parts        (xrt_device.h)          │
│                                                                     │
│  Key runtime files (NO vendor #includes or #ifdefs for new vendors):│
│  - oxr_session.c          — Kooima FOV, view pose computation       │
│  - comp_multi_compositor.c — window metrics (vendor-neutral path)   │
│  - comp_renderer.c        — display processor dispatch              │
│                                                                     │
│  The runtime reads display geometry from xrt_system_compositor_info │
│  (populated at init by whatever driver is active).  Kooima FOV and  │
│  window-adaptive rendering use xrt_eye_position / xrt_window_metrics│
│  — no vendor-specific types.                                        │
├─────────────────────────────────────────────────────────────────────┤
│                      LAYER 3: VENDOR DRIVER                         │
│                  (src/xrt/drivers/<vendor>/)                        │
│                                                                     │
│  Types used:  Both xrt_* types AND vendor SDK types                 │
│  - Vendor SDK headers included ONLY in .cpp files                   │
│  - Vendor-specific types (e.g. leiasr_eye_pair) are internal        │
│  - Exports xrt_* types at the boundary (xrt_device, display        │
│    processor vtables, xrt_system_compositor_info fields)             │
│                                                                     │
│  The driver is the ONLY place that:                                 │
│  - #includes vendor SDK headers                                     │
│  - Calls vendor SDK functions                                       │
│  - Uses #ifdef XRT_HAVE_<VENDOR>_* guards                          │
│  - Converts between vendor SDK types and xrt_* types                │
│                                                                     │
│  Vendor code populates xrt_system_compositor_info at device init    │
│  (display_width_m, display_height_m, nominal_viewer_*_m, etc.)      │
│  and implements xrt_display_processor / xrt_display_processor_d3d11 │
│  vtables.                                                           │
└─────────────────────────────────────────────────────────────────────┘
```

**Why this matters for new vendors:**  A new vendor integration adds files
**only** under `src/xrt/drivers/<vendor>/` and `src/xrt/targets/common/`.
No modifications to `oxr_session.c`, `comp_multi_compositor.c`, or any
compositor code are needed.  The runtime discovers your driver through the
builder system, reads your display specs from `xrt_system_compositor_info`,
calls your display processor through the generic vtable, and queries eye
positions through vendor-neutral interfaces.

---

## 2. What the Vendor Provides — Checklist

| # | Component | Description | Required? |
|---|-----------|-------------|-----------|
| 1 | **Display Processor** | Vulkan and/or D3D11 implementation of the stereo→display conversion | Yes (at least one) |
| 2 | **Device Driver** | `xrt_device` with display specs (resolution, physical size, refresh rate, FOV) | Yes |
| 3 | **Eye Tracking** | Predicted L/R eye positions in display-local coordinates | Recommended |
| 4 | **SDK Wrapper** | Opaque C wrapper isolating vendor headers from the codebase | Recommended |
| 5 | **Target Builder** | Builder .c file + registration in 5 build system files (see §8.1) | Yes |

---

## 3. OpenXR Extensions the Runtime Exposes

The runtime exposes three custom extensions that surface vendor data to
applications.  The vendor provides the underlying data; the runtime maps it
onto the OpenXR API.

### 3.1 `XR_EXT_display_info` (v4)

**Header:** `src/external/openxr_includes/openxr/XR_EXT_display_info.h`

Exposes physical display properties and recommended render scale via
`xrGetSystemProperties`:

```c
typedef struct XrDisplayInfoEXT {
    XrStructureType type;                       // XR_TYPE_DISPLAY_INFO_EXT
    void* XR_MAY_ALIAS next;
    XrExtent2Df     displaySizeMeters;          // Physical display size (m)
    XrVector3f      nominalViewerPositionInDisplaySpace; // Default eye position
    float           recommendedViewScaleX;      // sr_recommended_w / display_pixel_w
    float           recommendedViewScaleY;      // sr_recommended_h / display_pixel_h
    XrBool32        supportsDisplayModeSwitch;  // Can switch 2D/3D?
} XrDisplayInfoEXT;
```

**How vendor data reaches this extension:**  The vendor's device driver populates
`xrt_system_compositor_info` fields at init time:
- `info.display_width_m` / `info.display_height_m` → `displaySizeMeters`
- `info.nominal_viewer_x_m` / `_y_m` / `_z_m` → `nominalViewerPositionInDisplaySpace`
- `info.recommended_view_scale_x` / `_y` → `recommendedViewScale*`
- `info.supports_display_mode_switch` → `supportsDisplayModeSwitch`

The runtime reads from `xrt_system_compositor_info` — it never calls vendor SDK
functions directly.  For example, the Leia driver queries SR SDK during device
creation and stores the results; sim_display uses env vars and OS display queries.

**2D/3D mode switching:**

```c
typedef enum XrDisplayModeEXT {
    XR_DISPLAY_MODE_2D_EXT = 0,
    XR_DISPLAY_MODE_3D_EXT = 1,
} XrDisplayModeEXT;

XrResult xrRequestDisplayModeEXT(XrSession session, XrDisplayModeEXT displayMode);
```

The runtime auto-switches to 3D on `xrBeginSession` and back to 2D on
`xrEndSession`.  The vendor provides a `request_display_mode(bool enable_3d)`
function on their SDK wrapper.

**DISPLAY reference space** (`XR_REFERENCE_SPACE_TYPE_DISPLAY_EXT`):
A physically-anchored reference space with origin at the display center.
Eye tracking data from the vendor SDK is expressed in this space.

### 3.2 `XR_EXT_win32_window_binding` (Windows)

**Header:** `src/external/openxr_includes/openxr/XR_EXT_win32_window_binding.h`

Allows the app to pass an HWND for windowed rendering:

```c
typedef struct XrWin32WindowBindingCreateInfoEXT {
    XrStructureType type;        // XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_EXT
    const void*     next;
    void*           windowHandle; // HWND
} XrWin32WindowBindingCreateInfoEXT;
```

Also defines `XrCompositionLayerWindowSpaceEXT` for layers positioned in
fractional window coordinates with per-eye disparity shift.

**Vendor impact:** The vendor's SDK wrapper receives the window handle at init
time and must create the weaver/interlacer targeting that window.

### 3.3 `XR_EXT_macos_window_binding` (macOS)

**Header:** `src/external/openxr_includes/openxr/XR_EXT_macos_window_binding.h`

macOS equivalent — app passes an `NSView*` with `CAMetalLayer` backing:

```c
typedef struct XrMacOSWindowBindingCreateInfoEXT {
    XrStructureType type;        // XR_TYPE_MACOS_WINDOW_BINDING_CREATE_INFO_EXT
    const void*     next;
    void*           viewHandle;  // NSView* with CAMetalLayer backing
} XrMacOSWindowBindingCreateInfoEXT;
```

### 3.4 How `xrLocateViews` Returns Eye Positions: RAW vs RENDER_READY

The runtime supports two modes for returning eye tracking data to apps.
The mode is determined automatically based on whether the app uses the
`XR_EXT_display_info` extension.

#### RAW Mode (extension-aware apps)

When the app enables `XR_EXT_display_info` and creates a session with an
external window (`XR_EXT_win32_window_binding` / `XR_EXT_macos_window_binding`),
`xrLocateViews()` returns views in **RAW mode**:

- `XrView.pose.position` — the physical eye center in DISPLAY space, directly
  from the vendor's eye tracker (e.g. `{-0.032, 0.0, 0.6}` for left eye)
- `XrView.pose.orientation` — identity quaternion `{0, 0, 0, 1}`
- `XrView.fov` — advisory only; the app **ignores** these angles and computes
  its own Kooima projection from eye position + `displaySizeMeters`

The runtime applies **no convergence adjustment, no scene navigation transform,
and no camera policy** to RAW views.  The app is fully responsible for its
own camera model.  This is the intended mode for apps built specifically for
tracked 3D displays.

**Code path:** `oxr_session.c` — when `sess->has_external_window == true`:
```c
// SESSION TARGET: Use SR eye positions directly (app controls scene)
views[i].pose.position.x = sr_eye.x;
views[i].pose.position.y = sr_eye.y;
views[i].pose.position.z = sr_eye.z;
views[i].pose.orientation = (XrQuaternionf){0.0f, 0.0f, 0.0f, 1.0f};
```

#### RENDER_READY Mode (legacy OpenXR / WebXR apps)

When `XR_EXT_display_info` is **not** enabled (or no external window is
provided), the runtime returns views in **RENDER_READY mode**:

- `XrView.pose` — view pose with the qwerty/debug controller transform applied,
  allowing WASD/mouse scene navigation.  The eye positions from the vendor SDK
  are rotated and translated into the virtual world by the display's world-space
  pose.
- `XrView.fov` — Kooima asymmetric FOV angles computed by the runtime from
  the vendor's eye positions + display geometry.  The app constructs a standard
  projection matrix from these angles.

This mode exists so that **existing OpenXR and WebXR applications** — which
were designed for head-mounted displays and expect the runtime to own the camera
model — can run on a 3D display without modification.  The qwerty/debug
controller gives the user WASD/mouse navigation to move through the scene, and
the runtime handles all the Kooima projection math internally.

**Code path:** `oxr_session.c` — when `sess->has_external_window == false`:
```c
// MONADO WINDOW: Transform SR eye from display-local to world
// view_pos = display_pos + rotate(sr_eye, display_ori)
struct xrt_vec3 rotated_eye;
math_quat_rotate_vec3(&world_head_ori, &sr_eye, &rotated_eye);

views[i].pose.position.x = world_head_pos.x + rotated_eye.x;
views[i].pose.position.y = world_head_pos.y + rotated_eye.y;
views[i].pose.position.z = world_head_pos.z + rotated_eye.z;
views[i].pose.orientation = (XrQuaternionf){
    world_head_ori.x, world_head_ori.y, world_head_ori.z, world_head_ori.w};
```

#### Summary

| | RAW Mode | RENDER_READY Mode |
|---|---|---|
| **Target apps** | Extension-aware 3D display apps | Legacy OpenXR / WebXR apps |
| **Trigger** | `has_external_window = true` | `has_external_window = false` |
| **XrView.pose** | Raw eye position in DISPLAY space | Eye transformed to world space by qwerty controller |
| **XrView.fov** | Advisory (app ignores, computes own Kooima) | Runtime-computed Kooima FOV (app uses directly) |
| **Scene navigation** | App controls its own camera | Qwerty WASD/mouse via runtime |
| **Orientation** | Identity | Display orientation from qwerty controller |

---

## 4. Component 1: Display Processor

The display processor is the core vendor contribution.  It converts rendered
stereo views into the display's native format (interlaced light field pattern,
side-by-side, anaglyph, etc.).

### 4.1 Vulkan Interface

**Header:** `src/xrt/include/xrt/xrt_display_processor.h`

```c
struct xrt_display_processor
{
    void (*process_views)(struct xrt_display_processor *xdp,
                          VkCommandBuffer cmd_buffer,
                          VkImageView left_view,
                          VkImageView right_view,
                          uint32_t view_width,
                          uint32_t view_height,
                          VkFormat_XDP view_format,
                          VkFramebuffer target_fb,
                          uint32_t target_width,
                          uint32_t target_height,
                          VkFormat_XDP target_format);

    void (*destroy)(struct xrt_display_processor *xdp);
};
```

Key design points:
- **Separate L/R views**: Input is two distinct `VkImageView` objects
- **Command buffer recording**: Implementation records Vulkan commands into the
  provided command buffer (deferred execution)
- **Target framebuffer**: Output goes to the provided `VkFramebuffer`
- **`VkFormat_XDP`**: `int32_t` alias for `VkFormat` values, avoids pulling in
  full `vulkan.h` in this header

**Helper functions** for safe calling:

```c
// Call process_views through the vtable
xrt_display_processor_process_views(xdp, cmd_buffer, ...);

// Destroy and NULL the pointer
xrt_display_processor_destroy(&xdp);
```

### 4.2 D3D11 Interface

**Header:** `src/xrt/include/xrt/xrt_display_processor_d3d11.h`

```c
struct xrt_display_processor_d3d11
{
    void (*process_stereo)(struct xrt_display_processor_d3d11 *xdp,
                           void *d3d11_context,      // ID3D11DeviceContext*
                           void *stereo_srv,          // ID3D11ShaderResourceView*
                           uint32_t view_width,       // Width of one eye
                           uint32_t view_height,
                           uint32_t format,           // DXGI_FORMAT as uint32_t
                           uint32_t target_width,
                           uint32_t target_height);

    void (*destroy)(struct xrt_display_processor_d3d11 *xdp);
};
```

Key differences from Vulkan:
- **Side-by-side input**: Single SRV containing both eyes (left in left half,
  right in right half)
- **Immediate mode**: No command buffer — uses D3D11 device context directly
- **Bound render target**: Output goes to the currently bound render target
  (set via `OMSetRenderTargets` before the call)
- **`void*` types**: D3D11 types are passed as `void*` to avoid COM header deps

### 4.3 Ownership Model

The display processor wrapper does **NOT** own the underlying vendor SDK handle.
The caller is responsible for destroying the SDK handle separately, **after**
destroying the display processor.

```
Destruction order:
  1. xrt_display_processor_destroy(&xdp)     // free wrapper
  2. vendor_sdk_destroy(&vendor_handle)        // free vendor SDK resources
```

This is critical because the compositor may create/destroy display processors
per session, while the vendor SDK handle may be shared across sessions.

### 4.4 Reference: Leia Display Processor (Vulkan)

**File:** `src/xrt/drivers/leia/leia_display_processor.cpp`

```c
struct leia_display_processor
{
    struct xrt_display_processor base;  // Must be first member
    struct leiasr *leiasr;              // Borrowed reference (not owned)
};

static void
leia_dp_process_views(struct xrt_display_processor *xdp,
                      VkCommandBuffer cmd_buffer,
                      VkImageView left_view,
                      VkImageView right_view, ...)
{
    struct leia_display_processor *ldp = (struct leia_display_processor *)xdp;
    leiasr_weave(ldp->leiasr, cmd_buffer, left_view, right_view, ...);
}

xrt_result_t
leia_display_processor_create(struct leiasr *leiasr,
                              struct xrt_display_processor **out_xdp)
{
    struct leia_display_processor *ldp = calloc(1, sizeof(*ldp));
    ldp->base.process_views = leia_dp_process_views;
    ldp->base.destroy = leia_dp_destroy;
    ldp->leiasr = leiasr;  // Borrowed, not owned
    *out_xdp = &ldp->base;
    return XRT_SUCCESS;
}
```

### 4.5 Reference: sim_display Display Processor (Vulkan)

**File:** `src/xrt/drivers/sim_display/sim_display_processor.c`

The sim_display processor demonstrates a fully self-contained Vulkan
implementation without any external SDK:

- Pre-compiles 3 graphics pipelines at init (SBS, anaglyph, alpha-blend)
- Fullscreen triangle rendering with GLSL fragment shaders
- Persistent descriptor set updated each frame with left/right image views
- Mode switching is instant (just selects a different pre-compiled pipeline)

This is the best starting point for vendors developing without hardware.

---

## 5. Component 2: Device Driver (`xrt_device`)

The device driver describes the display hardware to the runtime.  It provides
physical display specs, view geometry, and a head tracking pose.

### 5.1 Key Header

**File:** `src/xrt/include/xrt/xrt_device.h`

### 5.2 Creation Pattern

```c
#include "util/u_device.h"
#include "util/u_distortion_mesh.h"

struct xrt_device *
my_vendor_hmd_create(void)
{
    // 1. Allocate with HMD flag
    enum u_device_alloc_flags flags =
        (enum u_device_alloc_flags)(U_DEVICE_ALLOC_HMD | U_DEVICE_ALLOC_TRACKING_NONE);
    struct my_vendor_hmd *hmd = U_DEVICE_ALLOCATE(struct my_vendor_hmd, flags, 1, 0);
    //                                            struct type, flags, num_inputs, num_outputs

    // 2. Set function pointers
    hmd->base.update_inputs    = u_device_noop_update_inputs;   // No physical inputs
    hmd->base.get_tracked_pose = my_vendor_get_tracked_pose;
    hmd->base.get_view_poses   = u_device_get_view_poses;       // Use standard helper
    hmd->base.get_visibility_mask = u_device_get_visibility_mask;
    hmd->base.destroy          = my_vendor_destroy;

    // 3. Set identity
    hmd->base.name        = XRT_DEVICE_GENERIC_HMD;
    hmd->base.device_type = XRT_DEVICE_TYPE_HMD;
    snprintf(hmd->base.str, XRT_DEVICE_NAME_LEN, "MyVendor 3D Display");
    snprintf(hmd->base.serial, XRT_DEVICE_NAME_LEN, "my_vendor_display_0");

    // 4. Set head pose input
    hmd->base.inputs[0].name = XRT_INPUT_GENERIC_HEAD_POSE;

    // 5. Set view count
    hmd->base.hmd->view_count = 2;

    // 6. Configure display geometry
    struct u_device_simple_info info;
    info.display.w_pixels = 3840;           // Native display resolution
    info.display.h_pixels = 2160;
    info.display.w_meters = 0.344f;         // Physical display size
    info.display.h_meters = 0.194f;
    info.lens_horizontal_separation_meters = 0.063f;  // ~63mm IPD
    info.lens_vertical_position_meters = info.display.h_meters / 2.0f;

    // FOV from display geometry and nominal viewing distance
    float nominal_z = 0.65f;
    float half_fov_h = atanf((info.display.w_meters / 2.0f) / nominal_z);
    info.fov[0] = half_fov_h * 2.0f;
    info.fov[1] = half_fov_h * 2.0f;

    // 7. Setup split side-by-side stereo
    u_device_setup_split_side_by_side(&hmd->base, &info);

    // 8. No distortion for flat panel displays
    u_distortion_mesh_set_none(&hmd->base);

    return &hmd->base;
}
```

### 5.3 Querying Display Specs from Vendor SDK

If the vendor SDK can provide display specs at runtime (recommended), query
them during device creation.  See `leia_device.c:122-158` for an example:

```c
// Query recommended view dimensions and refresh rate
uint32_t view_w, view_h, native_w, native_h;
float hz;
if (vendor_sdk_query_display_params(&view_w, &view_h, &hz, &native_w, &native_h)) {
    pixel_w = (int)native_w;
    pixel_h = (int)native_h;
    refresh_hz = hz;
}

// Query physical dimensions
float width_m, height_m, nominal_z_m;
if (vendor_sdk_get_display_dimensions(&width_m, &height_m, &nominal_z_m)) {
    display_w_m = width_m;
    display_h_m = height_m;
    nominal_z = nominal_z_m;
}
```

### 5.4 Head Tracking Pose

For a 3D display device (not a head-mounted display), the "head pose" represents
the nominal viewer position looking at the display:

```c
static xrt_result_t
my_vendor_get_tracked_pose(struct xrt_device *xdev,
                           enum xrt_input_name name,
                           int64_t at_timestamp_ns,
                           struct xrt_space_relation *out_relation)
{
    if (name != XRT_INPUT_GENERIC_HEAD_POSE) {
        return XRT_ERROR_DEVICE_CREATION_FAILED;
    }

    // Static pose: centered, at nominal viewing distance
    out_relation->pose.orientation.w = 1.0f;
    out_relation->pose.position.z = -0.65f;  // Negative Z = looking at display
    out_relation->relation_flags =
        XRT_SPACE_RELATION_ORIENTATION_VALID_BIT |
        XRT_SPACE_RELATION_POSITION_VALID_BIT |
        XRT_SPACE_RELATION_ORIENTATION_TRACKED_BIT |
        XRT_SPACE_RELATION_POSITION_TRACKED_BIT;

    return XRT_SUCCESS;
}
```

### 5.5 Reference Files

- **Leia:** `src/xrt/drivers/leia/leia_device.c` (233 lines)
- **sim_display:** `src/xrt/drivers/sim_display/sim_display_device.c`
  (includes configurable dimensions via env vars, orbit camera model,
  macOS auto-detection of physical display size)

---

## 6. Component 3: Eye Tracking

Eye tracking provides predicted eye positions used for Kooima asymmetric
frustum projection.  This is what makes the 3D effect correct for the viewer's
actual head position.

### 6.1 Data Types

The runtime uses **vendor-neutral types** for all eye tracking and display
geometry.  Vendor-specific types (e.g. `leiasr_eye_pair`) exist only inside
the vendor driver and are converted to these types at the driver boundary.

**Runtime interface types** — `src/xrt/include/xrt/xrt_display_metrics.h`:

```c
struct xrt_eye_position
{
    float x;  //!< Horizontal position (positive = right), meters
    float y;  //!< Vertical position (positive = up), meters
    float z;  //!< Depth position (positive = toward viewer), meters
};

struct xrt_eye_pair
{
    struct xrt_eye_position left;   //!< Left eye position in meters
    struct xrt_eye_position right;  //!< Right eye position in meters
    int64_t timestamp_ns;           //!< Monotonic timestamp when sampled
    bool valid;                     //!< True if eye positions are valid
    bool is_tracking;               //!< True if physical eye tracker has lock (v6)
};

struct xrt_window_metrics
{
    float display_width_m;           //!< Display physical width (meters)
    float display_height_m;          //!< Display physical height (meters)
    uint32_t display_pixel_width;    //!< Display pixel width
    uint32_t display_pixel_height;   //!< Display pixel height
    int32_t display_screen_left;     //!< Display left edge (screen coords)
    int32_t display_screen_top;      //!< Display top edge (screen coords)

    uint32_t window_pixel_width;     //!< Window client area width (pixels)
    uint32_t window_pixel_height;    //!< Window client area height (pixels)
    int32_t window_screen_left;      //!< Window client area left (screen coords)
    int32_t window_screen_top;       //!< Window client area top (screen coords)

    float window_width_m;            //!< Window physical width (meters)
    float window_height_m;           //!< Window physical height (meters)
    float window_center_offset_x_m;  //!< Offset from display center (meters, +right)
    float window_center_offset_y_m;  //!< Offset from display center (meters, +up)

    bool valid;                      //!< True if all metrics are valid
};
```

**Display geometry** is stored in `xrt_system_compositor_info`
(`src/xrt/include/xrt/xrt_compositor.h`), populated once at device init:

```c
// Inside struct xrt_system_compositor_info:
float display_width_m;            // Physical display width (meters)
float display_height_m;           // Physical display height (meters)
float nominal_viewer_x_m;         // Nominal viewer X (display space, meters)
float nominal_viewer_y_m;         // Nominal viewer Y (display space, meters)
float nominal_viewer_z_m;         // Nominal viewer Z (display space, meters)
float recommended_view_scale_x;   // Recommended render scale X
float recommended_view_scale_y;   // Recommended render scale Y
bool  supports_display_mode_switch; // 2D/3D mode switching
uint32_t supported_eye_tracking_modes; // Bitmask: SMOOTH_BIT=1, RAW_BIT=2 (v6)
uint32_t default_eye_tracking_mode;    // 0=SMOOTH, 1=RAW (v6)
```

**Coordinate system:** All positions are in display-local coordinates with
origin at the display center.  X = right, Y = up, Z = toward viewer.  Units
are meters.

**Vendor-specific types** (e.g. `leiasr_eye_position`, `leiasr_eye_pair` in
`leia_types.h`) are used only inside the driver's `.cpp` files.  At the
boundary, the driver converts to/from `xrt_*` types.  New vendors should
define their own internal types as needed but always expose `xrt_*` types
to the runtime.

### 6.2 Data Flow

The vendor's eye tracking data takes different paths depending on whether the
app uses the extension (RAW mode) or is a legacy app (RENDER_READY mode).
The vendor only provides the raw eye positions; the runtime decides what to
do with them.

Note the **boundary between vendor driver and runtime**: vendor-specific types
(`leiasr_eye_pair`, etc.) exist only inside the driver box.  At the boundary,
data is converted to vendor-neutral `xrt_eye_pair`.

```
 ┌ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┐
 │  VENDOR DRIVER  (src/xrt/drivers/<vendor>/)                 │
 │                                                              │
 │  ┌──────────────────────────────┐                            │
 │  │     Vendor Eye Tracking SDK  │                            │
 │  │  (face camera, IR tracker)   │                            │
 │  └──────────────┬───────────────┘                            │
 │                 │  vendor-specific types (internal)           │
 │                 ▼                                             │
 │  ┌──────────────────────────────┐                            │
 │  │     SDK Wrapper              │                            │
 │  │  vendor_get_predicted_eyes() │                            │
 │  └──────────────┬───────────────┘                            │
 └ ─ ─ ─ ─ ─ ─ ─ ─┼─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┘
     ══════════════╪══════════════════  BOUNDARY: xrt_eye_pair
 ┌ ─ ─ ─ ─ ─ ─ ─ ─┼─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┐
 │  OPENXR RUNTIME │ (vendor-neutral types only)                │
 │                 ▼                                             │
 │  ┌──────────────────────────────┐                            │
 │  │     Compositor               │                            │
 │  │  get_predicted_eye_positions │                            │
 │  │  → xrt_eye_pair             │                            │
 │  └──────────────┬───────────────┘                            │
 │                 │                                             │
 │                 ▼                                             │
 │  ┌──────────────────────────────────────────────────────────┐│
 │  │  oxr_session_locate_views()                              ││
 │  │  uses: xrt_eye_position, xrt_window_metrics,            ││
 │  │        xrt_system_compositor_info                        ││
 │  │                                                          ││
 │  │  ┌─────────────────────────┐  ┌────────────────────────┐││
 │  │  │  RAW mode               │  │  RENDER_READY mode     │││
 │  │  │  (has_external_window)  │  │  (!has_external_window) │││
 │  │  │                         │  │                         │││
 │  │  │  pose = raw eye in      │  │  pose = qwerty_transform│││
 │  │  │         DISPLAY space   │  │         × eye position  │││
 │  │  │  ori  = identity        │  │  ori  = qwerty orient.  │││
 │  │  │  fov  = advisory only   │  │  fov  = Kooima FOV     │││
 │  │  └────────────┬────────────┘  └──────────────┬──────────┘││
 │  └───────────────┼──────────────────────────────┼───────────┘│
 └ ─ ─ ─ ─ ─ ─ ─ ─ ┼─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┼─ ─ ─ ─ ─ ─┘
     ════════════════╪══════════════════════════════╪═  BOUNDARY: XrView
 ┌ ─ ─ ─ ─ ─ ─ ─ ─ ┼─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┼─ ─ ─ ─ ─ ─┐
 │  APPLICATION     │                              │            │
 │                  ▼                              ▼            │
 │  ┌──────────────────────────┐  ┌────────────────────────────┐│
 │  │  Extension-aware app     │  │  Legacy OpenXR / WebXR app ││
 │  │  • ignores XrView.fov    │  │  • uses XrView.fov directly││
 │  │  • computes own Kooima   │  │  • uses XrView.pose as-is  ││
 │  │    from eye pos + display │  │  • navigates via WASD/mouse││
 │  │    geometry               │  │    (qwerty debug controller)│
 │  └──────────────────────────┘  └────────────────────────────┘│
 └ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┘
```

### 6.3 Kooima Asymmetric Frustum Projection

The Kooima algorithm ("Generalized Perspective Projection") computes an
asymmetric frustum where the near plane maps to the physical screen edges
as seen from the eye position.

**Who computes it depends on the mode:**

- **RAW mode:** The **application** computes Kooima from the raw eye positions
  and `displaySizeMeters` returned by `XR_EXT_display_info`.  The runtime's
  `XrView.fov` values are advisory only.  This is the primary use case for
  apps designed for tracked 3D displays.

- **RENDER_READY mode:** The **runtime** computes Kooima internally and
  returns the result in `XrView.fov`.  This lets existing OpenXR/WebXR apps
  (designed for HMDs) run on 3D displays without modification.

The Kooima math itself is the same in both cases:

```
left   = nearZ * (-halfWidth  - eyeX) / eyeZ
right  = nearZ * (+halfWidth  - eyeX) / eyeZ
bottom = nearZ * (-halfHeight - eyeY) / eyeZ
top    = nearZ * (+halfHeight - eyeY) / eyeZ
```

Where `halfWidth = displaySizeMeters.width / 2`, `halfHeight = displaySizeMeters.height / 2`,
and `(eyeX, eyeY, eyeZ)` is the eye position in DISPLAY space.

The runtime's internal implementation (used for RENDER_READY mode) is in
`oxr_session.c:compute_kooima_fov()`.  Note the **vendor-neutral type**
`xrt_eye_position` — this function works with any vendor's eye data:

```c
static void
compute_kooima_fov(const struct xrt_eye_position *eye,   // vendor-neutral!
                   float screen_width_m,
                   float screen_height_m,
                   const char *eye_name,
                   bool should_log,
                   struct xrt_fov *out_fov)
{
    const float half_w = screen_width_m / 2.0f;
    const float half_h = screen_height_m / 2.0f;
    const float distance = eye->z;

    // Asymmetric frustum from eye to screen edges
    out_fov->angle_left  = atanf((-half_w - eye->x) / distance);
    out_fov->angle_right = atanf(( half_w - eye->x) / distance);
    out_fov->angle_up    = atanf(( half_h - eye->y) / distance);
    out_fov->angle_down  = atanf((-half_h - eye->y) / distance);
}
```

This function is **not** behind any vendor-specific `#ifdef`.  It operates on
vendor-neutral types and works with both SR tracked eye positions (from Leia)
and nominal viewer positions (from sim_display or any future vendor).

**Vendor takeaway:** The vendor only provides raw eye positions (via
`xrt_eye_pair`).  The runtime handles the RENDER_READY Kooima math;
extension-aware apps handle their own.

### 6.4 Fallback Positions

When eye tracking is unavailable, the vendor SDK should return default
positions representing a centered viewer at the nominal viewing distance:

```c
// Default fallback (no tracking)
out_left_eye[0]  = -0.032f;  // -32mm left of center
out_left_eye[1]  =  0.0f;
out_left_eye[2]  =  0.6f;    // 600mm from display
out_right_eye[0] =  0.032f;  // +32mm right of center
out_right_eye[1] =  0.0f;
out_right_eye[2] =  0.6f;
return false;  // Indicates fallback, not tracked
```

### 6.5 Integration Points for Vendor Eye Tracking

Eye positions flow from the vendor driver to the runtime through compositor
functions that return `xrt_eye_pair` (vendor-neutral):

1. **D3D11 path:** `comp_d3d11_compositor_get_predicted_eye_positions()`
   → internally calls vendor SDK → returns `xrt_vec3` left/right
2. **Vulkan path:** `multi_compositor_get_predicted_eye_positions()`
   → internally calls vendor SDK via per-session weaver → returns eye pair

Both paths feed into the runtime's `oxr_session_get_predicted_eye_positions()`
which dispatches based on the compositor type and converts to `xrt_eye_pair`.

**Display dimensions** are read from `xrt_system_compositor_info` which is
populated once at device init.  The runtime calls
`oxr_session_get_display_dimensions()` which reads `info.display_width_m`
and `info.display_height_m` — no vendor SDK call at runtime.

**Window metrics** flow through `multi_compositor_get_window_metrics()` which
returns `xrt_window_metrics`.  The Vulkan path computes window metrics from
generic Win32/macOS APIs (no vendor SDK needed); the D3D11 path may query
the vendor SDK if it manages the window.

### 6.6 Eye Tracking Mode Control (v6)

Version 6 of `XR_EXT_display_info` adds eye tracking mode control, allowing apps
to choose between smooth (SDK-filtered) and raw eye tracking.

#### Required Internal Fields

**`xrt_eye_pair.is_tracking`** — Set by the vendor's eye position function.
`true` when the physical eye tracker has lock on the user. When `false`, positions
are still valid — the vendor SDK provides reasonable fallback (last known, filtered,
nominal viewer). The runtime passes vendor values through unchanged.

**`xrt_system_compositor_info.supported_eye_tracking_modes`** — Bitmask set at init:
- `1` (SMOOTH_BIT): SDK handles grace period + smoothing
- `2` (RAW_BIT): SDK provides unfiltered positions
- `3` (both): Runtime supports both modes
- `0`: No eye tracking (display only)

**`xrt_system_compositor_info.default_eye_tracking_mode`** — `0` for SMOOTH, `1` for RAW.

#### Capability Advertisement

Set these fields in `target_instance.c` alongside other `xrt_system_compositor_info`
fields:

```c
// Leia SR: smooth only
xsysc->info.supported_eye_tracking_modes = 1; // SMOOTH_BIT
xsysc->info.default_eye_tracking_mode = 0;    // SMOOTH

// Sim display: raw only
xsysc->info.supported_eye_tracking_modes = 2; // RAW_BIT
xsysc->info.default_eye_tracking_mode = 1;    // RAW
```

#### `is_tracking` Contract

- Vendor **MUST** set `is_tracking` to reflect physical tracker lock status
- `xrLocateViews` **ALWAYS** returns fully populated views regardless of `is_tracking`
- When `is_tracking == false`, vendor SDK populates positions as it sees fit
  (nominal, last known, filtered)
- `isTracking` only tells the app whether positions are from live tracking or fallback

#### Smooth Mode Implementation

Vendor SDK handles grace/resume smoothing internally. The runtime passes positions
through unchanged. Eye positions converge smoothly to a rest position when tracking
is lost. `is_tracking` reflects the vendor heuristic (e.g., inter-eye distance
approaching zero).

#### Raw Mode Implementation

Vendor SDK provides unfiltered positions. Vendor is still responsible for providing
fallback positions when tracking is lost. The app uses `isTracking` to handle
tracking loss with its own animations/UI.

#### No-Tracking Displays

Set `supported_eye_tracking_modes = 0`. Runtime reports `supportedModes = NONE`,
`isTracking = XR_FALSE` always. `xrLocateViews` still returns fully populated views
(vendor SDK populates positions, e.g., nominal viewer). `xrRequestEyeTrackingModeEXT`
returns `XR_ERROR_FEATURE_UNSUPPORTED` for any mode.

#### Reference Implementations

| Vendor | `supported` | `default` | `is_tracking` source |
|--------|-------------|-----------|---------------------|
| Leia SR | `1` (SMOOTH) | `0` (SMOOTH) | Eye-distance heuristic: `dist² > 1e-6` |
| Sim display | `2` (RAW) | `1` (RAW) | Always `true` (simulated) |
| No-tracker | `0` (NONE) | N/A | Always `false` |

---

## 7. Component 4: SDK Wrapper

The SDK wrapper isolates vendor-specific headers from the rest of the codebase.
This is essential because:

- Vendor SDKs often use C++ classes, templates, exceptions
- The codebase core is C11
- Multiple vendors must coexist without header conflicts

### 7.1 Pattern: Opaque C Struct

**Header (.h):** Declares an opaque forward reference:

```c
// my_vendor_sdk.h
#pragma once
#include "xrt/xrt_results.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct my_vendor_sdk;  // Opaque — defined only in .cpp

xrt_result_t
my_vendor_sdk_create(void *window_handle, struct my_vendor_sdk **out);

void
my_vendor_sdk_destroy(struct my_vendor_sdk **sdk_ptr);

bool
my_vendor_sdk_get_predicted_eyes(struct my_vendor_sdk *sdk,
                                 float out_left[3],
                                 float out_right[3]);

bool
my_vendor_sdk_is_ready(struct my_vendor_sdk *sdk);

bool
my_vendor_sdk_request_display_mode(struct my_vendor_sdk *sdk, bool enable_3d);

#ifdef __cplusplus
}
#endif
```

**Implementation (.cpp):** Includes vendor headers, defines the struct:

```cpp
// my_vendor_sdk.cpp
#include "my_vendor_sdk.h"

// Vendor headers ONLY in .cpp
#include <VendorSDK/Core.h>
#include <VendorSDK/EyeTracker.h>
#include <VendorSDK/Interlacer.h>

extern "C" {

struct my_vendor_sdk
{
    VendorSDK::Core *core;
    VendorSDK::EyeTracker *tracker;
    VendorSDK::Interlacer *interlacer;
    // ... cached state ...
};

xrt_result_t
my_vendor_sdk_create(void *window_handle, struct my_vendor_sdk **out)
{
    struct my_vendor_sdk *sdk = new my_vendor_sdk();
    try {
        sdk->core = VendorSDK::Core::create();
        sdk->tracker = sdk->core->getEyeTracker();
        // Lazy interlacer creation — wait until first frame
    } catch (...) {
        delete sdk;
        return XRT_ERROR_DEVICE_CREATION_FAILED;
    }
    *out = sdk;
    return XRT_SUCCESS;
}

} // extern "C"
```

### 7.2 Platform-Specific Initialization

Guard platform-specific init with preprocessor checks:

```cpp
xrt_result_t
my_vendor_sdk_create(/* ... */)
{
#ifdef XRT_OS_ANDROID
    // JNI initialization for Android
    JNIEnv *env = /* ... */;
    VendorSDK::initAndroid(env, activity);
#endif

#ifdef XRT_OS_WINDOWS
    // COM initialization for Windows
    CoInitializeEx(NULL, COINIT_MULTITHREADED);
    VendorSDK::initWindows(hwnd);
#endif

    // Common initialization ...
}
```

### 7.3 Lazy Initialization

The vendor SDK may not be ready when the wrapper is created (e.g., display
detection is asynchronous).  Use lazy initialization:

```cpp
void
my_vendor_sdk_weave(struct my_vendor_sdk *sdk, /* ... */)
{
    // Lazily create interlacer on first call after SDK is ready
    if (sdk->interlacer == NULL && sdk->core->isReady()) {
        sdk->interlacer = sdk->core->createInterlacer(/* ... */);
    }

    if (sdk->interlacer == NULL) {
        return;  // Not ready yet, skip this frame
    }

    sdk->interlacer->processFrame(/* ... */);
}
```

### 7.4 Reference Files

- **Leia SR (Windows Vulkan):** `leia_sr.h` / `leia_sr.cpp` (675 lines)
- **Leia SR (Windows D3D11):** `leia_sr_d3d11.h` / `leia_sr_d3d11.cpp` (766 lines)
- **Leia CNSDK (Android):** `leia_cnsdk.h` / `leia_cnsdk.cpp` (151 lines)

---

## 8. Component 5: Target Builder and Build System Registration

The target builder registers the vendor's driver with the runtime.  When the
runtime starts, it iterates the builder list, calls `estimate_system()` on
each, and the highest-priority builder that claims hardware creates the devices.

**Registration requires touching 5 files** beyond your driver code.  Missing any
one silently excludes the builder from the runtime — there will be no error at
build time, the builder simply won't appear in the builder list at runtime.

### 8.1 Registration Checklist

Every step is required.  The table shows the exact file and what to add:

| # | File | What to Add | If Missing... |
|---|------|-------------|---------------|
| 1 | `CMakeLists.txt` (root) | `"MY_VENDOR"` to `AVAILABLE_DRIVERS` list | `XRT_BUILD_DRIVER_MY_VENDOR` never defined |
| 2 | `src/xrt/drivers/CMakeLists.txt` | `drv_my_vendor` library definition | Driver objects not compiled |
| 3 | `src/xrt/targets/common/target_builder_interface.h` | `T_BUILDER_MY_VENDOR` guard + function declaration | Builder not registered in target_lists.c |
| 4 | `src/xrt/targets/common/target_lists.c` | `t_builder_my_vendor_create` in builder array | Builder compiled but never instantiated |
| 5 | `src/xrt/targets/common/CMakeLists.txt` | `target_sources` + `target_link_libraries` | Builder .c file not compiled / linked |

### 8.2 Config Header System (Important)

The build system generates **two** config headers.  Understanding which one
contains your guard is critical — using the wrong header silently breaks
registration:

| Header | Generated From | Contains | Used For |
|--------|---------------|----------|----------|
| `xrt_config_drivers.h` | `AVAILABLE_DRIVERS` list in root `CMakeLists.txt` | `XRT_BUILD_DRIVER_*` defines | Driver compile guards |
| `xrt_config_have.h` | `option_with_deps()` calls in root `CMakeLists.txt` | `XRT_HAVE_*` defines | External dependency guards |

`target_builder_interface.h` includes **both** headers, so you can use either
guard style.  The recommended approach depends on how your SDK is found:

**Option A (recommended for most vendors):** Add your driver to `AVAILABLE_DRIVERS`
and use `XRT_BUILD_DRIVER_MY_VENDOR`.  This is the standard Monado pattern:

```cmake
# In root CMakeLists.txt, add to the AVAILABLE_DRIVERS list:
list(APPEND AVAILABLE_DRIVERS "MY_VENDOR")

# Then enable it based on SDK detection:
cmake_dependent_option(XRT_BUILD_DRIVER_MY_VENDOR
    "Enable MyVendor 3D display driver" ON "MY_VENDOR_SDK_FOUND" OFF)
```

**Option B (external SDK dependency):** If your driver is gated by
`find_package()` and uses `option_with_deps()`, you get an `XRT_HAVE_*` define
instead.  This is what Leia uses (`XRT_HAVE_LEIA_SR`).  Both approaches work in
`target_builder_interface.h`.

### 8.3 Builder Implementation

**File:** `src/xrt/targets/common/target_builder_<vendor>.c`

```c
#include "xrt/xrt_config_drivers.h"
#include "util/u_misc.h"
#include "util/u_debug.h"
#include "util/u_builders.h"
#include "util/u_system_helpers.h"
#include "target_builder_interface.h"
#include "target_builder_qwerty_input.h"
#include "my_vendor/my_vendor_interface.h"

static const char *driver_list[] = { "my_vendor" };

static xrt_result_t
my_vendor_estimate_system(struct xrt_builder *xb,
                          cJSON *config,
                          struct xrt_prober *xp,
                          struct xrt_builder_estimate *estimate)
{
    // Probe for hardware (or check env var, etc.)
    // Only claim certain.head if your display is actually present.
    estimate->certain.head = true;
    estimate->priority = -15;  // Higher number = higher priority
    return XRT_SUCCESS;
}

static xrt_result_t
my_vendor_open_system_impl(struct xrt_builder *xb,
                           cJSON *config,
                           struct xrt_prober *xp,
                           struct xrt_tracking_origin *origin,
                           struct xrt_system_devices *xsysd,
                           struct xrt_frame_context *xfctx,
                           struct u_builder_roles_helper *ubrh)
{
    struct xrt_device *head = my_vendor_hmd_create();
    if (head == NULL) {
        return XRT_ERROR_DEVICE_CREATION_FAILED;
    }

    xsysd->xdevs[xsysd->xdev_count++] = head;
    ubrh->head = head;

    // Add qwerty keyboard/mouse input devices (controllers + optional HMD for pose).
    // Pass &qwerty_hmd if you need pose delegation (e.g., WASD camera control),
    // or NULL if your SDK provides its own head tracking.
    t_builder_add_qwerty_input(xsysd, ubrh, U_LOGGING_INFO, NULL);

    return XRT_SUCCESS;
}

static void
my_vendor_destroy(struct xrt_builder *xb)
{
    free(xb);
}

struct xrt_builder *
t_builder_my_vendor_create(void)
{
    struct u_builder *ub = U_TYPED_CALLOC(struct u_builder);

    ub->base.estimate_system = my_vendor_estimate_system;
    ub->base.open_system     = u_builder_open_system_static_roles;
    ub->base.destroy         = my_vendor_destroy;
    ub->base.identifier      = "my_vendor";
    ub->base.name            = "MyVendor 3D Display";
    ub->base.driver_identifiers      = driver_list;
    ub->base.driver_identifier_count = ARRAY_SIZE(driver_list);
    ub->base.exclude_from_automatic_discovery = false;

    ub->open_system_static_roles = my_vendor_open_system_impl;

    return &ub->base;
}
```

### 8.4 Priority and Selection System

The runtime selects a builder in two passes:

1. **Certain pass:** Find the builder with the highest `priority` that set
   `estimate->certain.head = true`.  If found, use it.
2. **Maybe pass:** If no builder is certain, find the highest-priority builder
   that set `estimate->maybe.head = true`.

**Higher number = higher priority** (closer to 0 wins).  `-15` beats `-20`.

| Priority | Builder | Head Claim | Description |
|----------|---------|-----------|-------------|
| (high) | remote, simulated | `certain.head` | Debug/override drivers |
| -15 | leia | `certain.head` | Leia 3D display (SR SDK) |
| -20 | sim_display | `certain.head` | Simulation display |
| -19 | qwerty | `maybe.head` | Fallback when no display builder active |
| -20 | legacy | `maybe.head` | Upstream Monado catch-all |
| (lower) | lighthouse, wmr, etc. | `certain.head` | Traditional VR hardware |

Display builders must claim `certain.head` — this guarantees they win over
qwerty's `maybe.head` fallback regardless of priority.  Qwerty provides
keyboard/mouse input devices — display builders add them via
`t_builder_add_qwerty_input()` in their `open_system_impl`.

**Guideline:** Use a priority between -10 and -20 for real hardware.

### 8.5 Step-by-Step: Registration in `CMakeLists.txt` (root)

Add your driver name to the `AVAILABLE_DRIVERS` list (alphabetical order):

```cmake
list(
    APPEND
    AVAILABLE_DRIVERS
    ...
    "MY_VENDOR"    # <-- add here
    ...
)
```

This generates `#define XRT_BUILD_DRIVER_MY_VENDOR` in the auto-generated
`xrt_config_drivers.h` header when the driver is enabled.

### 8.6 Step-by-Step: Registration in `target_builder_interface.h`

Add the builder guard and function declaration:

```c
// In target_builder_interface.h:

// Use XRT_BUILD_DRIVER_MY_VENDOR (from xrt_config_drivers.h, via AVAILABLE_DRIVERS)
// or XRT_HAVE_MY_VENDOR_SDK (from xrt_config_have.h, via option_with_deps) —
// both headers are included at the top of this file.
#if defined(XRT_BUILD_DRIVER_MY_VENDOR) || defined(XRT_DOXYGEN)
#define T_BUILDER_MY_VENDOR
#endif

#ifdef T_BUILDER_MY_VENDOR
/*!
 * Builder for MyVendor 3D displays.
 */
struct xrt_builder *
t_builder_my_vendor_create(void);
#endif
```

### 8.7 Step-by-Step: Registration in `target_lists.c`

Add the builder to the builder list array:

```c
// In target_lists.c:
xrt_builder_create_func_t target_builder_list[] = {
    // ... existing entries ...

#ifdef T_BUILDER_MY_VENDOR
    t_builder_my_vendor_create,
#endif

    // ... existing entries ...
    NULL,
};
```

**Placement matters:** Place your entry in the list at the position
corresponding to your priority (after overrides, before traditional VR).

### 8.8 Step-by-Step: CMake Integration

Two more CMake files need changes:

#### `src/xrt/drivers/CMakeLists.txt`

```cmake
# MyVendor 3D Display Driver
if(XRT_BUILD_DRIVER_MY_VENDOR)
    add_library(drv_my_vendor STATIC
        my_vendor/my_vendor_device.c
        my_vendor/my_vendor_interface.h
        my_vendor/my_vendor_sdk.cpp
        my_vendor/my_vendor_sdk.h
        my_vendor/my_vendor_display_processor.cpp
        my_vendor/my_vendor_display_processor.h
    )
    target_link_libraries(drv_my_vendor PRIVATE
        xrt-interfaces
        aux_util
        aux_os
        aux_math
    )
    target_include_directories(drv_my_vendor PRIVATE
        ${MY_VENDOR_SDK_PATH}/include
    )
    target_link_libraries(drv_my_vendor PRIVATE
        ${MY_VENDOR_SDK_PATH}/lib/MyVendorCore.lib
    )
    list(APPEND ENABLED_DRIVERS my_vendor)
endif()
```

#### `src/xrt/targets/common/CMakeLists.txt`

```cmake
if(XRT_BUILD_DRIVER_MY_VENDOR)
    target_sources(target_lists PRIVATE target_builder_my_vendor.c)
    target_link_libraries(target_lists PRIVATE drv_my_vendor)
endif()
```

### 8.9 Verification

After completing all registration steps, verify your builder appears at runtime:

```
[INFO ] [p_create_system] Creating system:
    Builders:
        my_vendor: MyVendor 3D Display      <-- must appear here
        qwerty: Qwerty devices builder
        ...
    Selected my_vendor (priority -15) because it was certain it could create a head
```

If your builder is **missing from the list**, check:
1. Is `XRT_BUILD_DRIVER_MY_VENDOR` defined? → Check `AVAILABLE_DRIVERS` in root CMakeLists.txt
2. Is the guard in `target_builder_interface.h` using the right define? → `xrt_config_drivers.h` has `XRT_BUILD_DRIVER_*`, `xrt_config_have.h` has `XRT_HAVE_*`
3. Is the builder added to `target_lists.c` inside the `#ifdef T_BUILDER_MY_VENDOR` guard?
4. Is the builder .c file added to `target_sources` in `targets/common/CMakeLists.txt`?

### 8.10 Reference Files

- **Leia builder:** `src/xrt/targets/common/target_builder_leia.c`
- **sim_display builder:** `src/xrt/targets/common/target_builder_sim_display.c`
- **Builder interface:** `src/xrt/targets/common/target_builder_interface.h`
- **Builder list:** `src/xrt/targets/common/target_lists.c`
- **Available drivers:** root `CMakeLists.txt` → `AVAILABLE_DRIVERS`
- **Config headers:** `src/xrt/include/xrt/xrt_config_drivers.h.cmake_in`, `xrt_config_have.h.cmake_in`

---

## 9. Complete File Listing

A vendor integration creates the following files (using `my_vendor` as the
driver directory name):

```
src/xrt/drivers/my_vendor/
├── my_vendor_interface.h                  Public interface (hmd_create, builder)
├── my_vendor_device.c                     xrt_device implementation
├── my_vendor_sdk.h                        Opaque SDK wrapper header
├── my_vendor_sdk.cpp                      SDK wrapper implementation
├── my_vendor_types.h                      Internal vendor types (optional)
├── my_vendor_display_processor.h          Vulkan display processor header
├── my_vendor_display_processor.cpp        Vulkan display processor impl
├── my_vendor_display_processor_d3d11.h    D3D11 display processor header (Windows)
├── my_vendor_display_processor_d3d11.cpp  D3D11 display processor impl (Windows)
└── (optional vendor SDK wrappers per platform)

src/xrt/targets/common/
├── target_builder_my_vendor.c             Builder registration

Modified files (all 5 required — see §8.1 checklist):
├── CMakeLists.txt (root)                  Add to AVAILABLE_DRIVERS list
├── src/xrt/drivers/CMakeLists.txt         Add drv_my_vendor library
├── src/xrt/targets/common/CMakeLists.txt  Link builder to driver
├── src/xrt/targets/common/target_builder_interface.h  Declare builder guard + function
└── src/xrt/targets/common/target_lists.c  Register builder in list
```

For reference, the Leia driver has 13 files:

```
src/xrt/drivers/leia/
├── leia_interface.h                 Public interface
├── leia_device.c                    xrt_device (233 lines)
├── leia_types.h                     Shared types (85 lines)
├── leia_sr.h                        SR Vulkan wrapper header (190 lines)
├── leia_sr.cpp                      SR Vulkan wrapper (675 lines)
├── leia_sr_d3d11.h                  SR D3D11 wrapper header (299 lines)
├── leia_sr_d3d11.cpp                SR D3D11 wrapper (766 lines)
├── leia_cnsdk.h                     CNSDK wrapper header (82 lines)
├── leia_cnsdk.cpp                   CNSDK wrapper (151 lines)
├── leia_display_processor.h         Vulkan display processor header (40 lines)
├── leia_display_processor.cpp       Vulkan display processor impl (117 lines)
├── leia_display_processor_d3d11.h   D3D11 display processor header (40 lines)
└── leia_display_processor_d3d11.cpp D3D11 display processor impl (111 lines)
```

---

## 10. Data Flow Diagrams

### 10.1 Vulkan Rendering Pipeline

```
 ┌───────────┐     ┌──────────────────┐     ┌────────────────────┐
 │  OpenXR   │     │  multi_compositor │     │   comp_renderer    │
 │   App     │────▶│  (per-session)    │────▶│   (Vulkan)         │
 │ xrEndFrame│     │  layer submission │     │   composites layers│
 └───────────┘     └──────────────────┘     └─────────┬──────────┘
                                                       │
                                                       │ left/right VkImageView
                                                       ▼
                                            ┌─────────────────────┐
                                            │  Display Processor  │
                                            │  (vendor Vulkan)    │
                                            │  process_views()    │
                                            │  records Vk cmds    │
                                            └─────────┬───────────┘
                                                       │
                                                       │ VkFramebuffer
                                                       ▼
                                            ┌─────────────────────┐
                                            │  comp_target_swapchain│
                                            │  (window surface)    │
                                            │  vkQueuePresent      │
                                            └──────────────────────┘
```

### 10.2 D3D11 Rendering Pipeline

```
 ┌───────────┐     ┌───────────────────────┐
 │  OpenXR   │     │  comp_d3d11_compositor │
 │   App     │────▶│  (per-session)         │
 │ xrEndFrame│     │  composites layers     │
 └───────────┘     │  into SBS texture      │
                   └───────────┬────────────┘
                               │
                               │ SBS stereo ID3D11ShaderResourceView*
                               ▼
                   ┌───────────────────────┐
                   │  Display Processor    │
                   │  (vendor D3D11)       │
                   │  process_stereo()     │
                   │  immediate D3D11 draw │
                   └───────────┬───────────┘
                               │
                               │ bound render target
                               ▼
                   ┌───────────────────────┐
                   │  SwapChain Present    │
                   │  (DXGI)              │
                   └───────────────────────┘
```

### 10.3 Eye Tracking Pipeline

```
 ┌ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ┐
 │ VENDOR DRIVER                           │
 │ ┌────────────────────┐                  │
 │ │ Vendor Eye Tracker │                  │
 │ │ (face camera, IR)  │                  │
 │ └─────────┬──────────┘                  │
 │           │ vendor-specific types       │
 │           ▼                             │
 │ ┌────────────────────┐                  │
 │ │ SDK Wrapper        │                  │
 │ │ convert to meters  │                  │
 │ │ prediction filter  │                  │
 │ └─────────┬──────────┘                  │
 └ ─ ─ ─ ─ ─┼─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┘
    ═════════╪═══════  BOUNDARY: xrt_eye_pair
 ┌ ─ ─ ─ ─ ─┼─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┐
 │ RUNTIME   │ (vendor-neutral types)      │
 │           ▼                             │
 │ ┌────────────────────┐                  │
 │ │ Compositor         │                  │
 │ │ → xrt_eye_pair     │                  │
 │ └─────────┬──────────┘                  │
 │           │                             │
 │           ▼                             │
 │ ┌────────────────────────────────────┐  │
 │ │ oxr_session_locate_views()        │  │
 │ │ uses: xrt_eye_position,           │  │
 │ │       xrt_window_metrics,         │  │
 │ │       xrt_system_compositor_info  │  │
 │ │                                   │  │
 │ │ has_external_window?              │  │
 │ │ ┌──YES (RAW)────┐ ┌──NO (READY)─┐│  │
 │ │ │pose = raw eye  │ │pose = qwerty││  │
 │ │ │ori  = identity │ │fov  = Kooima││  │
 │ │ │fov  = advisory │ │ori  = qwerty││  │
 │ │ └──────┬─────────┘ └──────┬──────┘│  │
 │ └────────┼──────────────────┼───────┘  │
 └ ─ ─ ─ ─ ┼─ ─ ─ ─ ─ ─ ─ ─ ─┼─ ─ ─ ─ ─┘
    ════════╪══════════════════╪═  BOUNDARY: XrView
 ┌ ─ ─ ─ ─ ┼─ ─ ─ ─ ─ ─ ─ ─ ─┼─ ─ ─ ─ ─ ┐
 │ APP      ▼                  ▼           │
 │ ┌─────────────────┐ ┌─────────────────┐ │
 │ │ Extension-aware │ │ Legacy OpenXR / │ │
 │ │ reads raw eye   │ │ WebXR app       │ │
 │ │ computes Kooima │ │ uses fov + pose │ │
 │ │ ignores XrFov   │ │ WASD/mouse nav  │ │
 │ └─────────────────┘ └─────────────────┘ │
 └ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─ ─┘
```

---

## 11. Platform Support Matrix

| Component | Windows | Android | Linux | macOS |
|-----------|---------|---------|-------|-------|
| **Vulkan display processor** | Yes | Yes | Yes (if SDK available) | Yes (via MoltenVK) |
| **D3D11 display processor** | Yes | N/A | N/A | N/A |
| **Eye tracking** | SR SDK LookaroundFilter | CNSDK face tracking | Vendor SDK | Vendor SDK |
| **SDK wrapper (Vulkan)** | `leia_sr.cpp` | `leia_cnsdk.cpp` | — | — |
| **SDK wrapper (D3D11)** | `leia_sr_d3d11.cpp` | N/A | N/A | N/A |
| **Window binding** | `XR_EXT_win32_window_binding` | N/A | — | `XR_EXT_macos_window_binding` |
| **2D/3D mode switch** | SwitchableLensHint | Backlight API | Vendor-specific | Vendor-specific |

### Platform-Specific Notes

**Windows** (primary target):
- Both Vulkan and D3D11 display processors recommended
- D3D11 path is preferred for Intel GPUs (Vulkan interop issues)
- SR SDK provides eye tracking via LookaroundFilter
- `XR_EXT_win32_window_binding` for app-provided HWND

**Android:**
- Vulkan display processor required
- CNSDK provides interlacing and backlight control
- JNI initialization required (`#ifdef XRT_OS_ANDROID`)
- No window binding extension (app controls the Activity)

**Linux/macOS:**
- Vulkan display processor if vendor SDK has Linux/macOS support
- Useful for development/testing with sim_display
- macOS uses MoltenVK with `VK_EXT_external_memory_metal`

---

## 12. Testing with sim_display

The simulation display driver runs on any GPU and provides a complete reference
for developing and testing without vendor hardware.

### 12.1 Enabling sim_display

```bash
# Enable sim_display driver
export SIM_DISPLAY_ENABLE=1

# Choose output mode
export SIM_DISPLAY_OUTPUT=sbs        # Side-by-side (default)
export SIM_DISPLAY_OUTPUT=anaglyph   # Red-cyan anaglyph
export SIM_DISPLAY_OUTPUT=blend      # 50/50 alpha blend
```

### 12.2 Configuring Display Geometry

```bash
# Physical display size (meters)
export SIM_DISPLAY_WIDTH_M=0.344
export SIM_DISPLAY_HEIGHT_M=0.194

# Nominal viewing distance (meters)
export SIM_DISPLAY_NOMINAL_Z_M=0.65

# Display pixel resolution
export SIM_DISPLAY_PIXEL_W=3840
export SIM_DISPLAY_PIXEL_H=2160
```

### 12.3 Using sim_display for Development

1. **Build the runtime** with sim_display enabled (it's always enabled by
   default in the build system).

2. **Run your test app** with `SIM_DISPLAY_ENABLE=1`:
   ```bash
   export XR_RUNTIME_JSON=./build/openxr_monado-dev.json
   export SIM_DISPLAY_ENABLE=1
   export SIM_DISPLAY_OUTPUT=sbs
   ./your_openxr_app
   ```

3. **Verify the rendering pipeline** works end-to-end before integrating
   your actual vendor SDK.

4. **Compare your display processor output** with sim_display's SBS output
   to verify correctness.

### 12.4 sim_display as a Code Template

The sim_display driver is the minimal starting point:

| sim_display file | What it demonstrates |
|------------------|---------------------|
| `sim_display_device.c` | xrt_device creation, env var config, Kooima geometry |
| `sim_display_processor.c` | Vulkan display processor with GLSL shaders |
| `sim_display_processor_d3d11.cpp` | D3D11 display processor with HLSL shaders |
| `sim_display_interface.h` | Public interface, output mode enum, thread-safe state |
| `target_builder_sim_display.c` | Builder with env var gating, qwerty integration |

---

## Appendix A: Key Type References

### `xrt_eye_position` / `xrt_eye_pair` / `xrt_window_metrics`

```c
// From xrt_display_metrics.h — vendor-neutral runtime interface types
// See Section 6.1 for full definitions
```

These types define the **boundary contract** between vendor drivers and the
runtime.  Vendor code must produce data in these formats; the runtime
consumes them without knowing which vendor produced them.

### `xrt_system_compositor_info` (display geometry fields)

```c
// From xrt_compositor.h — populated by vendor device at init time
float display_width_m;            // Physical display width (meters)
float display_height_m;           // Physical display height (meters)
float nominal_viewer_x_m;         // Default viewer X (meters, display space)
float nominal_viewer_y_m;         // Default viewer Y
float nominal_viewer_z_m;         // Default viewer Z (distance from display)
```

The runtime reads these at runtime for Kooima FOV, `XR_EXT_display_info`,
and nominal eye positions when tracking is unavailable.

### `VkFormat_XDP`

```c
// From xrt_display_processor.h — avoids #include <vulkan/vulkan.h>
typedef int32_t VkFormat_XDP;
```

Use standard `VkFormat` enum values cast to `VkFormat_XDP`.  Inside your
`.cpp` implementation where you `#include <vulkan/vulkan.h>`, you can cast
freely: `(VkFormat)target_format`.

### `xrt_result_t`

```c
// Standard return codes — use these, don't invent new ones
XRT_SUCCESS                      // 0
XRT_ERROR_DEVICE_CREATION_FAILED // Device init failed
// Note: XRT_ERROR_DEVICE_NOT_FOUND does NOT exist
```

### `u_device_simple_info`

```c
struct u_device_simple_info {
    struct {
        uint32_t w_pixels, h_pixels;
        float w_meters, h_meters;
    } display;
    float lens_horizontal_separation_meters;
    float lens_vertical_position_meters;
    float fov[2];  // Horizontal FOV per eye in radians
};
```

---

## Appendix B: Quick-Start Recipe

For a vendor starting from scratch, here is the recommended order:

1. **Copy sim_display** as a template → rename to `my_vendor/`
2. **Replace the device** with your display's actual specs
3. **Replace the display processor** shaders with your interlacing algorithm
4. **Add your SDK wrapper** (.h/.cpp pair with opaque struct)
5. **Wire up eye tracking** — return `xrt_eye_pair` through the compositor interface
6. **Populate `xrt_system_compositor_info`** with display geometry at device init
7. **Register in the build system** (all 5 files — see §8.1 checklist):
   - Add `"MY_VENDOR"` to `AVAILABLE_DRIVERS` in root `CMakeLists.txt`
   - Add `drv_my_vendor` library in `src/xrt/drivers/CMakeLists.txt`
   - Add `T_BUILDER_MY_VENDOR` guard + declaration in `target_builder_interface.h`
   - Add `t_builder_my_vendor_create` in `target_lists.c`
   - Add `target_sources` + `target_link_libraries` in `targets/common/CMakeLists.txt`
8. **Add keyboard/mouse input** by calling `t_builder_add_qwerty_input()` in your builder's `open_system_impl`
9. **Verify builder appears** in the runtime log: `Builders: ... my_vendor: MyVendor 3D Display`
10. **Test with `SIM_DISPLAY_ENABLE=0`** and your actual hardware

**Remember:** vendor-specific code goes **only** in `src/xrt/drivers/<vendor>/`.
No changes to runtime files (`oxr_session.c`, `comp_multi_compositor.c`, etc.)
should be necessary.

Total lines of code for a minimal integration (without eye tracking):
approximately 300-500 lines across 4-5 files.

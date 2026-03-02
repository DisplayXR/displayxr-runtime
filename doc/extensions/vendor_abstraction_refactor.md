# Vendor Abstraction Refactor — Coordination Document

| Field | Value |
|-------|-------|
| **Created** | 2026-02-28 |
| **Authors** | David Fattal, Contributors |
| **Status** | Phase 1 Complete, Phases 2–4 In Progress |
| **Updated** | 2026-03-01 |
| **Tracks** | Vendor `#ifdef` removal, event system, multiview, rendering mode terminology, compositor architecture |
| **Related** | `XR_EXT_tracked_3d_display_proposal.md`, `vendor_integration_guide.md` |

---

## 1. Problem Statement

The vendor integration guide promises that a new 3D display vendor can integrate
by adding files **only** under `src/xrt/drivers/<vendor>/` and
`src/xrt/targets/common/`.  Zero changes to compositor or state tracker code.

**Reality**: approximately 60+ `#ifdef XRT_HAVE_LEIA_*` blocks exist in core
runtime files:

| File | Approx. `#ifdef` blocks | What they do |
|------|-------------------------|--------------|
| `comp_renderer.c` | ~20 | SR weaver creation, eye queries, display processor factory |
| `comp_multi_compositor.c` | ~8 | SR/CNSDK eye positions, display processor factory |
| `comp_d3d11_compositor.cpp` | ~15 | SR D3D11 weaver, eye queries, window resize |
| `oxr_session.c` | ~6 | D3D11 native compositor routing, eye position dispatch |
| `target_instance.c` | 2 | Field population (acceptable — builder-level code) |

A new vendor (Looking Glass, Dimenco, etc.) would need to add their own
`#ifdef` blocks to these same files.  This document tracks the refactoring
needed to eliminate that requirement and discusses three open design questions.

> **Update 2026-03-01**: Phase 1 is now complete.  The compositor and state
> tracker are clean of vendor `#ifdef` blocks.  See §6 task list and §9
> discussion log for details.

---

## 2. Three Abstractions to Genericize

### 2.1 Eye Position Query

**Current**: compositor files call `leiasr_get_predicted_eye_positions()` behind
`#ifdef XRT_HAVE_LEIA_SR_VULKAN`.

**Target**: a generic function pointer on `xrt_system_compositor` or `xrt_device`
that any vendor populates at init.  The compositor calls
`xsysc->get_predicted_eye_positions(xsysc, &eye_pair)` — no vendor `#ifdef`.

**Design sketch**:
```c
// In xrt_compositor.h or xrt_device.h:
struct xrt_eye_tracking_provider
{
    bool (*get_predicted_eyes)(struct xrt_eye_tracking_provider *provider,
                               int64_t at_timestamp_ns,
                               struct xrt_eye_pair *out_eyes);
    void (*destroy)(struct xrt_eye_tracking_provider *provider);
};
```

The vendor driver creates the provider and attaches it to the system compositor
info at device init.  The runtime queries eyes through this vtable.

### 2.2 Display Processor Factory

**Current**: compositor code has `#ifdef` blocks to call
`leia_display_processor_create()` or `sim_display_processor_create()`.

**Target**: the device driver (which already knows its vendor) provides a factory
function.  The compositor calls the factory generically.

**Design sketch**:
```c
// In xrt_device.h or xrt_display_processor.h:
typedef xrt_result_t (*xrt_display_processor_create_fn)(
    struct xrt_device *xdev,
    /* Vulkan device/instance or D3D11 device */
    void *graphics_context,
    struct xrt_display_processor **out_xdp);
```

The device populates this at creation.  The compositor calls it when it needs a
display processor for a new session — no vendor `#ifdef`.

### 2.3 Display Mode Routing

**Current**: `oxr_session.c` has `#ifdef` blocks to route
`request_display_mode` to the D3D11 native compositor or Vulkan compositor.

**Target**: a generic `request_display_mode(bool enable_3d)` function pointer on
the compositor interface, so the session code calls one path regardless of
backend.

---

## 3. Open Design Question: Display Mode vs Rendering Mode Terminology

### The Ambiguity

The checklist has two distinct concepts that share the word "mode":

1. **Display mode** (2D / 3D) — controls whether the display's optics/backlight
   are in stereoscopic mode or flat panel mode.  This is a **physical hardware
   state** that the app understands and reacts to (e.g., switching from stereo
   rendering to full-res mono rendering).

2. **Vendor rendering mode** — device-specific rendering variations *within* 3D
   mode (e.g., calibration pattern, tracking visualization overlay, debug
   interlacing, multi-view quality presets).  The app does **not** know what
   these modes are — they are opaque, vendor-defined.

### Current State

- Display mode (2D/3D): fully standardized via `xrRequestDisplayModeEXT`.
- Vendor rendering mode: not in the extension spec.  Currently handled through
  informal `setProperty` calls.

### Proposal: `xrSetDisplayPropertyEXT`

Add a generic vendor-passthrough function to the extension:

```c
XrResult xrSetDisplayPropertyEXT(
    XrSession           session,
    uint32_t            propertyId,    // Vendor-defined property identifier
    uint32_t            value);        // Property value
```

**Semantics**:
- `propertyId` is an opaque vendor-defined integer.  The extension defines no
  specific property IDs — vendors document their own.
- Convention: `propertyId = 0` with `value = 0` **must** reset all properties to
  vendor defaults (safe reset).
- The runtime forwards the call to the active vendor driver's handler.
- Returns `XR_ERROR_FEATURE_UNSUPPORTED` if the driver does not implement
  property handling.

**Pros**:
- Clean separation: the extension defines the *mechanism*, vendors define the
  *properties*.  No vendor-specific enums in the OpenXR header.
- Future-proof: new properties require no spec revision.
- Safe: `propertyId=0, value=0` always resets to known state.
- Simple for apps: single function, one line of code per property.
- Discoverable: vendor documentation lists their property IDs and values.

**Cons**:
- No compile-time type safety — `propertyId` and `value` are raw integers.
- No enumeration: app cannot discover which properties exist at runtime
  (would need a separate `xrEnumerateDisplayPropertiesEXT` for that).
- Risk of fragmentation: different vendors may use conflicting property ID
  ranges unless we define a convention (e.g., vendor ID in high 16 bits).

**Alternative A: Indexed rendering mode (simpler)**:

```c
XrResult xrRequestDisplayRenderingModeEXT(
    XrSession   session,
    uint32_t    modeIndex);    // 0 = standard, 1+ = vendor-defined
```

Simpler and more focused.  Mode 0 is always "standard 3D rendering" (or
standard 2D if display is in 2D mode).  Higher indices are vendor-defined.
No key-value complexity.

**Pros**: simpler API, clear default (0), no property ID collision risk.
**Cons**: less expressive — cannot set independent properties (e.g., set
anti-crosstalk level AND debug overlay independently).

**Alternative B: Fully opaque blob**:

```c
XrResult xrSetDisplayVendorDataEXT(
    XrSession       session,
    uint32_t        dataSize,
    const void*     data);
```

Maximum flexibility but no structure at all.  Hard to document, impossible to
validate.  Not recommended.

### Recommendation

**Start with Alternative A** (`xrRequestDisplayRenderingModeEXT` with indexed
modes) for v1.  It covers the stated use cases (calibration, debug overlay,
quality presets) with minimal API surface.  If vendors later need independent
property control, upgrade to `xrSetDisplayPropertyEXT` in v2.

### Terminology Summary

| Concept | API | Scope |
|---------|-----|-------|
| **Display mode** | `xrRequestDisplayModeEXT(session, XR_DISPLAY_MODE_2D/3D)` | Standard: physical optics state |
| **Rendering mode** | `xrRequestDisplayRenderingModeEXT(session, modeIndex)` | Vendor-specific: 0=standard, 1+=vendor |
| **Eye tracking mode** | `xrRequestEyeTrackingModeEXT(session, SMOOTH/RAW)` | Standard: tracking filter selection |

All three are independent axes.  An app can be in 3D display mode, smooth
tracking, rendering mode 0 (standard) — then switch to rendering mode 2
(debug calibration) without changing the other two.

---

## 4. Open Design Question: Polling vs Events

### The Problem

Two runtime state changes can occur asynchronously:
1. **Display mode change** — another app or system policy forces 2D/3D switch
2. **Tracking state change** — user walks away, camera occlusion, tracking loss

Currently both are poll-based: the app checks `isTracking` every frame via
`XrViewEyeTrackingStateEXT`, and tracks its own display mode requests.

### Option A: Polling Only (Current)

The app queries state every frame:
- `isTracking`: via `XrViewEyeTrackingStateEXT` chained to `xrLocateViews`
- Display mode: app tracks its own calls to `xrRequestDisplayModeEXT`

**Pros**: simple implementation, no event infrastructure needed.
**Cons**: app cannot detect externally-forced mode changes; constant overhead
for `isTracking` chaining even when tracking never changes.

### Option B: Events via `xrPollEvent` (Recommended)

Define two new event types delivered through the standard `xrPollEvent` queue:

```c
typedef struct XrEventDataDisplayModeChangedEXT {
    XrStructureType       type;       // XR_TYPE_EVENT_DATA_DISPLAY_MODE_CHANGED_EXT
    const void*           next;
    XrSession             session;
    XrDisplayModeEXT      currentMode;
} XrEventDataDisplayModeChangedEXT;

typedef struct XrEventDataEyeTrackingStateChangedEXT {
    XrStructureType       type;       // XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_EXT
    const void*           next;
    XrSession             session;
    XrBool32              isTracking;
} XrEventDataEyeTrackingStateChangedEXT;
```

**Precedent**: this follows the exact same pattern as existing OpenXR events:
- `XrEventDataUserPresenceChangedEXT` — fires when user is detected/lost
- `XrEventDataDisplayRefreshRateChangedFB` — fires when refresh rate changes
- `XrEventDataSessionStateChanged` — fires on session state transitions

The implementation pattern in Monado is straightforward (`oxr_event.c`):
```c
oxr_event_push_XrEventDataDisplayModeChangedEXT(log, sess, mode);
// Allocates event struct, fills fields, pushes to queue — ~15 lines
```

**Pros**:
- App reacts only when state actually changes (no per-frame polling overhead).
- Handles externally-forced mode changes (system policy, other apps).
- Follows established OpenXR event patterns — familiar to developers.
- Backward compatible: apps that don't poll events are unaffected.

**Cons**:
- Events are one-shot: app must handle the event or miss the transition.
  (Mitigation: app can still poll `isTracking` via `XrViewEyeTrackingStateEXT`
  as a fallback.)
- Adds implementation work: event structs, push functions, queue integration.

### Option C: Hybrid — Events + Polling

Provide **both** events and per-frame queryable state:
- Events fire on state transitions (for apps that want efficient notification).
- Polling via `XrViewEyeTrackingStateEXT.isTracking` still works every frame
  (for apps that prefer simplicity or need the value for rendering decisions).
- A new `xrGetCurrentDisplayModeEXT(session, &mode)` query for display mode.

This is the most robust but adds the most API surface.

### Recommendation

**Option C (Hybrid)** for maximum flexibility:
- Add events for display mode and tracking state changes.
- Keep the existing polling paths (they are already implemented).
- Add `xrGetCurrentDisplayModeEXT` query since the app currently has no way to
  read the actual mode (only what it requested).

Apps can choose their preferred pattern.  The event system is ~30 lines of
implementation per event type (following the `oxr_event.c` pattern), so cost is
low.

---

## 5. Open Design Question: Multiview from Day One

### Current Stereo Layout

The runtime currently operates with `PRIMARY_STEREO` (2 views):
- **Recommended dimensions**: per-eye, from `xrt_system_compositor_info.views[]`
- **Swapchain sizing**: app creates swapchains at `recommendedImageRectWidth x
  recommendedImageRectHeight` (per eye)
- **D3D11 internal SBS**: compositor allocates `2W x H` texture, left at x=0,
  right at x=W
- **Same-swapchain support**: Vulkan compositor detects when both views reference
  the same swapchain and does crop-blit to extract per-eye sub-rects

### Multiview Extension

A new `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTI_VIEW_EXT` would report N views.
The question: what swapchain layout to use?

### Swapchain Sizing for N Views

**Option A: One swapchain per view** (simple, current pattern extended)

Each view gets its own swapchain at per-view recommended dimensions.  The
compositor collects N separate textures and passes them all to the display
processor.

```
View 0: swapchain[0] at viewW x viewH
View 1: swapchain[1] at viewW x viewH
...
View N-1: swapchain[N-1] at viewW x viewH
```

**Pros**: simple for app; no tiling math; each view is independent.
**Cons**: N swapchain acquires/releases per frame; display processor needs to
accept N separate inputs (not ideal for GPU-side interlacing which wants a
single texture).

**Option B: Single atlas swapchain, row-major tiling** (recommended)

All N views packed into a single swapchain, row-major (left-to-right,
top-to-bottom).  The app renders each view into its `imageRect` tile.

For N views at `viewW x viewH` each:
```
cols = ceil(sqrt(N))        // e.g., N=4 → 2x2, N=8 → 3x3, N=2 → 2x1
rows = ceil(N / cols)
swapchainWidth  = cols * viewW
swapchainHeight = rows * viewH
```

Tile assignment:
```
View i:
  col = i % cols
  row = i / cols
  imageRect.offset = { col * viewW, row * viewH }
  imageRect.extent = { viewW, viewH }
```

For the common cases:
```
N=2 (stereo):   2x1 grid → [L|R]               (2W x H)  — matches current SBS
N=4:            2x2 grid → [0|1] / [2|3]        (2W x 2H)
N=8:            3x3 grid → [0|1|2] / [3|4|5] / [6|7|_]  (3W x 3H, 1 unused)
```

**Pros**: single swapchain acquire/release; GPU-friendly (single texture blit
to display processor); matches how light field display SDKs expect input.
**Cons**: slightly more complex for app (must compute per-view imageRect);
potential wasted space in non-square grid (e.g., N=8 uses 9-slot grid).

**Option C: Single atlas, column-major (vertical strip)**

```
swapchainWidth  = viewW
swapchainHeight = N * viewH
View i: offset = { 0, i * viewH }
```

Simple but produces very tall, narrow textures (4:1 or 8:1 aspect ratio).
Unfriendly to GPU texture caches.  Not recommended.

### Recommended Layout

**Option B (row-major atlas)** with the following convention:

- For `N=2`: 2x1 grid (SBS).  This is backward-compatible with the current
  stereo same-swapchain layout — the existing crop-blit path already handles it.
- For `N>2`: `ceil(sqrt(N))` columns, `ceil(N/cols)` rows.
- Views ordered left-to-right, top-to-bottom (row-major, view 0 at top-left).
- The runtime reports per-view `imageRect` via `xrEnumerateViewConfigurationViews`
  or a new query so apps don't need to compute tiling themselves.

### Display Processor Interface Change

The Vulkan `xrt_display_processor` currently takes two `VkImageView` arguments
(left + right).  For multiview, it needs to accept N views:

```c
struct xrt_display_processor
{
    void (*process_views)(struct xrt_display_processor *xdp,
                          VkCommandBuffer cmd_buffer,
                          uint32_t view_count,
                          const VkImageView *views,      // Array of N views
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

Or alternatively, pass a single atlas texture + per-view rects:

```c
void (*process_atlas)(struct xrt_display_processor *xdp,
                      VkCommandBuffer cmd_buffer,
                      VkImageView atlas_view,
                      uint32_t atlas_width,
                      uint32_t atlas_height,
                      uint32_t view_count,
                      const struct xrt_rect *view_rects,   // Per-view sub-rects
                      VkFramebuffer target_fb, ...);
```

The atlas variant is simpler for GPU interlacing (single texture read) and
matches the single-swapchain layout naturally.

### Implementation Scope

| Task | Estimated Effort |
|------|-----------------|
| New `XrViewConfigurationType` enum + registration | Small |
| `xrEnumerateViewConfigurationViews` returns N views with per-view rects | Small |
| `xrLocateViews` returns N poses (already supports arbitrary count) | Small |
| `xrEndFrame` validates viewCount = N | Small |
| Compositor: pass atlas or N views to display processor | Medium |
| Display processor interface: update signature for N views | Medium |
| sim_display: N-view shader (generalize SBS to NxM grid) | Medium |
| **Total** | **Medium** — no architectural changes, mostly parameter generalization |

### Recommendation

**Implement multiview from day one** with `N=2` as the default.  The work is
incremental — most changes are parameter widening (2 → N) in existing code.
The display processor interface change is the largest item but is
straightforward.  Having multiview ready makes the extension more compelling to
vendors with 4+ view displays and avoids a breaking interface change later.

Use the **atlas layout** (Option B) as the canonical convention.  Document the
tiling formula in the extension spec so apps know where to render each view.

---

## 6. Refactoring Task List

### Phase 1: Genericize (removes vendor `#ifdef` leakage) — COMPLETE

- [x] **1.1** Define eye tracking provider vtable interface
  - *Implemented as `get_predicted_eye_positions()` on `xrt_display_processor`
    and `xrt_display_processor_d3d11` rather than a separate provider struct.
    Cleaner design: eye tracking is inherently a display vendor function.*
- [x] **1.2** Vendor drivers create + attach eye tracking provider at init
  - *Vendors implement `get_predicted_eye_positions()` on their display
    processor.  Factories registered via `xsysc->info.dp_factory_vk` and
    `dp_factory_d3d11` in `target_instance.c`.*
- [x] **1.3** Compositor calls generic provider — remove `#ifdef` eye query blocks
  - *`comp_multi_compositor.c` and `comp_d3d11_compositor.cpp` call
    `xrt_display_processor[_d3d11]_get_predicted_eye_positions()` generically.
    Zero vendor `#ifdef` blocks in compositor.*
- [x] **1.4** Define display processor factory on system compositor info
  - *Factory function pointers `xrt_dp_factory_vk_fn_t` and
    `xrt_dp_factory_d3d11_fn_t` defined in `xrt_display_processor.h` /
    `xrt_display_processor_d3d11.h`, stored on `xrt_system_compositor_info`.*
- [x] **1.5** Compositor calls generic factory — remove `#ifdef` creation blocks
  - *Compositor calls factory from `xsysc->info` with no vendor `#ifdef`.*
- [x] **1.6** Define generic `request_display_mode` on compositor interface
  - *`request_display_mode(bool enable_3d)` on both `xrt_display_processor`
    and `xrt_display_processor_d3d11`, with NULL-safe inline helpers.*
- [x] **1.7** `oxr_session.c` calls generic path — remove `#ifdef` routing blocks
  - *`oxr_session_request_display_mode()` routes to D3D11 native or multi
    compositor generically.  Zero vendor `#ifdef` blocks in state tracker.*
- [x] **1.8** Verify: new vendor needs ZERO changes to compositor/state tracker
  - *Verified.  Compositor and state tracker directories have zero
    `#ifdef XRT_HAVE_LEIA` blocks.  Remaining vendor conditionals are in
    expected locations only (see §6.1 below).*

### Phase 1.1: Remaining Vendor Conditionals (acceptable locations)

These `#ifdef` blocks are **expected** and do not violate the vendor abstraction:

| File | Blocks | Purpose | Notes |
|------|--------|---------|-------|
| `target_instance.c` | 3 | Builder: populate display info, register factories | Builder-level code; each vendor adds their own block here |
| `target_builder_interface.h` | 1 | Conditional `T_BUILDER_LEIA` macro | Standard builder registration pattern |
| `ipc_server_handler.c` | 4 | Kooima FOV + SR eye poses in IPC service path | **Needs genericization** — should use display processor vtable |
| `comp_renderer.c` | 3 | Legacy `XRT_HAVE_CNSDK` interlacing path | **Needs removal** — superseded by generic display processor path |
| `leia_sr_probe.cpp` | 2 | Driver-internal SR hardware probe | Vendor driver code; expected |

**Remaining cleanup** (not blocking new vendors, but improves code hygiene):
- [ ] **1.9** Genericize IPC server view pose computation (move Kooima FOV to display processor vtable)
- [ ] **1.10** Remove legacy `XRT_HAVE_CNSDK` path from `comp_renderer.c` (CNSDK superseded by display processor)

### Phase 2: Events — NOT STARTED

- [ ] **2.1** Define `XrEventDataDisplayModeChangedEXT` struct in extension header
- [ ] **2.2** Define `XrEventDataEyeTrackingStateChangedEXT` struct
- [ ] **2.3** Implement push functions in `oxr_event.c`
- [ ] **2.4** Runtime fires display mode event when mode changes
- [ ] **2.5** Runtime fires tracking event on `is_tracking` transitions
- [ ] **2.6** Add `xrGetCurrentDisplayModeEXT` query function

### Phase 3: Rendering Mode — NOT STARTED (design only)

- [ ] **3.1** Define `xrRequestDisplayRenderingModeEXT` function
- [ ] **3.2** Add rendering mode handler to vendor driver interface
- [ ] **3.3** Implement in Leia driver (map to existing setProperty)
- [ ] **3.4** Implement no-op in sim_display

### Phase 4: Multiview — IN PROGRESS (~60%)

- [ ] **4.1** Define `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTI_VIEW_EXT`
  - *Blocked by 4.0.  Enum not yet registered in extension header.*
- [ ] **4.2** Update `xrEnumerateViewConfigurationViews` for N views
  - *Currently limited to `PRIMARY_MONO` and `PRIMARY_STEREO`.*
- [x] **4.3** Update display processor interface for N views / atlas
  - *Done: `process_views()` takes `view_count` + `views[]` array.*
- [x] **4.4** Update compositor crop-blit for NxM atlas layout
  - *Done: compositor paths (Vulkan, D3D11, multi) handle N views.*
- [ ] **4.5** Update `xrEndFrame` validation for viewCount = N
- [x] **4.6** Implement N-view sim_display shader
  - *Done: `SIM_DISPLAY_VIEWS` env var for hardware-free multiview testing.*
- [ ] **4.7** Document tiling convention in extension spec

**Prerequisite**:
- [ ] **4.0** Raise `XRT_MAX_VIEWS` from 2 to 64 in `xrt_limits.h`
  - *Currently hardcoded to 2.  All N-view infrastructure is ready but the
    limit prevents registration of the multiview view configuration type.*

---

## 7. Deployment Model: How Vendors Integrate

### The OEM Runtime + Public SDK Pattern

3D display vendors follow the same deployment model as GPU compute platforms
(CUDA, OpenCL, DirectX): the vendor's IP lives in a system-level runtime
installed by the device OEM, and a freely available public SDK provides only
headers and import stubs for building against it.

```
┌────────────────────────────────────────────────────────────────┐
│  DEVICE (OEM-installed by Samsung, Acer, ZTE, etc.)           │
│                                                                │
│  Vendor Runtime (e.g., "LeiaSR Runtime")                      │
│  Contains: real DLLs / .so with all IP                        │
│  - Interlacing / weaving algorithms                           │
│  - Eye tracking models + calibration data                     │
│  - Display-specific profiles + hardware parameters            │
│  - Switchable lens / backlight control                        │
│                                                                │
│  Installed at: system path (Program Files, /usr/lib, etc.)    │
│  Lifecycle: updated by OEM or vendor installer                │
└──────────────────────────────┬─────────────────────────────────┘
                               │ dynamic link at runtime
                               │ (LoadLibrary / dlopen)
┌──────────────────────────────┴─────────────────────────────────┐
│  PUBLIC SDK (freely downloadable, no IP)                      │
│                                                                │
│  Contains:                                                     │
│  - C/C++ headers declaring the API                            │
│  - Import libraries / stubs (.lib / .so symlinks)             │
│  - Documentation + samples                                     │
│                                                                │
│  Does NOT contain: algorithms, calibration, weights, shaders  │
│  If vendor runtime not installed → SDK calls fail gracefully  │
│                                                                │
│  Distribution: public download, NuGet, GitHub release, etc.   │
└──────────────────────────────┬─────────────────────────────────┘
                               │ linked at build time
                               │ (find_package / CMake)
┌──────────────────────────────┴─────────────────────────────────┐
│  OPENXR RUNTIME (this repository)                             │
│                                                                │
│  src/xrt/drivers/<vendor>/                                    │
│  - Open-source glue code (device, display processor, builder) │
│  - Translates vendor SDK types → xrt_* vendor-neutral types   │
│  - No vendor IP — just API calls through the public SDK       │
│                                                                │
│  Build: CI downloads each vendor's public SDK and links       │
│  Runtime: vendor's system DLLs must be present on device      │
└────────────────────────────────────────────────────────────────┘
```

### Industry Precedents

| Platform | System Runtime (OEM) | Public SDK (developer) | App links against |
|----------|---------------------|----------------------|-------------------|
| CUDA | `nvcuda.dll` (GPU driver) | CUDA Toolkit (headers + stubs) | Import libs |
| OpenCL | Vendor ICD (GPU driver) | Khronos headers + ICD loader | Loader |
| DirectX | `d3d11.dll` (Windows) | Windows SDK | Import libs |
| **LeiaSR** | **LeiaSR Runtime (OEM)** | **SR SDK (public download)** | **Import libs** |
| **Future vendor** | **Vendor Runtime (OEM)** | **Vendor SDK (public)** | **Import libs** |

### Version Compatibility Contract

Vendor runtimes and SDKs follow a **forward-compatible runtime, backward-
compatible SDK** rule — the same contract as CUDA, DirectX, and OpenCL:

| Scenario | Works? | Why |
|----------|--------|-----|
| **New runtime + old SDK** | Yes | Runtime exports a superset of all previous API versions. Old apps call old functions — they're still there. |
| **Old runtime + new SDK** | No | New SDK may call functions that the old runtime doesn't implement. |

```
SDK v3 ──── can call: foo_v1(), foo_v2(), foo_v3()  ── requires Runtime v3+
SDK v2 ──── can call: foo_v1(), foo_v2()            ── requires Runtime v2+
SDK v1 ──── can call: foo_v1()                      ── requires Runtime v1+

Runtime v3  implements: foo_v1(), foo_v2(), foo_v3()  ← all old apps work
Runtime v2  implements: foo_v1(), foo_v2()
Runtime v1  implements: foo_v1()
```

**Rule**: The runtime is the floor (max capability on device).  The SDK is
the ceiling (max capability the app can request).  A new runtime never
breaks old apps.  A new SDK may require a new runtime.

**Implication for the OpenXR driver**: The driver glue code in
`src/xrt/drivers/<vendor>/` should be built against the **latest** vendor
SDK (to access all features), but must handle the case where the vendor's
system runtime on the user's device is older.  Concretely:

- **Check API availability at runtime** — don't assume all SDK functions
  will succeed.  Use the vendor SDK's version query or capability check.
- **Degrade gracefully** — if a new feature (e.g., raw eye tracking mode)
  isn't available in the installed runtime, report the capability as
  unsupported (`supported_eye_tracking_modes = SMOOTH_BIT` only, not
  `SMOOTH_BIT | RAW_BIT`) rather than crashing.
- **Never require a minimum runtime version to start** — the driver should
  activate with whatever runtime is installed and expose the intersection
  of what the SDK declares and what the runtime actually provides.

This means the `estimate_system()` function in the target builder and
the SDK wrapper's `create()` function should probe the installed runtime's
capabilities and populate `xrt_system_compositor_info` fields accordingly.
The OpenXR app then sees only what the device actually supports.

### Implications for Multi-Vendor Builds

This model solves the multi-vendor build problem cleanly:

1. **CI downloads all public SDKs** — no license issues since SDKs contain
   no IP (just headers + stubs).  Each vendor's SDK is fetched in the CI
   workflow (see `.github/workflows/build-windows.yml` where `LEIASR_SDKROOT`
   is set after downloading the SR SDK).

2. **Single runtime binary supports all vendors** — the built runtime links
   against all vendor import libs.  At runtime, the builder system probes
   which vendor DLLs are actually installed on the device and activates the
   appropriate driver.

3. **Graceful degradation** — if Vendor A's runtime is not installed:
   - `find_package()` at build time: SDK stubs link fine.
   - At runtime: vendor SDK calls fail, `estimate_system()` returns "no
     hardware," that builder is skipped, next builder (or sim_display) wins.
   - The user gets a working OpenXR runtime without that vendor's 3D display.

4. **No vendor IP in this repo** — the driver code under
   `src/xrt/drivers/<vendor>/` is pure glue: it calls the vendor's public API
   and converts results to `xrt_*` types.  The algorithms, calibration data,
   and proprietary shaders live in the vendor's system runtime.

5. **Vendors can iterate independently** — a vendor can update their system
   runtime (new interlacing algorithm, improved tracking) without rebuilding
   the OpenXR runtime.  The public SDK's API contract is the stable boundary.

### Adding a New Vendor

```
Step 1: Vendor creates public SDK
        └── Headers + import libs, publicly downloadable
        └── System runtime installer for OEM devices

Step 2: Vendor (or contributor) writes driver glue
        └── src/xrt/drivers/<vendor>/  (open source)
        └── Uses xrt_* interfaces only at the boundary
        └── Links against vendor's public SDK import libs

Step 3: CI integration
        └── .github/workflows/ downloads vendor's public SDK
        └── Sets CMAKE_PREFIX_PATH / VENDOR_SDKROOT env var

Step 4: Builder registration (5-file checklist)
        └── See vendor_integration_guide.md §8.1

Result: Runtime binary ships with vendor support compiled in.
        On devices with the vendor's runtime → full 3D display.
        On devices without → graceful fallback.
```

### Relationship to Phase 1 Refactor

The Phase 1 genericization (§6) ensures that Step 2 requires **zero changes**
to compositor or state tracker code.  The vendor writes only:
- Device driver (display specs, head pose)
- Display processor (interlacing via vendor SDK)
- Eye tracking provider (eye positions via vendor SDK)
- SDK wrapper (opaque C struct)
- Target builder (registration)

All of these are under `src/xrt/drivers/<vendor>/` and
`src/xrt/targets/common/`.  The compositor interacts with the vendor's code
exclusively through generic vtable interfaces.

---

## 8. Compositor Architecture for 3D Displays

This section documents how the runtime's compositor stack differs from
traditional HMD runtimes and why.  Understanding these paths is essential for
vendors choosing how to integrate and for developers reasoning about multi-app
performance.

### 8.1 Three Independent Dimensions

Every session configuration is described by three orthogonal choices:

| Dimension | Options | Key files |
|-----------|---------|-----------|
| **Process mode** | In-process, Service (IPC), Hybrid | `targets/openxr/target.c`, `targets/service/main.c`, `ipc/client/` |
| **Graphics backend** | Vulkan (`comp_main`), Null/headless, D3D11 native, D3D11 service | `compositor/main/`, `compositor/null/`, `compositor/d3d11/`, `compositor/d3d11_service/` |
| **Window ownership** | App-provided (via `XR_EXT_win32_window_binding`), Runtime-created | `oxr_session.c`, `comp_multi_compositor.c`, `comp_d3d11_window.cpp` |

These compose freely.  For example: "in-process, D3D11 native, app-provided
window" or "service mode, null compositor, app-provided window."

### 8.2 The Two Compositor Paths

#### D3D11 Native Compositor (independent, parallel)

When a D3D11 app creates a session, `oxr_session.c` calls
`comp_d3d11_compositor_create()` which produces a **standalone** compositor
instance.  Each session gets its own:

- `ID3D11Device` (the app's device, passed in at session creation)
- `IDXGISwapChain` (bound to that session's HWND)
- SR D3D11 weaver (its own instance)
- D3D11 renderer

The compositor is set directly on the session (`sess->xcn = xcn`).  **No multi
compositor, no null compositor, no shared render thread.**  The render loop is
the app's own `xrEndFrame` call — rendering and presentation happen on the
calling thread.

```
App A (D3D11 + HWND_A)              App B (D3D11 + HWND_B)
  xrEndFrame()                        xrEndFrame()
    |                                   |
  comp_d3d11_compositor A             comp_d3d11_compositor B
    own D3D11 device                    own D3D11 device
    own DXGI swapchain                  own DXGI swapchain
    own SR D3D11 weaver                 own SR D3D11 weaver
    Present()                           Present()
```

No contention, no shared state, true GPU parallelism via the OS/driver
scheduler.

**Key files**: `compositor/d3d11/comp_d3d11_compositor.cpp`,
`state_trackers/oxr/oxr_session_gfx_d3d11_native.c`

#### Vulkan Multi Compositor (serialized, coordinated)

Non-D3D11 apps (OpenGL, Vulkan) go through the multi compositor.  A single
`multi_system_compositor` owns one native backend (typically `null_compositor`
for headless Vulkan) and creates a `multi_compositor` instance per session.

All sessions are driven by a **single render thread** (`multi_main_loop` in
`comp_multi_system.c`).  Each frame tick:

1. **Shared clients** (no external window): layers collected by
   `transfer_layers_locked()` and submitted to the native compositor.
2. **Per-session clients** (with HWND): `render_per_session_clients_locked()`
   iterates all per-session clients **sequentially**, rendering each to its own
   `VkSwapchainKHR` (created from the session's HWND via `comp_target_service`).

Each per-session client has its own Vulkan resources (`VkCommandPool`,
`VkCommandBuffer[]`, `VkFence[]`, display processor, SR weaver), but they all
share the same `VkDevice` and are driven by the same thread.

```
multi_main_loop (single thread)
  |
  +-- transfer_layers_locked()           <-- shared clients (no HWND)
  |     submit to native compositor
  |
  +-- render_per_session_clients_locked() <-- per-session clients (with HWND)
        for each session (sequentially):
          extract views
          Vulkan blit/composite
          display processor / weaver
          VkSwapchain present
```

**Key files**: `compositor/multi/comp_multi_system.c`,
`compositor/multi/comp_multi_compositor.c`, `compositor/multi/comp_multi_private.h`

### 8.3 Multi-App Performance Comparison

For the windowed 3D display use case (multiple apps, each with its own window):

| Aspect | D3D11 native | Vulkan multi (per-session) |
|--------|-------------|--------------------------|
| **Parallelism** | True — each app independent | Serialized — single render thread |
| **Interop cost** | Zero — D3D11 end-to-end | GL apps pay GL-to-Vulkan import |
| **Frame pacing** | Independent per app | All sessions share one `predicted_display_time_ns` |
| **GPU resources** | Each session: own device + weaver | Shared `VkDevice`, own per-session resources |
| **Bottleneck** | GPU bandwidth (natural) | Single render thread (artificial) |
| **Latency** | `xrEndFrame` to present: one hop | App thread to multi thread to present: two hops |

**D3D11 native is the better architecture for multi-app windowed scenarios.**

### 8.4 Why Monado Developed the Multi Compositor (HMD Context)

The multi compositor was designed for HMDs, where the constraints are
fundamentally different from windowed 3D displays:

| HMD constraint | Why it requires centralized compositing |
|---------------|---------------------------------------|
| **One display, no windows** | The HMD is a single screen — no window manager. Someone must own that display surface and merge all layers onto it. |
| **Distortion correction** | HMD lenses require barrel distortion as a final pass on the **combined** image. Cannot be done per-app. |
| **Overlay compositing** | System UI, notifications, guardian boundaries are separate sessions whose layers must be z-sorted and merged. |
| **Precise frame timing** | At 90 Hz (11 ms budget), a single compositor thread provides deterministic vsync submission and can apply reprojection on missed frames. |
| **Process isolation** | The compositor runs as a system service so app crashes don't black out the HMD display. |
| **Direct display mode** | The runtime takes exclusive GPU ownership of the HMD, bypassing the OS compositor entirely. |

For windowed 3D displays, none of these constraints apply: apps have their own
windows via the OS window manager, weaving is per-window (not a global final
pass), and the OS desktop compositor handles vsync.  The multi compositor's
coordination overhead becomes cost without benefit.

### 8.5 Should We Build a Vulkan Native Compositor?

**Question**: should we build a `comp_vulkan_native` (analogous to
`comp_d3d11_compositor`) for Vulkan/GL apps to bypass multi?

**Recommendation: No** — not as a new standalone module.  The multi per-session
path already has 90% of the required infrastructure (per-session `comp_target`,
`VkCommandPool`, fences, display processor).  The only real problem is that
it's driven by a single render thread.

**Preferred approach: parallelize the multi render loop.**

Instead of the current sequential iteration:
```c
// Current: sequential
for each per-session client:
    render_session_to_own_target(mc);   // one after another
```

Dispatch to a thread pool:
```c
// Proposed: parallel
for each per-session client:
    dispatch_to_thread_pool(render_session_to_own_target, mc);
wait_all();
```

Each per-session client already has its own Vulkan resources (command pool,
fences, target).  They share the `VkDevice` but Vulkan devices are thread-safe
for independent command pools.  The serialization is artificial — it's a `for`
loop, not a fundamental constraint.

**Rationale**:

1. **D3D11 native already covers the high-performance Windows case.**  Most
   Windows apps targeting 3D displays are D3D11.
2. **Vulkan apps don't pay interop tax.**  Unlike GL, a Vulkan app through
   multi uses Vulkan end-to-end.  The overhead is serialization and extra
   copies, not API translation.
3. **Minimal code change.**  A thread pool in `render_per_session_clients_locked()`
   is far less work than a new compositor module.
4. **Small target audience.**  Pure Vulkan windowed apps for 3D displays is a
   niche within a niche.

**Exception**: if macOS or Linux becomes a primary multi-app platform where
D3D11 doesn't exist, a standalone Vulkan native compositor could be revisited.

---

## 9. Discussion Log

*(Developers: add dated entries below as design decisions are made.)*

### 2026-02-28 — Initial draft

Created this document after combined Claude + Gemini review of the vendor
requirements checklist.  Key findings:

- Vendor `#ifdef` leakage is the highest-priority fix.
- Events are preferred over polling for mode/tracking changes.
- Multiview is feasible from day one with moderate effort.
- Rendering mode should use simple indexed API (mode 0 = standard).

Open for discussion: which phase to tackle first?  Phase 1 (genericize) is
prerequisite for all others and directly enables new vendors.

### 2026-02-28 — Deployment model clarified

Confirmed the vendor deployment model: OEM-installed system runtime (contains
all IP) + freely downloadable public SDK (headers + import stubs, no IP).
This follows the CUDA / OpenCL / DirectX pattern.  Added §7 documenting
this model.  Key insight: no plugin/DLL model needed at the OpenXR runtime
level — the vendor's own runtime is already the plugin, loaded dynamically by
the vendor's public SDK.  The OpenXR driver is just open-source glue code
calling the public SDK API.

### 2026-03-01 — Compositor architecture documented

Added §8 documenting the two compositor paths (D3D11 native vs Vulkan multi)
and their multi-app performance characteristics.  Key findings:

- **D3D11 native** (independent per-session compositors) is the better
  architecture for multi-app windowed 3D display scenarios: true parallelism,
  zero interop cost, independent frame pacing.
- **Vulkan multi** (single render thread, per-session rendering) exists because
  Monado was designed for HMDs where a centralized compositor is mandatory
  (single display, distortion correction, overlay merging, precise timing).
- For windowed 3D displays, the multi compositor's coordination overhead is
  cost without benefit — apps have their own windows and weaving is per-window.
- **Recommendation**: do NOT build a standalone Vulkan native compositor.
  Instead, parallelize the multi render loop (`render_per_session_clients_locked`)
  with a thread pool if GL/Vulkan multi-app performance becomes a bottleneck.
  The per-session resources are already independent; the serialization is an
  artificial `for` loop, not a fundamental constraint.

### 2026-03-01 — Phase 1 completion assessment

Audited the entire codebase for vendor `#ifdef` blocks.  **Phase 1 is complete.**

**Before** (Feb 28): ~60 `#ifdef XRT_HAVE_LEIA_*` blocks across `comp_renderer.c`,
`comp_multi_compositor.c`, `comp_d3d11_compositor.cpp`, and `oxr_session.c`.

**After** (Mar 1): Zero vendor `#ifdef` blocks in compositor or state tracker.
Remaining vendor conditionals (12 total across 5 files) are all in expected
locations: builder code (`target_instance.c`), builder interface header, and
vendor driver code (`leia_sr_probe.cpp`).

Key design decisions that evolved during implementation:
1. **Eye tracking embedded in display processor** — instead of a separate
   `xrt_eye_tracking_provider`, eye tracking is a method on `xrt_display_processor`.
   This is cleaner because eye tracking is inherently tied to display geometry.
2. **Factory on system compositor info** — display processor factories are stored
   on `xrt_system_compositor_info` (not `xrt_device` as originally proposed).
   This works better because the factory needs graphics context (VkDevice / ID3D11Device)
   which is compositor-level, not device-level.
3. **NULL-safe inline helpers** — all optional vtable methods have inline wrappers
   that return false/no-op when the function pointer is NULL, enabling graceful
   degradation.

**Remaining cleanup** (non-blocking):
- `ipc_server_handler.c`: 4 blocks of Kooima FOV computation behind
  `XRT_HAVE_LEIA_SR_D3D11`.  Should be genericized via display processor vtable
  (new task 1.9).
- `comp_renderer.c`: 3 blocks of legacy `XRT_HAVE_CNSDK` interlacing path.
  Fully superseded by the generic display processor path; should be removed
  (new task 1.10).

**Multiview progress**: display processor interface already generalized to N views
(`view_count` + `views[]` array), compositor paths updated, sim_display supports
`SIM_DISPLAY_VIEWS` env var.  Blocked only by `XRT_MAX_VIEWS=2` in `xrt_limits.h`.

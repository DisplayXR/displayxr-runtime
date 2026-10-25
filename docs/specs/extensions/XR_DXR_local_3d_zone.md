# XR_DXR_local_3d_zone

| Property | Value |
|----------|-------|
| Extension Name | `XR_DXR_local_3d_zone` |
| Spec Version | 5 |
| Type Values | `1004999160–1004999166` |
| Author | The DisplayXR Project |
| Platform | All (in-process native compositors for the mask object; Local2D layers also over IPC) |
| Requires | A session that presents into a window or surface the app owns (a window-binding extension, or a native compositor session) |

> **Status:** shipping. Implemented in `src/xrt/state_trackers/oxr/oxr_local_3d_zone.c`
> and the masked composite in every native compositor (D3D11, D3D12, Vulkan,
> Metal, OpenGL) and in the service compositor. Canonical header:
> `src/external/openxr_includes/openxr/XR_DXR_local_3d_zone.h`. This page
> describes what ships. The design history is in
> [`unified-2d-3d-compositing.md`](../../roadmap/unified-2d-3d-compositing.md)
> and [`local-3d-zones.md`](../../roadmap/local-3d-zones.md).
> [`XR_DXR_display_zones`](XR_DXR_display_zones.md) builds on this extension (§7).

---

## 1. Overview

A spatial display can show 3D in one part of an app's window and flat 2D in
the rest. Some panels can also switch regions of their lens between 2D and 3D.
`XR_DXR_local_3d_zone` lets the app say which part of its window is 3D, and
gives it a way to submit flat 2D content that is not woven.

It has two parts:

- **The mask.** A per-pixel scalar `M`, the "3D-ness" of each window pixel:
  `M = 1` is 3D (the weave shows), `M = 0` is 2D. Fractional values are
  allowed and blend continuously. The app authors it at one of three tiers
  (§3). The same mask drives the runtime's composite and, on panels with
  switchable lens zones, the display hardware, so the two always agree.
- **`XrCompositionLayerLocal2DDXR`.** A composition layer that places flat 2D
  content at a client-window pixel rect. It is composited at full resolution
  and never woven (§4).

An app can use either part alone. Submitting Local2D layers with no mask
object is the common case: the runtime derives the mask from the layers'
rects (§4.3).

## 2. Concepts

### 2.1 The mask is a separate channel, not the 2D layer's alpha

Region selection and content transparency are independent, so they have
separate channels. A translucent flat panel beside the 3D is `M = 0` with
its own alpha below 1: the weave contributes nothing there, and the panel's
transparency resolves against whatever is behind the window (the desktop),
not against interlaced 3D content. Folding the mask into the 2D alpha would
make every translucent 2D pixel blend with the weave and show its L/R
structure.

### 2.2 Coordinate space

The mask, the Local2D rects and the weave target all use **client-window
pixels**, after DPI scaling. That is the same space the runtime uses for the
window it presents into. Rects are `XrRect2Di` with the origin at the top
left of the client area.

### 2.3 Weave first, then composite 2D

Interlace phase is tied to absolute screen position. The runtime therefore
weaves the 3D region first and then composites 2D over it wherever `M < 1`.
The mask never changes the weave, so it cannot break phase, and a
non-rectangular or disconnected 3D region costs nothing extra. 2D content is
composited after the weave, so it keeps full resolution.

While a mask is active (an explicit submitted mask, or an implicit one from
Local2D layers), the weave region, view dimensions and projection span the
full client window, and the mask alone selects 2D vs 3D. Destroying the mask
and submitting no Local2D layers restores the default full-window 3D
behaviour on the next frame.

## 3. API Reference

### 3.1 Defines

```c
#define XR_DXR_local_3d_zone 1
#define XR_DXR_local_3d_zone_SPEC_VERSION 5
#define XR_DXR_LOCAL_3D_ZONE_EXTENSION_NAME "XR_DXR_local_3d_zone"

#define XR_TYPE_LOCAL_3D_ZONE_CAPABILITIES_DXR                  ((XrStructureType)1004999160)
#define XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_DXR              ((XrStructureType)1004999161)
#define XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D11_DXR           ((XrStructureType)1004999162)
#define XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D12_DXR           ((XrStructureType)1004999163)
#define XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_VULKAN_DXR          ((XrStructureType)1004999164)
#define XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR                  ((XrStructureType)1004999165)
#define XR_TYPE_EVENT_DATA_LOCAL_3D_ZONE_VIEW_SIZE_CHANGED_DXR  ((XrStructureType)1004999166)

XR_DEFINE_HANDLE(XrLocal3DZoneMaskDXR)
```

### 3.2 Capabilities

```c
typedef struct XrLocal3DZoneCapabilitiesDXR {
    XrStructureType    type;   // XR_TYPE_LOCAL_3D_ZONE_CAPABILITIES_DXR
    void* XR_MAY_ALIAS next;
    XrBool32           supported;
    uint32_t           hardwareZoneGridWidth;
    uint32_t           hardwareZoneGridHeight;
    uint32_t           maxMaskWidth;
    uint32_t           maxMaskHeight;
} XrLocal3DZoneCapabilitiesDXR;

XrResult xrGetLocal3DZoneCapabilitiesDXR(XrSession session,
                                         XrLocal3DZoneCapabilitiesDXR* capabilities);
```

- `supported` is `XR_TRUE` when the session can create and consume a mask
  object. That is true for a session on an in-process native compositor
  (D3D11, D3D12, Vulkan, Metal, OpenGL). It is `XR_FALSE` for any other
  session, including an IPC (service) session. The call itself never fails
  for an unsupported session.
- `hardwareZoneGridWidth/Height` describe the active display's switchable-lens
  zone grid: `1×1` is a panel that switches globally, larger is a panel with
  local zones, and `0×0` means the display reports no switchable zones (the
  mask still drives the composite). Today the D3D11 and Metal compositors
  query the display processor for it; the others report `0×0`.
- `maxMaskWidth/Height` bound the authored mask size (16384 in this runtime).

### 3.3 Creating and destroying a mask

```c
typedef struct XrLocal3DZoneMaskCreateInfoDXR {
    XrStructureType          type;   // XR_TYPE_LOCAL_3D_ZONE_MASK_CREATE_INFO_DXR
    const void* XR_MAY_ALIAS next;
    uint32_t                 maskWidth;   // 0 = runtime chooses
    uint32_t                 maskHeight;  // 0 = runtime chooses
} XrLocal3DZoneMaskCreateInfoDXR;

XrResult xrCreateLocal3DZoneMaskDXR(XrSession session,
                                    const XrLocal3DZoneMaskCreateInfoDXR* createInfo,
                                    XrLocal3DZoneMaskDXR* mask);
XrResult xrDestroyLocal3DZoneMaskDXR(XrLocal3DZoneMaskDXR mask);
```

The mask is bound to the session's window. `maskWidth/Height` is its
resolution in client-window pixels; 0 lets the runtime choose (clamped to
the client window). The compositor samples the mask at this resolution, so
2D/3D edges stay crisp. A hardware-zone display downsamples the same mask to
its grid.

Errors: `XR_ERROR_VALIDATION_FAILURE` when a dimension exceeds
`maxMaskWidth/Height`; `XR_ERROR_FEATURE_UNSUPPORTED` when `supported` is
`XR_FALSE` for the session.

### 3.4 The three authoring tiers

All three tiers write the same mask. The app picks whichever matches how its
layout is described, and may switch between them on the same mask.

**Tier 1, whole window.** One call, no per-frame work:

```c
XrResult xrSetLocal3DZoneWholeWindowDXR(XrLocal3DZoneMaskDXR mask, XrBool32 enable3D);
```

`XR_TRUE` sets `M = 1` everywhere (all 3D), `XR_FALSE` sets `M = 0` (all 2D).

**Tier 2, rect list.** The runtime rasterizes the rects:

```c
XrResult xrSetLocal3DZoneFromRectsDXR(XrLocal3DZoneMaskDXR mask,
                                      uint32_t rectCount, const XrRect2Di* rects);
```

`M = 1` inside the union of the rects, `M = 0` elsewhere. `rectCount` must be
at least 1. A single rect gives a 3D canvas inside a 2D window.

**Tier 3, freeform render target.** The app draws the mask itself, including
soft or animated edges:

```c
XrResult xrAcquireLocal3DZoneRenderTargetDXR(XrLocal3DZoneMaskDXR mask, void* binding);
```

`binding` points to one of the API-typed structs below, whose `type` the app
sets; the runtime fills the rest. The target is an `R8_UNORM` image in
client-window pixels, and the app writes `M` to its red channel.

```c
typedef struct XrLocal3DZoneRenderTargetD3D11DXR {
    XrStructureType    type;   // XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D11_DXR
    void* XR_MAY_ALIAS next;
    void*              renderTargetView; // ID3D11RenderTargetView*
    uint32_t           width;
    uint32_t           height;
} XrLocal3DZoneRenderTargetD3D11DXR;

typedef struct XrLocal3DZoneRenderTargetD3D12DXR {
    XrStructureType    type;   // XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_D3D12_DXR
    void* XR_MAY_ALIAS next;
    void*              resource; // ID3D12Resource*
    uint32_t           width;
    uint32_t           height;
} XrLocal3DZoneRenderTargetD3D12DXR;

typedef struct XrLocal3DZoneRenderTargetVulkanDXR {
    XrStructureType    type;   // XR_TYPE_LOCAL_3D_ZONE_RENDER_TARGET_VULKAN_DXR
    void* XR_MAY_ALIAS next;
    void*              image;     // VkImage
    void*              imageView; // VkImageView
    uint32_t           width;
    uint32_t           height;
} XrLocal3DZoneRenderTargetVulkanDXR;
```

- **D3D12:** the app creates its own render-target descriptor on the
  resource (descriptor heaps are app-owned). The resource is handed out in
  `D3D12_RESOURCE_STATE_RENDER_TARGET` and must be back in that state when the
  app calls `xrSubmitLocal3DZoneDXR`.
- **D3D12 and Vulkan sync:** the compositor shares the app's device and
  queue, so submission order on that queue is the contract: execute the mask
  draw, then call `xrSubmitLocal3DZoneDXR`. No fence.
- **Metal and OpenGL** have no Tier-3 binding. The call returns
  `XR_ERROR_FEATURE_UNSUPPORTED`; author with Tier 1 or 2. This includes a
  Vulkan app presented by the Metal compositor on macOS.
- A `binding` whose `type` does not match the session's graphics API returns
  `XR_ERROR_VALIDATION_FAILURE`.

### 3.5 Submitting

```c
XrResult xrSubmitLocal3DZoneDXR(XrLocal3DZoneMaskDXR mask);
```

Stages the mask's current contents for the next `xrEndFrame`. The mask, the
Local2D layers and the projection layers of that frame are consumed as one
set: the compositor never pairs one frame's mask with another frame's 2D
content. The submitted mask is **sticky**: it stays in force across frames
until the app submits again or destroys the mask (last submit wins). An app
that changes its layout updates the mask and the 2D content in the same
frame.

## 4. `XrCompositionLayerLocal2DDXR`

```c
typedef struct XrCompositionLayerLocal2DDXR {
    XrStructureType             type;       // XR_TYPE_COMPOSITION_LAYER_LOCAL_2D_DXR
    const void* XR_MAY_ALIAS    next;
    XrCompositionLayerFlags     layerFlags; // alpha bits honored
    XrSwapchainSubImage         subImage;   // source swapchain + sub-rect
    XrRect2Di                   rect;       // destination, client-window pixels
} XrCompositionLayerLocal2DDXR;
```

Submitted in `XrFrameEndInfo::layers` beside the projection layers. It places
`subImage` at `rect` and is composited at full resolution, never woven.

### 4.1 Validity

- The extension must be enabled on the instance, or the layer is rejected with
  `XR_ERROR_LAYER_INVALID`. Creating a mask object is not required.
- The session must present into a window or surface: a session on an
  in-process native compositor, an IPC (service) session, or a session with a
  window binding. Other sessions get `XR_ERROR_LAYER_INVALID`.
- The usual swapchain-layer rules apply (released swapchain, non-negative
  `imageRect`).

Local2D layers work over IPC: the service compositor composites them
server-side. Only the explicit mask object (§3) is in-process-only.

### 4.2 Flattening and order

Multiple Local2D layers flatten in layer-list order (later on top) with
premultiplied-alpha "over". `XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT`
is honored per layer. Outside its rect a layer contributes nothing.

Position in the layer list relative to the projection layer matters:

- **After the projection layer: overlay.** The flattened 2D is composited
  over the woven 3D (the rest of this section).
- **Before the projection layer: backdrop.** The flattened 2D is composited
  behind the 3D, over whatever is behind the window, so a semi-transparent
  backdrop still shows the desktop through it. The runtime hands the backdrop
  to the display processor as its 2D background. A display processor that does
  not take a 2D background shows no backdrop.

### 4.3 Implicit vs explicit mask

How a Local2D overlay combines with the 3D depends on whether the app
authored a mask:

- **Implicit (no submitted mask).** The union of the frame's Local2D rects is
  the 2D region (`M = 0` inside, `M = 1` elsewhere), and the 2D layer's own
  premultiplied alpha is the blend:

  ```
  final = twod + (1 − twod.a) · weave
  ```

  An opaque panel is a crisp flat panel. A translucent panel is glass over
  the 3D: the 3D scene shows through it. Uncovered pixels are the weave.

- **Explicit (a submitted mask).** The mask has total authority and the
  composite is a mask-lerp that keeps each side's own alpha:

  ```
  final.rgb = M · weave.rgb + (1 − M) · twod.rgb
  final.a   = M · weave.a   + (1 − M) · twod.a
  ```

  Where `M = 0`, a translucent 2D pixel resolves against the background, not
  the 3D (§2.1). Where `M = 0` and no Local2D layer covers the pixel, the
  output alpha goes to 0 and a transparent window shows the desktop. The lerp
  is continuous in `M`, so a gradient Tier-3 mask gives feathered 2D/3D edges
  and animated dissolves.

In both cases the output alpha is each winning layer's real alpha, which is
what lets transparent windows show the desktop behind them.

## 5. View-size event (soft-deprecated)

```c
typedef struct XrEventDataLocal3DZoneViewSizeChangedDXR {
    XrStructureType          type;   // XR_TYPE_EVENT_DATA_LOCAL_3D_ZONE_VIEW_SIZE_CHANGED_DXR
    const void* XR_MAY_ALIAS next;
    XrSession                session;
    uint32_t                 recommendedImageRectWidth;
    uint32_t                 recommendedImageRectHeight;
} XrEventDataLocal3DZoneViewSizeChangedDXR;
```

Queued when the recommended view size changes, for example when a mask is
activated or removed, or the window is resized. It fires only when the
dimensions actually change.

**Soft-deprecated since spec 5** in favour of the Khronos
`XR_EXT_view_configuration_views_change` event
(`XrEventDataViewConfigurationViewsChangedEXT`), followed by a re-read of
`xrEnumerateViewConfigurationViews`. This event is still emitted, with an
unchanged layout, and will keep being emitted. New apps should prefer the EXT
event. See
[`XR_EXT_view_configuration_views_change.md`](XR_EXT_view_configuration_views_change.md).

Both spellings are advisory. An app whose swapchain is sized for the
worst-case view never needs to reallocate: it moves
`subImage.imageRect` instead. The runtime scales whatever size is submitted,
so an app that reacts late is still correct, only softer for a frame.

## 6. Hardware zones

On a panel with switchable lens zones, the runtime publishes the same
authored mask to the display processor, which decides how cells follow it
(the default is that a cell is 3D wherever any covered pixel has `M > 0`).
Because the composite and the panel read one mask, a woven pixel is never
left under a flat lens cell or the reverse. On a display without switchable
zones the mask drives only the composite.

## 7. Relationship to `XR_DXR_display_zones`

[`XR_DXR_display_zones`](XR_DXR_display_zones.md) requires this extension
(≥ v4) and reuses it rather than replacing it:

- Its 2D zones are `XrCompositionLayerLocal2DDXR` layers, unchanged.
- Its explicit wish mask is an `XrLocal3DZoneMaskDXR`, authored with the same
  three tiers, but referenced per frame from the `xrEndFrame` chain
  (`XrDisplayZonesFrameEndInfoDXR::wishMask`) instead of through the sticky
  `xrSubmitLocal3DZoneDXR`.
- In a frame that declares zones, the sticky submitted mask and the implicit
  mask from Local2D rects are inert; each 3D zone carries its own rect. A
  frame with no zones behaves exactly as this page describes.

## 8. Version history

`SPEC_VERSION` continues the numbering from before the `XR_EXT_*` →
`XR_DXR_*` rename.

| Version | Changes |
|---------|---------|
| 1 | Mask object with the three authoring tiers; Tier 3 on D3D11; capabilities query |
| 2 | Tier-3 bindings for D3D12 and Vulkan (no Metal binding; Tier 1/2 work on Metal) |
| 3 | `XrCompositionLayerLocal2DDXR` (2D as a first-class composition layer, replacing the former shared-texture surround path) and `XrEventDataLocal3DZoneViewSizeChangedDXR` |
| 4 | Type values relocated from `1004999130–136` to `1004999160–166`, because the old block collided with `XR_DXR_mcp_tools`. No struct, field or entry-point change; consumers re-sync the header and rebuild |
| 5 | `XrEventDataLocal3DZoneViewSizeChangedDXR` soft-deprecated in favour of `XR_EXT_view_configuration_views_change`. Still emitted, layout unchanged; no ABI change |

All type values are in the provisional `1004999xxx` block and move if the
extension is registered with Khronos. Their current values are pinned by
`tests/tests_dxr_abi_snapshot.cpp`.

# XR_DXR_win32_window_binding — SPEC_VERSION 9: texture target binding

> **Shipped in runtime v2.55.0** (in-process D3D11, ADR-060 decision 3). This is the full text of
> the texture target binding; [`XR_DXR_win32_window_binding` §3.6](XR_DXR_win32_window_binding.md#36-texture-target-binding-v9)
> summarises it. Decision record: [ADR-060](../../adr/ADR-060-texture-target-reference-window-binding.md)
> (#697). The structure-type values 1004999015-018 are DisplayXR-provisional, recorded in
> `src/external/openxr_includes/openxr/README.md` and pinned by `tests/tests_dxr_abi_snapshot.cpp`.
> They are not Khronos-registered.

## 1. Summary

A `_texture` session can name **where its shared texture is drawn**: a **reference window**
(the app's own top-level window) plus a **target rect** in that window's client area. It no
longer hands the runtime an HWND to bind a weaver to. The runtime:

- weaves into the shared texture at `(0, 0, rect.w, rect.h)`: the rect is the canvas;
- phases the interlace from the target's live screen position (`set_present_origin`, ADR-033);
- frames the views (Kooima) to the target, per screen when the target spans several;
- never subclasses, reparents, or binds a DP to the reference window.

It is available only when the display processor can phase without a window. Otherwise the
session falls back to today's anchor path (§6).

## 2. New types

```c
#define XR_DXR_win32_window_binding_SPEC_VERSION 9

#define XR_TYPE_WIN32_TEXTURE_TARGET_BINDING_DXR            ((XrStructureType)1004999015)
#define XR_TYPE_WIN32_TEXTURE_TARGET_FRAME_DXR              ((XrStructureType)1004999016)
#define XR_TYPE_SYSTEM_WIN32_TEXTURE_TARGET_PROPERTIES_DXR  ((XrStructureType)1004999017)
#define XR_TYPE_WIN32_TEXTURE_TARGET_STATE_DXR              ((XrStructureType)1004999018)

typedef XrFlags64 XrWin32TextureTargetFrameFlagsDXR;
// Reserved in v9 (ADR-060 decision 5): the reference window is being moved or resized by
// the user right now. ADVISORY. The runtime forwards it as a hint and never does phase
// math from it (ADR-033). Ignoring it is conformant.
static const XrWin32TextureTargetFrameFlagsDXR XR_WIN32_TEXTURE_TARGET_FRAME_DRAGGING_BIT_DXR = 0x00000001;

typedef XrFlags64 XrWin32TextureTargetStateFlagsDXR;
static const XrWin32TextureTargetStateFlagsDXR XR_WIN32_TEXTURE_TARGET_STATE_WINDOWLESS_PHASE_BIT_DXR = 0x00000001;
static const XrWin32TextureTargetStateFlagsDXR XR_WIN32_TEXTURE_TARGET_STATE_ANCHOR_FALLBACK_BIT_DXR  = 0x00000002;
static const XrWin32TextureTargetStateFlagsDXR XR_WIN32_TEXTURE_TARGET_STATE_SEGMENTED_BIT_DXR        = 0x00000004;
static const XrWin32TextureTargetStateFlagsDXR XR_WIN32_TEXTURE_TARGET_STATE_FLAT_SEGMENT_BIT_DXR     = 0x00000008;
static const XrWin32TextureTargetStateFlagsDXR XR_WIN32_TEXTURE_TARGET_STATE_CLAMPED_BIT_DXR          = 0x00000010;

// System capability (xrGetSystemProperties chain). A PREDICTION; the session's
// XrWin32TextureTargetStateDXR is the authority (§6).
typedef struct XrSystemWin32TextureTargetPropertiesDXR {
    XrStructureType       type;              // XR_TYPE_SYSTEM_WIN32_TEXTURE_TARGET_PROPERTIES_DXR
    void* XR_MAY_ALIAS    next;
    XrBool32              supportsTextureTarget; // the runtime implements this struct set
    XrBool32              windowlessPhase;       // the active display processor phases without a window
} XrSystemWin32TextureTargetPropertiesDXR;

// Session creation: chained on XrWin32WindowBindingCreateInfoDXR::next.
typedef struct XrWin32TextureTargetBindingDXR {
    XrStructureType           type;             // XR_TYPE_WIN32_TEXTURE_TARGET_BINDING_DXR
    const void* XR_MAY_ALIAS  next;
    void*                     referenceWindow;  // HWND (top-level, this process) or NULL = desktop device px
    XrRect2Di                 initialTargetRect;// used until the first frame carries one
} XrWin32TextureTargetBindingDXR;

// Per frame: chained on XrFrameEndInfo::next (authoritative) and, optionally,
// on XrViewLocateInfo::next (frames that locate's views).
typedef struct XrWin32TextureTargetFrameDXR {
    XrStructureType           type;             // XR_TYPE_WIN32_TEXTURE_TARGET_FRAME_DXR
    const void* XR_MAY_ALIAS  next;
    void*                     referenceWindow;  // this frame's reference; NULL = desktop device px
    XrRect2Di                 targetRect;       // device px, in referenceWindow's client area (or the desktop)
    XrWin32TextureTargetFrameFlagsDXR flags;    // advisory hints; 0 = none
} XrWin32TextureTargetFrameDXR;

// Report: chained on XrFrameState::next at xrWaitFrame. Describes the most
// recently WOVEN frame (frameIndex < this wait's frame).
typedef struct XrWin32TextureTargetStateDXR {
    XrStructureType                   type;          // XR_TYPE_WIN32_TEXTURE_TARGET_STATE_DXR
    void* XR_MAY_ALIAS                next;
    XrWin32TextureTargetStateFlagsDXR flags;
    XrOffset2Di                       wovenScreenOrigin; // the target's resolved desktop origin, device px
    XrRect2Di                         wovenTargetRect;   // the rect that frame was woven for
    XrTime                            wovenDisplayTime;  // that frame's predictedDisplayTime
} XrWin32TextureTargetStateDXR;
```

## 3. Valid usage

**At `xrCreateSession`** (`XrWin32TextureTargetBindingDXR` chained):

- `XrWin32WindowBindingCreateInfoDXR::sharedTextureHandle` MUST be non-NULL. Otherwise
  `XR_ERROR_VALIDATION_FAILURE`, because a target binding only describes a texture target.
- `readbackCallback` MUST be NULL.
- `windowHandle` MAY be non-NULL. It is then the **fallback anchor**: it is used exactly as in
  SPEC_VERSION 8 if, and only if, the session cannot phase windowless (§6). When the binding is
  honored, the runtime ignores it (it does not hand it to any DP).
- `referenceWindow`, if non-NULL, MUST be a valid top-level window (`GetAncestor(h, GA_ROOT) == h`)
  of the calling process. Otherwise `XR_ERROR_VALIDATION_FAILURE`. A cross-process reference is
  reserved for a later version (see the weave service, R2).
- `initialTargetRect.extent` MUST be > 0 in both axes. Otherwise `XR_ERROR_VALIDATION_FAILURE`.
- A binding the session cannot honour is **ignored, not an error**: on any session other than
  the in-process D3D11 compositor (D3D12, GL, Vulkan, IPC), `xrCreateSession` succeeds, logs one
  WARN (`XrWin32TextureTargetBindingDXR IGNORED`), and `windowHandle`, if given, carries the
  weave as in SPEC_VERSION 8. The D3D12 hub route of §8 is the exception in progress (#2382).
- `transparentBackgroundEnabled` keeps its v8 meaning but applies to the app's own present, which
  the runtime does not perform. Under a target binding it only tells the DP the content carries
  alpha (`set_transparent_background(..., client_presents = true)`).

**At `xrEndFrame`** (`XrWin32TextureTargetFrameDXR` chained, in a session created with a binding):

- `targetRect.extent` MUST be > 0 in both axes; a zero rect is `XR_ERROR_VALIDATION_FAILURE`.
- The rect may lie partly or wholly off-screen, or off every 3D screen; that is not an error (§5).
- The rect is clamped to the shared texture's size. A larger rect is woven at the clamped size,
  `CLAMPED_BIT` is reported, and one WARN is logged per clamp episode (SPEC_VERSION 8 truncated
  silently).
- Omitting the struct on a frame reuses the last value given at **`xrEndFrame`** (or
  `initialTargetRect`). A value given only at `xrLocateViews` never counts as "the last value".
- `referenceWindow` may differ from the previous frame's (dock, undock, full screen). The change
  takes effect on this frame; nothing is recreated.
- If `referenceWindow` was destroyed, `XR_ERROR_VALIDATION_FAILURE`, and nothing is woven that
  frame. The runtime detects a dead window with `IsWindow`.

**At `xrLocateViews`:** an `XrWin32TextureTargetFrameDXR` chained on `XrViewLocateInfo` frames
**that locate's views only**. Validation there is best effort: an invalid value (a zero rect, a
destroyed window) is dropped without an error. Absent, the views are framed to the last `xrEndFrame` rect. A
locate value never changes what is woven, phased or reported in `XrWin32TextureTargetStateDXR`.
Only `xrEndFrame` does. The runtime therefore keeps the two apart: the locate value and the
`xrEndFrame` value are separate state. An engine that locates frame N+1 on one thread while
another thread is inside `xrEndFrame` N (the Unity and Unreal pattern) still weaves frame N at
frame N's rect. As with `XrDisplayZoneDXR`, the app chains the same values at locate and at
submit for one frame. A divergence mis-frames that frame's views, and never its weave.

**Coordinate space.** Every rect is in **physical device pixels**: the space a thread pinned to
`DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2` sees from `GetClientRect`/`ClientToScreen`. The
runtime pins its own thread for every read of `referenceWindow`, whatever the host process's DPI
awareness. **A DPI-unaware or system-aware host must convert** its logical rect, for example by
pinning its own thread for the measurement, or with `LogicalToPhysicalPointForPerMonitorDPI`. A
logical rect is not detected: it weaves at a plausible but wrong origin.

## 4. What the runtime does each frame

With `ref = referenceWindow` and `r = targetRect` of the frame being woven:

```
screen = ref ? ClientToScreen(ref, r.offset)   // under PMv2, at weave time
             : r.offset
window_metrics.window_screen_left/top    = screen
window_metrics.window_pixel_width/height = r.extent
canvas (in the shared texture)           = (0, 0, r.extent)
segment window                           = (screen, r.extent)
primary DP:  set_present_origin(screen - primary_panel.origin), then process_atlas(canvas)
spanning:    each screen's DP: set_present_origin(its segment origin), process_atlas(its segment)
```

The runtime reads the reference window's position at **weave time**, not at locate time, so
the phase follows a window that moved after the app measured it.

**Zones.** `XR_DXR_display_zones` rects (`XrDisplayZoneDXR::rect`,
`XrCompositionLayerLocal2DDXR::rect`, mask authoring tiers) are **target-local**. Where those
specs say "client window", read "the target". A frame without zones is one implicit 3D zone
covering the target. The SPEC_VERSION 8 rule "submit a zones frame every frame" (INV-5.6) does
not apply. `xrGetWorkspaceTileSizeDXR` returns `targetRect.extent` for a standalone session.

**Shared-texture read-back** (replaces the v8 formula for bound sessions):

```c
uvOffset = (0, 0);
uvScale  = (rect.w / (float)sharedWidth, rect.h / (float)sharedHeight);   // after the clamp
// draw that sub-rect 1:1 at targetRect in referenceWindow's client area
```

## 5. Multi-monitor

| Where the target is | Result in the shared texture |
|---|---|
| wholly on one 3D screen | woven by that screen's DP (the windowless primary, if it is the primary screen) |
| spanning a 3D and a 2D screen | the 3D part woven, the 2D part the flat left view; `SEGMENTED_BIT` + `FLAT_SEGMENT_BIT` |
| spanning two 3D screens | each part woven by its own screen's DP with its own viewer; the views follow the M3 per-segment metrics |
| wholly on a 2D screen | the flat left view; the session's 3D wish is released for that screen (#2247); `FLAT_SEGMENT_BIT` |
| a screen resamples window pixels (not 1:1) | that segment flat, unless the DP tolerates a resample (`XRT_DP_SCANOUT_FLAG_TOLERATES_RESAMPLE`) |

The app needs no special case for any of these. It keeps drawing the sub-rect 1:1.

## 6. Capability and fallback

1. The runtime creates the session's primary DP **windowless** through the plug-in's
   `create_dp_*_for_screen` with `window_handle = NULL`.
2. It reads the DP's scanout caps. `XRT_DP_SCANOUT_FLAG_PHASE_FROM_PRESENT_ORIGIN` (new,
   `xrt_display_scanout.h`, bit 1) set ⟹ the binding is honored: `WINDOWLESS_PHASE_BIT`.
3. Clear (vendor SDK without external routing, e.g. LeiaSR < 1.39.0; a plug-in without a
   `_for_screen` factory; a plug-in built before the bit):
   - with a fallback `windowHandle`, the windowless DP is destroyed and the session runs the
     SPEC_VERSION 8 anchor path. `ANCHOR_FALLBACK_BIT` is reported, and so are the rect and
     metrics (the target rect still defines the canvas and the zones' space). A target rect
     inset from the anchor's client origin is phased where the app draws it (#2406). A target
     left of or above the anchor, or outside its client area, cannot be corrected: it weaves as
     if it started at the anchor's client origin, and one WARN says so. Keep the anchor's client
     area covering the target. Under the anchor fallback, a change of `referenceWindow` does not
     move the anchor; a host that re-parents its viewport recreates the session;
   - without one, the session weaves **display-scoped** (no window phase): one WARN, no
     `WINDOWLESS_PHASE_BIT`.
4. `XrSystemWin32TextureTargetPropertiesDXR::windowlessPhase` predicts step 2 before any session
   exists, so an app can skip creating an anchor. A false prediction is possible (a vendor
   create can still fall back per weaver); the state flags are the authority. Passing an anchor
   anyway costs nothing, because an honoured binding never hands it to a DP. Recovering
   automatically when the bit is lost after creation is tracked in #2407.

The decision is made once per session. A primary-screen change does not re-open it. Segment DPs
are windowless already.

## 7. Example (an editor viewport)

```c
XrWin32TextureTargetBindingDXR tb = {XR_TYPE_WIN32_TEXTURE_TARGET_BINDING_DXR};
tb.referenceWindow   = editorMainHwnd;            // GetAncestor(viewportHwnd, GA_ROOT)
tb.initialTargetRect = viewportRectDevicePx;      // in editorMainHwnd's client area

XrWin32WindowBindingCreateInfoDXR wb = {XR_TYPE_WIN32_WINDOW_BINDING_CREATE_INFO_DXR, &tb};
wb.sharedTextureHandle = sharedNtHandle;          // worst-case sized, ADR-010
wb.windowHandle        = sysProps.windowlessPhase ? NULL : CreateAnchor();  // optional fallback

// every frame
XrWin32TextureTargetFrameDXR tf = {XR_TYPE_WIN32_TEXTURE_TARGET_FRAME_DXR};
tf.referenceWindow = GetAncestor(viewportHwnd, GA_ROOT);   // follows dock/undock
tf.targetRect      = MeasureViewportDevicePx();
locateInfo.next    = &tf;   xrLocateViews(...);
frameEndInfo.next  = &tf;   xrEndFrame(...);
DrawSharedSubRect1to1(tf.targetRect);                      // before the editor's own UI
```

## 8. Not in this version

- **Vulkan** texture mode (no Windows shared-texture export yet, R8). The structs are
  API-neutral; adoption needs no spec change.
- **IPC / service** texture sessions and a **cross-process** reference window. The present-owner
  route for that is `XR_DXR_weave` (`xrWeaveBindWindowDXR`), which phases windowless from the
  window's origin since R2 (#2379).
- **D3D12 and GL** honouring the binding. They reach texture mode through the ADR-059 hub's
  client-presents route (#2380, opt-in with `DXR_HUB=1`): a D3D12 texture session is rerouted
  there today, with its target taken from the `windowHandle`'s client area; honouring v9 on that
  route is #2382. GL is not carried yet (#2366).
- **A consumer for the drag hint.** The bit is reserved in v9 (ADR-060 decision 5). What the
  runtime does with it (for example, forwarding it to the DP's snap) is left to a later version.
- **The Cocoa variant** of the same pair, which comes in the next `XR_DXR_cocoa_window_binding`
  version (macOS has a texture class: `sharedIOSurface`). Xlib and Android get nothing until they
  have a texture class (ADR-060 decision 1).

## 9. Implementation (as landed)

On `main` since runtime v2.55.0. PRs and verification: [ADR-060 §Implementation](../../adr/ADR-060-texture-target-reference-window-binding.md#implementation-as-landed-2026-10-26).

- **Header:** `src/external/openxr_includes/openxr/XR_DXR_win32_window_binding.h` (SPEC_VERSION 9).
  The type values are recorded in that folder's `README.md` and pinned by `tests/tests_dxr_abi_snapshot.cpp`.
- **OpenXR:** `src/xrt/state_trackers/oxr/oxr_texture_target.c`, which holds the create-time valid
  usage, the per-frame forward at `xrEndFrame` and `xrLocateViews`, the state reply at `xrWaitFrame`,
  the system property and the IGNORED warning.
- **Shared resolve:** `src/xrt/compositor/util/comp_texture_target.h` and `comp_texture_target_win32.h`.
  They take (reference window, rect) in and give the desktop origin, the present origins, the
  window metrics and the segment window out, with every window read pinned to PMv2. The native
  D3D11 compositor and the hub both call them.
- **Windowless DP rule:** `src/xrt/compositor/util/comp_dp_windowless_d3d11.h`. The claim hint
  goes to `create_dp_d3d11_for_screen(window = NULL)`; the DP keeps it if it sets
  `XRT_DP_SCANOUT_FLAG_PHASE_FROM_PRESENT_ORIGIN`, and is destroyed otherwise.
- **D3D11 native:** `comp_d3d11_compositor.cpp`. The `frame` and `locate` slots are kept apart (§3),
  and the anchor-fallback padded weave is #2406.
- **Hub:** `comp_hub_d3d11` client-presents (#2380).
- **Tests:** `tests_comp_texture_target`, `tests_dp_windowless_d3d11`, `tests_dp_weave_scope`.
- **Reference client:** `test_apps/texture/cube_zones_texture_d3d11_win` with `DXR_TEXTURE_TARGET=1`.
  `DXR_TEXTURE_TARGET_INSET` draws at a sub-rect.

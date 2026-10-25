<!--
Copyright 2026, The DisplayXR Project
SPDX-License-Identifier: Apache-2.0
-->

# The spatial-display extension set

A one-page map of the `XR_DXR_*` extensions that let a standard OpenXR
application drive a **spatial display**: a glasses-free 3D monitor, laptop or
tablet that tracks the viewer's eyes. This is the subset a future Khronos
submission would start from. The per-extension specs in this directory are the
normative text; this page only says which part of each one is the core and why.

An HMD runtime knows where the content goes (the headset), what the display is
(fixed optics), how to render (one camera per eye) and that all of it is 3D. A
spatial display answers each of those questions differently, and each question
gets its own extension:

| # | Question | Extension(s) | Core of it |
|---|---|---|---|
| 1 | **Where does the content go?** | [`XR_DXR_win32_window_binding`](XR_DXR_win32_window_binding.md) · [`XR_DXR_cocoa_window_binding`](XR_DXR_cocoa_window_binding.md) · [`XR_DXR_xlib_window_binding`](XR_DXR_xlib_window_binding.md) · [`XR_DXR_wayland_surface_binding`](XR_DXR_wayland_surface_binding.md) · [`XR_DXR_android_surface_binding`](XR_DXR_android_surface_binding.md) | A `*BindingCreateInfoDXR` chained on `XrSessionCreateInfo` that hands the runtime the app's own window or surface, plus the shared `XrCompositionLayerWindowSpaceDXR` |
| 2 | **What is the display?** | [`XR_DXR_display_info`](XR_DXR_display_info.md) | `XrDisplayInfoDXR` (physical size, pixel size, nominal viewer position, recommended view scale) + rendering modes (enumerate, request, mode-changed and hardware-state events) |
| 3 | **How do I render for this viewer?** | [`XR_DXR_view_rig`](XR_DXR_view_rig.md) | A rig descriptor (`XrDisplayRigDXR` or `XrCameraRigDXR`) chained on `xrLocateViews`; the runtime returns render-ready `XrView{pose, fov}` |
| 4 | **Which part is 3D?** | `XR_DXR_local_3d_zone` (no formal spec yet; see its header) · [`XR_DXR_display_zones`](XR_DXR_display_zones.md) | A per-pixel 3D-ness mask, the `XrCompositionLayerLocal2DDXR` 2D layer, and N 3D zones per window |

## 1. Where does the content go: window and surface bindings

`XR_KHR_D3D11_enable` and its siblings tell the runtime **which graphics API** the
app renders with. The window bindings tell it **where the result is presented**:
in a window the app owns, rather than in a headset or in a window the runtime
creates. That is the main thing a spatial display adds to OpenXR. The app keeps
its message pump, input, window chrome and multi-window behaviour, and the
runtime weaves into that window at the right phase.

All five bindings have the same shape: one create-info chained on
`XrSessionCreateInfo`, one per platform, mutually exclusive. They differ only
where the platform forces them to:

| Binding | Handle | Live updates after create |
|---|---|---|
| win32 | `HWND` | none (the runtime observes the window) |
| cocoa | `NSView*` (CAMetalLayer-backed) | `XrCocoaWindowPlacementInfoDXR`: who owns drag / resize |
| xlib | `Display*` + `Window` | none |
| wayland | `wl_display*` + `wl_surface*` | `xrSetWaylandSurfaceGeometryDXR` (a `wl_surface` has no intrinsic size) |
| android | `ANativeWindow*` / `Surface` | `xrSetAndroidSurfaceDXR`, `xrSetAndroidWindowGeometryDXR`, `xrSubmitAndroidInputEventDXR`, layout-hint event (surfaces come and go with the activity lifecycle) |

All five enable `XrCompositionLayerWindowSpaceDXR`, a quad placed in fractional
window coordinates with a disparity. Its declaration is identical in each header
and guarded on its type value, so any combination of headers can be included.

**Not core:** the texture-handoff fields (`sharedTextureHandle` on win32,
`sharedIOSurface` on cocoa) serve producers that have no window the runtime can
present into, such as a browser or a capture target. They are being reworked
(ADR-060). `readbackCallback` is deprecated: new code passes NULL.

## 2. What is the display: `XR_DXR_display_info`

**Core:** `XrDisplayInfoDXR`, chained on `XrSystemProperties`, and the rendering
modes: `xrEnumerateDisplayRenderingModesDXR`, `xrRequestDisplayRenderingModeDXR`,
`XrEventDataRenderingModeChangedDXR`, `XrEventDataHardwareDisplayStateChangedDXR`.
That covers the physical facts plus 2D/3D switching, which every spatial display
has.

**Shipped, but not core:** eye-tracking mode control
(`xrRequestEyeTrackingModeDXR` and its events), desktop placement
(`XrDisplayDesktopPositionDXR`, `XrDisplayDesktopInfoDXR`, which are platform
plumbing), and multi-display (`XrDisplayDXR`, `xrEnumerateDisplaysDXR`, display
spaces, display bindings). These are useful, but a minimal spatial-display
runtime could omit them.

## 3. How to render for this viewer: `XR_DXR_view_rig`

The app declares framing intent and the runtime does the off-axis (Kooima) math.
There are two rigs: a **display rig**, where the window is a portal into the
scene, and a **camera rig**, where an app camera is perturbed by eye tracking.
The app chains one on `xrLocateViews` and gets back standard `XrView{pose, fov}`.
The descriptors carry no clip planes and no placement, because the runtime owns
the window geometry. `XrViewDisplayRawDXR` exposes the raw inputs for consumers
that do their own math.

## 4. Which part is 3D: `XR_DXR_local_3d_zone` + `XR_DXR_display_zones`

Some panels switch regions between 2D and 3D. `local_3d_zone` lets the app author
a per-pixel 3D-ness mask (whole window, a rect list, or a render target) and
submit flat content as `XrCompositionLayerLocal2DDXR`. `display_zones` builds on
it: N 3D zones per window, each with its own rig framing and projection layer,
plus M 2D zones and one wish mask per frame.

## Not in the set

These extensions ship, but they are product or ecosystem surfaces rather than
"drive a spatial display": `spatial_workspace`, `desktop_control`, `mcp_tools`,
`lift`, `stereo_camera`, `cursor_depth`, `workspace_file_dialog`,
`input_providers`, `depth_budget`, `atlas_capture`. Two sit close to question 1
and may join it later: `XR_DXR_weave` (an app that presents for itself, e.g. a
browser, hands the runtime its atlas to weave) and `XR_DXR_macos_gl_binding` (a
graphics binding, not a window binding).

## Known open questions

1. **Texture handoff:** fields on the binding create-info, or a binding struct of
   its own (ADR-060 is reworking it).
2. **`readbackCallback`:** deprecated now; a multi-vendor version would drop it.
3. **Window-space layer ownership:** today every binding enables it. A
   standalone layer extension is the alternative.
4. **Provisional type values:** every `XrStructureType` here is in the
   provisional `1004999xxx` block. Registration moves them, which is an ABI break
   for every shipped binary, so it needs a migration (the runtime accepts both
   values from apps; apps opt in to receiving new values from the runtime).
   Consumers must include the published headers rather than copy values.
5. **`display_info` scope:** the core above vs. everything it has accreted.
6. **`local_3d_zone`** has no formal spec yet, only its header.

ABI pin: `tests/tests_dxr_abi_snapshot.cpp` (generated by
`scripts/gen_dxr_abi_snapshot.py`) fails the build if any struct layout, type
value or `SPEC_VERSION` in this set moves. Cleanup plan:
[`docs/roadmap/khronos-proposal-binding-cleanup.md`](../../roadmap/khronos-proposal-binding-cleanup.md).

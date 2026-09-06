# Extension Apps vs Legacy Apps

Orthogonal to the [four app classes](../getting-started/app-classes.md), apps are either **extension apps** or **legacy apps** based on whether they enable `XR_DXR_display_info`. This distinction affects how the runtime handles rendering modes, swapchain sizing, and mode switching.

## Comparison

| Aspect | Extension App | Legacy App |
|--------|--------------|------------|
| **Detection** | Enables `XR_DXR_display_info` | Does not enable `XR_DXR_display_info` |
| **Rendering modes** | Enumerates all modes, handles `XrEventDataRenderingModeChangedDXR` | Unaware of modes, always renders stereo |
| **Swapchain sizing** | `max(tileColumns[i] * scaleX[i] * displayW)` across all modes | `recommendedImageRectWidth * 2` (compromise scale) |
| **Mode switching** | All modes: V toggle + 1/2/3 direct selection | Only V toggle between mode 0 (2D) and mode 1 (default 3D) |

## Which Apps Are Which?

- `_handle` and `_texture` apps are **always extension apps** — they need the extension for window binding.
- `_hosted` apps can be either:
  - A DisplayXR-aware `_hosted` app enables `XR_DXR_display_info` → **extension app**
  - A generic OpenXR `_hosted` app (e.g. WebXR, third-party) → **legacy app**

> **Note on WebXR pages.** Chrome's native WebXR implementation does not enable `XR_DXR_display_info`, so a WebXR session is always a legacy app at the OpenXR level, and the legacy compromise branch always fires. The [WebXR Bridge v2](../roadmap/webxr-support.md) sideband that once let a page escape that was retired in #1180; DisplayXR-aware web content targets inline 3D in the DisplayXR Browser instead, which is a weave present-owner rather than a WebXR session.

## Legacy App Compromise Scaling

Legacy apps don't know about rendering modes, so the runtime provides a **compromise scale** that works acceptably across modes. For SBS displays this is `0.5 × 1.0` (half-width, full-height).

The compromise scaling is computed in `oxr_system_fill_in()`. The `legacy_app_tile_scaling` flag on `xrt_system_compositor_info` disables 1/2/3 key mode selection for legacy apps (V toggle only).

See [ADR-006](../adr/ADR-006-legacy-app-compromise-view-scale.md) for the design rationale and [Legacy App Support](../specs/runtime/legacy-app-support.md) for the full algorithm (Case A/B).

## Runtime Behavior

The runtime detects which type of app it's dealing with at session creation time and adjusts:

1. **Swapchain dimensions** — reported via `xrEnumerateSwapchainFormats` / `recommendedImageRectWidth`
2. **Mode switching** — which keyboard shortcuts are active (V only vs V + 1/2/3)
3. **Event delivery** — `XrEventDataRenderingModeChangedDXR` only sent to extension apps
4. **Tile layout** — extension apps get the mode's native tile layout; legacy apps get a fixed compromise layout

## The View Path

The split above is about swapchain sizing and mode control; `xrLocateViews` is where the two
classes actually diverge in what they get back (the contract is [ADR-024, Amendment 1](../adr/ADR-024-raw-vs-render-ready-views.md)).

- **Render-ready (legacy apps, and any app that chains an `XR_DXR_view_rig` rig).** The runtime
  owns the camera: `XrView{pose, fov}` is complete for rendering and — since #1370 — is
  expressed in `XrViewLocateInfo::space`, exactly as the OpenXR spec says. Internally the view math
  runs in the head device's tracking-origin space (the display plane, the qwerty rig, the
  render-ready eyes all live there); the state tracker converts on **both legs**: a chained rig
  pose comes *in* from the locate space, and the eyes plus `displayPlanePose` go *out* to it,
  through the tracking-origin-to-base relation (`oxr_space_locate_device` for the head). Both legs
  or nothing — every shipping rig app locates in LOCAL and treats the result as rig-local, so
  converting only the output would shift them by the LOCAL offset. The same holds over IPC: the
  client converts, the server never sees the base space. Legacy clients keep the standard
  `T_base_head` chain (never the eye override — the #739 lesson), which carries the server's head
  motion in whatever base space the app asked for.
- **RAW (extension apps with `XR_DXR_display_info` and no rig chained).** The app owns the
  camera: `XrView.pose` is the display processor's eyes **relative to the display plane**, identity
  orientation, in every base space — ADR-024 / INV-6.1. `XrViewDisplayRawDXR::displayPlanePose`
  says where that plane is in the locate space, which is how a RAW app composes anything else it
  located there (grips, hand joints) into its own view.
- **Reference spaces are not redefined per class.** `LOCAL` is Monado's root + (0, 1.6, 0) for
  everyone (what legacy VR titles expect: STAGE at the floor, the head standing at 1.6 m); the
  qwerty rig is seeded at the same height. A hosted legacy title therefore sees its eyes about
  0.1 m above the LOCAL origin (+WASD), and 1.7 m above STAGE. The in-tree hosted test cubes draw
  their content at STAGE y = 1.6 for that reason — the former hosted world-absolute concession
  chain in `oxr_session_locate_views` existed only for them and was deleted in #1370.

## Further Reading

- [Multiview Tiling](../specs/runtime/multiview-tiling.md) — atlas layout algorithm
- [Legacy App Support](../specs/runtime/legacy-app-support.md) — full compromise scaling algorithm
- [XR_DXR_display_info](../specs/extensions/XR_DXR_display_info.md) — the extension specification

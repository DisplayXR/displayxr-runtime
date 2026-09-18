# Extension Apps vs Legacy Apps

Orthogonal to the [four app classes](../getting-started/app-classes.md), apps are either **extension apps** or **legacy apps** based on whether they enable `XR_DXR_display_info`. This distinction affects how the runtime handles rendering modes, swapchain sizing, and mode switching.

## Comparison

| Aspect | Extension App | Legacy App |
|--------|--------------|------------|
| **Detection** | Enables `XR_DXR_display_info` | Does not enable `XR_DXR_display_info` |
| **Rendering modes** | Enumerates all modes, handles `XrEventDataRenderingModeChangedDXR` | Unaware of modes, always renders stereo |
| **Swapchain sizing** | `max(tileColumns[i] * scaleX[i] * displayW)` across all modes | `recommendedImageRectWidth * 2` (compromise scale) |
| **Mode switching** | All modes: V toggle + 1/2/3 direct selection | Only V toggle between mode 0 (2D) and the default 3D mode |
| **Modes it may run in** | Only modes its view configuration can fill (2 under `PRIMARY_STEREO`, the device max under `PRIMARY_MULTIVIEW_DXR`) — #1499, *unless* the device pins its mode or the panel lease owns it in service mode | Only modes it can fill (`view_count ≤ 2`) — the mode floor, below |

## Which Apps Are Which?

- `_handle` and `_texture` apps are **always extension apps** — they need the extension for window binding.
- `_hosted` apps can be either:
  - A DisplayXR-aware `_hosted` app enables `XR_DXR_display_info` → **extension app**
  - A generic OpenXR `_hosted` app (e.g. WebXR, third-party) → **legacy app**

> **Note on WebXR pages.** Chrome's native WebXR implementation does not enable `XR_DXR_display_info`, so a WebXR session is always a legacy app at the OpenXR level, and the legacy compromise branch always fires. The [WebXR Bridge v2](../roadmap/webxr-support.md) sideband that once let a page escape that was retired in #1180; DisplayXR-aware web content targets inline 3D in the DisplayXR Browser instead, which is a weave present-owner rather than a WebXR session.

## Legacy App Compromise Scaling

Legacy apps don't know about rendering modes, so the runtime provides a **compromise scale** that works acceptably across modes. For SBS displays this is `0.5 × 1.0` (half-width, full-height).

The compromise scaling is computed in `oxr_system_fill_in()`. The `legacy_app_tile_scaling` flag on `xrt_system_compositor_info` disables 1/2/3 key mode selection for legacy apps (V toggle only).

### A legacy app in a mode with more than two views — the mode floor (#1510)

A legacy session submits a **fixed two views** (post-#1486 `PRIMARY_STEREO` reports exactly 2). A rendering mode whose `view_count` exceeds that has more tiles than the app can paint: the compositor's under-submit clamp paints the first two and the per-frame clear leaves the rest flat. Measured on sim-display's Quad mode (4 views, 2×2), a legacy app lost the bottom half of the canvas — to a mode it has no way to see, which is the capability loss #1486 rejected.

The runtime therefore applies a **mode floor**: *a legacy session does not run in a mode it cannot fill.* At `xrGetSystem` the runtime picks the mode the session will run in — the active one when it is fillable (`view_count ≤ 2`, which is every shipping configuration, since the Leia plug-in's modes are all 1- or 2-view), otherwise the first two-view 3D mode, else any fillable 3D mode, else mode 0 (2D). The compromise scale is computed from **that** mode, and `xrBeginSession` switches the display to it — so the mode, the scale, the compositor's tile grid and the display processor come up as one coherent set. The rule is `src/xrt/state_trackers/oxr/oxr_legacy_mode_rule.h`.

The floor is **not** a wider Case A. Giving the app `0.5 × 1.0` and laying its two views out 2×1 inside a 2×2 mode would break the contract every backend's `compute_effective_layout()` states — *the content recipe is the active mode's; submissions are clamped to it, never the other way round* — and the display processor receives that same grid, so a 2×1 atlas handed to a four-view weave de-tiles at the wrong stride. Two clean unpainted quadrants are strictly better than a corrupted image.

Two things outrank the floor, and in both the app keeps Case B and the under-submit clamp:

| Override | Why | What the log says |
|---|---|---|
| The device **pins** its mode (`XRT_DEVICE_PROPERTY_OUTPUT_MODE_PINNED`; sim-display's `SIM_DISPLAY_FORCE_MODE`) | The pin exists to hold a mode against every later request — that is what keeps the N-view under-submit path testable | `LEGACY session in an UNFILLABLE rendering mode (#1510)` at `xrCreateSession` |
| **Service mode** | The panel lease, not this app, owns the display-global mode; a legacy client must not yank it from a workspace controller or another client | same |

When the floor *does* move the display, `xrBeginSession` logs `oxr: LEGACY mode floor (#1510) - rendering mode N (...) cannot be filled ... switching to mode M`.

### The same floor for an EXTENSION app (#1499)

The rule turned out not to be about legacy apps at all. An extension app can see the modes and request one, but it still submits exactly as many views as the primary view configuration it **began** reports — 2 under `PRIMARY_STEREO`, even on a device sitting in a 4-view mode ([#1486](https://github.com/DisplayXR/displayxr-runtime/issues/1486)). The tiles it cannot paint are lost the same way, so [#1499](https://github.com/DisplayXR/displayxr-runtime/issues/1499) applies the same pick with `max_views` = the session's view count, in `oxr_legacy_mode_rule.h` (the `oxr_legacy_*` names are now thin wrappers binding it to 2).

Three differences from the legacy half, all forced by *when* the answer is knowable:

| | Legacy (#1510) | Extension (#1499) |
|---|---|---|
| Decided at | `xrGetSystem` — the app must be *sized* for the floored mode | `xrBeginSession` — the first moment `view_config_view_count` is authoritative |
| Sizing | Compromise view scale recomputed from the floored mode | **None.** The swapchain is worst-case-sized across all modes ([ADR-010](../adr/ADR-010-shared-app-iosurface-worst-case-sized.md)); only the recommended view *scales* move |
| Told? | No — a legacy app cannot receive mode events | `XrEventDataRenderingModeChangedDXR`, because apps enumerate modes *before* `xrBeginSession` and a cached `isActive` would go stale |

`xrRequestDisplayRenderingModeDXR` denies a mode the session could not fill, with `XR_DISPLAY_MODE_DENIAL_REASON_VIEW_CONFIG_CANNOT_FILL_DXR` — locally, before the request reaches the panel-lease holder. A session that was created but never *begun* is exempt: a workspace controller drives the panel on behalf of its clients rather than painting into it, so a painter's constraint must not be imposed on an orchestrator.

The same two overrides apply — a pinned device and service mode — and they apply to **both** halves: a pinned session is not floored *and* is not denied (it would otherwise be refused permission to re-request the mode it is already sitting in, and the device is the authority there anyway), and a service-mode client is neither floored *nor* answered locally, because its request has to reach the panel-lease holder. A `PRIMARY_MULTIVIEW_DXR` session is never floored or denied either — that is the invariant the whole change is built around.

Kill switch: `DXR_MODE_FLOOR=0` restores the pre-#1499 behaviour for extension sessions only — no floor, no denial, and none of the #1499 log lines, since a warning the runtime never used to print is part of what "pre-#1499" means. Full model: [View-Configuration Model](../reference/view-configuration-model.md#the-mode-floor-1499).

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

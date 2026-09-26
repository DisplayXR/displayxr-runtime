# DisplayXR OpenXR extension headers

The `XR_EXT_*.h` headers in this directory are the canonical source for
DisplayXR's custom OpenXR extensions. They auto-sync to the public
[`displayxr-extensions`](https://github.com/DisplayXR/displayxr-extensions)
repo on every push to `main`.

## Type-value allocation registry (`1004999xxx`)

All DisplayXR extension `XrStructureType` (and extension `XrResult` /
`XrViewConfigurationType`) values
live in the provisional `1004999xxx` block, pending Khronos registry
reconciliation at spec freeze. **This table is the single source of truth for
allocations — claim a block here BEFORE defining values in a new header.**
Three extensions previously collided by each guessing the "next free slot" in
their own header comments; do not repeat that.

Rules:

- New extension → claim the next free **decade** (10 values) below and record
  it here in the same PR that adds the header.
- Never renumber an existing value except to resolve a collision; a relocation
  is a consumer-visible break → bump the extension's `SPEC_VERSION` with a
  comment, and every consumer repo needs a header re-sync + rebuild.
- Deliberate cross-header sharing of one value for the *same* struct (see
  1004999002) is allowed and must be `#ifndef`-guarded + noted here.

| Values | Extension | Notes |
|---|---|---|
| 1004999001–002 | `XR_DXR_win32_window_binding` | 002 = `XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_DXR`, deliberately shared with the cocoa binding (`#ifndef`-guarded, same struct) |
| 1004999003–013 | `XR_DXR_display_info` | 003, 006–008, 010–013 assigned; 005/009 unused gaps |
| 1004999004 | `XR_DXR_cocoa_window_binding` | + shares 002 (see above) |
| 1004999100–110 | `XR_DXR_spatial_workspace` | |
| 1004999120–122 | `XR_DXR_workspace_file_dialog` | 122 is an `XrResult`, not an `XrStructureType` |
| 1004999130–132 | `XR_DXR_mcp_tools` | |
| 1004999140–142 | `XR_DXR_view_rig` | |
| 1004999150–153 | `XR_DXR_display_zones` | assigned (spec v1) |
| 1004999160–166 | `XR_DXR_local_3d_zone` | relocated from 130–136 (collided with mcp_tools) — spec v4 |
| 1004999170–171 | `XR_DXR_atlas_capture` | relocated from 120–121 (collided with workspace_file_dialog) — spec v3 |
| 1004999180 | `XR_DXR_macos_gl_binding` | relocated from 1004999010 (collided with display_info) — spec v2 |
| 1004999190–199 | `XR_DXR_weave` | 190 = `XR_TYPE_WEAVE_SUBMIT_INFO_DXR`, 191 = `XR_TYPE_WEAVE_OUTPUT_DXR` (#625), 192 = `XR_TYPE_WEAVE_SUBMIT_RECTS_DXR` (batch, spec v3), 193 = `XR_TYPE_WEAVE_SUBMIT_OVERLAYS_DXR` (2D overlay atlas, spec v4, browser#18), 194 = `XR_TYPE_WEAVE_SUBMIT_LAYOUT_DXR` (N-view worst-case atlas, spec v6, #774), 195 = `XR_TYPE_WEAVE_BIND_WINDOW_INFO_DXR`, 196 = `XR_TYPE_WEAVE_WINDOW_GEOMETRY_DXR`, 197 = `XR_TYPE_WEAVE_SUBMIT_HANDLES_DXR` (Android / platform-neutral handles + geometry, spec v7, #1036), 198 = `XR_TYPE_WEAVE_SUBMIT_FLAT_REGIONS_DXR` (per-region hardware wish, spec v8, browser#88). **199 is RESERVED** for a spec v9 per-submit wish MASK (R8 texture instead of a rect list, for non-rectangular flat geometry) — do not assign it elsewhere |
| 1004999200–209 | `XR_DXR_xlib_window_binding` | 200 = `XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR` (#660 Phase 3) |
| 1004999210–219 | `XR_DXR_display_info` (v16+ additions) | 210 = `XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR` (#715); fresh decade rather than reusing the 005/009 gaps in the original block. 211 = `XR_TYPE_DISPLAY_DESKTOP_INFO_DXR` (spec v18, #1301) — full panel desktop rect + GDI device name, superseding 210. 212 = `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR` (spec v19, #1486) — an `XrViewConfigurationType`, not an `XrStructureType`: the block is shared across every extended core enum. 213 = `XR_TYPE_VIEW_ACTIVITY_STATE_DXR` (spec v21, ADR-041) — per-frame `activeViewCount` chained on `XrViewState`, the other half of the fixed-view-count submission contract |
| 1004999220–229 | `XR_DXR_android_surface_binding` | 220 = `XR_TYPE_ANDROID_WINDOW_GEOMETRY_DXR` (#1037), 221 = `XR_TYPE_EVENT_DATA_ANDROID_WINDOW_LAYOUT_HINT_DXR` (spec v2, #1396). The extension's `XR_TYPE_ANDROID_SURFACE_BINDING_CREATE_INFO_DXR` keeps **1004999005** — one of the two unused gaps in the `XR_DXR_display_info` decade — because that value was already published in the (then unimplemented) sketch in `docs/specs/extensions/XR_DXR_display_info.md` §4 |
| 1004999230–239 | `XR_DXR_result_codes` | Not an extension — runtime-wide `XrResult` values returned from core entry points, where no extension can have been enabled. 230 = `XR_ERROR_RUNTIME_VERSION_SKEW_DXR` (browser#103), an **error**-class result, so it appears in the header negated as `-1004999230` |
| 1004999240–249 | `XR_DXR_weave` (v9+ additions) | 240 = `XR_TYPE_WEAVE_IPC_CONNECTION_DXR` (`xrWeaveExportIpcConnectionDXR`, spec v9, browser#103); 241 = `XR_TYPE_WEAVE_DMABUF_DESC_DXR`, 242 = `XR_TYPE_WEAVE_OVERLAY_DMABUF_DESC_DXR`, 243 = `XR_TYPE_WEAVE_OUTPUT_DMABUF_DXR`, 244 = `XR_TYPE_WEAVE_SUBMIT_SYNC_DXR`, 245 = `XR_TYPE_WEAVE_OUTPUT_SYNC_DXR` (desktop-Linux dma-buf transport + sync_file fences, spec v10, #1699); 246 = `XR_TYPE_WEAVE_SNAP_GRID_INFO_DXR` (`xrWeaveSnapWindowGridDXR`, bulk grid snap, spec v11, #1723). A fresh decade because the original 190–199 block is exhausted (199 is reserved for the wish mask) |
| 1004999250–259 | `XR_DXR_wayland_surface_binding` | 250 = `XR_TYPE_WAYLAND_SURFACE_BINDING_CREATE_INFO_DXR` (#757), 251 = `XR_TYPE_WAYLAND_SURFACE_GEOMETRY_DXR` (spec v2) — the app-declared surface size + output refresh, because a `wl_surface` has no intrinsic size and the compositor geometry service cannot bootstrap one. **250 was renumbered from 1004999210**, which collided with `XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR` — the 210–219 decade was already `XR_DXR_display_info`'s. Safe to move: SPEC_VERSION 1, no shipped app chains it |
| 1004999260–269 | `XR_DXR_depth_budget` | 260 = `XR_TYPE_REAR_DEPTH_BUDGET_DXR`, 261 = `XR_TYPE_CONTENT_BOUNDS_DXR`, 262 = `XR_TYPE_EVENT_DATA_REAR_DEPTH_BUDGET_STATE_CHANGED_DXR`, 263 = `XR_TYPE_CONTENT_MASK_DXR` (ADR-040). Was taken without a registry row — recorded retroactively in #1486 PR 2 |
| 1004999270–289 | `XR_DXR_lift` — **reserved, on branch `feat/lift-ext`, not yet on `main`** | Held here so the stereo camera block below cannot collide with it. The lift PR replaces this row with its own two rows (270–279, 280–289) |
| 1004999290–299 | `XR_DXR_stereo_camera` | 290 = `XR_TYPE_STEREO_CAMERA_PROPERTIES_DXR`, 291 = `XR_TYPE_STEREO_CAMERA_CALIBRATION_DXR`, 292 = `XR_TYPE_STEREO_CAMERA_STREAM_CREATE_INFO_DXR`, 293 = `XR_TYPE_STEREO_CAMERA_FRAME_DXR`, 294 = `XR_TYPE_STEREO_CAMERA_STREAM_INFO_DXR`, 295 = `XR_TYPE_STEREO_CAMERA_STREAM_TRANSPORT_DXR`, 296 = `XR_TYPE_STEREO_CAMERA_STREAM_STATS_DXR`, 297 = `XR_TYPE_EVENT_DATA_STEREO_CAMERA_STATE_CHANGED_DXR`, 298 = `XR_TYPE_EVENT_DATA_STEREO_CAMERAS_CHANGED_DXR` (spec v1, ADR-043). 299 = `XR_OBJECT_TYPE_STEREO_CAMERA_STREAM_DXR` — an `XrObjectType`, not an `XrStructureType` |
| 1004999300–309 | `XR_DXR_stereo_camera` (continued) | 300 = `XR_STEREO_CAMERA_FRAME_NOT_READY_DXR` — a **success**-class `XrResult` (positive). A second decade because 290–299 is full |
| 1004999310+ | **next free** | |

`XR_DXR_android_surface_binding` was implemented in #1037 and now has its own
header and decade — see the row above for why its create-info type value sits
outside that decade.

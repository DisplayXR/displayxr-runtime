# DisplayXR OpenXR extension headers

The `XR_EXT_*.h` headers in this directory are the canonical source for
DisplayXR's custom OpenXR extensions. They auto-sync to the public
[`displayxr-extensions`](https://github.com/DisplayXR/displayxr-extensions)
repo on every push to `main`.

## Type-value allocation registry (`1004999xxx`)

All DisplayXR extension `XrStructureType` (and extension `XrResult`) values
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
| 1004999190–194 | `XR_DXR_weave` | 190 = `XR_TYPE_WEAVE_SUBMIT_INFO_DXR`, 191 = `XR_TYPE_WEAVE_OUTPUT_DXR` (#625), 192 = `XR_TYPE_WEAVE_SUBMIT_RECTS_DXR` (batch, spec v3), 193 = `XR_TYPE_WEAVE_SUBMIT_OVERLAYS_DXR` (2D overlay atlas, spec v4, browser#18), 194 = `XR_TYPE_WEAVE_SUBMIT_LAYOUT_DXR` (N-view worst-case atlas, spec v6, #774) |
| 1004999200–209 | `XR_DXR_xlib_window_binding` | 200 = `XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_DXR` (#660 Phase 3) |
| 1004999210–219 | `XR_DXR_display_info` (v16+ additions) | 210 = `XR_TYPE_DISPLAY_DESKTOP_POSITION_DXR` (#715); fresh decade rather than reusing the 005/009 gaps in the original block |
| 1004999220+ | **next free** | |

`XR_DXR_android_surface_binding` defines no `1004999xxx` values in this
directory as of this writing; if it gains any, claim a decade here first.

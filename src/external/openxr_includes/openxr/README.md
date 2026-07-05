# DisplayXR OpenXR extension headers

The `XR_EXT_*.h` headers in this directory are the canonical source for
DisplayXR's custom OpenXR extensions. They auto-sync to the public
[`displayxr-extensions`](https://github.com/DisplayXR/displayxr-extensions)
repo on every push to `main`.

## Type-value allocation registry (`1000999xxx`)

All DisplayXR extension `XrStructureType` (and extension `XrResult`) values
live in the provisional `1000999xxx` block, pending Khronos registry
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
  1000999002) is allowed and must be `#ifndef`-guarded + noted here.

| Values | Extension | Notes |
|---|---|---|
| 1000999001–002 | `XR_EXT_win32_window_binding` | 002 = `XR_TYPE_COMPOSITION_LAYER_WINDOW_SPACE_EXT`, deliberately shared with the cocoa binding (`#ifndef`-guarded, same struct) |
| 1000999003–013 | `XR_EXT_display_info` | 003, 006–008, 010–013 assigned; 005/009 unused gaps |
| 1000999004 | `XR_EXT_cocoa_window_binding` | + shares 002 (see above) |
| 1000999100–110 | `XR_EXT_spatial_workspace` | |
| 1000999120–122 | `XR_EXT_workspace_file_dialog` | 122 is an `XrResult`, not an `XrStructureType` |
| 1000999130–132 | `XR_EXT_mcp_tools` | |
| 1000999140–142 | `XR_EXT_view_rig` | |
| 1000999150–153 | `XR_EXT_display_zones` | assigned (spec v1) |
| 1000999160–166 | `XR_EXT_local_3d_zone` | relocated from 130–136 (collided with mcp_tools) — spec v4 |
| 1000999170–171 | `XR_EXT_atlas_capture` | relocated from 120–121 (collided with workspace_file_dialog) — spec v3 |
| 1000999180 | `XR_EXT_macos_gl_binding` | relocated from 1000999010 (collided with display_info) — spec v2 |
| 1000999190–191 | `XR_EXT_weave` | 190 = `XR_TYPE_WEAVE_SUBMIT_INFO_EXT`, 191 = `XR_TYPE_WEAVE_OUTPUT_EXT` (#625) |
| 1000999200–209 | `XR_EXT_xlib_window_binding` | 200 = `XR_TYPE_XLIB_WINDOW_BINDING_CREATE_INFO_EXT` (#660 Phase 3) |
| 1000999210+ | **next free** | |

`XR_EXT_android_surface_binding` defines no `1000999xxx` values in this
directory as of this writing; if it gains any, claim a decade here first.

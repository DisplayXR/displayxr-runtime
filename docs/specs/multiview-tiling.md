---
status: Active
owner: David Fattal
updated: 2026-03-15
issues: [77]
code-paths: [src/xrt/compositor/, src/xrt/include/xrt/xrt_compositor.h]
---

# Multiview Tiling & Swapchain Model

## Context

The current rendering mode system uses `view_scale_x/y` as an overloaded concept -- it sizes viewports, derives tile layout implicitly (`cols = round(1/scale_x)`), and determines swapchain dimensions via a "min scale across modes" hack. Swapchain sizing is hardcoded to `display_pixel_width / 2` for stereo. This won't generalize to N-view displays (Looking Glass: 45 views, lightfield: 4-8 views).

**Goal:** Replace the ad-hoc approach with an explicit tiling model where:
1. The device provides per-mode `view_count` and `view_scale_x/y` (fraction of display per view)
2. The runtime computes optimal tile layout, per-view pixel dims, and atlas (swapchain) size
3. The app receives everything it needs -- no implicit derivation
4. Swapchain is created once at init (worst-case across all modes), never reallocated
5. GPU texture limits respected (near-square atlas)

**Key invariant:** `hmd->view_count` is fixed at init to `max(mode.view_count)` across all modes. Never mutated at runtime.

---

## Terminology: Display, Window, Canvas

Five spatial concepts matter for view sizing:

| Term | Definition |
|------|-----------|
| **Display** | The physical screen (e.g., 2240×1400 px, 0.304m × 0.190m) |
| **Window** | The app's OS window on the display (may be fullscreen or smaller) |
| **Canvas** | The sub-rect within the window where 3D content appears |
| **Swapchain** | Runtime-allocated GPU texture, worst-case sized at init |
| **Atlas** | The per-frame content region within the swapchain (tile_columns × view_width × tile_rows × view_height) |

**Key invariant:** View dimensions and Kooima screen size must be based on **canvas** dimensions, not display or window dimensions. This ensures correct aspect ratio and perspective geometry.

### Canvas for Each App Class

| Class | Canvas definition |
|-------|------------------|
| `_ext` (window-handle) | Canvas = window client area. The app provides a window via `XR_EXT_*_window_binding`; the entire client area is 3D content. |
| `_shared` (shared-texture) | Canvas = output rect — the sub-rect where the app places the shared texture in its window. May be a fraction of the window. |
| `_rt` (runtime-managed) | Canvas = window. The runtime owns the window, so the full window is 3D content. |

For `_ext` and `_rt` apps, canvas and window dimensions typically match (or are trivially derived). For `_shared` apps, the canvas can be arbitrarily smaller than both the window and the display — this is the case that requires special handling.

> **Current limitation:** `u_tiling_compute_mode()` always uses display dimensions (`D_w`, `D_h`) as the base for view sizing. For `_shared` apps, these should be canvas dimensions instead. Similarly, Kooima projection in sim_display uses full display physical size (`hmd->display_width_m / display_height_m`) — this should be canvas physical size for `_shared` apps. See the code fix tracking issue for details.

---

## The Tiling Algorithm

Given per mode: N views, `view_scale_x`, `view_scale_y`, display `D_w x D_h`:

```
V_w = D_w * scale_x          // per-view width in pixels
V_h = D_h * scale_y          // per-view height in pixels
C = ceil(sqrt(N * V_h / V_w)) // tile columns (prefer wider atlas for landscape)
if C < 1 then C = 1
if C > N then C = N
R = ceil(N / C)                // tile rows
S_w = C * V_w                 // atlas width for this mode
S_h = R * V_h                 // atlas height for this mode
```

System swapchain (once at init): `max(S_w) x max(S_h)` across all modes.

View i viewport: `x = (i % C) * V_w`, `y = (i / C) * V_h`, size `V_w x V_h`.

### view_scale semantics

`view_scale_x/y` is the **fraction of display resolution** occupied by one view:
- **2D**: `view_scale_x=1.0, view_scale_y=1.0` -- single full-resolution view
- **Stereo**: `view_scale_x=0.5, view_scale_y=0.5` -- each view is half-width, half-height (quarter of display pixels)
- **N-view lightfield**: e.g. `view_scale_x=0.1, view_scale_y=0.1` -- each view is 10% of display in each dimension

The tiling algorithm uses `ceil` (not `round`) for the column count, which biases toward wider (more columns) layouts. This ensures stereo with 0.5x0.5 scale produces horizontal SBS (C=2, R=1) rather than vertical stacking.

### Worked examples

| Device | Mode | N | scale_x x scale_y | V_w x V_h | C x R | Atlas | Notes |
|--------|------|---|-------------------|---------|-------|-------|-------|
| sim_display (1920x1080) | 2D | 1 | 1.0x1.0 | 1920x1080 | 1x1 | 1920x1080 | |
| sim_display (1920x1080) | Stereo | 2 | 0.5x0.5 | 960x540 | 2x1 | 1920x540 | SBS (horizontal) |
| sim_display (1920x1080) | Quad | 4 | 0.5x0.5 | 960x540 | 2x2 | 1920x1080 | 2x2 grid |
| Looking Glass (2560x1600) | Multi | 45 | 0.1x0.1 | 256x160 | 6x8 | 1536x1280 | Near-square, well under 4096 |
| Looking Glass (2560x1600) | 2D | 1 | 1.0x1.0 | 2560x1600 | 1x1 | 2560x1600 | |
| Leia (3840x2160) | 2D | 1 | 1.0x1.0 | 3840x2160 | 1x1 | 3840x2160 | |
| Leia (3840x2160) | Stereo | 2 | 0.5x0.5 | 1920x1080 | 2x1 | 3840x1080 | SBS (horizontal) |

System swapchain for sim_display: max(1920,1920,1920) x max(1080,540,1080) = **1920x1080** (display res).
System swapchain for Looking Glass: max(2560,1536) x max(1600,1280) = **2560x1600** (display res).
System swapchain for Leia: max(3840,3840) x max(2160,1080) = **3840x2160** (display res).

In all cases, the 2D mode (1 view at full display) dominates. The N-view modes tile smaller views that fit within.

---

## Implementation Status -- All Phases Complete

### Phase A + C: Tiling utility + all compositors + all DP interfaces

Commit `fc5f82ff1` + follow-up fixes (`febd2fd05`, `c4cd31b1e`, `735e6dfa8`, `91d32aff5`):

**`u_tiling.h` created** -- header-only C utility with:
- `u_tiling_compute_layout(N, V_w, V_h)` -> `(C, R)`
- `u_tiling_compute_mode(mode, D_w, D_h)` -> fills tile_columns/rows, view/atlas pixel dims
- `u_tiling_compute_system_atlas(modes[], count)` -> `(max_atlas_w, max_atlas_h)`
- `u_tiling_view_origin(view_index, C, V_w, V_h)` -> `(x, y)`
- `u_tiling_can_zero_copy(view_count, rects, swapchain_dims, mode)` -- zero-copy eligibility check

**All display processor interfaces renamed `process_stereo` -> `process_atlas`** with generalized signatures:
- **Vulkan** (`xrt_display_processor`): `process_atlas(atlas_view, view_width, view_height, tile_columns, tile_rows, ...)`
- **D3D11** (`xrt_display_processor_d3d11`): `process_atlas(atlas_srv, view_width, view_height, tile_columns, tile_rows, ...)`
- **D3D12** (`xrt_display_processor_d3d12`): `process_atlas(atlas_texture_resource, atlas_srv_gpu_handle, ..., tile_columns, tile_rows, ...)`
- **OpenGL** (`xrt_display_processor_gl`): `process_atlas(atlas_texture, view_width, view_height, tile_columns, tile_rows, ...)`
- **Metal** (`xrt_display_processor_metal`): `process_atlas(atlas_texture, view_width, view_height, tile_columns, tile_rows, ...)`

**All compositors updated** (32 files, +1668 -1405 lines):
- Metal, GL, D3D11, D3D12, Vulkan native compositors: tile-aware viewport layout
- Multi-compositor: passes tile info through
- All sim_display and Leia display processor implementations: updated to `process_atlas` signature

### Phase B: Swapchain sizing

- `oxr_system.c`: `recommendedImageRectWidth = displayPixelWidth * view_scale_x`, `recommendedImageRectHeight = displayPixelHeight * view_scale_y`
- `recommended_view_scale_x = min(scaleX across all modes)` set in `target_instance.c`
- Apps compute swapchain size as `max(tileColumns[i] * scaleX[i] * displayPixelWidth)` across all modes for width, similar for height
- Legacy apps (no `XR_EXT_display_info`): max taken over modes 0 and 1 only; special compromise scale logic for `view_count == 2 && scaleX <= 0.5 && scaleY <= 0.5` -> uses 0.5x1.0
- Zero-copy passthrough: when app's swapchain matches mode's atlas dimensions exactly, compositor skips atlas blit

### Phase D: Extended `xrt_rendering_mode` struct

`xrt_rendering_mode` in `xrt_device.h` now has:
```c
// Driver-provided:
uint32_t tile_columns, tile_rows;
// Runtime-computed (u_tiling_compute_mode fills these):
uint32_t view_width_pixels, view_height_pixels;
uint32_t atlas_width_pixels, atlas_height_pixels;
```

### Phase E: Extension struct update

`XrDisplayRenderingModeInfoEXT` includes `tileColumns`, `tileRows`, `viewWidthPixels`, `viewHeightPixels`. Populated by `oxr_xrEnumerateDisplayRenderingModesEXT`.

### Phase F: Runtime init computation

In `target_instance.c`, after `display_pixel_width/height` are known:
```c
for (mi = 0; mi < head->rendering_mode_count; mi++)
    u_tiling_compute_mode(&head->rendering_modes[mi], display_pixel_width, display_pixel_height);
```

### Phase G: Test apps use tiling info

All test apps (macOS + Windows, ext/shared/rt classes):
- Dynamic view count: `eyeCount = display3D ? modeViewCount : 1` (no cap at 2)
- Tile-aware viewports: `vpX = (eye % tileColumns) * renderW`, `vpY = (eye / tileColumns) * renderH`
- Dynamic `XrView` arrays (std::vector, not fixed [2])
- Swapchain sizing: `max(cols * scaleX * displayW)` across all modes
- HUD: dynamic `Eye[N]` display (replaces fixed "Eye L/R")
- `xrEndFrame` validation: accepts mode's view_count (not hardcoded 1-or-2)
- 1/2/3 key mode selection gated for legacy apps; HUD references removed

### Phase H: N=4 quad test mode

sim_display has quad mode (4 views, 2x2 tiling). Verified working end-to-end with ext_metal_macos. `xrLocateViews` returns 4 views, apps render 4 viewports in 2x2 grid, display processor receives 2x2 atlas.

### Bug fixes during implementation

- **GL compositor 2D freeze**: zero-copy path skipped VAO binding (`glBindVertexArray`), causing `GL_INVALID_OPERATION` in OpenGL 3.3 core profile. Fixed by binding VAO before present section.
- **VK apps `XR_ERROR_SIZE_INSUFFICIENT`**: `XrView views[2]` too small for quad mode. Fixed to dynamic `std::vector<XrView>`.

---

## Verification

| Phase | Test | Status |
|-------|------|--------|
| A+C | All compositors render identically -- CI green | DONE |
| B | Swapchain sizes correct for all modes | DONE |
| D | Struct fields populated correctly at init | DONE |
| E | Extension enumeration returns new fields | DONE |
| F | Computed tiling logged at init | DONE |
| G | Test apps use new fields, correct visual output | DONE |
| H | N=4 mode renders 2x2 grid correctly | DONE |

## Current Code Patterns

All compositors use the general tiling model:
- **Atlas size**: `tile_columns * view_width` x `tile_rows * view_height`
- **Per-view X offset**: `(view_index % tile_columns) * view_width`
- **Per-view Y offset**: `(view_index / tile_columns) * view_height`
- **2D mode**: `tile_columns=1, tile_rows=1`, single view at full size
- **Stereo SBS**: `tile_columns=2, tile_rows=1` (from `view_scale 0.5x0.5` + ceil formula)
- **Quad**: `tile_columns=2, tile_rows=2` (from `view_scale 0.5x0.5`, 4 views)

No more stereo special case -- all view layouts handled through tiling.

## Compositor-Side Contract: Swapchain → Crop → Display Processor

### Two Distinct Swapchains

The compositor sits between two separate swapchains — they are unrelated and serve different purposes:

| Swapchain | Created by | Size | Purpose |
|---|---|---|---|
| **App swapchain** | Runtime via `xrCreateSwapchain` | Fixed at init, worst-case across all modes | App renders atlas of tiled views into this |
| **Target swapchain** | Compositor at init (or app-provided window) | Window/display resolution | DP writes interlaced output here; compositor presents to display |

End-to-end pipeline:

```
App renders into app swapchain (fixed size, atlas of tiled views)
        ↓
Compositor reads atlas from app swapchain
Compositor crops to content dims (if smaller than swapchain)
        ↓
DP interlaces cropped atlas → target swapchain (window-sized)
        ↓
Compositor presents target swapchain to display
```

The app swapchain flows **into** the compositor; the target swapchain flows **out** to the display. They never share images.

### Swapchain images are worst-case sized

Swapchain images are runtime-allocated (via `xrCreateSwapchain`) and **fixed at init** to the worst-case dimensions across all rendering modes:

```
swapchain_width  = max(tile_columns[i] * view_width[i])  across all modes i
swapchain_height = max(tile_rows[i] * view_height[i])    across all modes i
```

This is computed by `u_tiling_compute_system_atlas()`. In practice, the 2D mode (1 view at full display resolution) typically dominates, making the swapchain equal to the display resolution.

### Per-frame atlas is typically smaller

Each frame, the app renders into a **content region** determined by the active rendering mode:

```
content_width  = tile_columns * view_width    (for active mode)
content_height = tile_rows * view_height      (for active mode)
```

In stereo mode (e.g., 2×1 tiles at half-height views), the content region is smaller than the swapchain. The content occupies the top-left corner of the swapchain image.

### Compositors must crop before passing to the DP

The display processor (DP) has **no knowledge of swapchain dimensions** — it expects a texture whose dimensions match the content exactly. Every compositor must:

1. Compute `content_w = tile_columns * view_width` and `content_h = tile_rows * view_height`
2. If `content_w == atlas_tex_width && content_h == atlas_tex_height`: pass the atlas directly (no copy needed — this is the common case for 2D mode)
3. If content is smaller than the atlas: **blit/copy** the valid content region `(0, 0, content_w, content_h)` into a correctly-sized intermediate texture, then pass that to the DP

This crop step is performed lazily — the intermediate texture is only created when content dims differ from atlas dims, and is recreated when content dims change (e.g., on mode switch).

### Why the DP can't just be told the content region

The DP's `process_atlas()` already receives `view_width`, `view_height`, `tile_columns`, `tile_rows` — but many DP implementations (e.g., LeiaSR weaver) use the **texture dimensions** to set up their internal rendering pipeline. If the texture is larger than the content, the DP samples padding/garbage from the unused region.

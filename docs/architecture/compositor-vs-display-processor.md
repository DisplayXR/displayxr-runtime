---
status: Active
owner: David Fattal
updated: 2026-03-15
issues: [78]
code-paths: [src/xrt/compositor/, src/xrt/include/xrt/xrt_display_processor.h]
---

# Compositor vs Display Processor: Moving Weaving to the Display Processor

## Problem

The Metal and GL compositors contain built-in anaglyph/blend/SBS fragment shaders that hardcode SBS (side-by-side) view layout assumptions:

**Metal** (`comp_metal_compositor.m`):
```msl
// Anaglyph: red from left eye (left half), cyan from right eye (right half)
float2 left_uv  = float2(in.texCoord.x * 0.5, in.texCoord.y);
float2 right_uv = float2(in.texCoord.x * 0.5 + 0.5, in.texCoord.y);
```

**GL** (`comp_gl_compositor.c`):
```glsl
vec2 uv_left = vec2(v_uv.x * 0.5, v_uv.y);
vec2 uv_right = vec2(v_uv.x * 0.5 + 0.5, v_uv.y);
```

These shaders bypass the display processor architecture. With the multiview tiling model (#77), the atlas layout is no longer necessarily SBS -- stereo with scale 0.5x0.5 produces vertical stacking (C=1, R=2). The hardcoded `x * 0.5` / `x * 0.5 + 0.5` breaks.

## Correct Architecture

```
Compositor                    Display Processor
  render views ------------>  atlas texture  ------------>  weaved output
  (tile layout)              + tile_columns/rows        (anaglyph/blend/SBS/lenticular)
```

- **Compositor**: packs views into atlas using tile layout from active mode. Knows nothing about weaving.
- **Display processor**: receives atlas + tile layout, extracts views, produces final output. sim_display does anaglyph/blend/SBS. Leia does lenticular.

## Changes Needed

### 1. Update display processor interfaces

Accept `tile_columns` and `tile_rows` (now implemented as `process_atlas()`):
- `xrt_display_processor_metal.h` -- `process_atlas()` with tile params
- `xrt_display_processor_gl.h` -- same
- `xrt_display_processor.h` (Vulkan) -- same
- `xrt_display_processor_d3d11.h` / `d3d12` -- same

### 2. Update sim_display display processors

Use tile layout for view extraction instead of assuming SBS:
- `sim_display_dp_metal.m`
- `sim_display_dp_gl.c`
- `sim_display_dp_vk.c`
- `sim_display_dp_d3d11.cpp` / `d3d12`

### 3. Remove built-in anaglyph/blend/SBS shaders from compositors

- `comp_metal_compositor.m` -- remove `anaglyph_fragment`, `blend_fragment`, `FS_SBS` and their pipelines
- `comp_gl_compositor.c` -- remove `FS_ANAGLYPH`, `FS_BLEND`, `FS_SBS` and their programs
- The fallback path (3D mode without display processor) becomes an error or passthrough blit

### 4. Compositor always delegates to display processor

No more `switch (output_mode)` in compositor code for 3D output.

## Relation to Multiview Tiling (#77)

This is a prerequisite for multiview tiling (#77) to work correctly. The tiling algorithm produces optimal near-square atlas layouts that are not necessarily SBS. Display processors must be tile-layout-aware.

## Affected Files

- `src/xrt/include/xrt/xrt_display_processor_metal.h`
- `src/xrt/include/xrt/xrt_display_processor_gl.h`
- `src/xrt/include/xrt/xrt_display_processor.h`
- `src/xrt/include/xrt/xrt_display_processor_d3d11.h`
- `src/xrt/include/xrt/xrt_display_processor_d3d12.h`
- `src/xrt/drivers/sim_display/sim_display_dp_metal.m`
- `src/xrt/drivers/sim_display/sim_display_dp_gl.c`
- `src/xrt/drivers/sim_display/sim_display_dp_vk.c`
- `src/xrt/drivers/sim_display/sim_display_dp_d3d11.cpp`
- `src/xrt/drivers/sim_display/sim_display_dp_d3d12.cpp`
- `src/xrt/compositor/metal/comp_metal_compositor.m`
- `src/xrt/compositor/gl/comp_gl_compositor.c`
- `src/xrt/compositor/vk_native/comp_vk_native_renderer.c`
- `src/xrt/compositor/d3d11/comp_d3d11_renderer.cpp`
- `src/xrt/compositor/d3d12/comp_d3d12_renderer.cpp`

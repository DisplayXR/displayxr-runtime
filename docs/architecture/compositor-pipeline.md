# Compositor Pipeline

This document describes the shipping rendering pipeline from app submission to display output. For the architectural decision behind this separation, see [ADR-007: Compositor Never Weaves](../adr/ADR-007-compositor-never-weaves.md).

## Pipeline Overview

```
Compositor                         Display Processor
  app swapchain  ──crop──>  atlas  ──process_atlas()──>  weaved output
  (tiled views)            + tile_columns/rows          (target swapchain)
```

- **Compositor**: packs views into atlas using tile layout from active rendering mode. Knows nothing about weaving or vendor-specific display formats.
- **Display processor**: receives atlas + tile layout, extracts individual views, produces final interlaced/weaved output. sim_display does anaglyph/blend/SBS. Leia does lenticular interlacing.

## Step-by-Step

1. **App renders** tiled views into the app swapchain (worst-case sized, allocated at session creation)
2. **Compositor receives** the submitted atlas via `xrEndFrame`
3. **Compositor crops** the atlas to the active mode's content dimensions — the atlas region may be smaller than the worst-case allocation
4. **Compositor calls** `display_processor->process_atlas(atlas, tile_columns, tile_rows, ...)` on the vendor's display processor
5. **Display processor** extracts views from the atlas using tile layout, applies vendor-specific processing (interlacing, lenticular weaving, etc.), writes to the target swapchain
6. **Compositor presents** the target swapchain to the display/window

## Key Principles

- **Compositor never weaves** — no vendor-specific display format logic in compositor code. All 3D output processing is delegated to the display processor via `process_atlas()`.
- **Tile-layout-aware** — the display processor receives `tile_columns` and `tile_rows` rather than assuming any particular view arrangement (e.g., side-by-side). This supports arbitrary multiview layouts.
- **Vendor isolation** — adding a new display vendor requires zero changes to compositor code. The vendor implements the display processor vtable under `src/xrt/drivers/<vendor>/`.

## Display Processor Interface

The `process_atlas()` method exists in 5 API-specific variants:

| API | Header |
|-----|--------|
| Vulkan | `xrt_display_processor.h` |
| D3D11 | `xrt_display_processor_d3d11.h` |
| D3D12 | `xrt_display_processor_d3d12.h` |
| Metal | `xrt_display_processor_metal.h` |
| OpenGL | `xrt_display_processor_gl.h` |

See [Display Processor Interface](../specs/display-processor-interface.md) for the unified vtable design and [Vendor Integration Guide](../guides/vendor-integration.md) for implementation guidance.

## Further Reading

- [Swapchain Model](../specs/swapchain-model.md) — two-swapchain architecture and canvas concept
- [Separation of Concerns](separation-of-concerns.md) — layer boundaries
- [ADR-003: Vendor Abstraction](../adr/ADR-003-vendor-abstraction-via-display-processor-vtable.md) — why vendor code is isolated behind the DP vtable

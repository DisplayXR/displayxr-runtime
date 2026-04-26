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
- **Canvas sub-rect flows to DP** — for `_texture` apps, the canvas may be a sub-rect of the window. The compositor passes `canvas_offset_x`, `canvas_offset_y`, `canvas_width`, and `canvas_height` through to `process_atlas()` so the display processor can compute correct phase alignment. The app's real window handle (HWND / NSView) is passed directly to the display processor — no hidden windows are involved.
- **DP expects linear input** — the display processor's input is sampled as linear float values; the compositor is responsible for delivering an atlas whose bytes the DP's SRV will read as linear. For SRGB-encoded source swapchains this means linearizing on the way in. See *Color-space handling* below.
- **Vendor isolation** — adding a new display vendor requires zero changes to compositor code. The vendor implements the display processor vtable under `src/xrt/drivers/<vendor>/`.

## Color-space handling (D3D11 service compositor, shell mode)

The DP weaver expects linear input. App swapchains can be either UNORM (linear bytes) or SRGB (gamma-encoded bytes), and the compositor must reconcile both into a linear stream by the time the DP samples.

**Non-shell mode** does this at the swapchain → per-client atlas blit: SRGB swapchains are sampled through an SRGB SRV (GPU auto-linearizes) and written to the UNORM atlas as linear bytes via the projection-blit shader. UNORM swapchains take a raw `CopySubresourceRegion` (already linear). The DP then reads the per-client atlas directly.

**Shell mode** uses a multi-compositor stage (per-client atlas → combined atlas → crop → DP). The swapchain → per-client atlas blit must be a raw `CopySubresourceRegion` regardless of source format — a shader-blit at this boundary races with the keyed-mutex release back to the app and can leave per-eye tiles stale on subsequent frames. The reinterpretation therefore happens at the **multi-comp read** boundary instead:

- Per-client atlas storage is `R8G8B8A8_TYPELESS` (shell mode only). Two parallel SRVs view the same bytes:
  - `atlas_srv` (UNORM-typed) — used when the source swapchain bytes are already linear.
  - `atlas_srv_srgb` (UNORM_SRGB-typed) — used when the source bytes are gamma-encoded; the GPU auto-linearizes on sample.
- A per-client `atlas_holds_srgb_bytes` flag is set in `compositor_layer_commit` from `view_is_srgb[0]` after the blit. `multi_compositor_render` selects the SRV at sample time.
- The multi-comp blit shader runs at `convert_srgb=0.0` (passthrough) and writes linear values to the combined atlas (UNORM). The crop step preserves bytes, and the DP receives linear input.

This keeps the shell vs non-shell pipelines distinct downstream — a load-bearing invariant; do not refactor the swapchain → atlas blit into a shared shader path. The non-shell atlas remains UNORM-typed; only the shell-mode atlas is TYPELESS-with-dual-SRV.

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

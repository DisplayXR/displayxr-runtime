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
- **DP-handoff encoding is declared by the DP, not fixed** — DisplayXR owns a vendor-neutral compose space; the display processor *declares* whether it accepts `LINEAR`, `ENCODED`, or `EITHER` input, and the runtime converts compose→handoff to match (a matched-pair step, no-op when they already agree). The runtime never derives the convention from — or bakes a curve of — any particular vendor's weaver (ADR-003/007). Production DPs typically declare `EITHER` (their weaver has an explicit input/output sRGB-conversion control and can do the output encode itself), and apps store display-referred bytes, so the in-process path passes through encoded (GL #407, D3D11/D3D12/Vulkan/Metal #408). The app's obligation: request an sRGB swapchain and store a correctly-encoded image. Full contract incl. the matched-pair invariant and both-direction conversion: [ADR-021](../adr/ADR-021-color-management-encoding-state-invariant.md). (This supersedes an earlier "DP expects linear input" note — that described a service-path *intermediate*, not the handoff, and is the latent half-conversion documented below.)
- **Vendor isolation** — adding a new display vendor requires zero changes to compositor code. The vendor implements the display processor vtable under `src/xrt/drivers/<vendor>/`.

## Color-space handling (D3D11 service compositor)

The DP declares its accepted handoff encoding and the runtime declares (per frame) the encoding it is sending; production DPs typically declare **`EITHER`** (their weaver has an input/output sRGB-conversion control that handles both directions, including the output encode) — see [ADR-021](../adr/ADR-021-color-management-encoding-state-invariant.md). Apps write encoded bytes into both UNORM and SRGB swapchains, so today the runtime sends **encoded** and the correct path is **passthrough** — hand the app's bytes to the DP unchanged. The multi-compositor stage runs the blit shader at `convert_srgb=0.0` (passthrough) into the combined atlas; the crop step preserves bytes; the DP receives the app's encoded bytes verbatim. (A `LINEAR`-only DP would instead get a matched decode at the handoff step.)

The swapchain → per-client atlas blit is a raw `CopySubresourceRegion` regardless of source format — a shader-blit at this boundary races with the keyed-mutex release back to the app and can leave per-eye tiles stale on subsequent frames. Per-client atlas storage is `R8G8B8A8_TYPELESS` (workspace mode), and two parallel SRVs view the same bytes, selected at the **multi-comp read** boundary by a per-client `atlas_holds_srgb_bytes` flag (set in `compositor_layer_commit` from `view_is_srgb[0]`):

- `atlas_srv` (UNORM-typed) — raw bytes, no conversion. For a client that stored encoded bytes in a UNORM swapchain this is correct passthrough → encoded reaches the DP.
- `atlas_srv_srgb` (UNORM_SRGB-typed) — the GPU **decodes** sRGB→linear on sample.

> ⚠️ **Known latent half-conversion (ADR-021 / [#409](https://github.com/DisplayXR/displayxr-runtime/issues/409)).** The `atlas_srv_srgb` branch decodes to linear but **nothing re-encodes** before `process_atlas()` (the blit shader is passthrough, and `linear_to_srgb()` in `d3d11_service_shaders.h` is defined but never called). So an sRGB-swapchain client would reach the weaver ~2.2× too dark — the same unmatched decode #407/#408 fixed in-process. It is **dead in practice today** (every workspace app stores encoded-into-UNORM → flag false → UNORM SRV), and the branch is guarded with a one-shot warning in `multi_compositor_render`. The fix is the Model-A passthrough baseline (always use the UNORM SRV here) or, if/when the compose path moves to Model B, the matched re-encode at the DP boundary. Do not "fix" this by linearizing more — that deepens the half-conversion.

The non-workspace atlas remains UNORM-typed (single `atlas_srv`); only the workspace-mode atlas is TYPELESS-with-dual-SRV. Do not refactor the swapchain → atlas blit into a shared shader path.

## Per-tile alpha (workspace mode)

The multi-compositor's tile-blit phase respects each IPC client's
projection layer flags when compositing tiles into the combined atlas:

- `XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT` clear → tile is
  treated as opaque and overwrites the workspace background.
- Bit set + `UNPREMULTIPLIED_ALPHA_BIT` clear → straight-alpha source,
  blended with `blend_premul`.
- Bit set + `UNPREMULTIPLIED_ALPHA_BIT` set → unpremultiplied source,
  blended with `blend_alpha`.

Capture clients (2D window snapshots) and clients that submitted no
projection layer this frame fall through to opaque blending.

The combined atlas itself is presented opaquely to the display
processor — per-tile alpha lets one client tile reveal the workspace
background through its transparent regions, but the workspace's
output to the desktop is always opaque. See
[`workspace-controller-registration.md`](../specs/runtime/workspace-controller-registration.md#workspace-output-is-opaque)
for why.

## Display Processor Interface

The `process_atlas()` method exists in 5 API-specific variants:

| API | Header |
|-----|--------|
| Vulkan | `xrt_display_processor.h` |
| D3D11 | `xrt_display_processor_d3d11.h` |
| D3D12 | `xrt_display_processor_d3d12.h` |
| Metal | `xrt_display_processor_metal.h` |
| OpenGL | `xrt_display_processor_gl.h` |

See [Display Processor Interface](../specs/vendor/display-processor-interface.md) for the unified vtable design and [Vendor Integration Guide](../guides/vendor-plugin-onboarding.md) for implementation guidance.

## Further Reading

- [Swapchain Model](../specs/runtime/swapchain-model.md) — two-swapchain architecture and canvas concept
- [Separation of Concerns](separation-of-concerns.md) — layer boundaries
- [ADR-003: Vendor Abstraction](../adr/ADR-003-vendor-abstraction-via-display-processor-vtable.md) — why vendor code is isolated behind the DP vtable

# Swapchain Model

Each compositor maintains two distinct, unrelated swapchains. Understanding this separation is essential for compositor and display processor development.

## Two Swapchains

### App Swapchain

- **Allocated by**: the runtime via `xrCreateSwapchain`
- **Sized to**: worst-case atlas dimensions across all rendering modes (allocated once at session creation)
- **Content**: the app renders a tiled atlas of views into this swapchain
- **Flow direction**: app → compositor (input)

### Target Swapchain

- **Allocated by**: the compositor
- **Sized to**: the output window dimensions
- **Content**: the display processor writes interlaced/weaved output here
- **Flow direction**: compositor → display (output)

These two swapchains are unrelated — the app swapchain flows in, the target swapchain flows out.

## Pipeline

```
App Swapchain          Compositor              Display Processor        Target Swapchain
(worst-case atlas) --> crop to content dims --> process_atlas()      --> (window-sized)
                                                (interlace/weave)        --> present
```

1. **App** renders tiled views into the app swapchain (atlas layout from active rendering mode)
2. **Compositor** crops the atlas to the active mode's content dimensions (the atlas may be smaller than the worst-case allocation)
3. **Display processor** receives the cropped atlas + tile layout (`tile_columns`, `tile_rows`), extracts views, and produces the final interlaced/weaved output into the target swapchain
4. **Compositor** presents the target swapchain to the display

**Crop is the default; zero-copy is the exception.** Because the swapchain is the worst-case
envelope, the per-frame content normally occupies only its top-left sub-rect, so step 2 must
**always** crop — *unless* the app's submission exactly fills the swapchain at the active
mode's tiling, the one case where the compositor may hand the swapchain straight to the DP.
That case is decided solely by `u_tiling_can_zero_copy()` and, by construction, only occurs
for a full-screen window in the mode that hits the per-dim worst case in both dimensions
(on the Windows Leia DP: 2D only; on Android: never). See
[ADR-030](../../adr/ADR-030-crop-before-dp-zero-copy-only-when-swapchain-equals-atlas.md) and
[Multiview Tiling — Zero-copy eligibility](multiview-tiling.md#zero-copy-eligibility--the-single-rule).

See [Multiview Tiling — Compositor-Side Contract](multiview-tiling.md) for the full crop-blit algorithm.

## Canvas Concept

The **canvas** is the sub-rect of the window where 3D content appears. For `_handle` and `_hosted` apps, the canvas equals the window. For `_texture` apps, the canvas may be smaller than the display — the app dedicates only part of its window to 3D content.

View dimensions and Kooima projection must be based on **canvas** size, not display size. This is critical for `_texture` apps.

The canvas rect of each 3D region is declared as an `XrDisplayZoneEXT` 3D zone via [`XR_EXT_display_zones`](../extensions/XR_EXT_display_zones.md) (the legacy `xrSetSharedTextureOutputRectEXT` entry point was removed — ADR-031). The compositor plumbs the zone rect through to the display processor's `process_atlas()` call as `canvas_offset_x/y` and `canvas_width/height`, enabling correct phase alignment for lenticular interlacing. The app's real window handle (HWND / NSView) is passed directly to the display processor — no hidden windows are involved.

See [Multiview Tiling — Terminology: Display, Window, Canvas](multiview-tiling.md) for formal definitions.

## Further Reading

- [Multiview Tiling](multiview-tiling.md) — atlas layout algorithm and compositor contract
- [Compositor Pipeline](../../architecture/compositor-pipeline.md) — end-to-end rendering pipeline
- [ADR-007: Compositor Never Weaves](../../adr/ADR-007-compositor-never-weaves.md) — why the compositor only crops, never interlaces
- [ADR-010: Shared App IOSurface Worst-Case Sized](../../adr/ADR-010-shared-app-iosurface-worst-case-sized.md) — why app swapchain uses worst-case dimensions

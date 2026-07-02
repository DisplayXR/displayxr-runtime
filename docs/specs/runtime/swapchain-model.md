# Swapchain Model

Each compositor maintains two distinct, unrelated swapchains. Understanding this separation is essential for compositor and display processor development.

## Two Swapchains

### App Swapchain

- **Allocated by**: the runtime via `xrCreateSwapchain`
- **Sized to**: worst-case atlas dimensions across all rendering modes (allocated once at session creation)
- **Content**: the app renders a tiled atlas of views into this swapchain
- **Flow direction**: app → compositor (input)

#### View addressing: tiled vs layered (array) swapchains

`XrSwapchainSubImage` exposes **two** ways to address a per-view image, and the
runtime honors whichever the app submits on **every** graphics API:

- **Tiled** (`arraySize = 1`): views packed side-by-side in one image; each view
  is addressed by `imageRect.offset`/`extent`. This is the layout native and
  tool apps use, and the only layout that can host `view_count > 2` light-field
  content.
- **Layered / array** (`arraySize = N`): one texture-array swapchain whose slices
  hold the per-view images; each view is addressed by `imageArrayIndex` (with
  `imageRect` selecting a sub-rect *within* the slice). This is what engine
  single-pass-instanced stereo (Unity SPI, Unreal Instanced Stereo) and every
  PC-VR runtime produce, and is capped at the engine's efficient multiview count
  (typically 2).

Both are first-class on D3D11, D3D12, Vulkan, OpenGL, and Metal — the compositor
selects the array slice at the per-view atlas-blit sampling site (D3D12/D3D11:
`Texture2DArray` SRV; Vulkan: `baseArrayLayer`; GL: `sampler2DArray` layer; Metal:
per-slice 2D texture view). The two addressing modes are orthogonal (a layered
submission may still use `imageRect`). The **display-processor contract is
unchanged** either way: the DP always receives a single flat tiled atlas (see
`process_atlas` below) — array resolution happens entirely upstream, in the
compositor. Consequently a layered submission never satisfies
`u_tiling_can_zero_copy()` (slices ≠ tiles), so it always takes the crop/blit
path; one atlas blit per frame is the floor for array content, matching every
PC-VR runtime.

Rationale, per-backend mechanism, and the app-side authoring rules (including the
per-slice depth clear): [ADR-032](../../adr/ADR-032-array-layered-swapchains-first-class.md)
and [displayxr-app-rules INV-4.8](../../guides/displayxr-app-rules.md).

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

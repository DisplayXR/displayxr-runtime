# ADR-032: Array (Layered) Swapchains Are First-Class Alongside the Tiled Atlas

**Status:** Accepted
**Date:** 2026-07-02

## Context

OpenXR's `XrSwapchainSubImage` exposes **two** ways for an app to address the
per-view image it submits in a projection layer:

- **`imageRect`** — a sub-rectangle within one image. A "tiled atlas" app packs
  its N views side-by-side in a single-layer swapchain (`arraySize = 1`) and
  gives each view a distinct `imageRect.offset`.
- **`imageArrayIndex`** — a slice of a layered (array) swapchain
  (`arraySize = N`). A "layered" app renders each view into its own array slice
  and gives each view a distinct `imageArrayIndex` (with `imageRect` selecting a
  region *within* that slice).

Both are conformant, and a runtime must honor whichever the app submits. The two
are **orthogonal**: a layered submission may still use `imageRect`.

Which layout an app uses is not a runtime choice — it is dictated by how the app
renders:

- **Tiled** is what DisplayXR-native apps, tools, and any **>2-view light-field**
  content use. It is the only layout that can host `view_count > 2`, because no
  engine renders more than a few views into array slices efficiently.
- **Layered** is what game engines produce for stereo: Unity single-pass-
  instanced (SPI) and Unreal Instanced Stereo render both eyes into a 2-slice
  array in one pass, and every PC-VR runtime (SteamVR/Oculus/WMR) consumes the
  same shape. The DisplayXR Unity display provider
  ([displayxr-unity#166](https://github.com/DisplayXR/displayxr-unity/issues/166))
  submits `arraySize = 2` for 3D and `arraySize = 1` for 2D.

At the time this was assessed ([#681](https://github.com/DisplayXR/displayxr-runtime/issues/681)),
only **D3D12** (PR #656) and **Vulkan** honored `imageArrayIndex`. The D3D11,
OpenGL, and Metal compositors ignored it and sampled array slice 0 for every
view — both eyes got the left image, i.e. **flat output with no disparity**.
That is a conformance defect independent of any engine integration: an app that
submits `arraySize > 1` with `imageArrayIndex` 0/1 has a right to expect the two
slices to be sampled.

## Decision

**Both addressing modes are first-class on every compositor backend (D3D11,
D3D12, Vulkan, OpenGL, Metal). The compositor honors whichever the app submits.**

1. **Slice selection happens at the single per-view atlas-blit sampling site,
   gated on `arraySize > 1`.** The single-layer path is byte-identical to before.
   Per backend:
   - D3D11 / D3D12 — a `TEXTURE2DARRAY` SRV with `FirstArraySlice =
     sub.array_index`, `ArraySize = 1`.
   - Vulkan — the blit source `subresourceRange.baseArrayLayer = sub.array_index`.
   - OpenGL — allocate `GL_TEXTURE_2D_ARRAY` when `arraySize > 1` and sample the
     layer via a `sampler2DArray` blit variant.
   - Metal — allocate a plain `MTLTextureType2DArray` (a single-plane IOSurface
     cannot back a multi-slice array; the native app gets the `id<MTLTexture>`
     directly, and cross-API Vulkan import of a layered swapchain is not a path)
     and sample the slice via a per-slice 2D texture view.

2. **The display-processor contract is unchanged.** `process_atlas()` always
   receives a single **flat tiled atlas** plus `tile_columns`/`tile_rows`
   ([swapchain-model](../specs/runtime/swapchain-model.md),
   [multiview-tiling](../specs/runtime/multiview-tiling.md)). Array resolution is
   entirely **upstream**, in the compositor. `u_tiling` stays array-blind by
   design — it describes the atlas, not the app's submission.

3. **Layered submissions never zero-copy.** Because the DP consumes a flat atlas
   and array slices are not tiles, a layered submission can never satisfy
   `u_tiling_can_zero_copy()` (ADR-030). One atlas blit per frame is the floor
   for array content — the same cost every PC-VR runtime pays. Every backend's
   zero-copy gate already excludes `array_index != 0`.

4. **Tiled remains the path for `view_count > 2`.** Layered is capped at the
   engine's efficient multiview count (typically 2); "support both" lets 2-view
   engine content use arrays while N-view light-field content uses the atlas. An
   app that requests a layered swapchain drives only modes whose `view_count`
   fits its slice count.

## Consequences

- **Engine interop is unblocked beyond D3D12.** A layered stereo app (Unity SPI,
  Unreal ISR, or any PC-VR-shaped app) weaves correctly on all five backends,
  not just D3D12.
- **Conformance.** The silent slice-0 output on D3D11/GL/Metal is gone; CTS
  `arraySize > 1` cases exercise the real slice.
- **True zero-copy (app → DP, zero blits) is off the table for arrays by
  design** — but "native-app parity" (app → swapchain → one atlas blit) is the
  achievable and correct target. The remaining copy for a cross-device engine
  provider is the device bridge, not the layout (bind the session to the engine's
  own device to remove it).
- **App-side per-slice depth discipline.** When a *test/demo* app renders the
  layered layout, it must clear depth **per slice** — all slices render
  full-viewport into the same depth buffer, so a once-per-frame clear lets slice
  1 z-test against slice 0's depth and the other eye's geometry punches a shadow
  through (the [#613] gotcha; observed as a subtle right-eye shadow). Tiled views
  are immune (disjoint depth regions). This is an authoring rule for apps, not a
  runtime concern; see [displayxr-app-rules](../guides/displayxr-app-rules.md).
- **Test coverage.** Array/tiled are both exercised per-backend: dedicated array
  apps `cube_zones_{d3d12,vk}_win`, and a `DISPLAYXR_ARRAY_LAYOUT=1` toggle on
  `cube_zones_{d3d11,gl}_win` and `cube_handle_{d3d11,gl}_win`
  (default off = tiled).

## References

- Assessment + per-backend gap analysis: [#681](https://github.com/DisplayXR/displayxr-runtime/issues/681)
- Compositor fixes: PR #682 (D3D11/GL/Metal), PR #656 (D3D12, prior)
- Demo toggles + per-slice depth fix: PR #683
- Layout contract: [swapchain-model.md § View addressing](../specs/runtime/swapchain-model.md)
- Crop / zero-copy law: [ADR-030](ADR-030-crop-before-dp-zero-copy-only-when-swapchain-equals-atlas.md)
- Worst-case swapchain sizing: [ADR-010](ADR-010-shared-app-iosurface-worst-case-sized.md)

[#613]: https://github.com/DisplayXR/displayxr-runtime/issues/613

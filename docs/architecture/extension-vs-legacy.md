# Extension Apps vs Legacy Apps

Orthogonal to the [four app classes](../getting-started/app-classes.md), apps are either **extension apps** or **legacy apps** based on whether they enable `XR_DXR_display_info`. This distinction affects how the runtime handles rendering modes, swapchain sizing, and mode switching.

## Comparison

| Aspect | Extension App | Legacy App |
|--------|--------------|------------|
| **Detection** | Enables `XR_DXR_display_info` | Does not enable `XR_DXR_display_info` |
| **Rendering modes** | Enumerates all modes, handles `XrEventDataRenderingModeChangedDXR` | Unaware of modes, always renders stereo |
| **Swapchain sizing** | `max(tileColumns[i] * scaleX[i] * displayW)` across all modes | `recommendedImageRectWidth * 2` (compromise scale) |
| **Mode switching** | All modes: V toggle + 1/2/3 direct selection | Only V toggle between mode 0 (2D) and mode 1 (default 3D) |

## Which Apps Are Which?

- `_handle` and `_texture` apps are **always extension apps** — they need the extension for window binding.
- `_hosted` apps can be either:
  - A DisplayXR-aware `_hosted` app enables `XR_DXR_display_info` → **extension app**
  - A generic OpenXR `_hosted` app (e.g. WebXR, third-party) → **legacy app**

> **Note on WebXR pages.** Chrome's native WebXR implementation does not enable `XR_DXR_display_info`, so a WebXR session is always a legacy app at the OpenXR level. However, a DisplayXR-aware web page can install the [WebXR Bridge v2](../roadmap/webxr-support.md) Chrome extension to read display info and rendering-mode events via a metadata sideband and override its `XRWebGLLayer` framebuffer dimensions — effectively behaving like an extension app from the developer's perspective while its underlying OpenXR session remains legacy. The runtime does not need to distinguish these cases; the legacy compromise branch still fires and is simply ignored by the page.

## Legacy App Compromise Scaling

Legacy apps don't know about rendering modes, so the runtime provides a **compromise scale** that works acceptably across modes. For SBS displays this is `0.5 × 1.0` (half-width, full-height).

The compromise scaling is computed in `oxr_system_fill_in()`. The `legacy_app_tile_scaling` flag on `xrt_system_compositor_info` disables 1/2/3 key mode selection for legacy apps (V toggle only).

See [ADR-006](../adr/ADR-006-legacy-app-compromise-view-scale.md) for the design rationale and [Legacy App Support](../specs/runtime/legacy-app-support.md) for the full algorithm (Case A/B).

## Runtime Behavior

The runtime detects which type of app it's dealing with at session creation time and adjusts:

1. **Swapchain dimensions** — reported via `xrEnumerateSwapchainFormats` / `recommendedImageRectWidth`
2. **Mode switching** — which keyboard shortcuts are active (V only vs V + 1/2/3)
3. **Event delivery** — `XrEventDataRenderingModeChangedDXR` only sent to extension apps
4. **Tile layout** — extension apps get the mode's native tile layout; legacy apps get a fixed compromise layout

## Further Reading

- [Multiview Tiling](../specs/runtime/multiview-tiling.md) — atlas layout algorithm
- [Legacy App Support](../specs/runtime/legacy-app-support.md) — full compromise scaling algorithm
- [XR_DXR_display_info](../specs/extensions/XR_DXR_display_info.md) — the extension specification

# ADR-010: Shared App IOSurface Worst-Case Sized

**Status:** Accepted
**Date:** 2026-03-16

## Context

In `_texture` apps, the runtime and app communicate via a shared IOSurface (macOS) or shared texture handle (Windows). The app renders into this surface; the compositor reads it, crops to the canvas rect, and feeds the display processor.

We initially sized the IOSurface to match the canvas (the sub-rect of the window where 3D content appears) and added dynamic recreation on canvas resize:

- `xrUpdateSharedSurfaceEXT` OpenXR function
- `comp_metal_compositor_update_shared_iosurface` compositor function
- Per-frame size comparison + recreation logic in test apps

## Decision

**IOSurface = swapchain size (worst-case atlas across all rendering modes), allocated once at session creation.**

The canvas rect is communicated separately via `xrSetSharedTextureOutputRectEXT` (part of the [window binding extensions](../specs/extensions/XR_EXT_win32_window_binding.md#35-xrsetsharedtextureoutputrectext)), which the compositor uses for Kooima FOV calculation and view sizing. The IOSurface itself never needs to be resized.

## Rationale

1. **Negligible memory cost.** A 2560x1440 BGRA8 IOSurface is ~14 MB. Allocating at worst-case atlas size wastes at most a few MB compared to canvas-sized.

2. **Zero compute savings from smaller IOSurface.** The compositor already sizes views to the canvas rect regardless of IOSurface dimensions. The display processor renders into the target swapchain (compositor-owned), decoupled from the IOSurface size. A smaller IOSurface saves no GPU work.

3. **Unnecessary API surface removed.** `xrUpdateSharedSurfaceEXT` added a new OpenXR function, a compositor method, and per-frame resize logic in every `_texture` app --- all for zero benefit.

4. **Reduced fragility.** Dynamic IOSurface recreation introduces race conditions between the app's render thread and the compositor's read. A fixed-size surface eliminates this class of bugs.

## Rejected Alternative

**Dynamic IOSurface recreation on canvas resize.** This was the original implementation. It required `xrUpdateSharedSurfaceEXT`, per-frame size checks, and careful synchronization. The complexity was not justified by any measurable benefit.

## Read-Back Contract

The compositor writes interlaced/composited output at offset **`(canvasX, canvasY)`** inside the IOSurface, sized `canvasW × canvasH` — matching the rect the app passed to `xrSetSharedTextureOutputRectEXT`. The remainder of the surface is undefined.

```
IOSurface (swapchain-sized, e.g. 3024×1964)
┌────────────────────────────┐
│    unused                  │
│   ┌────────────┐           │
│   │ Valid      │           │
│   │ (canvasW × │  unused   │
│   │  canvasH)  │           │
│   │ at         │           │
│   │ (canvasX,  │           │
│   │  canvasY)  │           │
│   └────────────┘           │
│    unused                  │
└────────────────────────────┘
```

**App-side blit:** Sample the canvas sub-rect of the IOSurface. Suggested UV math:

```
uvScale  = (canvasW / surfaceW, canvasH / surfaceH)
uvOffset = (canvasX / surfaceW, canvasY / surfaceH)
sampled  = texture.sample(uvOffset + uv * uvScale)
```

Letterbox using the **canvas** aspect ratio, not the IOSurface aspect ratio.

**The app already knows the canvas rect** — it set it via `xrSetSharedTextureOutputRectEXT`. No runtime-to-app query API is needed. The contract is symmetric: the app sends `(x, y, w, h)`; the runtime writes `(w × h)` at `(x, y)`; the app reads `(w × h)` from `(x, y)`.

**Why `(canvasX, canvasY)` and not origin:** Vendor weavers (e.g. Leia SR) rely on the viewport position inside the backbuffer matching the eventual screen-space position within the window to compute correct interlacing phase. Writing at `(canvasX, canvasY)` keeps the on-display pixel position of the weaved output stable across backbuffer-vs-HWND size differences, and the symmetric read-back means the app's blit re-aligns the content to the same HWND client coords it passed in. This eliminates the need for an HWND-sized intermediate texture on Windows and lets drag-resize be driven purely by changing `canvas.x/y/w/h` per frame while the shared texture stays fixed.

## Consequences

- IOSurface is created once at worst-case swapchain dimensions (`max(tileColumns * viewWidth)` × `max(tileRows * viewHeight)` across all rendering modes, assuming canvas = full window = full display).
- `xrUpdateSharedSurfaceEXT` and `comp_metal_compositor_update_shared_iosurface` are removed from the codebase.
- `xrSetSharedTextureOutputRectEXT` remains and must be called per-frame when the canvas moves or resizes.
- Future `_texture` apps on other platforms (Windows shared texture) should follow the same pattern: allocate at display size, communicate canvas rect separately.

# ADR-010: Shared App IOSurface Worst-Case Sized

**Status:** Accepted
**Date:** 2026-03-16

## Context

In `_shared` apps, the runtime and app communicate via a shared IOSurface (macOS) or shared texture handle (Windows). The app renders into this surface; the compositor reads it, crops to the canvas rect, and feeds the display processor.

We initially sized the IOSurface to match the canvas (the sub-rect of the window where 3D content appears) and added dynamic recreation on canvas resize:

- `xrUpdateSharedSurfaceEXT` OpenXR function
- `comp_metal_compositor_update_shared_iosurface` compositor function
- Per-frame size comparison + recreation logic in test apps

## Decision

**IOSurface = display size (worst case), allocated once at session creation.**

The canvas rect is communicated separately via `xrSetSharedTextureOutputRectEXT`, which the compositor uses for Kooima FOV calculation and view sizing. The IOSurface itself never needs to be resized.

## Rationale

1. **Negligible memory cost.** A 2560x1440 BGRA8 IOSurface is ~14 MB. Allocating at display size (worst case) wastes at most a few MB compared to canvas-sized.

2. **Zero compute savings from smaller IOSurface.** The compositor already sizes views to the canvas rect regardless of IOSurface dimensions. The display processor renders into its own output (e.g., Leia SR's hidden HWND), decoupled from the IOSurface size. A smaller IOSurface saves no GPU work.

3. **Unnecessary API surface removed.** `xrUpdateSharedSurfaceEXT` added a new OpenXR function, a compositor method, and per-frame resize logic in every `_shared` app --- all for zero benefit.

4. **Reduced fragility.** Dynamic IOSurface recreation introduces race conditions between the app's render thread and the compositor's read. A fixed-size surface eliminates this class of bugs.

## Rejected Alternative

**Dynamic IOSurface recreation on canvas resize.** This was the original implementation. It required `xrUpdateSharedSurfaceEXT`, per-frame size checks, and careful synchronization. The complexity was not justified by any measurable benefit.

## Consequences

- IOSurface is created once at `app.displayPixelWidth x displayPixelHeight` (falling back to initial canvas size if display info is unavailable).
- `xrUpdateSharedSurfaceEXT` and `comp_metal_compositor_update_shared_iosurface` are removed from the codebase.
- `xrSetSharedTextureOutputRectEXT` remains and must be called per-frame when the canvas moves or resizes.
- Future `_shared` apps on other platforms (Windows shared texture) should follow the same pattern: allocate at display size, communicate canvas rect separately.

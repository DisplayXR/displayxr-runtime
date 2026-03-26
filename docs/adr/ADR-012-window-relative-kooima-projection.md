# ADR-012: Window-Relative Kooima Projection

**Status:** Accepted
**Date:** 2026-03-26

## Context

When an app runs windowed (not fullscreen), the Kooima projection must
account for the fact that the visible screen area is smaller than the
physical display and may be offset from its center.

The original approach computed a viewport scale factor:

```
vs = min(display_dim) / min(window_dim)
screen = {window_width_m * vs, window_height_m * vs}
perspective_factor *= vs
```

This artificially scaled the screen dimensions back toward the full display
size and multiplied the eye position scaling factor to compensate.  While
correct in output, the indirection obscured the geometric intent and
required every call site to carry the `vs` computation.

## Decision

Replace the viewport-scale approach with **window-relative Kooima**:

1. **Screen dimensions** = actual physical window size (meters).
   No scaling; Kooima is told the screen is exactly the window.

2. **Eye positions** are shifted by the window center offset so they
   are relative to the window center, not the display center.
   The eye tracker reports positions relative to display center; we
   subtract the window center offset (already computed by each
   compositor in `xrt_window_metrics`).

3. **`perspective_factor`** is no longer multiplied by `vs`.
   The `m2v` factor (`virtual_display_height / screen_height_m`)
   naturally increases as the window shrinks, producing the correct
   perspective scaling without manual intervention.

### Why this works

Kooima projection computes an asymmetric frustum from the eye's position
relative to the screen center and the screen's physical half-extents.
If the "screen" is the window:

- The frustum angles match the physical subtended angles of the window
  as seen by the viewer — geometrically correct.
- Moving the window off-center shifts the eye offset, producing the
  correct asymmetric frustum for the new window position.
- The `m2v` ratio grows for smaller windows, naturally amplifying the
  eye displacement in virtual units — matching the stronger perspective
  effect of a smaller physical viewport.

### Applies to both projection pipelines

The eye offset is subtracted from `raw_eyes[]` before branching into
display-centric or camera-centric paths.  Both receive window-relative
eye positions automatically.  For camera-centric, the screen dimensions
only affect aspect ratio, which is correct as-is.

## Edge cases

| Scenario | Offset | Screen | Behavior |
|----------|--------|--------|----------|
| Fullscreen | (0, 0) | display dims | Identity — unchanged |
| No window metrics | (0, 0) | display dims (fallback) | Same as fullscreen |
| Centered window | (0, 0) | window dims | Symmetric frustum, correct |
| Off-center window | nonzero | window dims | Asymmetric frustum matching physical geometry |

## Consequences

- Removes ~5 lines of `vs` computation from every app and the runtime
- Apps need window screen position (Win32: `ClientToScreen` + `MonitorFromWindow`;
  macOS: `NSWindow.frame` + `NSScreen.frame`)
- Hosted / hosted_legacy apps are unaffected — they receive FOVs from
  `xrLocateViews`, which uses the updated runtime-side logic

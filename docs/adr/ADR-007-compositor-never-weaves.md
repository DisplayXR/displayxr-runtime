---
status: Accepted
date: 2026-03-05
source: "#78"
---
# ADR-007: Compositor Never Weaves

## Context
In the original architecture, some compositor code contained interlacing/weaving logic. This violated the separation of concerns between compositing (layer accumulation) and display processing (format conversion).

## Decision
Weaving/interlacing is exclusively the display processor's responsibility. Compositors call `process_atlas()` and never contain vendor-specific display format logic.

The compositor's contract with the DP is purely geometric: hand it the render target, full target dimensions, and the canvas sub-rect within that target (`canvas_offset_x/y, canvas_width, canvas_height`). The compositor must not assume anything about how a particular vendor's weaver consumes those parameters — including, but not limited to:

- Whether the DP requires `backbuffer == HWND client area`
- Whether the DP needs the canvas at the texture origin
- Whether the DP wants its own intermediate scratch buffer
- Whether the DP cares about phase alignment, pixel pitch, or display orientation

If a vendor needs an internal intermediate texture, dimension snapping, or extra synchronisation, it lives **inside that vendor's DP**, never in the compositor. If a vendor's DP cannot work with the compositor-supplied target, the fix is a DP-side workaround (or a DP vtable extension), not a compositor branch.

This rule is what lets us add a third or fourth display vendor without auditing every native compositor.

## Consequences
Clear boundary: compositor owns layer compositing and atlas creation; display processor owns atlas-to-display conversion for all advertised modes, including 2D (view_count=1). New display formats never require compositor changes. How each vendor handles 2D (native blit, shader passthrough, etc.) is implementation-defined.

Historically the D3D11 native compositor briefly carried a Leia-shaped workaround (HWND-sized intermediate texture + `CopySubresourceRegion`) to accommodate an assumption that the SR weaver needed `backbuffer == HWND`. That workaround was removed once Leia confirmed the weaver handles `backbuffer > HWND` via viewport positioning (issue #85). Future contributors: do not reintroduce that pattern. If a vendor's weaver constrains backbuffer geometry, push the constraint into the vendor's DP — not the compositor.

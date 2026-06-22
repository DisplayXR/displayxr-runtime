---
status: Accepted
date: 2026-06-21
issues: [634]
supersedes-mechanism-of: [ADR-027]
---
# ADR-031: Remove the 2D-surround / output-rect mechanism — display-zones is the sole region paradigm

## Context

The runtime carried **two** mechanisms for expressing 2D-vs-3D regions in a window:

1. **Legacy "2D surround / output rect"** — `xrSetSharedTextureOutputRectEXT` (confine
   weaved 3D to one canvas sub-rect) + `xrSetSharedTextureSurround2DEXT` /
   `…Surround2DFenceEXT` (fill the complement with a monolithic full-window 2D shared
   texture, via a bespoke per-API strip blit + keyed-mutex/fence sync).
2. **Canonical `XR_EXT_display_zones` (ADR-027)** — N 3D zones (`XrDisplayZoneEXT`, each
   rect + rig + swapchain) + M 2D zones (`XrCompositionLayerLocal2DEXT`) + a per-pixel
   wish mask (`XrDisplayZonesFrameEndInfoEXT`).

(1) is a strict, less-capable special case of (2): an output rect ≡ one 3D zone; a 2D
surround ≡ one Local2D zone covering the complement (spec §6 degenerate mapping). Keeping
both doubled the compositor's 2D-fill code and the app-facing API surface.

The #634 deprecation retired (1) in five steps: deprecation markers → a translation shim
(`DISPLAYXR_SURROUND_SHIM`) that rerouted legacy surround frames through the canonical
unified-mask composite → flip the shim default ON (ADR-031's predecessor work; shim +
`u_capability` gate) → migrate first-party apps to native zones (the `cube_zones_texture_*`
apps) → **this step: delete the legacy mechanism entirely.** Each step was validated on real
Leia SR hardware (D3D11 + D3D12) and on macOS/Metal. Full history:
`docs/roadmap/surround-zones-deprecation.md` (this ADR's background).

## Decision

**`XR_EXT_display_zones` is the single canonical model for all 2D/3D region expression, for
every app class.** The 2D-surround / output-rect mechanism is removed:

- The three entry points (`xrSetSharedTextureOutputRectEXT`,
  `xrSetSharedTextureSurround2DEXT`, `xrSetSharedTextureSurround2DFenceEXT`) are deleted from
  `XR_EXT_win32_window_binding` / `XR_EXT_cocoa_window_binding` (spec version bumped) and from
  the oxr dispatch.
- The bespoke surround compositor code (strip blit, surround shader, keyed-mutex/fence
  surround sync, `u_surround_2d_handle`) is deleted from all five native compositors.
- The surround→zones translation shim and its `u_capability` capability gate — transitional
  scaffolding only — are deleted.
- The legacy `cube_texture_{d3d11_win, d3d12_win, metal_macos}` test apps are deleted; the
  `cube_zones_texture_*` apps are the canonical texture-class coverage.

**App class (handle / texture / hosted) is orthogonal to region expression.** It governs only
who owns the output surface (app window / shared texture / runtime window). Zones is how *all*
of them express regions. A plain full-window app is the degenerate one-zone case: an unchained
`xrLocateViews` + `XrDisplayRigEXT` frames the full window (so single-canvas apps need no
explicit zone chain); `XR_EXT_view_rig` v2 + `XR_EXT_local_3d_zone` v3 remain that documented
path.

## Consequences

- **External apps** that called the surround/output-rect entry points stop resolving them
  (`xrGetInstanceProcAddr` returns null); the spec-version bump signals the removal. This is
  the accepted, deprecation-gated outcome.
- **The `canvas` concept is retained as runtime-internal infrastructure** (`u_canvas_rect`,
  `*_effective_canvas`, `u_canvas_apply_to_metrics`, view-sizing, DP `canvas_offset/size`,
  Kooima). Only the app-facing *setter* (output-rect) is gone; the canvas now derives from the
  window (full-window default) and zones carve sub-regions. Folding the now-always-full-window
  canvas field into plain window dims is a tracked follow-up, not part of this change.
- **ADR-010** (worst-case-sized shared surface) keeps its decision; its mechanism prose is
  updated — the canvas rect is no longer app-set via output-rect.
- **ADR-027** is unchanged in body (it *added* zones; it did not decide to remove surround) —
  it gains a one-line forward pointer here. The surround references in its Context/Consequences
  are accurate history.

## Rejected alternative

**Keep output-rect, delete only the two surround calls.** Rejected: output-rect's sole role —
confine 3D to a sub-rect — *is* "one 3D zone." Retaining it would preserve a second, redundant
way to say what a zone already says, defeating the convergence. The `cube_zones_texture_*` apps
already run without ever calling output-rect (hardware-validated), proving the canvas-derives-
from-window path is sufficient.

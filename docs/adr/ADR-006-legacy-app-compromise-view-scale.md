---
status: Accepted
date: 2026-03-05
source: "#79"
---
# ADR-006: Legacy App Compromise View Scale

## Context
Legacy OpenXR apps (no `XR_DXR_display_info`) don't know about rendering modes. They create swapchains once at session start. Need a compromise resolution that works for both 2D (1 view, full res) and 3D (2+ views, scaled).

## Decision
For SBS displays (view_count==2, scaleX<=0.5, scaleY<=0.5), report `recommendedViewScale = 0.5x1.0`. App renders half-width tiles. Compositor handles scaling differences between modes. For other configurations, use the 3D mode's actual scale.

## Consequences
Legacy apps render at a reasonable resolution without mode awareness. Some quality compromise vs. mode-aware apps. `legacy_app_tile_scaling` flag signals compositors to handle the difference.

## Amendment 1 (#1510): the mode this is computed FROM
"The 3D mode" above was read as the *active* mode. A legacy session submits a fixed two views, so an active mode with more than two views has more tiles than the app can paint: the fallback branch sized the app for that mode's per-view scale and the compositor's under-submit clamp left the remaining tiles at the clear colour (measured on sim-display Quad — 4 views, 2×2 — where a legacy app lost half the canvas to a mode it cannot see).

The decision is unchanged; what it applies to is now the mode the session will actually **run** in. A legacy session does not run in a mode it cannot fill: the runtime picks a fillable mode (`view_count ≤ 2`) before the app is sized and `xrBeginSession` moves the display there, so the scale, the mode, the compositor grid and the DP agree. The branch above is deliberately *not* widened to `view_count >= 2` — a `0.5 × 1.0` scale only makes sense on a 2×1 grid, while the compositor and the DP stay on the mode's own grid (ADR-030 / the per-backend `compute_effective_layout()` contract), so widening it would trade unpainted tiles for a wrong-stride weave. A device that PINS its mode, and service mode (where the panel lease owns it), outrank the floor; there the original fallback stands and `xrCreateSession` warns. Rule: `src/xrt/state_trackers/oxr/oxr_legacy_mode_rule.h`.

> **Note (#1499).** The rule header keeps its `oxr_legacy_mode_rule.h` name but is no longer legacy-only: the pick is parameterised on how many views the session can submit, and the `oxr_legacy_*` entry points are thin wrappers binding that to 2. This ADR's decision is unchanged and still legacy-only — the compromise view scale exists because a legacy app cannot be told about modes, which has nothing to do with an extension app. What #1499 generalises is only *which mode the session runs in*. See [View-Configuration Model](../reference/view-configuration-model.md#the-mode-floor-1499).

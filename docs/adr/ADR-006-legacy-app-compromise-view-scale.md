---
status: Accepted
date: 2026-03-05
source: "#79"
---
# ADR-006: Legacy App Compromise View Scale

## Context
Legacy OpenXR apps (no `XR_EXT_display_info`) don't know about rendering modes. They create swapchains once at session start. Need a compromise resolution that works for both 2D (1 view, full res) and 3D (2+ views, scaled).

## Decision
For SBS displays (view_count==2, scaleX<=0.5, scaleY<=0.5), report `recommendedViewScale = 0.5x1.0`. App renders half-width tiles. Compositor handles scaling differences between modes. For other configurations, use the 3D mode's actual scale.

## Consequences
Legacy apps render at a reasonable resolution without mode awareness. Some quality compromise vs. mode-aware apps. `legacy_app_tile_scaling` flag signals compositors to handle the difference.

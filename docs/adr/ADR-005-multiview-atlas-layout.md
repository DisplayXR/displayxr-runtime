---
status: Accepted
date: 2026-03-01
source: "#77"
---
# ADR-005: Multiview Atlas Layout

## Context
Displays support varying view counts (1 for 2D, 2 for stereo, 4+ for multiview). Need a consistent layout for passing multiple views from compositor to display processor.

## Decision
Use row-major atlas layout with ceil(sqrt(view_count)) columns. Views are tiled left-to-right, top-to-bottom into a single texture. The atlas dimensions and tile layout (tile_columns, tile_rows) are passed to `process_atlas()`.

## Consequences
Single texture pass to display processor regardless of view count. Compositors use a uniform tiling function. Display processors receive atlas + layout metadata.

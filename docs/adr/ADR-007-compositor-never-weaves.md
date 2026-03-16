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

## Consequences
Clear boundary: compositor owns layer compositing and atlas creation; display processor owns atlas-to-display conversion for all advertised modes, including 2D (view_count=1). New display formats never require compositor changes. How each vendor handles 2D (native blit, shader passthrough, etc.) is implementation-defined.

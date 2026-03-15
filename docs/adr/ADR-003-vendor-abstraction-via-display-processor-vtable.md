---
status: Accepted
date: 2026-02-28
source: "vendor_abstraction_refactor.md §9"
---
# ADR-003: Vendor Abstraction via Display Processor Vtable

## Context
Vendor-specific code (LeiaSR `#ifdef` blocks) was scattered across compositor and state tracker files (~60 blocks). A new vendor would need to add their own `#ifdef` blocks to the same files.

## Decision
Create `xrt_display_processor` vtable interfaces (one per graphics API). Eye tracking, weaving, window metrics, and display mode control are methods on this vtable. Display processor factories are registered on `xrt_system_compositor_info` by drivers. Compositors instantiate display processors generically.

## Consequences
Zero vendor `#ifdef` blocks in compositor or state tracker code. New vendors add files only under `src/xrt/drivers/<vendor>/` and `src/xrt/targets/common/`. All ~60 LeiaSR `#ifdef` blocks removed from core.

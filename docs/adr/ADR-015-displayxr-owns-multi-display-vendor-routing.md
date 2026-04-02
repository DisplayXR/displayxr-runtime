---
status: Accepted
date: 2026-04-02
source: "#69"
---
# ADR-015: DisplayXR Owns Multi-Display Vendor Routing

## Context

The current system selects a single vendor driver globally via the builder priority system in `p_prober.c`. The winning builder populates one set of display processor (DP) factories on `xrt_system_compositor_info` — every compositor instance gets the same vendor DP regardless of which physical monitor its window is on.

This breaks when a machine has multiple 3D displays from different vendors (e.g., Leia on monitor A, a future vendor on monitor B), or when a developer wants to force `sim_display` on one specific monitor while using the real vendor DP on another.

Investigation of the Leia SR SDK revealed that it internalizes multi-monitor handling: `getDrawRegions()` splits a window into 3D/2D regions per monitor, `canWeave()` auto-detects whether the window is on a 3D display, `installCustomWindowProc()` intercepts window positioning for phase alignment, and two background polling threads monitor display configuration changes. There is currently no flag to disable any of this behavior.

Two options were evaluated:

- **Option A — Vendor SDKs handle it**: Each SDK manages its own display detection. Problem: vendor SDKs only know about their own displays and cannot coordinate routing across vendors. Also prevents configurations like sim_display-on-monitor-A + Leia-on-monitor-B.
- **Option B — DisplayXR handles it**: The runtime maintains a per-display DP factory registry, determines which monitor a window is on, and selects the correct DP. Vendor SDKs are told to treat the entire window as being on a 3D display (skip internal monitor detection).

## Decision

DisplayXR owns multi-display vendor DP routing (Option B). This extends ADR-003's vendor abstraction principle: vendors own their DP implementations, but DisplayXR owns the routing of which DP is instantiated for which compositor/window/display combination.

Specifically:

1. **Vendor display probe interface**: Each vendor driver provides a `probe_displays()` function that identifies which OS monitors it recognizes as its hardware. Vendors use proprietary detection methods — e.g., Leia uses EDID matching (hardcoded manufacturer+product ID table) combined with an FPC (Front Projection Controller) USB handshake that provides device serial numbers via shared memory. DisplayXR does not need to understand these mechanisms; it only needs the result: a list of claimed monitors with confidence levels.
2. **DP factory registry**: Replace the scalar `dp_factory_*` fields on `xrt_system_compositor_info` with a per-display registry that maps monitor identifiers to per-API factory sets, built from vendor probe results. When multiple vendors claim the same monitor, the claim with the highest confidence wins (or is overridden by user config). Single-display systems use a single-entry registry (backward compatible).
3. **Per-compositor multi-DP support**: A single compositor may hold multiple DP instances simultaneously — one per display that the window overlaps. The compositor determines which displays the window spans, creates a DP for each, and routes the correct atlas sub-region to each DP.
4. **Split-weave for spanning windows**: When a window spans multiple displays, the compositor splits the atlas at the display boundary and calls each DP's `process_atlas()` with the sub-region corresponding to that display. Each DP weaves its portion independently. The compositor composites the results back into the window's output. This is a first-class use case — tiled multi-monitor 3D configurations are shipping products. Acer's ["Panoramic View"](https://spatiallabs.acer.com/developer/docs/2299cdda-f90f-11ed-b3b8-067bb43818a8/f0b4e145-f433-4411-b30c-88ffa00add90) combines three SpatialLabs Pro displays into one wider viewing area, each weaved independently but presenting a continuous 3D scene with a widened viewing frustum.
5. **DP lifecycle management**: DPs are created/destroyed as the window's display coverage changes. Moving a window fully from monitor A to monitor B destroys DP-A and creates DP-B. Sliding a window across a boundary temporarily holds both.
6. **Per-display eye tracking via DP vtable**: Camera-to-display pairing is vendor-specific hardware (e.g., Leia's FPC bundles display + camera + calibration as one unit). DisplayXR does not manage this pairing. Each DP instance is already per-display, and `get_predicted_eye_positions()` on the DP vtable returns eye positions calibrated to that specific display. The vendor probe must include enough identity (e.g., FPC serial) for the DP factory to create a DP tied to the correct camera/calibration. In multi-display setups, this means multiple simultaneous eye tracking streams — one per DP.
7. **Vendor SDK EXTERNAL_ROUTING mode**: Vendor SDKs must support a mode where internal monitor polling, window message interception, and canWeave checks are disabled. The SDK treats the entire input as being on its target 3D display and always produces interlaced output. DisplayXR owns the split decision and region routing externally.

## Consequences

- Any combination of vendor displays works on a single machine without conflicts.
- A single window can span multiple 3D displays, each weaved by the correct vendor DP — enabling tiled/stacked multi-monitor 3D setups.
- `sim_display` can be forced on a specific monitor via per-display configuration, even when a vendor SDK claims that monitor.
- Same-vendor multi-display works (e.g., two Leia monitors, force sim on one).
- Requires vendor SDK changes — Leia must add an EXTERNAL_ROUTING mode. This is a blocking external dependency for the Leia-specific multi-display path, but sim_display works immediately (no internal monitor detection).
- Adds significant complexity to native compositors: each must implement display-boundary detection, atlas splitting, per-region DP dispatch, and result compositing. This logic should live in a shared helper (`comp_base` or a new `comp_multi_display` utility) to avoid duplicating across all 5 API-specific compositors.
- The `process_atlas()` canvas parameters (`canvas_offset_x/y`, `canvas_width/height`) already support sub-region rendering, which is the mechanism for per-display atlas splitting.
- Single-display systems pay no cost (single-entry registry, single DP, no splitting).

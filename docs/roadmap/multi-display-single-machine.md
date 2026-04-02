---
status: Proposal
owner: David Fattal
updated: 2026-04-02
issues: [69]
code-paths: [src/xrt/drivers/, src/xrt/compositor/multi/]
---

> **Status: Proposal** — not yet implemented. Tracking issue: [#69](https://github.com/DisplayXR/displayxr-runtime-pvt/issues/69)

# Multi-Display Compositing on a Single Machine

## Summary

Extend the multi-compositor to route layers to multiple physical displays connected to the same machine. Each display has a known pose in a shared room-scale coordinate system.

## Scope and Related Docs

This doc extends the spatial OS compositor to **multiple physical displays on one machine**. It assumes the single-display compositing pipeline and shell already work.

| Doc | Relationship |
|-----|-------------|
| [spatial-os.md](spatial-os.md) (#43) | **Single-display compositing.** The multi-compositor pipeline this doc extends. Prerequisite. |
| [3d-shell.md](3d-shell.md) (#44) | **Window manager.** Manages window placement; this doc adds display-routing beneath it. Prerequisite. |
| [multi-display-networked.md](multi-display-networked.md) (#70) | **Networked extension.** Generalizes this doc's multi-display routing across machines. Depends on this doc. |

## Architecture

```
                                    +-> native compositor -> Display A (pose A)
Apps -> IPC -> multi-compositor ----+-> native compositor -> Display B (pose B)
                                    +-> native compositor -> Display C (pose C)
```

One `displayxr-service` process discovers and manages all local displays. The multi-compositor treats each as a separate render target with its own pose.

## Tasks

### Display Enumeration & Compositing
- [ ] Enumerate multiple physical displays from a single service instance (multiple Leia/sim_display devices)
- [ ] Define per-display pose configuration (JSON config or calibration)
- [ ] Route compositor layers to the correct display(s) based on window pose vs display frustum
- [ ] Handle windows spanning multiple displays (split compositing)
- [ ] Frame synchronization across local displays

### Vendor Display Processor Routing

When multiple displays are from different vendors, each compositor must instantiate the correct vendor's DP. A single window may span multiple displays — a first-class use case for tiled 3D monitor configurations. Acer's ["Panoramic View"](https://spatiallabs.acer.com/developer/docs/2299cdda-f90f-11ed-b3b8-067bb43818a8/f0b4e145-f433-4411-b30c-88ffa00add90) ships exactly this: three SpatialLabs Pro displays combined into one wider 3D viewing area, each weaved independently but presenting a continuous scene. The current single-factory-per-API model (`dp_factory_vk`, `dp_factory_d3d11`, etc. on `xrt_system_compositor_info`) does not support this. See [ADR-015](../adr/ADR-015-displayxr-owns-multi-display-vendor-routing.md).

**Multi-app (IPC) path** — one native compositor per display, each with its own DP:
```
                                    +-> native compositor A -> DP(Leia)  -> Display A
Apps -> IPC -> multi-compositor ----+-> native compositor B -> DP(sim)   -> Display B
                                    +-> native compositor C -> DP(Leia)  -> Display C
```

**Single-app (in-process) path** — one compositor holds multiple DPs, splits atlas at display boundaries:
```
                      +--> region A --> DP(Leia)  --> Display A
App -> compositor --->|
                      +--> region B --> DP(Leia)  --> Display B

  Window spans Display A (top) + Display B (bottom)
  Compositor splits atlas at boundary, routes each region to correct DP
```

```
              DP Factory Registry:
                monitor-A -> { vk: leia_vk, d3d11: leia_d3d11, ... }
                monitor-B -> { vk: sim_vk,  d3d11: sim_d3d11,  ... }
                monitor-C -> { vk: leia_vk, d3d11: leia_d3d11, ... }
```

**Vendor display probe (hardware identification):**

Each vendor driver must tell DisplayXR which OS monitors it recognizes. Vendors use proprietary detection — e.g., Leia uses EDID matching (hardcoded manufacturer+product ID table from the monitor's registry data) plus an FPC USB handshake (serial number via `Global\sharedDeviceSerialMemory`). DisplayXR doesn't need to understand these mechanisms; it only needs the result.

- [ ] Define `xrt_display_vendor` interface with `probe_displays()` — returns list of `(os_monitor_id, confidence, serial)` claims
- [ ] Implement `probe_displays()` for Leia driver (wraps `getKnownMonitors()` + FPC shared memory read)
- [ ] Implement `probe_displays()` for sim_display (claims all unclaimed monitors, lowest confidence)
- [ ] Conflict resolution: when multiple vendors claim same monitor, highest confidence wins (or user override)

**Eye tracking in multi-display setups:**

Camera-to-display pairing is vendor-internal (e.g., Leia's FPC bundles display + camera + calibration as one hardware unit). DisplayXR does not manage this pairing — each DP instance provides `get_predicted_eye_positions()` calibrated to its specific display. The vendor probe must include enough identity (e.g., FPC serial) so the DP factory creates a DP tied to the correct camera/calibration.

- [ ] Extend DP factory signatures to accept display identity from probe (serial/claim data), so vendor can bind to correct camera
- [ ] Verify existing `get_predicted_eye_positions()` contract works per-display (each DP returns eyes relative to its own display)
- [ ] **External dep**: Leia SDK must support multiple simultaneous eye trackers for multi-FPC setups (currently single-primary only)

**sim_display defaults for unclaimed (2D) monitors:**
- [ ] Add `"2d"` as valid `SIM_DISPLAY_OUTPUT` value (#112)
- [ ] Default sim_display to 2D mode (`view_count=1`) when acting as fallback on unclaimed monitors
- [ ] Keep `SIM_DISPLAY_OUTPUT=sbs|anaglyph|blend` for dev/debug override

**Registry & routing:**
- [ ] Define `xrt_dp_factory_registry` struct (map from OS monitor ID to per-API factory set), built from vendor probe results
- [ ] Populate registry at system init: run all vendor probes, resolve claims, assign factories
- [ ] Modify compositor creation to look up DP factory from registry by monitor (instead of scalar `xsysc->info.dp_factory_*`)
- [ ] Per-display override configuration (force sim_display on a specific monitor)
- [ ] **External dep**: Leia SDK EXTERNAL_ROUTING mode — disable internal monitor polling, WndProc hook, canWeave checks (#111)

**Split-weave for spanning windows:**
- [ ] Detect which displays a window overlaps (monitor enumeration + window rect intersection)
- [ ] Split atlas at display boundaries — compute sub-regions using `canvas_offset_x/y`, `canvas_width/height`
- [ ] Hold multiple DP instances per compositor (one per overlapped display), manage lifecycle as coverage changes
- [ ] Route each atlas sub-region to the correct DP's `process_atlas()`
- [ ] Composite weaved sub-regions back into the window output
- [ ] Shared split-weave helper in `comp_base` or new `comp_multi_display` utility (avoid duplicating across all 5 native compositors)

**Testing:**
- [ ] Integration test: single window spanning two displays, each weaved by correct DP
- [ ] Integration test: Leia on monitor A + sim_display on monitor B, verify correct DP routes to each
- [ ] Test DP hot-swap: drag window fully from one monitor to another

## Dependencies

- #43 (multi-compositor)
- #44 (spatial window manager)
- ADR-015 (vendor DP routing)

## Context

This is step 2a of the multi-display architecture. All displays are local to one machine, one process, shared memory -- no network involved.

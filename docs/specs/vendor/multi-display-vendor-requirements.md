---
status: Active
owner: David Fattal
updated: 2026-06-11
issues: [69, 111]
adr: ADR-015
code-paths:
  - src/xrt/include/xrt/xrt_plugin.h
  - src/xrt/include/xrt/xrt_display_processor_d3d11.h
  - src/xrt/compositor/util/comp_dp_factory.h
  - src/xrt/targets/common/target_plugin_loader.c
---

# Vendor SDK Requirements for Multi-Display Routing

## Summary

DisplayXR owns multi-display vendor display-processor (DP) routing per
[ADR-015](../../adr/ADR-015-displayxr-owns-multi-display-vendor-routing.md):
the runtime decides which DP is instantiated for which monitor, splits a
window's atlas at display boundaries, and creates/destroys DPs as a window
moves between displays. For this to work with a vendor SR SDK (Leia/Dimenco
being the first), the **underlying weaving SDK** must stop doing its own
parallel monitor detection and trust the runtime's routing.

This document is the stable, versioned statement of **what the runtime needs
from the vendor SR SDK** — the artifact to hand the vendor team. It supersedes
the original "`EXTERNAL_ROUTING` flag" framing in issue #111; the contract is
now driven by **whether `window_handle` is `NULL`**, not by a flag.

> **Two layers, don't conflate them.** Our `displayxr-leia-plugin` (we own it)
> *wraps* the third-party Dimenco SR SDK (`DimencoWeaving` / `srDirectX`). The
> requirements below are about behavior baked into the **SR SDK** that the
> plugin cannot work around — today the plugin passes `window_handle` straight
> into `leiasr_d3d11_create(...)`, so whatever the SDK does internally is out
> of our hands.

## What the runtime already provides (so the vendor doesn't have to ask)

These landed on the runtime/plugin side already; they are the contract surface
the SDK requirements below plug into:

| Capability | Where | Status |
|---|---|---|
| `probe_displays()` vtable entry + `struct xrt_display_claim {monitor_id, confidence, supported_apis, serial[64]}` | `xrt_plugin.h` | **Done** |
| Per-monitor DP factory registry built from probe results | `target_plugin_loader.c`, `comp_dp_factory.h` | **Done (Phase 3a: single-display routes through registry)** |
| `create_dp_<api>(... window_handle ...)` — `window_handle` documented "may be `NULL`" on every API | `xrt_display_processor_<api>.h` | **Done** |
| Phase derived from `canvas_offset_x/y` in `process_atlas()` (D3D11) | `displayxr-leia-plugin/src/drv_leia/leia_display_processor_d3d11.cpp` | **Done** |
| EDID + SR-install + SR-service three-layer display identification | `displayxr-leia-plugin/src/drv_leia/{leia_edid_probe.c, leia_sr_probe.cpp}` | **Done** |

Runtime work still ahead (ours, **not** a vendor dependency): ADR-015
**Phase 3b** — real per-window monitor id, simultaneous multi-DP per
compositor, atlas split-weave for spanning windows, primary/secondary HWND
lifecycle.

## Requirements (what we need from the vendor SR SDK)

### R1 — Trust the runtime's routing even when an HWND is provided  *(priority 1)*

When a weaver is created **with** a real `window_handle` (the primary display's
DP), the SDK must:

- **Keep** `WndProc` phase-snapping (hook `WM_WINDOWPOSCHANGING`) — this is
  vendor-owned and proprietary; DisplayXR does not want it.
- **Skip** the internal "am I on a 3D display?" gate (`canWeave()` /
  `canWeaveInternal()` must behave as always-true).
- **Skip** internal window→monitor splitting (`getDrawRegions()`): treat the
  full input as one 3D region. DisplayXR supplies the correct sub-region via
  `process_atlas()` canvas parameters.
- **Not** spawn background monitor/FPC polling threads that race the runtime's
  create/destroy decisions.

DisplayXR guarantees it only creates a vendor DP for a window that is on that
vendor's display, so the SDK's own detection is redundant and actively
conflicts. This is priority 1 because it improves **single-display**
integration today and is the prerequisite for R2/R3.

### R2 — Headless (NULL-HWND) cooperative weaver  *(priority 2 — unblocks spanning)*

When a weaver is created with `window_handle = NULL` (a **secondary** display's
DP, for a window that spans displays), the SDK must:

- **Not** hook a window procedure, call `MonitorFromWindow()`, or spawn polling
  threads.
- **Always weave** whatever it is given (no `canWeave()` gate, no
  `getDrawRegions()` split).
- Compute interlacing **phase entirely from the canvas offset** passed in
  `process_atlas()` (`canvas_offset_x/y`), not from window position.

Rationale: a window spanning two displays gets two DP instances. The display
holding the majority of the window is "primary" (gets the HWND, owns phase
snapping during drag); the other is "secondary" (NULL HWND, interlaces its
slice from canvas offset).

### R3 — Multiple simultaneous eye trackers (one per FPC/display)  *(priority 3 — true Panoramic)*

The SR SDK currently supports one primary eye tracker per service instance
(vendor's own TODOs). Tiled multi-display (e.g. Acer SpatialLabs
["Panoramic View"](https://spatiallabs.acer.com/developer/docs/2299cdda-f90f-11ed-b3b8-067bb43818a8/f0b4e145-f433-4411-b30c-88ffa00add90)
— three displays, one widened 3D frustum) requires **N independent eye
trackers**, each:

- bound to a specific FPC/camera by identity (serial), and
- returning eye positions calibrated to **its own** display's camera.

On the DisplayXR side each DP instance is already per-display and
`get_predicted_eye_positions()` returns per-display positions — we just need
the SDK to support creating multiple independent tracker instances.

### R4 — Expose the FPC serial from the probe handshake  *(enables R3)*

Our EDID layer identifies *which monitor* is vendor hardware, but only the
SDK's FPC handshake knows the **serial** that ties a specific camera +
calibration to that display. The SDK must surface that serial so the runtime
can:

- populate `xrt_display_claim::serial` from `probe_displays()`, and
- pass it back into the DP factory to create the per-display eye tracker in R3.

## Priority & dependency order

```
R1 (trust routing, HWND present)   ── prerequisite, improves single-display now
   └─ R2 (NULL-HWND weaver)        ── unblocks window spanning two displays
        └─ R3 (multi-FPC trackers) ── true Panoramic multi-display 3D
             └─ R4 (FPC serial)    ── data R3 needs; small, do alongside R3
```

R1, R2, R3, R4 are **hard external dependencies** on the vendor SR SDK.
ADR-015 Phase 3b (runtime split-weave) is **ours** and proceeds in parallel —
single-display correctness does not wait on any of the above.

## What DisplayXR owns vs. what the vendor owns

| Responsibility | Owner |
|---|---|
| Which monitors exist and their geometry | OS / DisplayXR |
| Which monitors are vendor hardware | **Vendor SDK** (probe) |
| Which DP is used for which monitor | **DisplayXR** (routing) |
| Display-boundary detection + atlas splitting | **DisplayXR** |
| Interlacing / weaving algorithm | **Vendor SDK** |
| Phase snapping during window drag | **Vendor SDK** (primary HWND only) |
| Eye tracking per display | **Vendor SDK** (per-DP instance) |
| Creating/destroying DPs on display changes | **DisplayXR** |

## References

- [ADR-015 — DisplayXR Owns Multi-Display Vendor Routing](../../adr/ADR-015-displayxr-owns-multi-display-vendor-routing.md)
- Issue #111 (vendor SR-SDK requirements tracker) and #69 (multi-display compositing)
- [`xrt_plugin_iface` reference](../../reference/xrt_plugin_iface.md)
- [Eye-tracking MANAGED/MANUAL contract](eye-tracking-modes.md)

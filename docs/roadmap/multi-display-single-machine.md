---
status: In progress
owner: David Fattal
updated: 2026-05-31
issues: [69]
code-paths: [src/xrt/drivers/, src/xrt/targets/common/, src/xrt/compositor/]
---

> **Status: In progress.** Phases 1–2 (vendor-neutral enumeration + per-display `probe_displays()` claims + the `xrt_dp_factory_registry`) **shipped** — runtime **v1.9.0** (PR #382) + leia **v1.2.0** (PR #25). Phase 3 (compositors *consuming* the registry — per-display routing / split-weave) is the remaining work; see [Phase 3 design decisions](#phase-3-design-decisions) below. Tracking issue: [#69](https://github.com/DisplayXR/displayxr-runtime/issues/69)

# Multi-Display Compositing on a Single Machine

## Summary

Extend the multi-compositor to route layers to multiple physical displays connected to the same machine. Each display has a known pose in a shared room-scale coordinate system.

## Scope and Related Docs

This doc extends the spatial OS compositor to **multiple physical displays on one machine**. It assumes the single-display compositing pipeline and shell already work.

| Doc | Relationship |
|-----|-------------|
| [multi-compositor.md](../architecture/multi-compositor.md) (#43) | **Single-display compositing.** The multi-compositor pipeline this doc extends. Prerequisite. |
| [separation-of-concerns.md](../architecture/separation-of-concerns.md) (#44) | **Window manager (controller).** Manages window placement; this doc adds display-routing beneath it. Prerequisite. |
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

- [x] Define the vendor probe interface — shipped as `xrt_plugin_iface::probe_displays()` returning `xrt_display_claim` `{monitor_id, confidence, supported_apis, serial}` (runtime v1.9.0).
- [x] Implement `probe_displays()` for Leia (EDID-table match per descriptor; EDID→`EDID`, +SDK+service→`VERIFIED`; serial deferred — see follow-up below) (leia v1.2.0).
- [x] Implement `probe_displays()` for sim_display (claims every descriptor at `FALLBACK`) (runtime v1.9.0).
- [x] Conflict resolution: highest confidence wins, ties by ProbeOrder — `target_plugin_resolve_displays` (runtime v1.9.0).
- [ ] **Follow-up:** read the FPC serial from `Global\sharedDeviceSerialMemory` so same-vendor multi-display can pair each monitor with its camera/calibration (currently `serial=""`).

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
- [x] Define `xrt_dp_factory_registry` struct (monitor ID → per-API factory set + confidence + plugin id + serial), on `xrt_system_compositor_info` (runtime v1.9.0).
- [x] Populate registry at system init: load all plug-ins, run `probe_displays`, resolve claims, assign factories — `target_instance.c::build_dp_registry` (runtime v1.9.0).
- [ ] **(Phase 3)** Modify compositor creation to look up DP factory from registry by monitor (instead of scalar `xsysc->info.dp_factory_*`) — see [Phase 3 design decisions](#phase-3-design-decisions).
- [ ] Per-display override configuration (force sim_display on a specific monitor) — generalize the global `PreferredPlugin` (#378) to `PreferredPlugin\<monitor-key>`.
- [ ] **External dep (handle/texture only)**: Vendor DPs must accept `window_handle = NULL` (no WndProc hook, phase from canvas_offset only) (#111) — *not* needed for hosted; see design decisions.

**HWND ownership and phase snapping:**

On Windows, DWM composites the last weaved frame at new positions during drag without re-running the shader — any sub-pixel shift breaks the interlaced 3D. Vendor SDKs handle this by hooking `WM_WINDOWPOSCHANGING` to phase-snap the window to lens-aligned positions. DisplayXR does not need to know the vendor's phase grid — it just decides which DP gets the HWND.

- [ ] Primary display HWND policy: determine which display has majority of window area, pass HWND only to that DP
- [ ] Secondary DPs created with `window_handle = NULL` — weave using `canvas_offset_x/y` only
- [ ] Primary display hot-swap: when window moves mostly onto a different display, destroy old primary DP, create new one with HWND, demote old to NULL-HWND
- [ ] During cross-display drag: primary DP phase-snaps to its grid, secondary displays accept imperfect phase (transient)

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
- [ ] Test phase snapping: drag window on primary display, verify 3D quality maintained

## Phase 3 design decisions

Phases 1–2 shipped enumeration + `probe_displays()` claims + the `xrt_dp_factory_registry` (the registry is currently *informational*; compositors still read the scalar `dp_factory_*`, which equal the primary registry entry → single-display behavior is byte-identical). Phase 3 makes compositors *consume* the registry and weave per display. The approach **splits by who owns the window**.

### Hosted apps — per-display windows (no vendor dependency)

The runtime owns window creation (`own_window` in `*_compositor_create`). When a hosted session's output spans displays, create **one runtime-owned overlay window per display, each with a real HWND**, each driving that display's DP in its normal single-window mode. No DP ever receives `window_handle = NULL`, so **#111 does not apply**. Cost: N-window create / position / composite / input management — but it's all runtime-side (single repo, no coupled vendor release).

### Handle / texture apps — split-weave into the single app-owned surface (needs #111)

The *app* owns one window (handle) or one shared texture + window (texture); the runtime cannot replace it with N OS windows. So one in-process native compositor holds **multiple DPs** and split-weaves:

- Determine overlapped displays + each slice (window-client → screen coords; the registry gives monitor bounds + the owning DP).
- Build the multiview atlas **once**; for each overlapped display call its DP's `process_atlas()` on that slice's sub-region with `canvas_offset_x/y` = the slice's **screen-space** position (the DP's lenticular-phase input); write into the single backbuffer / shared texture; present once.
- **HWND / phase owner = the majority-area display's DP** — it gets the real HWND and owns WndProc phase-snapping. **Secondary DPs get `window_handle = NULL`** and weave their sub-rect from `canvas_offset` only. **This is #111**, and it is unavoidable for this class because the app owns the one window.
- This is **not** the IPC / `comp_multi` path — that combines multiple *apps*. One app spanning monitors stays in its single native compositor; the multi-DP / split-weave logic belongs in a shared `comp_base` / new `comp_multi_display` helper (don't duplicate across the 5 APIs).
- A *stationary* spanning window can be correctly phased on both displays simultaneously (phase is computed per-region from each slice's screen offset, not by moving the window). The transient problem is **drag**: DWM recomposites the last frame without re-running the weave → crosstalk until the move settles, then re-render from canvas offsets.

> **Gating spike before committing to the handle/texture split-weave:** confirm with Leia whether the SR weaver can be externally driven to interlace an *arbitrary sub-rect* with a *supplied phase* and `window_handle = NULL`. If not, #111 is a hard vendor blocker for spanning handle/texture windows (hosted is unaffected).

### Atlas tile sizing across mixed displays

Per-view tile size = **drawing-surface size × scaleXY**, where the surface is the **window** (handle) or the **canvas sub-rect** (texture) — *not* the display's native pixels. (The current `u_tiling_compute_mode(display_w, …)` call is the fullscreen single-display case where window == display; Phase 3 keys tiling off the actual window/canvas size.) The surface size is **common** across the spanned monitors, so the **only** per-monitor variable is `recommended_view_scale_x/y`; each display's native panel resolution feeds only that display's DP *weave output*, never the shared atlas.

Pick **max(scaleXY) over the engaged displays — never majority/min:**
- **Oversampling is free** (the weaver downsamples a higher-res source); **undersampling is a visible regression** (blur/aliasing on the higher-demand slice). So `max` never hurts quality; majority undersamples the minority slice exactly when it's the *sharper* display.
- This is a **different axis** from the HWND/phase owner, which *is* majority — two separate "which monitors" decisions that happen to share inputs.
- **Sizing set:** currently-engaged displays, recomputed on overlap change, surfaced via the existing rendering-mode / recommended-dims change event (the app reallocates render targets exactly as it does on a `xrRequestDisplayRenderingModeEXT` mode switch) — realloc fires only on the rare first cross onto a *more-demanding* display. **Fallback:** static `max` over all connected DP displays (never reallocs; over-renders when parked on a low-demand monitor). Unlike view-*count* worst-casing (free per-frame — inactive tiles aren't rendered), tile-*resolution* worst-casing is paid **every frame**, so the engaged-set policy is preferred for GPU-bound apps.
- **Mechanics:** generalize `u_tiling_compute_system_atlas` to also `max` the per-view `surface × scaleXY` over the engaged set (it already `max`es atlas dims over rendering modes — same philosophy, new axis). Per-monitor DPI folds in via effective density.

### Suggested 3a / 3b sequencing

- **3a (single-display, no regression, verifiable now):** introduce a `comp_dp_factory_for_window(info, monitor) → factory set` accessor that **degenerates to the scalar `dp_factory_*` for a single-entry registry**, and migrate the 12 compositor call sites to it one at a time. Single display → identical pointer → byte-identical weave. Verify: `cube_handle` on d3d11/d3d12/gl/vk + shell, before/after each migration, must be visually identical; `selftest` stays green.
- **3b (needs a second 3D display + the #111 spike):** the actual multi-DP split-weave + per-display lifecycle + atlas-sizing-over-engaged-set above. Inherently unverifiable on one panel.

## Dependencies

- #43 (multi-compositor)
- #44 (spatial window manager)
- ADR-015 (vendor DP routing)

## Context

This is step 2a of the multi-display architecture. All displays are local to one machine, one process, shared memory -- no network involved.

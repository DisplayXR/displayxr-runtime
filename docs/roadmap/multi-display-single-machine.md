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
- [ ] Enumerate multiple physical displays from a single service instance (multiple vendor/sim_display devices)
- [ ] Define per-display pose configuration (JSON config or calibration)
- [ ] Route compositor layers to the correct display(s) based on window pose vs display frustum
- [ ] Handle windows spanning multiple displays (split compositing)
- [ ] Frame synchronization across local displays

### Vendor Display Processor Routing

When multiple displays are from different vendors, each compositor must instantiate the correct vendor's DP. A single window may span multiple displays — a first-class use case for tiled 3D monitor configurations. Acer's ["Panoramic View"](https://spatiallabs.acer.com/developer/docs/2299cdda-f90f-11ed-b3b8-067bb43818a8/f0b4e145-f433-4411-b30c-88ffa00add90) ships exactly this: three SpatialLabs Pro displays combined into one wider 3D viewing area, each weaved independently but presenting a continuous scene. The current single-factory-per-API model (`dp_factory_vk`, `dp_factory_d3d11`, etc. on `xrt_system_compositor_info`) does not support this. See [ADR-015](../adr/ADR-015-displayxr-owns-multi-display-vendor-routing.md).

**Multi-app (IPC) path** — one native compositor per display, each with its own DP:
```
                                    +-> native compositor A -> DP(vendor A) -> Display A
Apps -> IPC -> multi-compositor ----+-> native compositor B -> DP(sim)      -> Display B
                                    +-> native compositor C -> DP(vendor A) -> Display C
```

**Single-app (in-process) path** — one compositor holds multiple DPs, splits atlas at display boundaries:
```
                      +--> region A --> DP(vendor) --> Display A
App -> compositor --->|
                      +--> region B --> DP(vendor) --> Display B

  Window spans Display A (top) + Display B (bottom)
  Compositor splits atlas at boundary, routes each region to correct DP
```

```
              DP Factory Registry:
                monitor-A -> { vk: vendorA_vk, d3d11: vendorA_d3d11, ... }
                monitor-B -> { vk: sim_vk,     d3d11: sim_d3d11,     ... }
                monitor-C -> { vk: vendorA_vk, d3d11: vendorA_d3d11, ... }
```

**Vendor display probe (hardware identification):**

Each vendor driver must tell DisplayXR which OS monitors it recognizes. Vendors use proprietary detection — e.g., a vendor may use EDID matching (manufacturer+product ID from the monitor's registry data) plus a hardware USB handshake (serial number via a shared-memory channel). DisplayXR doesn't need to understand these mechanisms; it only needs the result.

- [x] Define the vendor probe interface — shipped as `xrt_plugin_iface::probe_displays()` returning `xrt_display_claim` `{monitor_id, confidence, supported_apis, serial}` (runtime v1.9.0).
- [x] Implement `probe_displays()` for Leia (EDID-table match per descriptor; EDID→`EDID`, +SDK+service→`VERIFIED`; serial deferred — see follow-up below) (leia v1.2.0).
- [x] Implement `probe_displays()` for sim_display (claims every descriptor at `FALLBACK`) (runtime v1.9.0).
- [x] Conflict resolution: highest confidence wins, ties by ProbeOrder — `target_plugin_resolve_displays` (runtime v1.9.0).
- [ ] **Follow-up:** read the FPC serial from `Global\sharedDeviceSerialMemory` so same-vendor multi-display can pair each monitor with its camera/calibration (currently `serial=""`).

**Eye tracking in multi-display setups:**

Camera-to-display pairing is vendor-internal (e.g., a vendor may bundle display + camera + calibration as one hardware unit). DisplayXR does not manage this pairing — each DP instance provides `get_predicted_eye_positions()` calibrated to its specific display. The vendor probe must include enough identity (e.g., a hardware serial) so the DP factory creates a DP tied to the correct camera/calibration.

- [ ] Extend DP factory signatures to accept display identity from probe (serial/claim data), so vendor can bind to correct camera
- [ ] Verify existing `get_predicted_eye_positions()` contract works per-display (each DP returns eyes relative to its own display)
- [ ] **External dep**: the vendor SDK must support multiple simultaneous eye trackers for multi-camera setups (currently single-primary only)

**sim_display defaults for unclaimed (2D) monitors:**
- [ ] Add `"2d"` as valid `SIM_DISPLAY_OUTPUT` value (#112)
- [ ] Default sim_display to 2D mode (`view_count=1`) when acting as fallback on unclaimed monitors
- [ ] Keep `SIM_DISPLAY_OUTPUT=sbs|anaglyph|blend` for dev/debug override

**Registry & routing:**
- [x] Define `xrt_dp_factory_registry` struct (monitor ID → per-API factory set + confidence + plugin id + serial), on `xrt_system_compositor_info` (runtime v1.9.0).
- [x] Populate registry at system init: load all plug-ins, run `probe_displays`, resolve claims, assign factories — `target_instance.c::build_dp_registry` (runtime v1.9.0).
- [ ] **(Phase 3)** Modify compositor creation to look up DP factory from registry by monitor (instead of scalar `xsysc->info.dp_factory_*`) — see [Phase 3 design decisions](#phase-3-design-decisions).
- [ ] Per-display override configuration (force sim_display on a specific monitor) — generalize the global `PreferredPlugin` (#378) to `PreferredPlugin\<monitor-key>`.
- [ ] **External dep (vendor weaver)** — three tiers (see [Phase 3 design decisions](#phase-3-design-decisions)): **Tier 1** `EXTERNAL_ROUTING` flag (#111 §1–5; needed for *all* multi-display, incl. hosted/workspace); **Tier 2** per-display weaver binding (any 2+ same-vendor displays); **Tier 3** external phase origin for windowless sub-rects (standalone handle/texture spanning). Tier 1 is specced; Tiers 2–3 are the substantive asks.

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
- [ ] Integration test: a vendor DP on monitor A + sim_display on monitor B, verify correct DP routes to each
- [ ] Test DP hot-swap: drag window fully from one monitor to another
- [ ] Test phase snapping: drag window on primary display, verify 3D quality maintained

## Phase 3 design decisions

Phases 1–2 shipped enumeration + `probe_displays()` claims + the `xrt_dp_factory_registry` (the registry is currently *informational*; compositors still read the scalar `dp_factory_*`, which equal the primary registry entry → single-display behavior is byte-identical). Phase 3 makes compositors *consume* the registry and weave per display. The approach **splits by who owns the window**.

### Vendor weaver interference & the three-tier vendor ask (#111)

Code review of a representative vendor weaver shows it is architected as the **sole owner of one window on one primary 3D display**, deriving lenticular phase from `primaryDisplayLocation + liveWindowPosition + viewportOffset`. Concretely it:

1. binds to a single `getPrimaryActiveSRDisplay()` (`.ipp:373`; `screenWidth/Height` + `SRMonitorRectangle` all derive from it) — no "weave for *this* display" input;
2. self-splits the window per monitor in `getDrawRegions()` (`.ipp:47`) and only interlaces the region it deems on its SR monitor (`dx11weaver.cpp:1253–1258`);
3. gates weaving on its own monitor/occlusion view in `canWeaveInternal()` (`.ipp:450–562`);
4. subclasses the root window and ±2px phase-snaps it on `WM_WINDOWPOSCHANGING` via `installCustomWindowProc()` (`.ipp:165,314`);
5. runs two background threads re-scanning monitors/FPCs and re-picking the primary display (`.ipp:344,351`);
6. computes phase from the live window position vs the primary display (`window_WeavingX` via `getScreenRect`, `dx11weaver.cpp:1075,1089,1237`) — only the viewport sub-rect (`vpX/vpY`) is caller-controllable, **not** the window-vs-display term.

Items 2–6 are exactly the jobs ADR-015 gives DisplayXR. So the vendor ask has **three tiers**, of which #111's `EXTERNAL_ROUTING` flag is only the first:

- **Tier 1 — `EXTERNAL_ROUTING` flag (#111 §1–5):** disable the polling threads, skip `installCustomWindowProc`, always-weave (skip `canWeave`), treat the window as one region (skip `getDrawRegions`). Silences items 2–5. **Necessary for every multi-display case; not sufficient for the two below.**
- **Tier 2 — bind a weaver to a *specific* display (multi-camera):** construct/bind each weaver to a caller-named vendor display (serial/HMONITOR) with its own lens/calibration instead of the global primary. Fixes item 1. **Hard blocker for any 2+ same-vendor setup — in *both* the hosted and handle/texture models.** (= #111 comment 3's multi-camera item.)
- **Tier 3 — external phase origin:** accept a per-frame absolute screen-space phase origin from the caller, overriding the `window_WeavingX` + primary-display term, so a sub-rect with **no window of its own** (the secondary slice of a spanning handle/texture window) weaves with correct alignment. Fixes item 6. **Hard blocker for handle/texture spanning.** (#111 §4 collapses the region list but does *not* expose this phase input — it must be added.)

### Runtime-owned window — hosted **and workspace/shell** (avoids Tier 3; still needs Tiers 1–2 for multi-vendor-display)

The split is really by **who owns the output window**, and *both* the **hosted** app class **and the workspace/shell (IPC/service) path** fall on the runtime-owned side: hosted uses `own_window` in `*_compositor_create`; workspace/shell apps are `_ipc` clients that submit layers, while the **service** owns and presents the workspace output. Either way the runtime, not the app, owns the surface — so when output spans displays, create **one runtime-owned window per display, real HWND each** (this is exactly the roadmap's "one native compositor per display" arrangement for the multi-app/IPC path), each driving that display's DP in its normal single-window mode — so no DP is ever windowless and **Tier 3 (external phase origin) is not needed**. But this is **not** dependency-free (correcting an earlier note that said hosted "avoids #111"): the SDK's threads / WndProc / self-split still race DisplayXR's positioning, so **Tier 1 (`EXTERNAL_ROUTING`) is still wanted**, and **2+ same-vendor displays still hit the single-primary-display limit → Tier 2 (per-display binding) is still required**. The one case that works almost as-is today is **mixed-vendor hosted (1 vendor 3D display + 1 commodity display)**: a single 3D display, the vendor DP is a normal single-window weave, and Tier 1 just cleans up the nuisances. Cost otherwise: N-window create / position / composite / input — all runtime-side (single repo).

### App-owned window — standalone handle / texture (split-weave; needs Tier 3)

The *app* owns one window (handle) or one shared texture + window (texture) — this is the **standalone** (non-workspace) case; the runtime cannot replace it with N OS windows. So one in-process native compositor holds **multiple DPs** and split-weaves:

- Determine overlapped displays + each slice (window-client → screen coords; the registry gives monitor bounds + the owning DP).
- Build the multiview atlas **once**; for each overlapped display call its DP's `process_atlas()` on that slice's sub-region with `canvas_offset_x/y` = the slice's **screen-space** position (the DP's lenticular-phase input); write into the single backbuffer / shared texture; present once.
- **HWND / phase owner = the majority-area display's DP** — it gets the real HWND and owns WndProc phase-snapping. **Secondary DPs are windowless** and must weave their slice from an externally-supplied phase origin — **Tier 3 above** (plus **Tier 1**, plus **Tier 2** if that slice is a *second* same-vendor display). Unavoidable for this class because the app owns the one window.
- This is **not** the IPC / `comp_multi` path — that combines multiple *apps*. One app spanning monitors stays in its single native compositor; the multi-DP / split-weave logic belongs in a shared `comp_base` / new `comp_multi_display` helper (don't duplicate across the 5 APIs).
- A *stationary* spanning window can be correctly phased on both displays simultaneously (phase is computed per-region from each slice's screen offset, not by moving the window). The transient problem is **drag**: DWM recomposites the last frame without re-running the weave → crosstalk until the move settles, then re-render from canvas offsets.

> **Gating spike before committing to Phase 3b** — two questions for the vendor beyond the already-specced Tier 1 `EXTERNAL_ROUTING`: **(a) Tier 2** — can a weaver be bound to a *caller-specified* vendor display (serial/HMONITOR) with its own lens/calibration, instead of the vendor's primary-display selector? **(b) Tier 3** — can the per-frame phase origin be supplied externally so a *windowless* sub-rect weaves with correct alignment? Tier 2 gates any 2+ same-vendor setup (hosted or spanning); Tier 3 gates spanning handle/texture. Mixed-vendor hosted (1 vendor 3D display + 1 commodity display) needs neither — only Tier 1.

### Atlas tile sizing across mixed displays

Per-view tile size = **drawing-surface size × scaleXY**, where the surface is the **window** (handle) or the **canvas sub-rect** (texture) — *not* the display's native pixels. (The current `u_tiling_compute_mode(display_w, …)` call is the fullscreen single-display case where window == display; Phase 3 keys tiling off the actual window/canvas size.) The surface size is **common** across the spanned monitors, so the **only** per-monitor variable is `recommended_view_scale_x/y`; each display's native panel resolution feeds only that display's DP *weave output*, never the shared atlas.

Pick **max(scaleXY) over the engaged displays — never majority/min:**
- **Oversampling is free** (the weaver downsamples a higher-res source); **undersampling is a visible regression** (blur/aliasing on the higher-demand slice). So `max` never hurts quality; majority undersamples the minority slice exactly when it's the *sharper* display.
- This is a **different axis** from the HWND/phase owner, which *is* majority — two separate "which monitors" decisions that happen to share inputs.
- **Sizing set:** currently-engaged displays, recomputed on overlap change, surfaced via the existing rendering-mode / recommended-dims change event (the app reallocates render targets exactly as it does on a `xrRequestDisplayRenderingModeEXT` mode switch) — realloc fires only on the rare first cross onto a *more-demanding* display. **Fallback:** static `max` over all connected DP displays (never reallocs; over-renders when parked on a low-demand monitor). Unlike view-*count* worst-casing (free per-frame — inactive tiles aren't rendered), tile-*resolution* worst-casing is paid **every frame**, so the engaged-set policy is preferred for GPU-bound apps.
- **Mechanics:** generalize `u_tiling_compute_system_atlas` to also `max` the per-view `surface × scaleXY` over the engaged set (it already `max`es atlas dims over rendering modes — same philosophy, new axis). Per-monitor DPI folds in via effective density.

### Current state (today, shipped)

**Multi-monitor is not yet functional in any mode.** Workspace/shell == hosted == standalone: all run on the **single primary 3D display**. The service compositor creates one window pinned fullscreen to `display_screen_left/top` (`comp_d3d11_service.cpp:5326–5347`) and one DP from the scalar `dp_factory_d3d11` (`:2861–2863`); a hosted/standalone session is the same shape via `own_window`. The `xrt_dp_factory_registry` is **populated but consumed by nothing** (`grep dp_registry src/xrt/compositor/` → 0 hits). A second monitor is just a normal 2D Windows desktop; the workspace neither extends onto it nor weaves there. The vendor weaver's own `getDrawRegions`/`canWeave` will weave the 3D-monitor slice of a *straddling* window and pass the rest through 2D, but that never drives a second 3D display and the workspace window is pinned fullscreen so it never straddles.

### Suggested 3a / 3b sequencing

**3a is the only Phase-3 work that is useful, regression-safe, AND verifiable on a single-3D-display box today.** Do it now; it converts the registry from dead weight into a live, continuously-validated artifact and isolates 3b's riskiest logic. Two pieces:

- **3a.1 — registry accessor on the live path.** Introduce `comp_dp_factory_for_window(info, monitor) → factory set` and migrate the 12 `dp_factory_*` call sites to it, one at a time. For a single/empty-entry registry it returns the **primary entry's** factory and, if that diverges from the scalar, **logs once and falls back to the scalar** (drift detector + safety net → zero single-display risk). Net effect: the single-display render now *flows through* registry resolution, so any resolution bug surfaces immediately under the existing cube/`selftest` gates instead of lying dormant until multi-display. Verify: `cube_handle` d3d11/d3d12/gl/vk + shell visually identical, `selftest` green, drift log silent.
- **3a.2 — slice geometry as pure, unit-tested logic.** Add `comp_multi_display_compute_slices(window_rect, monitor_rects[]) → [{atlas sub-region, canvas_offset, backbuffer offset}]` (no compositor wiring yet) with headless unit tests, mirroring `u_tiling`. This is 3b's highest-risk math and the one part fully testable without hardware — so it's ready and proven before a second display is ever attached.

**Why not more today:** split-weave dispatch, per-display DP lifecycle, HWND ownership, hot-swap, and atlas-sizing-over-engaged-set all need a second 3D display to verify *and* depend on unanswered SR-weaver tiers — building them blind risks the regressions 3a exists to prevent, and may be reworked once Tier 2/3 answers land.

**Run in parallel (not code):** open the **vendor Tier 2 / Tier 3 spike** (per-display weaver binding; external phase origin — see [Vendor weaver interference](#vendor-weaver-interference--the-three-tier-vendor-ask-111)). Vendor turnaround is the long pole, so start it now regardless of 3a.

- **3b (needs a second 3D display + the Tier 2/3 answers):** wire 3a.2's slices into per-display DP dispatch + lifecycle + atlas-sizing-over-engaged-set. The cheapest *first* 3b increment that needs **no** vendor-weaver tier: extend the workspace onto a second **commodity (non-3D)** monitor as a plain 2D region (runtime-owned window, no weave) — a useful capability that exercises the routing end-to-end with a commodity second display.

## Dependencies

- #43 (multi-compositor)
- #44 (spatial window manager)
- ADR-015 (vendor DP routing)

## Context

This is step 2a of the multi-display architecture. All displays are local to one machine, one process, shared memory -- no network involved.

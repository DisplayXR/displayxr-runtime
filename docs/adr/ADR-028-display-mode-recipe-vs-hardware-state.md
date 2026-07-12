---
status: Accepted
date: 2026-06-12
issues: [542, 541, 522]
---
# ADR-028: The rendering mode is the content recipe; the hardware state is an orthogonal override

## Context

A 2D↔3D change on a switchable 3D display involves three separable things:

1. **Content** — how many view tiles the app renders and how they pack into
   the atlas the runtime hands the display processor.
2. **DP processing** — whether the DP weaves the atlas (multi-view) or
   flat-blits it (single view). Per ADR-007 this lives in the DP, never the
   compositor.
3. **Hardware state** — the physical switchable element (a switchable
   lens, a backlight, …) on or off.

Historically all three hung off one `hardware_display_3d` flag: the flag drove
the DP's physical element *and* its weave-vs-blit branch, and every native compositor used
it to clamp the content tile count (`hardware_3d ? N : 1`). That conflation
made app-authored transitions inexpressible — most acutely the MANUAL
eye-tracking loss flow (#522), where an app wants the panel flat *immediately*
while it fades its stereo content to zero parallax over several frames.

Two failed shapes preceded this decision (preserved here because the bug class
recurs — it also produced the Android 2D regression #533 fixed):

- **"Hardware = physical element + DP processing":** the hardware request also switched
  the DP to its flat-blit branch, so a divergence showed a clean single tile —
  destroying the transition (an instant content jump instead of a blur that
  converges as parallax → 0).
- **"Atlas geometry from the submission":** treating the submitted view count
  / imageRects as the atlas recipe breaks on two *documented-legitimate*
  inputs: `xrLocateViews` always reports the **max** view count (returning
  identical centered views in a mono mode), so always-stereo apps submit
  2 identical views in 2D forever — the compat guarantee, not a divergence
  signal; and `ZONE_3D` layers carry zone-sized imageRects that are never
  atlas geometry. Symptom: content packed into a small left-corner region
  ("3D shifts left").

## Decision

A **rendering mode is a complete recipe**: tile layout, view count, scales,
**and a default hardware state**.

1. **`xrRequestDisplayRenderingModeDXR(modeIndex)`** requests a mode; the
   hardware state follows its default automatically (unchanged behavior).
2. **`xrRequestDisplayModeDXR(2D/3D)`** (repurposed, `XR_DXR_display_info`
   v15) overrides the **hardware state alone** for the current mode. The
   active mode, the app's content, and the DP's processing are untouched —
   only the physical element changes. The override holds until the next mode
   request; it is reported via `XrEventDataHardwareDisplayStateChangedDXR`
   (no mode-changed event — the mode did not change).
3. **The atlas content recipe is the ACTIVE MODE's.** Per-frame, the
   compositors clamp the submission to it: `views = min(submitted, mode
   tiles)`; `views == 1` renders one tile spanning the full content region;
   otherwise the mode grid. Submissions never define atlas geometry.
4. **The DP's `request_display_mode` is hardware-only** (the physical
   element and nothing else). The DP selects weave vs flat-blit from the **per-frame
   atlas grid** handed to `process_atlas` (`tiles > 1` ⇒ weave, `1×1` ⇒
   blit) — regardless of the hardware state.

Net effect of the override: hardware-2D over an active 3D mode keeps the
weave running with the element off — the panel shows the woven atlas flat
(blurry), and an app fading parallax to zero converges back to a sharp image.
That blur-to-sharp ramp **is** the MANUAL tracking-loss transition; the
reverse order (fade first, then flip) also composes.

App-side contract (reference behavior: the `cube_handle_*` and
`cube_zones_d3d11_win` test apps): read the **active** mode's view count from
the mode enumeration and submit only that many views — one full-resolution
tile in a 2D mode. Over-submitting is tolerated (the runtime clamps; identical
views lose nothing) but is the compat path, not the recommendation.

## Consequences

- The `hardware_3d ? N : 1` clamps are gone from all native compositors; the
  mode clamp subsumes the old legacy-app special case (a 2D mode ⇒ mono).
- Zero-copy requires `submitted view_count == mode view_count` (the per-view
  loops would otherwise read stale `proj.v[]` slots — a latent bug this work
  fenced).
- The compositor-side `hardware_display_3d` flag survives only for the HUD,
  the V-key mode toggle, and diagnostics. Under an override it reflects the
  mode's default, not the physical state (the hardware truth is the DP's
  `get_hardware_3d_state`).
- Vendor DPs must keep their processing decision out of the hardware channel
  (contract: `docs/reference/xrt_plugin_iface.md`; per-vendor mechanisms are
  documented in each vendor's plug-in repo).

## Known divergences (tracked, not design)

(The Android single-app OOP divergence was retired by #553: the CONTENT mode
crosses IPC via `compositor_request_rendering_mode` — mirroring #541's
hardware-bit message — and `comp_multi_system.c` adopts the mode clamp.)

(The Metal divergence was retired by #556: `comp_metal_compositor.m` adopts the
mode clamp instead of deriving the atlas from the submission, dropping the
latent always-stereo-in-2D bug.)

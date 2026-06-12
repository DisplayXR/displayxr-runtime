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
3. **Hardware state** — the physical switchable element (e.g. the Leia SR
   lenticular lens) on or off.

Historically all three hung off one `hardware_display_3d` flag: the flag drove
the DP's lens *and* its weave-vs-blit branch, and every native compositor used
it to clamp the content tile count (`hardware_3d ? N : 1`). That conflation
made app-authored transitions inexpressible — most acutely the MANUAL
eye-tracking loss flow (#522), where an app wants the panel flat *immediately*
while it fades its stereo content to zero parallax over several frames.

Two failed shapes preceded this decision (preserved here because the bug class
recurs — it also produced the Android 2D regression #533 fixed):

- **"Hardware = lens + DP processing":** the hardware request also switched
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

1. **`xrRequestDisplayRenderingModeEXT(modeIndex)`** requests a mode; the
   hardware state follows its default automatically (unchanged behavior).
2. **`xrRequestDisplayModeEXT(2D/3D)`** (repurposed, `XR_EXT_display_info`
   v15) overrides the **hardware state alone** for the current mode. The
   active mode, the app's content, and the DP's processing are untouched —
   only the physical element changes. The override holds until the next mode
   request; it is reported via `XrEventDataHardwareDisplayStateChangedEXT`
   (no mode-changed event — the mode did not change).
3. **The atlas content recipe is the ACTIVE MODE's.** Per-frame, the
   compositors clamp the submission to it: `views = min(submitted, mode
   tiles)`; `views == 1` renders one tile spanning the full content region;
   otherwise the mode grid. Submissions never define atlas geometry.
4. **The DP's `request_display_mode` is hardware-only** (lens hint and
   nothing else). The DP selects weave vs flat-blit from the **per-frame
   atlas grid** handed to `process_atlas` (`tiles > 1` ⇒ weave, `1×1` ⇒
   blit) — regardless of the lens state.

Net effect of the override: hardware-2D over an active 3D mode keeps the
weave running with the lens off — the panel shows the woven atlas flat
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
  mode's default, not the lens (the lens truth is the DP's
  `get_hardware_3d_state`).
- Vendor DPs must keep their processing decision out of the hardware channel
  (`displayxr-leia-plugin` PR #45 is the reference port; see
  `docs/reference/xrt_plugin_iface.md`).

## Known divergences (tracked, not design)

- **Android single-app OOP (`comp_multi_system.c`, #541):** the active mode
  does not cross IPC (only the hardware bit does, via
  `compositor_request_display_mode`), so the server's mode copy is stale and
  the submission is the only honest geometry signal — that path builds a
  `view_count×1` atlas from the submission. Bounded in practice (no zones on
  that path; identical-view over-submission weaves to ≈the original image).
  Retire by plumbing the mode index (or the content grid) over IPC, then
  adopting the mode clamp there.
- **Metal (`comp_metal_compositor.m`):** still derives the atlas from the
  submission for non-zone layers (the pre-fix model) — carries the
  always-stereo-in-2D bug latently. Port the mode clamp.

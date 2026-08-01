# ADR-033: The Party That Owns Placement Reports Geometry; the Weaver Owns Everything Phase

**Status:** Accepted
**Date:** 2026-08-01

## Context

Windowed weaving anchors the lenticular interlacing pattern to the **absolute
position of the drawn region on the panel**. Somebody has to (a) know where the
pixels land, and (b) turn that position into an interlacing phase. Historically
the two jobs were fused inside the vendor weaver: on Windows it derives the
window's screen rect from the window handle itself (`GetClientRect` +
`ClientToScreen`) and computes the phase internally, including any
**phase-snapping** behavior during window drags.

Two developments broke the fusion:

1. **Wayland** structurally withholds a surface's absolute position from its
   client. No window handle the weaver could interrogate carries the answer;
   only the display server knows. The Linux answer (#817,
   `docs/specs/runtime/wayland-window-geometry.md`) has the *compositor*
   publish per-window geometry, with the runtime forwarding a panel-space
   origin to the display processor each frame via the `set_present_origin`
   slot — consumed by the vendor SDK's caller-supplied phase-origin API, which
   replaces exactly the window-position term the handle used to supply.
2. **Cases where the handle is absent or lies.** Offscreen / shared-texture
   weaving has no window the weaver could locate; display-zones need per-zone
   origins within a canvas no HWND describes; service-mode composition puts
   several apps' content in one surface whose own window position is
   meaningless per-app.

This raised the question: should position sourcing live weaver-side (Windows
model) or caller-side (Linux model) — and does caller-side sourcing leak
vendor IP? Phase-snapping is the sharp end: snapping requires the lens design
parameters (pitch, slant, subpixel layout), which are vendor-confidential.

## Decision

**The party that owns placement reports geometry; the weaver owns everything
phase.**

- **Geometry crosses the boundary; phase never does.** What the runtime (or
  compositor, or app) supplies is a *raw panel-space position* — public
  information any process could observe. The mapping position → phase, and
  every phase-derived behavior including snapping, quantization, and
  settle-on-drag heuristics, stays inside the vendor weaver, computed with
  lens parameters that never leave it.
- **Who reports is platform-dependent; the contract is not.** On Windows the
  OS lets the weaver self-serve placement from the window handle — that
  remains the default (it serves every non-DisplayXR SDK consumer with zero
  integration work, and the shipped API surface is frozen anyway). On
  Wayland the display server is the only party that knows placement, so
  caller-supplied origin is simply the only mode. Same contract, different
  default.
- **The caller-supplied origin is an override for whoever knows better, not a
  Linux-ism.** The runtime uses it on any platform where it is the source of
  truth: Wayland windows, offscreen/texture targets, display-zone origins,
  service-mode composition.

## Consequences

- The runtime, workspace controllers, and apps MUST NOT implement
  phase-snapping or any position → phase math client-side. If a snap behavior
  needs a signal the position stream doesn't carry (e.g. "a drag is in
  progress" from window messages), the fix is a *hint* on the geometry
  channel — never moving phase math across the boundary.
- Vendor SDK phase-origin APIs must accept raw panel coordinates and remain
  free to post-process them internally (snap, clamp, quantize) exactly as if
  they had been self-derived from a window handle.
- New geometry sources (a KDE publisher, a compositor-native protocol, an OEM
  display server) plug in by feeding the same `set_present_origin` chain; no
  vendor SDK change is implied by adding one.
- Display-info position (`screen_left/top`) and window geometry stay in the
  same desktop coordinate space on every platform, so `panel origin =
  window − display` is the whole runtime-side computation.

## References

- `docs/specs/runtime/wayland-window-geometry.md` — the Wayland geometry
  provider (#817), first caller-supplied-only platform.
- `docs/adr/ADR-007-compositor-never-weaves.md` — the sibling boundary: the
  compositor prepares content, the DP weaves.
- Vendor-side precedent: the Linux phase-origin design in the first
  integration's SDK explicitly locked "replace the window-position term only;
  all phase math unchanged" (LeiaSR#85).

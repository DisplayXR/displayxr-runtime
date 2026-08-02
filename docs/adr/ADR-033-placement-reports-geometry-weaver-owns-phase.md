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
- **The placement authority varies by scenario; geometry must always come from
  whoever it currently is.** Not from whoever finds it convenient, and never
  from inference downstream. Three authorities in practice:

  | Scenario | Placement authority | How geometry reaches the weaver |
  |---|---|---|
  | Plain app window on Windows | Window manager | It publishes to any process (`GetClientRect`/`ClientToScreen`), so the weaver reads Win32 directly |
  | Wayland window | Compositor | It declines to publish, so we construct the channel (#817) and the runtime relays the origin |
  | Display-zone, offscreen/texture target, service-composited surface | The **DisplayXR compositor** — *it* decides where those pixels land | The runtime supplies the origin; the OS window rect would be actively wrong |

- **Win32 satisfies this rule — it is not exempt from it.** The weaver reading
  the window rect is not *deriving* placement; it is consuming the placement
  owner's own public report, through the channel that owner provides. A window
  handle is a pointer to the authority's record ("ask the OS about this
  window"), not an assertion of position, and a window rect is public
  information any process may query. Wayland has the identical contract and a
  missing transport: the compositor-published geometry service is a **polyfill
  for a reporting channel Win32 provides natively**, not a different model.
  Windows therefore keeps the handle-derived path as its default — it also
  serves every non-DisplayXR SDK consumer with zero integration work, and the
  shipped API surface is frozen regardless.
- **Push vs pull is transport, not boundary.** The invariant is *who is
  authoritative* and *what crosses the vendor boundary* — not the direction of
  the call. The shipped X11 path pulls (`xcb_translate_coordinates` is a query,
  not a subscription); Wayland pushes. Both comply.
- **The caller-supplied origin is therefore not a Linux-ism.** The runtime uses
  it on any platform where it has become the placement authority — including
  Windows, where in zones/offscreen/composited modes a weaver reading the HWND
  would compute a *wrong* phase.

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
- **Platform geometry sourcing is the runtime's burden, never a vendor's.** A
  vendor plug-in's entire windowed-weaving surface is the optional
  `set_present_origin(xdp, panel_x, panel_y)` slot (two integers; absent slot ⟹
  display-scoped, per ADR-020 append-only negotiation). Vendors write no
  Wayland, D-Bus, or compositor-extension code. This is deliberate: the work is
  byte-identical for every vendor and touches nothing about lenses, so
  duplicating it per vendor would multiply identical code, fracture the
  compositor-coverage matrix per vendor (incoherent product behavior for what
  users perceive as a platform capability), and — where a shared side channel is
  involved — have several publishers contend for one channel. See
  `docs/guides/vendor-plugin-onboarding.md`.
- **A geometry side channel has one publisher and many consumers.** Where a
  platform needs a constructed channel (D-Bus service, compositor extension),
  exactly one component ships the publisher; every interested runtime subscribes
  independently. A second publisher for the same channel is a defect, not
  redundancy — on D-Bus a well-known name is singly owned, so a duplicate binds
  nothing and silently serves a possibly-skewed schema. Consumers are
  unconstrained: a vendor-SDK runtime serving its own non-DisplayXR apps may
  subscribe to the same publisher, and should, rather than shipping a rival one.
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

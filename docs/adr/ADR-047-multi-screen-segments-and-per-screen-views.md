---
status: Accepted
date: 2026-10-07
source: "docs/roadmap/multi-screen.md, #69"
supersedes: "ADR-015 §3–§4 (per-compositor multi-DP over ONE atlas, split-weave at the display boundary)"
---
# ADR-047: Multi-screen — segments and per-screen views

## Context

ADR-015 made DisplayXR the owner of multi-display vendor routing and sketched the
mechanism: one compositor holds one display processor (DP) per display its window
overlaps, renders **one atlas**, splits it at the display boundary and hands each DP its
slice. That mechanism was never built (the registry it needs is empty on Linux, and
`u_multi_display_compute_slices` has no caller), and a fresh read in October 2026 found
two problems with it:

1. **One atlas is only correct when every screen shares the same eyes and geometry.**
   Each DP owns its own eye tracker and its own panel geometry (ADR-015 §6 says so
   itself). A tracked Leia panel next to an untracked laptop panel, or two Leia panels
   with two cameras, need two different Kooima frusta. Slicing one atlas puts the wrong
   perspective on one of them.
2. **The OpenXR surface was never addressed.** Every prior document keeps one
   `XrSystem`, one view count, one `xrLocateViews` per frame. Per-display eye streams
   stop inside each DP; nothing says how a second screen's eyes become app views.

Linux also changed the premises: the weaver is windowless and takes an explicit phase
origin per frame (ADR-033), so ADR-015 §7's primary/secondary HWND rule is a Windows
detail, not the model.

## Decision

1. **Screens are first-class runtime objects.** A screen is an OS monitor. The runtime
   keeps a screen registry built from the OS monitor list joined with every loaded
   plug-in's `probe_displays()` claims; each entry carries geometry, physical size,
   identity (EDID ids, connector, vendor serial), the bound plug-in and its DP
   factories, and the screen's own display info and eye-tracking capability. Several
   plug-ins are resident at once. One head device remains (it is the pose source, not
   display geometry); `xrt_system_compositor_info`'s display fields become a cached copy
   of screen 0 for compatibility.
2. **A spanning window is split into segments, not atlas slices.** A segment is the
   window's canvas intersected with one screen. Each segment has its own DP instance
   (created from that screen's factory with a screen binding), its own present origin,
   its own eye positions (that DP's tracker, or the screen's nominal viewer) and its
   own Kooima frustum computed from the segment rect relative to its screen. The app
   renders each segment's views; the compositor gives each DP its segment's tiles with
   `canvas = segment rect` and composites the woven results into the one presented
   surface. The slice math survives as segment-rect math. On Linux every segment DP is
   windowless with an explicit origin; on Windows the real HWND goes to the
   majority-area segment's DP only for drag phase-snapping.
3. **Per-screen views ride the existing multiview surface.** No new view-configuration
   type. `PRIMARY_MULTIVIEW_DXR` already advertises a maximum view count and a per-frame
   `activeViewCount` (ADR-041). `XR_DXR_display_info` gains display enumeration, a
   DISPLAY reference space per display, an optional session→display binding, and a
   per-view display binding chained on `XrViewState` (view → display id + segment rect
   in window pixels). Views are contiguous per segment. `PRIMARY_STEREO` apps keep two
   views from the majority segment and the shipped flat-2D treatment elsewhere
   (#1654), so nothing that works today changes until an app opts in.

## Consequences

- ADR-015 §1–§2 (probe + registry), §5 (DP lifecycle), §6 (per-DP eye tracking) and §8
  (vendor-owned phase snapping) stand. §3–§4 are superseded by Decision 2; §7 is
  narrowed to Windows drag snapping.
- A spanning window costs one view pair per 3D segment. The single-segment case, which
  is every window today, pays nothing.
- The DP factory ABI gains a screen binding (ABI bump, ADR-020 append rule); sim_display
  moves its panel state from process globals to per-instance state.
- The vendor side needs, in order: an opt-in external-routing weaver mode (null window,
  always weave, phase = origin + viewport, no lens vote), display enumeration with
  identity, binding display/weaver/lens/tracker to a display id, and N trackers per
  service. The first three shipped on the LeiaSR Linux line on 2026-10-07; the fourth
  needs two panels.
- Milestones, gates and the SR work items: `docs/roadmap/multi-screen.md`.

## Amendment 1 (2026-10-09): segments under the weave-on-scanout split

The first Windows implementation refused to segment a window whenever the D3D11 #918
output-device split was engaged, which ADR-039 makes the default on every hybrid box and
keeps even on one adapter, so the shipping configuration never segmented. Decision: **a
segment DP lives on the device that presents.** Under the split that is the output
(scanout) device, where the session's own DP already is: the segment DPs are created there
through the unchanged `create_dp_d3d11_for_screen` slot (it is handed the output device and
context), they weave the output-side copy of the composed atlas (the egress slot), and each
segment keeps its own viewport, scissor and present origin exactly as off the split. The
split is not a separate segment path; the primary screen's DP keeps the real HWND. Detail:
`docs/architecture/comp-segments.md` § *Windows / D3D11*.

## Amendment 2 (2026-10-09): the HWND follows the majority segment

Decision 2's "the real HWND goes to the majority-area segment's DP" ships on Windows D3D11
(it first shipped with the HWND pinned to the primary screen's DP, David 2026-10-07). The
owner is the segment with the largest window-px area, by the same rule that frames a single
view set (`oxr_segment_views_majority` / `comp_segments_majority`). A hand-off is due only
when another screen beats the owner by at least 20 % of the window's on-screen area (a
60/40 dead band between two screens) continuously for 0.5 s, so a drag across a seam never
flaps it; a target that fails is not retried until the majority leaves it. Because a weaver
is one-per-HWND and a live DP's window is never re-pointed, the hand-off creates a
windowless replacement for the old owner (the primary screen always keeps a DP, as the
session's own), destroys the old owner's DP, then creates the new owner's DP with the HWND
through `create_dp_d3d11_for_screen` — rolling back if that fails — all between two weaves,
so a frame weaves with the old pair or the new pair and never with a gap. The windowless
primary takes its phase from `set_present_origin` like any segment; drag snap, the window
anchor and the hosted window's snap provider follow the owner. Eyes and view routing are
unchanged: the primary screen's DP still serves the primary screen. Logic:
`comp_segments_owner_*` (unit-tested); detail: `docs/architecture/comp-segments.md`.

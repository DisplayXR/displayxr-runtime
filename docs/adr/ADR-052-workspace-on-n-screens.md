---
status: Accepted
date: 2026-10-10
deciders: David Fattal
issues: []
related: [ADR-018, ADR-035, ADR-039, ADR-047, ADR-051]
---

# ADR-052: Workspace on N screens — one presenter per 3D screen, one desktop-space layout

## Context

ADR-047 made screens first-class and split a spanning window into per-screen segments, each
woven by its own screen's display processor (DP). Its amendments carried that to every
compositor and to the service's direct pipeline (Amendment 3) and present-owners (Amendment
4). Amendment 3 scoped one path out on purpose: the **workspace / compose path**
(`multi_compositor_render`) keeps ONE presenter — the service window, full-screen on the
system-default panel — so everything a workspace controller places is composed into one atlas
and woven by one DP. On a box with two 3D screens the second one shows nothing of the
workspace, and a window the controller drags across the seam is cut at the panel edge.

The workspace contract itself is single-screen too. `XR_DXR_spatial_workspace` (spec_version
24) places windows in "display-centric space" — origin at THE display's centre — reports
cursors in "display pixels", and has no way to say which screens exist. A controller cannot
even learn that a second screen is there.

Three constraints frame the decision:

- **The runtime knows no workspace product.** A workspace controller is a client class
  (`CONTROLLER`, ADR-035 D1) that hosts other apps' 3D windows through the spatial-workspace
  extensions; whatever it is called, the runtime owns mechanism and the controller owns
  policy (ADR-018). The multi-screen contract must be expressible by any controller.
- **Pre-existing controllers must keep working byte for byte.** A controller written against
  spec_version 24 never asks about screens; on a one-screen box nothing may change at all.
- **The segment machinery already exists.** `comp_segments_compute` (the half-open seam
  rule), the screen registry with per-screen DP factories (`create_dp_d3d11_for_screen`),
  the device rule under the weave-on-scanout split (ADR-047 Amendment 1) and the status
  model (ADR-051) are reused, not reinvented.

## Decision

### D1 — One presenter per claimed 3D screen in compose mode

While a workspace is active the service presents on **every claimed 3D screen**, one
presenter per screen. A presenter is a full-screen window on its screen with its own swap
chain, its own DP created from that screen's registry entry through
`create_dp_d3d11_for_screen` on the device the presenter presents from (ADR-047
Amendment 1's rule, a DP lives where its target is), its own present origin and its own
eye tracker. The primary presenter is today's service window with today's panel DP,
unchanged.

A screen is a presenter when its winning claim is a 3D plug-in above the fallback confidence
that can make a DP for it (the same "has a DP factory" test the segment path uses); the
system-default screen always is. Presenters are created when the workspace activates and
whenever the screen set changes (a monitor added, removed or moved, a claim changed —
the display re-probe that already drives the segment managers), and torn down with the
workspace or when their screen leaves the set. A retired presenter's DP goes through the
same create → publish → retire graveyard as the panel DP. One render thread composes and
presents every presenter; the primary's frame-latency waitable paces it.

### D2 — The workspace is one desktop-space layout

The controller places each window in the display space of an **anchor screen** (origin at
that screen's centre, +x right, +y up, +z toward the viewer, that screen's metres). The
runtime turns the placement into the window's **footprint** — the desktop-pixel rect it
covers, read through the anchor's own pixel pitch — and every presenter the footprint
overlaps draws the window, at the pose the same footprint has in **its** display space
(through its own pixel pitch). Depth and orientation carry over unchanged. Because the
mapping goes through desktop pixels it composes exactly (anchor → A → B equals anchor → B)
and a window straddling a seam is pixel-continuous across it, the way the OS desktop is.

The footprint is split across presenters with `comp_segments_compute`, so the seam rule is
the one every other segment path uses (half-open: a seam column belongs to exactly one
screen), and each part is composed into its presenter's atlas and woven by that screen's
DP. A part that lands on no presenter (a 2D monitor, off-desktop) is not drawn.

Views: a client renders its window from the eyes and the geometry of the screen it is on
(Amendment 1 below). A window wholly on one presenter is framed from that presenter's
screen; a window straddling presenters gives a segment-aware client one view set per part,
each framed from its own screen's viewer, and any other client one view set framed from the
screen holding most of it.

### D3 — A product-neutral contract: screens, a change event, placement on any screen

`XR_DXR_spatial_workspace` spec_version 25 adds, all additively:

- **`xrEnumerateWorkspaceScreensDXR`** — every monitor of the desktop, primary first: id
  (equal to the `XR_DXR_display_info` displayId, so the two lists join), desktop rect,
  the same rect in workspace pixels, physical size, flags (`PRIMARY`, `3D`, `PRESENTER`,
  `TRACKED`), the presenter's current mode and view count, its nominal viewer and the
  claiming plug-in's id. Calling it subscribes the controller to the event below.
- **`XR_WORKSPACE_INPUT_EVENT_SCREENS_CHANGED_DXR`** — on the existing input queue, carrying
  the set's new generation. A controller that never enumerated never receives it.
- **`xrSetWorkspaceClientWindowPlacementDXR` / `xrGetWorkspaceClientWindowPlacementDXR`** —
  a placement (`screenId`, pose, size) in any screen's space. The anchor sticks to the
  client; the pre-v25 pose calls and `WINDOW_POSE_CHANGED` keep speaking the primary
  screen's space, and the pre-v25 setter re-anchors on the primary.
- **Workspace pixels** name the space every cursor coordinate was always in: desktop pixels
  relative to the primary screen's top-left. With one presenter that is the service window's
  client area, exactly as before; a cursor on another screen has coordinates outside the
  primary's rect, and each screen's `workspaceOffset` locates it.

The controller decides what goes where; **the runtime decides which presenter draws what**.
Hit-testing stays controller-owned (spec_version 22): the controller finds the screen under
the cursor from the screen list and raycasts in that screen's space.

### D4 — A 2D monitor is not a presenter (v1)

Every monitor is listed so a controller sees the whole desktop, but a monitor without a 3D
claim never presents: a window part over it is not drawn by the workspace. Letting a
controller opt a 2D monitor in as a flat presenter is a later, additive request.

### D5 — What stays single-screen, and the fallbacks

- **Controller overlays** (`xrSetWorkspaceOverlayDXR`, anchored in normalised display
  coordinates) and **frame capture** (`xrCaptureWorkspaceFrameDXR`) stay on the primary
  presenter; **fullscreen** (`xrRequestWorkspaceClientFullscreenDXR`) fills the primary.
- **The rendering mode and the lens are one per service.** The panel lease (ADR-035 D2)
  flips every presenter together; a presenter whose DP declines the mode presents flat.
- **The cursor sprite** is drawn by the presenter under the pointer, at the controller's
  pushed depth; FRAME_TICK's viewer stays the primary's.
- **A presenter with no DP** — its plug-in cannot make one for that screen — still
  presents its composition, unwoven (the primary's own no-DP fallback), and the status
  reports it `woven: false`. A screen whose claim disappears leaves the set: its presenter
  is torn down and the controller is told.
- **Secondary presenters live on the render device.** They compose from the clients'
  atlases there and their swap chains are created there, so by ADR-047 Amendment 1's rule
  their DPs live there too; the #918 weave-on-scanout split stays the primary's (a
  cross-adapter transport per presenter is a later optimisation, not a correctness need).
- **`DXR_SEGMENTS=0`** (the multi-screen kill switch) or **`DXR_WORKSPACE_SCREENS=single`**
  keeps the single presenter; every screen is still listed, only the primary presents.
  `DXR_WORKSPACE_SCREENS=fallback` lets a fallback claim present (development boxes with no
  3D hardware).

## Consequences

- ADR-047 Amendment 3's "the workspace / compose path keeps one presenter" is superseded
  by D1–D2; its direct-pipeline decisions stand unchanged. ADR-035 D3's *service window*
  presenter strategy becomes one service window per 3D screen.
- Cost scales with presenters: one compose pass, one weave and one present per presenter
  per frame, and one tracker per 3D screen (the vendor side's "N trackers per service",
  ADR-047 Consequences). A one-screen box pays nothing: the set has one presenter, and every
  pre-v25 call behaves exactly as before.
- Mixed pixel pitch is handled by the footprint rule (pixel continuity), not by physical
  continuity: a window straddling two screens of different pitch changes physical size at
  the seam, the way the OS desktop does.
- The display status (ADR-051) workspace block gains `screens[]` — the presenter screens,
  each with `primary` and `woven` — additively, so the dashboard shows which screens the
  workspace uses.
- Rollout: the contract (D3), the screen set, the placement mapping and the status block
  landed first with only the primary flagged `PRESENTER` — so the answer a controller reads
  was always the truth about what is drawn — and the per-screen presenters (D1) followed on
  top of the same set, flipping the flag on. Implementation: one compose pass per presenter
  (`multi_compositor_compose_pass`), the primary's unchanged and a secondary's with a
  thread-local presenter frame installed (`svc_ws_pass`), so every window, chrome and
  cursor path is the one the primary has always run.
- Contract: `docs/specs/extensions/XR_DXR_spatial_workspace.md` (spec_version 25). Geometry:
  `src/xrt/compositor/util/comp_workspace_screens.h`.

## Amendment 1 — per-screen views (2026-10-10)

**Status:** Accepted. Implements the part of D2 that the first rollout left as a follow-up.

**Why.** The first rollout composed every window on every presenter it overlaps, woven by
that screen's own DP, but rendered its content from the primary's eyes in the primary's
display space. On the two-panel rig a window sitting wholly on the second panel was woven
by that panel's DP, but its perspective followed the first panel's tracking camera. It was
also framed against a canvas half a metre off-centre on a panel it was not on.

**Decision.** A workspace window's views are framed from the screen it is on.

- **Wholly on one presenter.** That presenter's screen frames the window. The client's
  `xrLocateViews` (the server-side view-pose path, `ipc_try_get_sr_view_poses`) takes three
  inputs from that screen:
  - **Eyes.** The screen's presenter DP's predicted eyes, in its own display space. If the
    DP gives no answer, the screen's nominal viewer is used, so the window is never framed
    from another screen's eyes.
  - **Canvas.** The window metrics in that screen's display space: the placement mapped
    through desktop pixels (`comp_ws_map_pose`), with depth and orientation unchanged.
  - **Nominal viewer.** The screen's own.

  The window-relative Kooima is the same math as before, run against that screen. On the
  primary nothing changes. On a one-presenter workspace nothing is even looked up.
- **Straddling presenters.** The service hands the client a per-segment table
  (`compositor_get_segment_metrics`) cut from the window's footprint. There is one segment
  per presenter part, each with that screen's eyes: the panel DP's for the primary part, and
  the presenter DP's for the others. The table has the shape the direct pipeline's table
  already has, so the state tracker reads it unchanged (`oxr_session_locate_views`,
  `oxr_segment_views_*`). It carries one new field, `xrt_segment_metrics::window_depth_m`,
  which is the tile's z, so every segment's canvas sits at the window's depth.
  - **A segment-aware client.** This is a `PRIMARY_MULTIVIEW_DXR` session with set
    capacity > 1, the M3 contract. It renders one view set per part (views 0..1 for the
    primary's part, 2..3 for the other) and routes them back
    (`compositor_set_view_routing`). The commit paints the mosaic: each part's views go into
    that part's rect in every tile (`comp_segments_route_place`). Every presenter's compose
    pass samples the whole tile at the window's footprint, so each presenter draws the views
    framed for its own screen. The compose pass needs no change.
  - **Any other client.** It keeps one view set framed from the **majority** part's screen
    (most pixels; a tie goes to the primary). A client that reads the table and the server
    path pick the same screen and build the same canvas (unit-tested against each other).
- **Tilted straddlers keep one view set.** The table is cut from the footprint, which for a
  tilted window is the bound of its rotated corners. The client's view tiles do not map onto
  that bound, so a tilted window over a seam keeps the majority's one view set. The same
  holds for a window over more presenters than the client has view sets.
- **The controller is unchanged (D5).** Its FRAME_TICK viewer, and its own locates, stay
  the primary's.

**Consequences.**

- Exactness. A straddler's parts meet exactly at the seam at z = 0. At other depths the
  compose pass draws the window parallax-shifted per eye, so the seam between the two parts'
  views moves off the physical seam by that shift. It moves a few pixels at a typical
  window depth.
- Cost on a one-presenter workspace: none. Cost on a multi-presenter workspace, per locate
  of a placed tile: a copy of the screen set under its leaf mutex, the footprint split, and
  for an off-primary window one presenter DP eye query under `ws_presenters_lock`.
- Diagnostics. Each line below is a WARN, logged only when it changes:
  - `workspace views: client … window wholly on screen 0x…: views from that screen's eyes`
  - `… window straddles 2 presenters: per-segment table …`
  - `… window on the primary screen: the primary's eyes …`
- Geometry: `comp_ws_frame_window`, `comp_ws_window_metrics`, `comp_ws_segment_table`
  (`util/comp_workspace_screens.h`), unit-tested in `tests_comp_workspace_screens` against
  the state tracker's `oxr_segment_views_*`.

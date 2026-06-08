---
status: Accepted
date: 2026-06-08
issues: [396]
---
# ADR-024: Raw vs Render-Ready Views (XR_EXT_view_rig)

## Context

Every DisplayXR app re-implemented the Kooima view math: it read raw eye
positions out of `xrLocateViews`, ignored the runtime's `XrView.fov`, and ran
its own `display3d` / `camera3d` `compute_views`. Meanwhile the runtime *already
computes* render-ready views through the very same two-rig math — but no app
could drive it (the only driver was the qwerty debug device, and external-window
apps were forced to identity-m2v display-centric). So every app, demo, and
engine plugin carried a duplicate of the math purely to get virtual-display-
height / camera-centric / factor control.

## Decision

Add **`XR_EXT_view_rig`**: an app hands the runtime a **rig descriptor** (the
Kooima tunables) chained onto `xrLocateViews` and consumes standard
`XrView{pose, fov}` — render-ready and clip-independent — like on any other
OpenXR runtime. The runtime owns the raw eyes, the display plane, and the
window/canvas rect, so it does all the math internally.

- **Render-ready is a fixed point.** `XrView{pose, fov}` is complete for
  rendering. Rig descriptors carry **no clip parameters** (fov is
  clip-independent — near/far + depth convention stay app-side) and **no
  placement parameters** (the runtime owns the window/canvas geometry).
- **Two rigs**, matching the two pipelines: a **display rig** (window-as-portal;
  virtual display height + ipd/parallax/perspective factors) and a **camera
  rig** (an app camera perturbed by eye tracking; ipd/parallax factors,
  convergence, vertical FOV). Strictly per-locate — chain on every locate you
  want it to drive; a locate that chains nothing keeps the default behavior,
  including the raw-eye transport in `XrView.pose` for external-window apps.
- **Raw result channel** (`XrViewDisplayRawEXT` on `XrViewState::next`): the
  complete untransformed input set (display-space eyes, display-plane pose,
  effective canvas rect + meters, sample time, tracking lock) for aware
  consumers that keep doing their own math (the WebXR bridge).
- **Equivalence by construction.** The runtime's render-ready path and an
  app-from-raw computation run the **same** type-neutral math core
  (`displayxr::math`; the runtime's old `m_*_view` ports are deleted).
  Equivalence isn't tested, it's structural.
- **Validation is clamp + one-shot WARN per session**, never reject (per-frame
  error handling would be awkward); if both rig structs are chained, the camera
  rig wins.

### Eyes come from the display processor

The raw channel reports the **DP's eyes verbatim** — one eye per active view.
Multi-view eye fill is the **DP's responsibility** (sim_display reports N for
`>2`-view modes; Leia is 2-view, 2 eyes). **The runtime never synthesizes
eyes**: the former runtime surplus-synthesis was redundant (it only fired when a
DP under-reported vs the active mode, which no current DP does) and was deleted
in favor of a one-shot WARN. The IPC raw path likewise reports the DP's full
count, not a truncated two. `isTracking` is the only lock signal; when unlocked
the runtime still reports the DP's nominal-viewer eyes.

### Workspace interaction

A non-controller workspace client's own rig is **honored by default** — rig
choice is app visual policy within its own canvas, not a shared-mode resource
like display mode. The workspace controller may **take over** its clients' view
geometry via `xrSetWorkspaceViewRigEXT` (e.g. forcing identity m2v during a
layout animation): while an override is set, the server substitutes it for
non-controller locates. There is **no client-side gate** — gating client-side
would drop the locate off the rig route and break a rig-consuming app's
render-ready expectation; the override is the sole, server-side enforcement
point.

## Consequences

- Native apps delete their per-frame Kooima block and consume `XrView` directly;
  engine plugins keep building matrices *from fov* (engine conventions,
  reverse-Z) but stop computing the fov. The WebXR bridge consumes the explicit
  `XrViewDisplayRawEXT` channel.
- Works in-process and over IPC: service-mode sessions route a rig-chained (or
  raw-chained) locate through the same server path as the legacy locate, plus
  the rig overrides and the server-gathered raw block.
- `xrSetWorkspaceViewRigEXT` is inert until a workspace app consumes the rig API
  and a controller (the shell) adopts the call.
- The surplus-eye and workspace-interaction open questions from the original
  design are resolved as above.

## References

- Header `src/external/openxr_includes/openxr/XR_EXT_view_rig.h` (SPEC_VERSION 2).
- `src/xrt/state_trackers/oxr/oxr_session.c` (rig parse + raw fill + IPC route),
  `src/xrt/ipc/server/ipc_server_handler.c` (server rig math + workspace
  override), `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` (override
  storage).
- Projection math: `docs/architecture/kooima-projection.md`.
- Epic #396 (W7). DP-owned raw eyes + the `xrSetWorkspaceViewRigEXT` override
  landed in PR #488.

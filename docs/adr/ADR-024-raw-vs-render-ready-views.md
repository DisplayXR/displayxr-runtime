---
status: Accepted (amended 2026-09-06, see Amendment 1)
date: 2026-06-08
issues: [396, 1370]
---
# ADR-024: Raw vs Render-Ready Views (XR_DXR_view_rig)

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

Add **`XR_DXR_view_rig`**: an app hands the runtime a **rig descriptor** (the
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
- **Raw result channel** (`XrViewDisplayRawDXR` on `XrViewState::next`): the
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
`>2`-view modes; a 2-view vendor display reports 2 eyes). **The runtime never synthesizes
eyes**: the former runtime surplus-synthesis was redundant (it only fired when a
DP under-reported vs the active mode, which no current DP does) and was deleted
in favor of a one-shot WARN. The IPC raw path likewise reports the DP's full
count, not a truncated two. `isTracking` is the only lock signal; when unlocked
the runtime still reports the DP's nominal-viewer eyes.

### Workspace interaction

A non-controller workspace client's own rig is **honored by default** — rig
choice is app visual policy within its own canvas, not a shared-mode resource
like display mode. The workspace controller may **take over** its clients' view
geometry via `xrSetWorkspaceViewRigDXR` (e.g. forcing identity m2v during a
layout animation): while an override is set, the server substitutes it for
non-controller locates. There is **no client-side gate** — gating client-side
would drop the locate off the rig route and break a rig-consuming app's
render-ready expectation; the override is the sole, server-side enforcement
point.

## Consequences

- Native apps delete their per-frame Kooima block and consume `XrView` directly;
  engine plugins keep building matrices *from fov* (engine conventions,
  reverse-Z) but stop computing the fov. The WebXR bridge consumes the explicit
  `XrViewDisplayRawDXR` channel.
- Works in-process and over IPC: service-mode sessions route a rig-chained (or
  raw-chained) locate through the same server path as the legacy locate, plus
  the rig overrides and the server-gathered raw block.
- `xrSetWorkspaceViewRigDXR` is inert until a workspace app consumes the rig API
  and a controller (the shell) adopts the call.
- The surplus-eye and workspace-interaction open questions from the original
  design are resolved as above.

## Amendment 1 — Views are expressed in the locate space, on both legs (2026-09-06)

**Status:** Accepted. Closes #1370. Makes explicit a rule the original text
assumed and the implementation never honoured.

### What was wrong

`XrView.pose` is specified relative to `XrViewLocateInfo::space`, and the rig
descriptors above document their `pose` as "in the locate space". The runtime
did neither: every render-ready path composed the eyes in the head device's
**tracking-origin space** (the root — where the qwerty rig, the display plane
and the DP eyes all live) and read a chained rig pose as if it were there too.
Hands and body were rebased into the base space; views were not. So
`xrLocateSpace(VIEW, base)` and the `XrView` centroid disagreed by the LOCAL
offset for every class except Chrome WebXR over IPC and workspace tiles —
and each earlier fix (the hosted world-absolute concession, its appcontainer
carve-out, `server_sent_head_pose`, the Windows-only motion leg of #739/#741)
patched one class with a per-class constant.

### Decision

1. **One composition site, two legs.** The state tracker resolves the
   tracking-origin-to-base relation once (`oxr_space_locate_device` for the
   head — the overseer links a device to its origin space, the tracked pose
   rides separately) and converts on **both** legs: a chained rig pose comes
   *in* from the locate space; the render-ready eyes and
   `XrViewDisplayRawDXR::displayPlanePose` go *out* to it. Never through
   `T_base_head`, which already carries the head motion the eyes carry too.
   Both legs or nothing: every shipping rig app locates in LOCAL and treats the
   result as rig-local, so a one-leg conversion shifts them by the LOCAL
   offset and a two-leg conversion is a numerical no-op for them.
2. **The same over IPC.** The client converts; `ipc_view_rig_info::pose` is a
   tracking-origin pose on the wire, and the server never sees the base space.
   Legacy clients keep the standard `T_base_head` chain (the #739 lesson:
   never route a legacy client through the eye override; carry the server's
   head motion through the chain, in whatever base the app asked for).
3. **RAW mode is the one deliberate exception — by contract, not by
   accident.** With `XR_DXR_display_info` enabled and no rig chained,
   `XrView.pose` is the display processor's eyes **relative to the display
   plane**, identity orientation, in every base space (INV-6.1). The former
   wording "regardless of the reference space" was retired: the eyes are not
   space-agnostic, they are plane-relative, and `displayPlanePose` reports
   that plane in the locate space so a RAW app can rebase anything else it
   located there (grips, hand joints) into its own view.
4. **No reference space is redefined per session class.** LOCAL stays the
   root + (0, 1.6, 0) of upstream Monado — what legacy VR titles expect: STAGE at
   the floor, the head standing at 1.6 m — and the qwerty rig is seeded at the same
   height. A hosted legacy title therefore sees its eyes ~0.1 m above the LOCAL
   origin (+WASD) and 1.7 m above STAGE. Content that must sit at standing
   height belongs in STAGE; the in-tree hosted test cubes locate there. No
   further per-class standing-height constants will be accepted.

### Consequences

- The concession chain in `oxr_session_locate_views` is deleted. The
  non-Windows service-mode skip went with it; to keep legacy hosted clients
  correct on macOS/Android the OOP service path now reports the head device
  pose in `head_relation` for a runtime-owned window (per-view poses stay
  head-local, so the #48 plane-relative transport is unchanged).
- `tests/tests_oxr_view_space.cpp` pins the contract headlessly against the
  built runtime: base-invariance of the eye centroid relative to VIEW for
  LOCAL/STAGE/VIEW, the rig round trip across the three spaces, and
  `displayPlanePose` in the locate space. The LOCAL offset is read from the
  runtime, never hard-coded.
- Deferred: VIEW as the eye centroid (today VIEW is the plane; the views sit in
  front of it in every class). That will be a VIEW-space offset, not a head
  change — the head device pose must stay parallax-free for the ADR-034
  Amendment 2 rig source.

## References

- Header `src/external/openxr_includes/openxr/XR_DXR_view_rig.h` (SPEC_VERSION 2).
- `src/xrt/state_trackers/oxr/oxr_session.c` (rig parse + raw fill + IPC route),
  `src/xrt/ipc/server/ipc_server_handler.c` (server rig math + workspace
  override), `src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp` (override
  storage).
- Projection math: `docs/architecture/kooima-projection.md`.
- Epic #396 (W7). DP-owned raw eyes + the `xrSetWorkspaceViewRigDXR` override
  landed in PR #488.
- #1370 (Amendment 1): two-leg base-space conversion, concession chain
  deleted, `tests/tests_oxr_view_space.cpp`; app-side wording in
  `docs/guides/displayxr-app-rules.md` F-4 / INV-6.1 and the view-path section
  of `docs/architecture/extension-vs-legacy.md`.

---
status: Implemented (extension surface + shared core + IPC)
owner: David Fattal
updated: 2026-06-07
issues: [396]
code-paths:
  - src/external/openxr_includes/openxr/XR_EXT_view_rig.h
  - src/xrt/state_trackers/oxr/oxr_session.c
  - src/xrt/drivers/qwerty/qwerty_device.c
  - src/xrt/auxiliary/math/CMakeLists.txt
  - src/xrt/include/xrt/xrt_display_metrics.h
  - src/xrt/ipc/server/ipc_server_handler.c
  - src/xrt/ipc/shared/ipc_protocol.h
---

> **Status: implemented** — the `XR_EXT_view_rig` extension surface ships
> (header, registration, per-locate rig chaining, raw-result channel; verify
> vehicles: `cube_handle_d3d11_win` R-toggle, `cube_texture_d3d11_win`
> raw-canvas log); the equivalence-by-construction half is structural (the
> runtime's `m_*_view` ports are deleted — the runtime and every app/engine
> consumer run displayxr-common's type-neutral core `dxr_view_math`,
> v0.4.0); and the rig + raw channel work over IPC for service-mode sessions
> (`session_locate_views_rig`, same server code path as the legacy locate
> plus rig overrides). Remaining tails are optional: consumer migrations onto
> the rig request, the WebXR bridge's move to the explicit raw result,
> surplus-eye exposure, the workspace constraint hook, and ADR promotion of
> this doc. Tracked as
> **W7 of [#396](https://github.com/DisplayXR/displayxr-runtime/issues/396)**.

## Phase 1 decisions (implementation, 2026-06-06)

- **Per-locate, not sticky.** A rig drives exactly the locates that chain it.
  Sticky state would break the raw-eye transport contract: an external-window
  app that stops chaining must get raw display-local eyes in `XrView.pose`
  again (its local math consumes them), not a latched rig's `eye_world`.
- **Raw chain point**: single `XrViewDisplayRawEXT` on `XrViewState::next`
  (as recommended).
- **Validation**: clamp + one-shot WARN per session, never reject. Factors
  clamp to their doc ranges; `virtualDisplayHeight` to [0.01, 1000] (math-safe
  bounds, deliberately wider than qwerty's keyboard-UX range);
  `convergenceDiopters` to [0, 20]; `verticalFov` to [0.01, π−0.01]. Both
  structs chained → one-shot WARN, camera rig wins.
- **Raw eyes**: tracked set verbatim (pre legacy-2D centering, pre surplus
  synthesis); nominal-viewer pair with `isTracking = false` when the DP has no
  lock. Synthesized surplus eyes stay runtime-internal (gap noted below).
- **Boundary conversions**: `convergenceDiopters` → `inv_convergence_distance`
  is numerically identity (qwerty's `cam_convergence` is already diopters);
  `verticalFov` → `tanf(v/2)`.
- **Former Phase-1 gaps, now closed** (W7 completion, 2026-06-07):
  - *IPC*: service-mode locates that chain a rig and/or the raw struct route
    through `session_locate_views_rig` — the same server code path as the
    legacy `device_get_view_poses` (`ipc_try_get_sr_view_poses`), with the
    rig overrides applied and the raw block gathered server-side. Per-locate
    on both sides; non-chained locates take the legacy path untouched.
  - *Texture-canvas raw rect*: turned out to need no plumbing — every
    compositor's `get_window_metrics` already rewrites the window fields to
    the effective canvas sub-rect (`u_canvas_apply_to_metrics`), so the raw
    channel (and the runtime's own rig math) consume the canvas by
    construction. Verified live via `cube_texture_d3d11_win`'s raw-canvas
    log.
- **Still open** (optional): workspace-controller rig constraints (rig stays
  app visual policy); surplus-eye exposure in the raw channel.

# Raw vs Render-Ready Views — the rig API

Today every DisplayXR app re-implements the Kooima view math: it reads raw eye
positions out of `xrLocateViews`, ignores the runtime's `XrView.fov`, and runs
its own copy of `display3d_compute_views` / `camera3d_compute_views` (now
consolidated in `displayxr::math`, #396 W1–W3). Meanwhile the runtime *already
computes* render-ready views through the very same two-rig math — but no app
can drive it.

This doc proposes closing that loop: an app hands the runtime a **rig
descriptor** (the handful of Kooima tunables) chained onto `xrLocateViews`, and
consumes standard `XrView{pose, fov}` like on any other OpenXR runtime. The
runtime already knows the raw eyes in physical-screen coordinates, the display
plane, and the effective window/canvas rect — it can do all the math
internally. The app's job collapses to *feeding descriptors to the rig*.

For the projection math itself (both pipelines, derivations, conventions) see
[`docs/architecture/kooima-projection.md`](../architecture/kooima-projection.md)
— this doc is about *who runs it and through what API*, not the math.

## The model

Three concepts, one data flow:

```
RAW inputs ──> RIG (generator) ──> RENDER-READY output
```

- **Render-ready** is a *fixed point*: a standard `XrView{pose, fov}` pair per
  view. It is **complete for rendering** — view matrix = `inverse(pose)`,
  projection matrix = any off-axis-frustum builder from the skewed asymmetric
  `fov` + the app's own near/far + the app's graphics-API depth convention.
  Crucially this means **clip policy stays app-side**: rig descriptors carry
  *no* near/far parameters, because `fov` is clip-independent. (The W1
  migration showed clip policy and depth conventions are exactly where
  per-consumer divergence lives — the rig API keeps them out of the contract
  entirely.)
- **Rigs** are input-side *generators* that route raw → render-ready. Two
  exist, matching the two canonical pipelines:
  - **display rig** (display-centric, `display3d_view`): the window is a
    portal; tunables = virtual display height (m2v scale), ipd factor,
    parallax factor, perspective factor.
  - **camera rig** (camera-centric, `camera3d_view`): an app-defined camera
    whose frustum is perturbed by eye tracking; tunables = ipd factor,
    parallax factor, convergence, vertical FOV.
- **Raw** is the complete input set the rigs consume: per-eye positions in
  physical display space, the display-plane pose, the **effective canvas
  rect** (handle apps → the window client area; texture apps → the
  `xrSetSharedTextureOutputRectEXT` sub-rect; hosted apps → the runtime's own
  window), the sample timestamp, and the tracking-lock flag.

The whole pipeline is **strictly per-view**: each raw eye position
independently yields one `{pose, fov}`. N views are N independent evaluations
of the same math — nothing anywhere depends on the view count, so the model is
N-view by construction; a rendering mode's view count only changes the array
length, never the math.

A consumer that takes raw + a rig descriptor and runs the shared math gets
*exactly* the render-ready output — that equivalence is the backbone of the
design (see "Equivalence by construction" below).

## What already ships

The surprise that motivated this spike: **render-ready is not a future feature
— it ships today as a complete two-rig system.** What's missing is purely the
app-facing API.

- **The two-rig branch** lives in `oxr_session.c:1392-1442`. Per
  `xrLocateViews` call it selects `camera3d_compute_views` (camera-centric)
  or `display3d_compute_views` (display-centric) based on
  `view_state.camera_mode`, feeds them the raw eyes + screen dims + display
  pose, and extracts `{fov, eye_world}` per view into the returned `XrView`s
  (the runtime discards the matrices — apps build their own from fov).
- **All tunables exist** in `qwerty_view_state` (`qwerty_interface.h:32-50`):
  camera-side `cam_spread_factor / cam_parallax_factor / cam_convergence /
  cam_half_tan_vfov`, display-side `disp_spread_factor / disp_parallax_factor
  / disp_vHeight`. Defaults: camera-centric at convergence 0.5 D (2 m)
  (`qwerty_device.c:642,647`).
- **But the only driver is the qwerty debug device** — keyboard-only: `P`
  toggles the rig (`qwerty_toggle_camera_mode`, `qwerty_device.c:1066`),
  `±`/`[`/`]`/`Space` adjust factors. No API, no per-app control, no way for a
  release build to touch any of it.
- **External-window apps are locked out.** Handle/texture sessions hit the
  `!sess->has_external_window` guard (`oxr_session.c:1393,1416`) and are
  *forced* display-centric at identity m2v (`dt.virtual_display_height =
  screen_height_m`). An external-window app cannot get camera-centric views or
  a non-identity virtual display height from the runtime at all — which is
  precisely why every app does its own math.
- **Window/canvas resolve is already runtime-side.** `oxr_session.c:1311-1340`
  pulls `xrt_window_metrics` and rebases the eyes to the window-local frame
  (dims → screen meters, center offset, even window orientation) — the runtime
  equivalent of `displayxr::math`'s Layer 1 `display3d_resolve_window_rect`,
  with one advantage: the runtime *owns* the HWND / canvas sub-rect, so
  nothing about placement ever needs to cross the API.
- **Surplus-view synthesis is runtime-only.** For rendering modes with more
  views than tracked eyes (e.g. quad modes), `oxr_session.c:1355-1369`
  synthesizes the extra eye positions from the mode's per-view optical offsets
  (`xdev->hmd->view_eye_offsets`). The math itself is per-view and indifferent
  to N — what an app doing its own math lacks is the **input**: the
  synthesized eye positions, because the per-mode optical layout is runtime/DP
  knowledge. Today that means such an app silently feeds wrong (or missing)
  surplus views; under the rig API it inherits the synthesis for free.
- **A raw channel half-exists.** The IPC headless fast-path
  (`ipc_server_handler.c:416-436`, compositor-less clients) returns raw
  display-space eyes + device-default FOVs, skipping the rig math — this is
  what the WebXR bridge consumes and runs its own Kooima on. It's the raw
  concept, but reachable only by headless IPC clients.
- **Raw inputs are complete in the tree** — no new DP/plugin accessor needed:
  `xrt_eye_positions` already carries display-space eyes + `timestamp_ns` +
  `is_tracking` + `valid` (`xrt_display_metrics.h:35-60`);
  `xrt_window_metrics` carries the rect in px and meters
  (`xrt_display_metrics.h:71-97`); display dims/nominal viewer come from
  `xrt_plugin_display_info`.

### And what apps do with it: nothing

The render-ready output is computed every frame and **ignored by every 3D
app**. `cube_handle_d3d11_win/main.cpp:773-775` is representative: the fov
each app submits per view is a ternary — in 3D mode it takes the app's own
`display3d_compute_views()` output; the runtime's `rawViews[i].fov` is
consumed only in the mono/legacy branch.

In 3D mode (`useAppProjection`, i.e. `XR_EXT_display_info` present) the app
uses `XrView.pose.position` as a raw-eye transport, recomputes everything with
its own `displayxr::math` calls, and submits its own fov back through
`xrEndFrame`. The runtime's fov is consumed only in mono/legacy mode. The same
pattern is vendored into both demos and both engine plugins (Unity overrides
its per-view camera projection matrices; Unreal rebuilds its own reverse-Z
projection per ADR-003).

This isn't app authors being perverse — it's the only way to get vHeight /
camera-centric / factor control, because the runtime's render-ready path is
(a) locked to identity-m2v display-centric for external windows and (b) only
tunable via a debug keyboard. Fix the API gap and the recomputation becomes
pointless.

## Proposal: `XR_EXT_view_rig`

One extension, two halves: a **rig request** (app → runtime, chained on
`XrViewLocateInfo::next`) and a **raw result** (runtime → app, chained on
`XrViewState::next`). Decisions locked 2026-06-06: per-session rig state with
qwerty demoted to debug fallback; a single extension for both halves; the
equivalence guarantee via a shared math core (next section).

Sketch (type values from the reserved `1000999xxx` range, next free block
after mcp_tools' `…132`):

```c
#define XR_EXT_view_rig 1
#define XR_EXT_view_rig_SPEC_VERSION 1
#define XR_EXT_VIEW_RIG_EXTENSION_NAME "XR_EXT_view_rig"

#define XR_TYPE_DISPLAY_RIG_EXT      ((XrStructureType)1000999140)
#define XR_TYPE_CAMERA_RIG_EXT      ((XrStructureType)1000999141)
#define XR_TYPE_VIEW_DISPLAY_RAW_EXT ((XrStructureType)1000999142)

// ---- Request: chain exactly ONE of these on XrViewLocateInfo::next. ----

// Display-centric rig: the window/canvas is a portal into the scene.
typedef struct XrDisplayRigEXT {
    XrStructureType          type;   // XR_TYPE_DISPLAY_RIG_EXT
    const void* XR_MAY_ALIAS next;
    XrPosef pose;                  // virtual display plane pose in the locate space
    float   virtualDisplayHeight;  // app units; m2v = this / physical canvas height
    float   ipdFactor;             // [0,1] multiplies view-pose spread about the center
    float   parallaxFactor;        // [0,1] lerps eye centroid toward nominal viewer
    float   perspectiveFactor;     // [0.1,10] scales eye XYZ (object perspective)
} XrDisplayRigEXT;

// Camera-centric rig: an app camera whose frustum eye tracking perturbs.
typedef struct XrCameraRigEXT {
    XrStructureType          type;   // XR_TYPE_CAMERA_RIG_EXT
    const void* XR_MAY_ALIAS next;
    XrPosef pose;                  // camera pose in the locate space
    float   ipdFactor;             // [0,1] multiplies view-pose spread about the center
    float   parallaxFactor;        // [0,1]
    float   convergenceDiopters;   // 1/m to the convergence plane; 0 = infinity
    float   verticalFov;           // radians, full vertical angle
} XrCameraRigEXT;

// ---- Result: app chains this on XrViewState::next; runtime fills it. ----

typedef struct XrViewDisplayRawEXT {
    XrStructureType    type;       // XR_TYPE_VIEW_DISPLAY_RAW_EXT
    void* XR_MAY_ALIAS next;
    XrVector3f  rawEyes[8];        // display-space eye positions (meters,
                                   // display-center origin, +X right +Y up +Z toward viewer)
    uint32_t    eyeCountOutput;    // tracked eyes actually written
    XrPosef     displayPlanePose;  // physical display plane in the locate space
    XrRect2Di   canvasRectPx;      // effective canvas on the panel (window client
                                   // area / texture sub-rect / runtime window)
    XrExtent2Df canvasSizeMeters;  // physical size of that canvas
    int64_t     sampleTimeNs;      // when the eyes were sampled (monotonic)
    XrBool32    isTracking;        // physical tracker lock (vs nominal fallback)
} XrViewDisplayRawEXT;
```

Notes on the shape:

- **Descriptors are exactly the existing tunables** — `XrDisplayRigEXT` maps
  1:1 onto `Display3DTunables` + display pose, `XrCameraRigEXT` onto
  `Camera3DTunables` + camera pose, with two ergonomic conversions at the
  boundary (`convergenceDiopters` → `inv_convergence_distance`, full-angle
  `verticalFov` → `half_tan_vfov`). Nothing new is invented; the extension is
  a transport for state the runtime already consumes.
- **No clip parameters, no placement parameters.** Clip policy is app-side by
  construction (fov is clip-independent). Placement (window rect / canvas
  sub-rect) is runtime-owned — handle/texture/hosted apps never describe their
  own geometry, the runtime already has it (`xrt_window_metrics`, OutputRect).
  This is *stronger* than the app-side Layer 1, which still needs each app to
  fetch its platform rect.
- **Per-locate = per-frame-updatable for free.** Chaining the descriptor on
  `XrViewLocateInfo` means animating convergence or vHeight is just passing
  different values next frame — no setter API, no event round-trip.
- **Chaining a rig lifts the external-window forcing.** The
  `!has_external_window` guard exists because the runtime had no way to know
  what a windowed app wants; an explicit `XrCameraRigEXT` *is* that knowledge.
  Sessions that chain nothing keep today's behavior exactly.
- **Raw result is one struct on `XrViewState::next`**, not per-`XrView`: the
  payload is mostly shared (display pose, canvas, timestamp), only the eye
  array is per-view, and `XrViewState` is already the per-locate output
  container. (Per-`XrView::next` chaining is the rejected-by-default
  alternative — see open questions.)
- **Raw eyes are display-space verbatim** (`xrt_eye_positions` pass-through),
  *not* canvas-rebased — together with `canvasRectPx`/`canvasSizeMeters` the
  raw consumer has the complete untransformed input set and applies
  `display3d_resolve_window_rect` itself. This keeps the raw channel exactly
  dual to what the runtime's own rig path consumes, and formalizes what the
  WebXR bridge gets from the headless fast-path today.

### Who uses which half

| Consumer | Uses | Effect |
|---|---|---|
| Native apps (cube_*, demos) | rig request | delete the per-frame Kooima block; consume `XrView` directly |
| Unity / Unreal plugins | rig request | keep rebuilding matrices *from fov* (engine conventions, reverse-Z) — but stop computing the fov |
| WebXR bridge | raw result | formalizes the headless fast-path; keeps doing its own math |
| Hybrid / debugging | both | request a rig AND read raw to cross-check (the selftest in app form) |

### Per-session state, qwerty as fallback

Rig state moves from the single global `qwerty_view_state` snapshot to a
**per-session** field on `oxr_session`:

- A session that chains a rig descriptor uses it — for that locate, that
  session. Multi-app workspaces compose naturally: each app drives its own rig
  (consistent with "controllers own visual policy"; the runtime stays
  plumbing).
- A session that never chains one falls back to today's behavior: the global
  qwerty debug state (P / ± / [ ] keys) for runtime-window sessions, forced
  identity display-centric for external-window sessions. Qwerty becomes what
  it always was meant to be — a debug device — rather than the only driver.
- Both writers produce the same *shape* (the tunables structs the math
  consumes), so the branch in `oxr_session.c` doesn't fork — it just reads
  session state instead of the qwerty snapshot when a descriptor was chained.

## Equivalence by construction: one math core

The contract "render-ready ≡ what you'd compute yourself from raw" is only
trustworthy if both sides **run the same code**. Today they nearly do — but
via parallel ports:

- Apps/engines link `displayxr::math` (OpenXR-typed, v0.2.0, 5 consumers
  pinned, drift-guard CI in every repo).
- The runtime carries its own xrt-typed FOV-only ports: `m_display3d_view`,
  `m_camera3d_view`, `m_multiview` (`src/xrt/auxiliary/math/`) — the hidden
  **7th consumer** of the Kooima math, and the only one W3 didn't migrate.

The fix is the type-neutral core that W2/W3 deferred:

1. **`displayxr::math` grows a type-neutral core layer** — its own minimal
   `dxr_vec3 / dxr_quat / dxr_pose / dxr_fov` types, zero OpenXR dependency.
   All the math moves here verbatim.
2. **Two thin typed wrappers** over the core:
   - the existing **OpenXR-typed API stays byte-compatible** (`XrVector3f`
     etc. are layout-identical to the core types — wrappers are casts), so the
     5 pinned consumers are untouched until they choose to bump;
   - a new **xrt-typed wrapper** for the runtime, replacing
     `m_display3d_view` / `m_camera3d_view` (and folding `m_multiview`'s
     FOV-only variants).
3. **The runtime FetchContents `displayxr::math` by tag** like every other
   consumer, deleting its ports. From then on, render-ready output and
   app-from-raw output are *the same function* — equivalence isn't tested,
   it's structural. (The existing `display3d_selftest()` still guards the
   core's numerics; the per-repo `no-vendored-math` CI guards against
   re-forking.)

Side benefit: the type-neutral core ends the per-consumer OpenXR-header
provisioning friction (the v0.1.1/v0.1.2 ordering dance) for any future
consumer that doesn't already speak OpenXR.

Sequencing: the core extraction is a `displayxr-common` minor tag (additive —
the OpenXR-typed API doesn't change); the runtime fold-in lands with the
extension implementation so the rig path is born on the shared core.

## Migration / consumers

Once the extension ships, per consumer:

- **cube test apps + demos**: delete the per-frame Kooima block (locate →
  resolve → compute_views → fov extraction) and the depth-remap call —
  replaced by one chained struct + standard fov-to-projection. The
  `displayxr::math` link remains only where an app keeps a raw-mode debug path.
  The qwerty-mirroring key handlers (vHeight, factors, P) become *app-side
  state* feeding the descriptor — same keys, app-owned values, no debug-device
  dependency.
- **Unity / Unreal**: request the extension like `XR_EXT_display_info` today,
  chain the rig, drop the vendored math invocation; matrix construction from
  fov (engine conventions, reverse-Z) is untouched. This deletes the math
  *call sites* the W3 engine migrations had to carefully preserve.
- **WebXR bridge**: moves from the implicit headless fast-path contract to the
  explicit `XrViewDisplayRawEXT` result.
- **Relation to W4 (`displayxr::common`)**: the per-frame view-math block is
  one of the duplicated scaffolding chunks W4 would otherwise extract — the
  rig API deletes it instead, shrinking W4's surface.

## Open questions

- **Raw-result chain point.** `XrViewState::next` (single struct, recommended
  above) vs per-`XrView::next` (more OpenXR-idiomatic for per-view data, but
  7/8 of the payload would be duplicated per view). Decide at spec-writing
  time.
- **Validation/clamping policy.** Qwerty clamps its tunables (ipd ∈ [0.01,1],
  convergence ∈ [0,2] D, vHeight ∈ [0.1,10] m). Should the runtime clamp app
  descriptors to the same ranges (forgiving, but silently alters output) or
  return `XR_ERROR_VALIDATION_FAILURE` (strict, but per-frame error handling
  is awkward)? Leaning: clamp + one-shot WARN log.
- **Workspace interaction.** Does a workspace controller get to constrain
  per-app rigs (cf. the workspace-owns-display-mode rule)? Default answer: no
  — rig choice is app visual policy within its own canvas, not a shared-mode
  resource like 2D/3D. But a controller-imposed override (e.g. forcing
  identity m2v during a layout animation) may want a hook.
- **Surplus-view interplay.** With >2-view rendering modes the runtime
  synthesizes extra eyes from the mode's optical layout. The rig math then
  runs on the synthesized set — fine. But what should `XrViewDisplayRawEXT`
  report: tracked eyes only (pure raw, recommended — synthesis is a rig-side
  concept) or the synthesized set (what the rig actually consumed)?
- **`xrt_eye_positions.valid == false` frames.** Today the runtime falls back
  to nominal-viewer eyes. The raw struct exposes `isTracking` but not
  `valid`-vs-nominal; decide whether nominal-fallback eyes are reported (with
  `isTracking = false`) or the struct is left unwritten.
- **Naming.** `XR_EXT_view_rig` vs folding into a `XR_EXT_display_info`
  spec-bump was decided in favor of a new extension (display_info is static
  per-system properties; rigs are dynamic per-locate state). Struct names
  (`XrDisplayRigEXT` / `XrCameraRigEXT` / `XrViewDisplayRawEXT`) are working
  titles.
- **Implementation note (not a question):** the runtime walks **no**
  `XrViewLocateInfo::next` chain today (`oxr_api_session.c:267` validates the
  base struct only) — input-chain walking in `oxr_xrLocateViews` is new but
  small, and the per-session state + lifted forcing all live inside the
  existing `oxr_session.c:1392-1442` branch.

# ADR-048: Lift viewpoint policy — window-relative viewpoints, rig gains, axis mode and ease-back are runtime policy

**Status:** Accepted (2026-10-08) · Phase 1 implemented · Addendum A (app rig + auxiliary depth, spec v3) implemented · spec:
[XR_DXR_lift.md §4.1](../specs/extensions/XR_DXR_lift.md#41-viewpoint-frame-and-policy-spec-v2-adr-048) ·
extends [ADR-042](ADR-042-vendor-2d3d-conversion-supersedes-default.md) · applies
[ADR-012](ADR-012-window-relative-kooima-projection.md) to lifted content · same split of labour as
[ADR-040](ADR-040-rear-depth-budget.md) and [ADR-046](ADR-046-depth-aware-cursor.md)

## In one paragraph

When the runtime lifts 2D content into 3D (ADR-042), the vendor conversion module synthesizes
views for viewpoints the runtime hands it. Until now those were the tracked eyes as the panel
reports them — relative to the **panel** centre. A lifted video in a window in the corner of the
panel was therefore rendered as if the viewer were looking at it from far off-axis, even when they
faced it squarely. And the look-around was entirely the vendor plug-in's business: a fixed eye
distance, a fixed clamp, vertical parallax switched off by hand, knobs read once per process.
This ADR makes the viewpoints **window-relative** — the runtime rebases them to the centre of the
lifted rect (or the submitting window) — and makes everything that shapes them **runtime policy**:
display-rig ipd and parallax factors, which axes follow the viewer, an offset clamp, and an
**ease-back** that slowly returns a viewer who settles off-axis to the straight-on view. Apps
steer it per stream with `XrLiftViewControlDXR`. The plug-in only translates units.

## Context

### What the module needs

A conversion module renders a frame for N viewpoints. It does not know the panel, the window, or
the viewer — only the numbers it is given. Two things decide whether the result looks right:

1. **The origin.** A viewer facing a lifted rect should get the rect's straight-on views. That
   is the window-relative rule ADR-012 set for Kooima projection: subtract the window's centre
   offset from the eyes, and use the window's physical size. Lift never applied it; the eyes
   reached the module panel-centred.
2. **The shaping.** Raw look-around is a lot of motion parallax for flat content that was never
   captured from more than one viewpoint. Too much exposes disocclusions; vertical parallax on a
   horizontally-trained model jumps; a viewer who simply sits off-centre gets a permanently
   skewed picture. Each of those is a judgment call about the **experience**, the same on every
   vendor's panel.

### Where the shaping lived

In the vendor plug-in, as fixed constants and process-wide environment knobs: an assumed 63 mm
eye distance, a fixed clamp, y and z forced to zero. Nothing per stream, nothing an app could
ask for, nothing shared with how the runtime renders real 3D content (the display / camera rigs
of `XR_DXR_view_rig`).

## Decision

### D1. Viewpoints are relative to the lifted region's centre

The lift thread rebases every viewpoint — tracked or explicit — to the centre of the region the
frame came from, in display axes, metres:

- a lifted weave rect: that rect on the weave client's window (rect pixels → window fractions →
  the window's metres, from the service's existing window metrics);
- `xrSubmitLiftFrameDXR`: the session's window;
- no window (a headless connection): the panel, i.e. the old behaviour.

This is a **fix**, so it is unconditional. The DP is told the frame (`viewpoint_frame = RECT`)
and the region's physical size (`rect_width_m`, `rect_height_m`).

### D2. The runtime owns the policy; the plug-in owns units

For TRACKED viewpoints the lift thread applies, in order:

1. **ipd factor** — scale each eye's offset from the pair midpoint (display-rig step 1a);
2. **parallax factor** — lerp the midpoint toward the nominal viewer (step 1b);
3. **axis mask** — X (default), XY or XYZ: which midpoint components follow the viewer; the
   others are pinned (y = 0, z = the nominal viewing distance);
4. **recenter** — the ease-back filter (D3);
5. **clamp** — the midpoint's x / y offset to `maxOffsetMeters` (0 = none).

The DP receives the processed viewpoints plus `baseline_m` (the resulting eye separation),
`axis_mode` and `max_offset_m`, in fields appended to `xrt_dp_lift_params` behind
`XRT_DP_LIFT_HAS_VIEWPOINT_POLICY` (ADR-020: append-only, no ABI bump). The plug-in normalises by
`baseline_m` instead of a fixed eye distance, honours every component it is sent, and keeps its
own knobs only as defaults. It makes no experience decisions.

The math is a pure, lock-free, clock-free C module (`u_lift_viewpoint`) with unit tests, so the
policy is identical for every vendor and testable without hardware.

### D3. Ease-back: the reference follows the viewer

One filter per stream, frame-rate independent, reset on stream create and on tracking loss:

- **EASE_BACK (default).** The camera eases back to (0, 0, 0), the scene camera origin. If you
  move your head you see look-around temporarily; once your offset has stayed beyond a few
  millimetres for `recenterHoldSeconds` (default 1 s), the camera returns to centre, with no
  look-around, with time constant `recenterTimeConstantSeconds` (default 2 s). Any new head
  motion gives immediate, temporary look-around again. Mechanically, a reference viewer position
  follows the head after the hold and the rendered offset is the head's offset from it, so it
  decays to 0 wherever the viewer settles.
- **OFF.** The plain rect-relative offset.

An "ease to the rect axis" variant (decay the offset, restore look-around only once the viewer
returns to the rect's axis) was considered and rejected: a viewer who settles off-axis would
lose look-around until they moved back.

### D4. Apps steer it per stream, and can see what was used

`XrLiftViewControlDXR` (XR_DXR_lift v2) chains on `XrLiftOptionsDXR` or directly on
`XrWeaveRectLiftDXR`, and is sticky per stream. `XrLiftResultViewpointsDXR` on
`XrLiftResultDXR` echoes the viewpoints a result was synthesized for, with the region's centre
and size — so an app compositing its own 3D over a lifted frame can render for the same eyes.

### D5. Defaults change behaviour, with an A/B switch

Defaults are ipd 1, parallax 1, axis X, no clamp, EASE_BACK, 1 s hold, 2 s time constant.
Existing callers (the browser's lifted media) therefore change in two ways without asking: the
viewpoint is centred on the lifted rect, and an off-axis viewer is eased back after a second.
The first is the fix; the second is the chosen experience. `DXR_LIFT_RECENTER=off` in the
service's environment forces recentering off for every stream, for A/B comparison.

## Consequences

- Lifted content in a window looks straight-on to a viewer facing that window, on every panel.
- Vertical and depth look-around become available to apps that ask (XY / XYZ) once the vendor
  plug-in stops discarding those components — a plug-in change, no runtime change.
- A plug-in built before this ADR ignores the appended fields and keeps its fixed eye
  distance, but it already receives rect-relative eyes: the rebase takes effect immediately, the
  rest of the shaping once the plug-in is updated. A runtime built before this ADR passes a
  short `struct_size`, which an updated plug-in reads as DISPLAY-frame (panel-centred) viewpoints.
- The recenter filter needs real eye tracking to do anything: with the vendor's fallback eyes
  (tracking lost) it stays reset, and sim_display, which reports no tracking, never eases.

## Later phases (not decided here)

- ~~Accept a full `XrCameraRigDXR` / `XrDisplayRigDXR` on lift options~~ — decided in
  Addendum A (A1).
- A metric, off-axis camera input on the vendor module (eye positions in metres relative to a
  screen plane of given size plus a depth scale), replacing today's dimensionless viewpoints.
- ~~Metric depth as an auxiliary output aligned to the rigs~~ — decided in Addendum A (A2).
- Lift in-process (D3D12) for engine apps.

## Addendum A (2026-10-08): the app's rig drives the viewpoints; depth is an auxiliary output

**Status:** Accepted · XR_DXR_lift spec v3 ·
spec: [XR_DXR_lift.md §4.2–4.3](../specs/extensions/XR_DXR_lift.md#42-app-rig-drives-the-viewpoints-spec-v3-adr-048-addendum-a)

An app that mixes its own rendered 3D with lifted 2D content (a 3D scene around a lifted video
panel, a depth-aware cursor over lifted video) needs both to share one camera and one metric
space. Phase 1 gave lifted content window-relative viewpoints and a runtime-owned policy; this
addendum lets the app hand the runtime **its own rig**, and lets the module hand back **depth**.

### A1. An app rig is computed with the `xrLocateViews` math — never re-implemented

An `XrDisplayRigDXR` or `XrCameraRigDXR` (`XR_DXR_view_rig`) chained on `XrLiftOptionsDXR` or
`XrWeaveRectLiftDXR` makes the runtime derive the TRACKED lift viewpoints by running **the same
shared rig core `xrLocateViews` runs** (displayxr-common, `dxr_view_math.h`), with the lifted
rect as the screen. The display rig's eyes come straight out of `dxr_display3d_compute_views`.
The camera rig's eyes are the physical viewer whose Kooima frustum onto the rect equals the
camera's off-axis frustum (`E = Z0·(l·invd + ẑ)`, `Z0 = rect_h / 2·tan(vfov/2)`) — the only
local step, and it is checked against the shared core frustum by frustum in
`tests_aux_lift_rig_depth`. Why not a separate lift camera model: two code paths for "the eyes
this rig sees" would drift, and the whole point is that the app's 3D and the lifted pixels agree.

The rig replaces the policy's ipd / parallax factors (the rig has its own); the axis mask,
ease-back and clamp still shape the viewer before the rig maps it, so the Phase 1 defaults keep
working for an app that only wants its rig's scale. An app that wants the located eyes exactly
chains XYZ + recentering OFF. The rig rides with the options (like explicit viewpoints), matching
`XR_DXR_view_rig`'s own per-call rule. EXPLICIT viewpoints are now accepted on lifted weave rects:
the Phase 1 refusal only existed because nothing could consume them there.

The DP is told the viewpoints' origin (`viewpoint_source`: TRACKED / EXPLICIT / DISPLAY_RIG /
CAMERA_RIG) and the reference distance (`nominal_z_m`), appended to `xrt_dp_lift_params`
(`XRT_DP_LIFT_HAS_APP_RIG`). Rig viewpoints are the app's eyes, so the plug-in reproduces them as
given — no extra view gain.

### A2. Depth is an auxiliary output of SBS / NVIEW streams, from the same inference

A separate DEPTH stream runs a separate conversion, so its depth need not belong to the frame
whose views are woven. Instead an SBS / NVIEW stream created with `XrLiftDepthRequestDXR`
returns, with each result, the depth of **that** conversion (`XrLiftDepthResultDXR`): a texture
behind the views' own fence, its encoding and units, the module's intrinsics, the convergence
depth it placed on the screen, the viewpoint it refers to, and **`depthToDisplay`**.

`depthToDisplay` is runtime policy, derived — not reported by the vendor: the source image fills
the lifted rect seen from the viewpoint midpoint `C`, and the convergence depth `dc` lies on the
screen plane, so a texel `(un, vn)` of depth `d` lands at `R + C + (P(un, vn) − C)·d/dc`. That is
linear in `(un·d, vn·d, d, 1)`: one 4×4. It is honest about what it is — the **display** geometry
of the woven result, the same perspective as the echoed eyes — and the intrinsics stay available
separately for the **camera** geometry. With RELATIVE depth the runtime reports neither, rather
than inventing a scale.

DP contract (ADR-020, append-only, no ABI bump): `xrt_dp_lift_caps` += `aux_outputs`,
`aux_depth_semantics`; `xrt_dp_lift_stream_info` += `aux_outputs`, `aux_depth_format`; a new
`struct xrt_dp_lift_depth`; and D3D11 DP slot 33 **`lift_get_depth`**
(`XRT_DP_D3D11_HAS_LIFT_DEPTH`), called right after a successful `lift_convert` of a stream that
asked for depth. A slot rather than an out-struct on `lift_convert`: `lift_convert`'s signature
shipped and is frozen, and a separate call keeps depth optional per stream and per plug-in (an
absent slot reads as "no depth", which is exactly the old-plug-in case).

### Consequences

- An app renders its 3D with the echoed eyes (or its own rig) and the lifted picture lines up;
  with metric depth it can occlude and point at lifted content in display metres.
- Full fidelity needs a metric, off-axis camera input in the vendor module (the module's own
  camera today is an oblique shear around a convergence plane, not a pinhole); until then the
  runtime's rig viewpoints reach the module through the same dimensionless translation as Phase 1.
- Depth quality and "same inference" are vendor capabilities, reported per result; the runtime
  never synthesizes depth.
- Spec v3 (not folded into v2): v2 can ship on its own, and an app keys these features on
  `SPEC_VERSION >= 3`.

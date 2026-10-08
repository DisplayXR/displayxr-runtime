# ADR-048: Lift viewpoint policy — window-relative viewpoints, rig gains, axis mode and ease-back are runtime policy

**Status:** Accepted (2026-10-08) · Phase 1 implemented · spec:
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

- Accept a full `XrCameraRigDXR` / `XrDisplayRigDXR` on lift options so lifted frames and
  app-rendered 3D share one camera computed with the `xrLocateViews` math.
- A metric, off-axis camera input on the vendor module (eye positions in metres relative to a
  screen plane of given size plus a depth scale), replacing today's dimensionless viewpoints.
- Metric depth as an auxiliary output aligned to the rigs.
- Lift in-process (D3D12) for engine apps.

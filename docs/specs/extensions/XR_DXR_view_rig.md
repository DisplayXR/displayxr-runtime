# XR_DXR_view_rig

| | |
|---|---|
| **Name** | `XR_DXR_view_rig` |
| **Spec version** | 4 |
| **Header** | [`src/external/openxr_includes/openxr/XR_DXR_view_rig.h`](../../../src/external/openxr_includes/openxr/XR_DXR_view_rig.h) |
| **Type values** | `1004999140`–`142` ([registry](../../../src/external/openxr_includes/openxr/README.md)) |
| **Decision** | [ADR-024](../../adr/ADR-024-raw-vs-render-ready-views.md) (raw vs render-ready views, W7 of #396); spec 4: #2189 |

## 1. What it is

The app **declares** a view rig and consumes render-ready `XrView{pose, fov}`;
the runtime owns the view math. It never re-implements Kooima from raw eyes.
Two rigs match the two canonical pipelines:

| Descriptor | Pipeline | Tunables |
|---|---|---|
| `XrDisplayRigDXR` | display-centric: the canvas is a portal into the scene | `pose` (the virtual display plane), `virtualDisplayHeight` (m2v = this / canvas height), `ipdFactor`, `parallaxFactor`, `perspectiveFactor` |
| `XrCameraRigDXR` | camera-centric: an app camera whose frustum eye tracking perturbs | `pose` (the camera), `ipdFactor`, `parallaxFactor` (absolute), `convergenceDiopters`, `verticalFov`, `metersToVirtual` (spec 3) |

Chain exactly one on `XrViewLocateInfo::next`, on every locate it should
drive (the rig is strictly per-locate). Out-of-range values are clamped with
a one-shot WARN, never rejected. Descriptors carry no clip parameters and no
placement: the runtime owns the canvas (window client area, texture sub-rect,
zone, runtime window).

`XrViewDisplayRawDXR` on `XrViewState::next` returns the raw inputs for aware
consumers that do their own math: the DP's per-view eyes in physical display
space, the display-plane pose, the effective canvas (pixels and metres), the
sample time and the tracker lock.

`xrSetWorkspaceViewRigDXR` is for the workspace controller only: it can
impose a rig on its clients' locates, which the service applies server-side.

## 2. Rig-scoped hand joints (spec 4)

**Problem.** `xrLocateHandJointsEXT` returns joints in its base space (LOCAL
= physical panel metres). The views of an app that declares a rig are in the
rig frame: scaled and placed at the rig pose. So without spec 4, joints and
views coincide only for an unscaled rig centred on the panel. That breaks
inline elements, posed or camera rigs, and any scaled canvas. Re-deriving the
rig transform app-side would break the "declare, never compute" rule this
extension exists for.

**Rule.** The descriptor an app chains on `XrViewLocateInfo::next` may also be
chained on **`XrHandJointsLocateInfoEXT::next`**. The runtime then returns
each joint in the **rig frame**, applying exactly the transform the views get
for that descriptor:

| | Position | Radius, linear velocity | Orientation, angular velocity |
|---|---|---|---|
| Display rig | display space → canvas frame (minus the canvas centre, inverse canvas orientation) → × `perspectiveFactor` × `virtualDisplayHeight` / canvas height → placed at `pose` | × the same factor | rotated by `pose` × inverse(canvas orientation) × inverse(display plane) |
| Camera rig | canvas frame → × `metersToVirtual` about the nominal viewer plane (`z` − nominal z) → placed at `pose` | × `metersToVirtual` | as above |
| Nothing chained | base-space metres, unchanged | unchanged | unchanged |

- The eye-pair tunables (`ipdFactor`, `parallaxFactor`) act on the eye pair, not
  on points, and do not apply. The camera frustum (`convergenceDiopters`,
  `verticalFov`) does not move a point either.
- The rig `pose` is taken in `XrHandJointsLocateInfoEXT::baseSpace`. For views
  it is taken in `XrViewLocateInfo::space`, so use the same space for both.
- Hands stay rig-relative (ADR-034 Amendment 2: they follow the rig, never head
  parallax). The transform here is the app's *declared* rig: exactly the frame
  its views are in.
- One implementation: the point mapping is the shared `displayxr-common`
  core's eye mapping for an eye at the joint, with the eye-pair factors neutral
  (`m_view_rig_point.h`). `tests_view_rig_point` pins it bit-for-bit against
  the core across a sweep of canvases, poses and tunables.

**Which canvas — joints are consistent with the most recent view locate.** The
canvas and the physical display plane come from the session's most recent
`xrLocateViews`: the one the runtime resolved for those views, through the
same helper (`locate_resolve_canvas`). That covers window, zone, the #499
portrait swap, and the majority segment of a stereo window split across
screens. In service mode it is the canvas the **service** resolved for that
locate, carried back in the view reply (`ipc_view_rig_canvas`, stamped with
the locate's predicted display time). It is never re-derived from window
rects in the app.

**Staleness is the consumer's contract, not bounded by the runtime.** The
runtime does not refuse or re-resolve an old canvas: joints follow the most
recent view locate, however long ago it ran. A consumer whose canvas can move
(window drag, zone rect, layout animation) must call `xrLocateViews` before
`xrLocateHandJointsEXT` **every frame**. The runtime logs the canvas age once
per verdict change per session.

So locate views before joints in a frame. Before a session's first view
locate, the runtime uses the session's own canvas in-process, and the full
display in service mode. The display plane is re-expressed in the joint
locate's base space at the joint time.

**Gate on the spec version, not on the extension's presence.** A runtime
older than spec 4 ignores a rig chained on `XrHandJointsLocateInfoEXT` and
returns base-space joints with no error. A consumer must read `XR_DXR_view_rig`'s
`extensionVersion` from `xrEnumerateInstanceExtensionProperties` and rely on
rig-frame joints only when it is **≥ 4**; below that, keep joints in base space
or skip rig-scoped hand features.

## 3. Version history

| Spec | Change |
|---|---|
| 1–2 | `XrDisplayRigDXR`, `XrCameraRigDXR`, `XrViewDisplayRawDXR`, `xrSetWorkspaceViewRigDXR` (#396 W7; ADR-024). The version continues the pre-rename `XR_EXT_*` numbering (#738) |
| 3 | `XrCameraRigDXR::metersToVirtual` |
| 4 | The rig descriptors are valid on `XrHandJointsLocateInfoEXT::next`: rig-frame hand joints, radii and velocities (#2189) |

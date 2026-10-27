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
| `XrCameraRigDXR` | camera-centric: an app camera whose frustum eye tracking perturbs | `pose` (the camera), `ipdFactor`, `parallaxFactor` (absolute), `convergenceDiopters`, `verticalFov`, `metersToVirtual` (spec 3). Choose `metersToVirtual` from the gain rule in §3, not from the framing |

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

## 3. Camera-rig gain: `metersToVirtual`, `convergenceDiopters` and `verticalFov` (#2422)

**The gain.** A camera rig magnifies the viewer's physical angles and
disparity by one number:

```
f = metersToVirtual × convergenceDiopters × N
```

`N` is the nominal viewer distance the display reports, in metres. The
runtime takes the display's eye (tracked, or its nominal viewer when nothing
is tracked) relative to the **canvas centre**, multiplies its offset by
`metersToVirtual` and places it in the camera frame (the canvas-centre eye
`(0, 0, N)` maps to `pose`). It then shears the frustum by that offset times
`convergenceDiopters`. An eye `y` metres off the canvas axis sees the frustum
centre turned by `atan(f·y/N)`; the physical angle is `atan(y/N)`.

- **`f = 1` is angle-true.** A viewer offset keeps its physical angle, and the
  disparity matches what the physical window would show.
- **`f > 1` is a gain.** Head-motion parallax and disparity are both
  multiplied by `f`. The runtime does not clamp `f` for a chained rig.
- **`verticalFov` is not a scale.** It sets only the base frustum
  half-tangent, `tan(verticalFov/2)`. It does not enter `f`, and nothing the
  eye moves through is sized by it.

The display rig needs no such rule. It scales the eye and the canvas by the
same `virtualDisplayHeight / canvas height`, so it is angle-true by
construction. Converting a camera rig into a display rig carries `f` into the
display rig's `ipdFactor` and `parallaxFactor`.

### Choosing `metersToVirtual`

Derive it from the convergence distance, not from the framing:

```
metersToVirtual = 1 / (convergenceDiopters × N)        → f = 1
```

It maps the nominal distance `N` onto the convergence distance, so the
viewer-to-window geometry becomes a uniformly scaled copy of the
camera-to-convergence-plane geometry. The app's `verticalFov` still frames the
convergence plane exactly as the app asked.

The tempting value is the **framing scale at the convergence plane**, the
world height of the frustum there divided by the window height:
`2·tan(verticalFov/2) / (convergenceDiopters × canvas height)`. That is the
display rig's meaning of a metres-to-world scale. On a camera rig it gives
`f = tan(verticalFov/2) / tan(canvas vFOV/2)`, where the canvas vFOV is the
angle the window subtends from `N`. A wide app framing on a small window
therefore multiplies every angle, and it looks as if `verticalFov` lifts the
eyes.

### The axis is the canvas, not the viewer

The camera axis is the canvas normal through the canvas centre. Most displays
report a nominal viewer above the window centre, so even at `f = 1` the eye
sits `metersToVirtual × y` world units above `pose` and sees the frame centre
`atan(y/N)` below the camera axis. At `f = 1` that is the true viewing angle,
which is correct for a portal, but it is not "the app camera is the viewer".

An app that wants the nominal eye **on its camera axis** should chain
`XrDisplayRigDXR` instead:

| Field | Value |
|---|---|
| `pose.position` | the glass point: the camera position moved `1 / convergenceDiopters` along the camera's forward axis |
| `pose.orientation` | the camera orientation pitched up by `atan(viewerY / viewerZ)` about its local X axis |
| `virtualDisplayHeight` | `2·tan(verticalFov/2) / convergenceDiopters`, the frustum height at the glass point |
| `ipdFactor`, `parallaxFactor`, `perspectiveFactor` | 1 |

`viewerY` and `viewerZ` are the nominal eye relative to the canvas centre
(the raw eyes of `XrViewDisplayRawDXR`, rebased by `canvasRectPx`). The
direction from the glass point to the nominal eye is then the camera's
backward axis, and the app's framing holds at the glass point.

### Worked example (#2422)

A window 0.1049 m tall on a display that reports its nominal viewer at
`(0, 0.10, 0.60)` m, so `N = 0.6` m and the window subtends about 10°. The app
frames 40° (`verticalFov`) with the convergence plane at 375 cm
(`convergenceDiopters = 1/375` per cm).

| `metersToVirtual` | `f` | nominal eye vs. camera | frustum `tan` up / down | frame centre below the camera axis |
|---|---|---|---|---|
| 2601 cm/m (framing scale at the glass) | 4.16 | 260 cm along the camera's up axis (2.56 m above it in world space for a camera pitched 10.4° down) | −0.330 / 1.058 | 34.7° |
| 625 cm/m (`1 / (convergenceDiopters × N)`) | 1 | 62.5 cm along the camera's up axis | 0.197 / 0.531 | 9.46°, the true viewing angle |

With the first value the whole frustum lies below the camera axis, and
parallax and disparity run at 4.16 times the physical window. With the second
the 40° framing holds at the glass and the eye looks down at the real 9.46°.

To put the eye on the camera axis instead, use the display rig above:
`pose` at the glass point pitched up 9.46°, `virtualDisplayHeight` =
2 × 375 × tan 20° = 272.98 cm. The nominal eye then lands on the camera's
axis about 12 m behind the camera. That is a telephoto view that keeps the
40° framing at the glass.

## 4. Version history

| Spec | Change |
|---|---|
| 1–2 | `XrDisplayRigDXR`, `XrCameraRigDXR`, `XrViewDisplayRawDXR`, `xrSetWorkspaceViewRigDXR` (#396 W7; ADR-024). The version continues the pre-rename `XR_EXT_*` numbering (#738) |
| 3 | `XrCameraRigDXR::metersToVirtual` |
| 4 | The rig descriptors are valid on `XrHandJointsLocateInfoEXT::next`: rig-frame hand joints, radii and velocities (#2189) |

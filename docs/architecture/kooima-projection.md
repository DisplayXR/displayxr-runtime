# Kooima Projection: Display-Centric and Camera-Centric Pipelines

This document describes the two projection pipelines used throughout
the project for 3D (light field) displays with eye tracking.  Both pipelines
take the same raw eye-tracking inputs and share a common first stage, then
diverge in how they build the view and projection matrices.

**Reference**: Robert Kooima, *Generalized Perspective Projection* (2009).

**Canonical implementations** (pure C, OpenXR types):

| Path | Header | Source |
|------|--------|--------|
| Display-centric | `test_apps/common/display3d_view.h` | `test_apps/common/display3d_view.c` |
| Camera-centric | `test_apps/common/camera3d_view.h` | `test_apps/common/camera3d_view.c` |

Runtime-side port (xrt types, FOV-only — no matrices):
`src/xrt/auxiliary/math/m_multiview.h`

---

## Inputs (common to both pipelines)

| Parameter | Type | Description |
|-----------|------|-------------|
| `raw_left` | `XrVector3f` | Raw left-eye position in **display space** (from eye tracker via `xrLocateViews`) |
| `raw_right` | `XrVector3f` | Raw right-eye position in **display space** |
| `nominal_viewer` | `XrVector3f*` | Default viewer position in display space; NULL defaults to `{0, 0, 0.5}` (50 cm from screen center) |
| `screen` | width/height in meters | Physical screen dimensions |
| `near_z`, `far_z` | `float` | Near and far clip plane distances |

**Display space** is defined with the origin at the center of the physical
screen, +X right, +Y up, +Z pointing out of the screen toward the viewer.
Eye positions are in meters.

---

## Common Stage: Eye Factor Processing (Step 1)

Both pipelines begin with `m_multiview_apply_eye_factors()`.  This function
takes the raw eye positions and produces processed positions by applying two
successive factors:

### Step 1a — IPD Factor (`ipd_factor`, range [0, 1])

Scales the inter-eye vector around the midpoint of the two eyes.

```
center   = (raw_left + raw_right) / 2
left_vec = (raw_left  - center) * ipd_factor
right_vec = (raw_right - center) * ipd_factor
```

- `ipd_factor = 1.0` preserves the physical IPD.
- `ipd_factor = 0.0` collapses both eyes to the midpoint (mono).

The midpoint itself is unchanged — only the separation changes.

### Step 1b — Parallax Factor (`parallax_factor`, range [0, 1])

Lerps the eye-pair center toward the nominal viewer position.  Only the Z
component lerps toward `nominal_viewer.z`; X and Y lerp toward zero (the
display-center axis).

```
center'.x = parallax_factor * center.x
center'.y = parallax_factor * center.y
center'.z = nominal_z + parallax_factor * (center.z - nominal_z)

processed_left  = center' + left_vec
processed_right = center' + right_vec
```

- `parallax_factor = 1.0` keeps the tracked position as-is.
- `parallax_factor = 0.0` locks the viewpoint on the display-center axis at
  the nominal distance (no head-tracking parallax, stereo from IPD only).

After this stage, `processed_left` and `processed_right` are ready for the
pipeline-specific steps below.

---

## Display-Centric Pipeline

**Concept**: the physical display is a window into the virtual world.  The
screen edges define the frustum boundaries; the eye's position relative to
those edges determines the asymmetric projection.  Think *"looking through a
window."*

### Tunables (`Display3DTunables`)

| Field | Range | Description |
|-------|-------|-------------|
| `ipd_factor` | [0, 1] | See common stage |
| `parallax_factor` | [0, 1] | See common stage |
| `perspective_factor` | [0.1, 10] | Scales eye XYZ in display space (changes perspective strength) |
| `virtual_display_height` | > 0 | Virtual display height in app units; determines the meters-to-virtual ratio (`m2v`) |

### Pipeline

**Step 1** — Apply IPD and parallax factors (common stage above).

**Step 2** — Derive the display rig's meters-to-virtual factor:

```
m2v = virtual_display_height / screen.height_m
```

This maps physical meters into the app's coordinate system.  If
`virtual_display_height` equals `screen.height_m`, the ratio is 1:1 (one
app unit = one meter).

This `m2v` is **derived**, and it is specific to the display rig: the eye and
the canvas are scaled by the same factor (Steps 3–4), so the rig stays
angle-true whatever `virtual_display_height` is (`perspective_factor` aside).  The camera rig's
`metersToVirtual` is a separate, app-supplied input with a different rule;
setting it to this display-rig value is the mistake described in the
camera-centric section's *Gain* step (#2422).

**Step 3** — Scale eye position into virtual units:

```
eye_scaled = processed * perspective_factor * m2v
```

All three components (x, y, z) are scaled uniformly.  `perspective_factor`
lets the app exaggerate or dampen the perspective effect independently of
the virtual display size.

**Step 4** — Scale screen dimensions into virtual units:

```
kScreenW = screen.width_m  * m2v
kScreenH = screen.height_m * m2v
```

**Step 5** — Transform `eye_scaled` from display space to world space:

```
eye_world = quat_rotate(display_orientation, eye_scaled) + display_position
```

If no `display_pose` is provided, this is an identity transform.

**Step 6** — Build the view matrix:

```
viewMatrix = transpose(R) * translate(-eye_world)
```

where `R` is the rotation matrix from `display_pose.orientation`.  This is
the standard "inverse camera transform" — rotate into display frame, then
translate to the eye.

**Step 7** — Build the Kooima projection matrix.  Given `eye_scaled = (ex, ey, ez)`
and scaled screen half-dimensions `halfW`, `halfH`:

```
halfW = kScreenW / 2
halfH = kScreenH / 2

left   = near_z * (-halfW - ex) / ez
right  = near_z * ( halfW - ex) / ez
bottom = near_z * (-halfH - ey) / ez
top    = near_z * ( halfH - ey) / ez
```

These are the classic Kooima similar-triangle ratios: each screen edge is
projected through the eye onto the near plane.  The resulting asymmetric
frustum is assembled into a standard OpenGL-style column-major projection
matrix.

If `ez <= 0.001`, it falls back to 0.65 m (arm's length) to avoid
division by zero.

**Step 8** — Compute FOV angles from the same geometry:

```
fov.angleLeft  = atan((-halfW - ex) / ez)
fov.angleRight = atan(( halfW - ex) / ez)
fov.angleUp    = atan(( halfH - ey) / ez)
fov.angleDown  = atan((-halfH - ey) / ez)
```

### Outputs (`Display3DView` per eye)

| Field | Description |
|-------|-------------|
| `view_matrix[16]` | Column-major 4x4 view matrix |
| `projection_matrix[16]` | Column-major 4x4 asymmetric Kooima projection |
| `fov` | `XrFovf` — four signed angles in radians |
| `eye_display` | Processed eye position in display space (after all factors and scaling) |
| `eye_world` | Eye position in world space |
| `orientation` | Display orientation (same for both eyes) |

---

## Camera-Centric Pipeline

**Concept**: the app defines a virtual camera (position, orientation, base
vFOV).  Eye tracking data produces asymmetric perturbations to the camera's
frustum.  The physical screen dimensions contribute only the aspect ratio.
Think *"a virtual camera whose frustum shifts with your head."*

### Tunables (`Camera3DTunables`)

| Field | Range | Description |
|-------|-------|-------------|
| `ipd_factor` | ≥ 0 | See common stage.  **Absolute** in the camera rig (`XrCameraRigDXR::ipdFactor`): a stereo camera's inter-axial is not bounded to the natural IPD |
| `parallax_factor` | ≥ 0 | See common stage.  Absolute, like `ipd_factor` |
| `inv_convergence_distance` | ≥ 0 | `1 / convergence_distance` in 1/**world units** (`XrCameraRigDXR::convergenceDiopters`; 1/meters when `m2v` = 1).  0 = convergence at infinity (parallel views).  Controls how much eye displacement shifts the frustum. |
| `half_tan_vfov` | > 0 | `tan(vFOV / 2)` (`XrCameraRigDXR::verticalFov`, converted at the boundary).  Defines the base symmetric FOV before asymmetric shifts.  Divide by a zoom factor at the call site for easy zooming (no trig round-trip).  **Never a scale**: it does not move the eye and does not enter the gain `f`. |
| `m2v` | > 0 | `XrCameraRigDXR::metersToVirtual` (spec 3): meters → world units on the eye.  ≤ 0 / unset = 1.  An **input**, not derived from the frustum; choose it from the gain rule below |

### Pipeline

**Step 1** — Apply IPD and parallax factors (common stage above, reuses
`m_multiview_apply_eye_factors()`).

**Step 2** — Compute the eye displacement from the canvas-centre eye
`(0, 0, nominal_z)`, scaled into world units:

```
eye_local.x = m2v * processed.x
eye_local.y = m2v * processed.y
eye_local.z = m2v * (processed.z - nominal_z)
```

A viewer sitting at exactly the nominal distance has `eye_local.z = 0`.
Only `z` is measured from the nominal viewer: `x` and `y` are measured from
the **canvas axis** (the canvas normal through the canvas centre; in the
runtime the eyes are canvas-rebased first, see *Windowed Mode* below).  A
nominal viewer the display reports above the window centre is therefore an
eye offset like any tracked one, and `pose` is where the canvas-centre eye
maps, not where the nominal viewer maps.

**Step 3** — Transform `eye_local` from display space to world space via the
camera pose:

```
eye_world = quat_rotate(camera_orientation, eye_local) + camera_position
```

**Step 4** — Build the view matrix (same formula as display-centric):

```
viewMatrix = transpose(R) * translate(-eye_world)
```

where `R` is the rotation matrix from `camera_pose.orientation`.

**Step 5** — Scale `eye_local` (world units) by `inv_convergence_distance`
for the projection shift:

```
dx = eye_local.x * inv_convergence_distance
dy = eye_local.y * inv_convergence_distance
dz = eye_local.z * inv_convergence_distance
```

This controls how strongly the eye displacement affects the frustum
asymmetry.  A larger convergence distance (smaller `invd`) means the
frustum shifts less per unit of eye movement.

**Gain.**  Steps 2 and 5 together turn an eye `y` meters off the canvas axis
into a frustum centre turned by `atan(m2v * invd * y)`; the physical angle
is `atan(y / nominal_z)`.  Their ratio is the rig's angle and disparity gain:

```
f = m2v * inv_convergence_distance * nominal_z
```

`f = 1` is angle-true and comfort-neutral (a viewer offset keeps its physical
angle; disparity matches the physical window); `f > 1` multiplies head-motion
parallax and disparity by `f`.  `half_tan_vfov` does not enter `f`.  For
angle-true framing pick

```
m2v = 1 / (inv_convergence_distance * nominal_z)
```

Picking `m2v` as the framing scale at the convergence plane instead
(`2 * half_tan_vfov / (inv_convergence_distance * screen.height_m)`, i.e. the
display rig's Step 2 applied to the camera frustum) gives
`f = half_tan_vfov / tan(canvas_vfov / 2)`: a wide app vFOV on a small window
multiplies every angle (#2422).  The same `f` is the comfort quantity
(`ipd_factor * f`) and is what `dxr_view_rig_camera_to_display` carries into the
display rig's `ipd_factor` / `parallax_factor` when it converts a camera rig.
Worked example and the display-rig recipe for putting the nominal eye on the
camera axis: [`XR_DXR_view_rig` §3](../specs/extensions/XR_DXR_view_rig.md#3-camera-rig-gain-meterstovirtual-convergencediopters-and-verticalfov-2422).

**Step 6** — Compute the base symmetric half-tangents:

```
ro = half_tan_vfov * aspect    (horizontal half-tangent)
uo = half_tan_vfov             (vertical half-tangent)

where aspect = screen.width_m / screen.height_m
```

**Step 7** — Compute asymmetric tangent half-angles by shifting the base
FOV with the scaled eye displacement:

```
denom     = 1 + dz

tan_right = (ro - dx) / denom
tan_left  = (ro + dx) / denom
tan_up    = (uo - dy) / denom
tan_down  = (uo + dy) / denom
```

When `dx = dy = dz = 0` (no eye displacement), this reduces to the
symmetric frustum `[-ro, +ro] x [-uo, +uo]`.  Non-zero displacements
shift and scale the frustum asymmetrically.

**Step 8** — Build the projection matrix from the tangent half-angles:

```
left   = -tan_left  * near_z
right  =  tan_right * near_z
bottom = -tan_down  * near_z
top    =  tan_up    * near_z
```

Then assemble the standard asymmetric frustum projection matrix.

**Step 9** — Convert tangents to `XrFovf` angles (OpenXR signed convention):

```
fov.angleLeft  = -atan(tan_left)
fov.angleRight =  atan(tan_right)
fov.angleUp    =  atan(tan_up)
fov.angleDown  = -atan(tan_down)
```

### Outputs (`Camera3DView` per eye)

| Field | Description |
|-------|-------------|
| `view_matrix[16]` | Column-major 4x4 view matrix |
| `projection_matrix[16]` | Column-major 4x4 asymmetric frustum projection |
| `fov` | `XrFovf` — four signed angles in radians |
| `eye_world` | Eye position in world space (`camera_pose` + rotated `eye_local`) |
| `orientation` | Camera orientation (same for both eyes) |

No `eye_display` — the display-space eye position is not meaningful in
camera-centric mode since the projection is derived from the camera's
base FOV, not from screen geometry.

---

## When to Use Which

| Use case | Pipeline | Why |
|----------|----------|-----|
| AR / passthrough / "window into world" | Display-centric | The physical screen *is* the viewing window; its geometry must drive the frustum |
| 3D game with free camera | Camera-centric | The app controls the camera; eye tracking perturbs the frustum for stereo |
| Fixed viewpoint with known display | Display-centric | Direct physical correspondence between screen and frustum |
| Zoom / vFOV control needed | Camera-centric | `half_tan_vfov / zoom` is trivial; display-centric has no FOV knob |
| 1:1 physical scale required | Display-centric | Set `virtual_display_height = screen.height_m` for metric fidelity |

---

## Windowed Mode: Window-Relative Kooima

When the app runs in a window smaller than the full display, the Kooima
inputs are adjusted so the projection matches the physical window geometry.
See `docs/adr/ADR-012-window-relative-kooima-projection.md` for the full
rationale.

### Adjustment (applied before either pipeline)

1. **Screen dimensions** = window physical size in meters (not display size).
2. **Eye offset**: subtract the window center offset from each raw eye
   position so eyes are relative to the window center, not the display
   center.

```
pixel_size   = display_meters / display_pixels
window_m     = window_pixels * pixel_size
eye_offset   = (window_center - display_center) * pixel_size   (in meters)

screen       = {window_width_m, window_height_m}
raw_eye.xy  -= eye_offset.xy
```

The display rig's `m2v` factor (`virtual_display_height / screen_height_m`)
naturally grows as the window shrinks, producing correct perspective scaling.
The camera rig's `m2v` is an app input and does not track the window, so the
camera rig's gain `f` does not change with the window size.  No
artificial `vs` viewport-scale multiplication is needed.

### Per-segment canvas (multi-screen M3)

A window spanning two displays gets one view set per **segment** (the part of the window
on each display). Each set is window-relative Kooima with the **segment** as the canvas:
the segment's size in its own display's metres, the eye that display's (tracked, or its
nominal viewer) taken relative to the segment's centre as it sits on that display. The
sets are expressed in one frame — the display space of the display holding most of the
window — with the other segment placed beside it the way the window's pixels continue
across the seam, so a display rig still centres the whole window on its pose and each
segment's virtual canvas is offset from it by `m2v ×` that segment's offset from the
window centre. The rig's `virtualDisplayHeight` sizes the WHOLE window: each segment
gets it scaled by its share of the window's height (`h_seg / union_h`), so `m2v` is one
value for every segment — stacked screens are not magnified per half, and panels of
different pitch meet at the seam. Geometry: `src/xrt/state_trackers/oxr/oxr_segment_views.{h,c}`; contract:
`docs/reference/view-configuration-model.md` § *Per-segment views*.

### Where the adjustment lives

| Path | Location |
|------|----------|
| Runtime (hosted apps, FOV-only) | `oxr_session.c` — subtracts `window_center_offset_{x,y}_m` from `raw_eyes[]` |
| App-side (handle/texture apps) | Each app's render loop — uses `ClientToScreen`/`MonitorFromWindow` (Win32) or `NSWindow.frame`/`NSScreen.frame` (macOS) |

---

## Matrix Convention

All output matrices are **column-major** (OpenGL / Vulkan / Metal order).
DirectX callers using row-major `XMMATRIX` should transpose on load.

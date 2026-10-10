# sim_display Integration

`sim_display` is the reference simulation vendor that ships with DisplayXR. It renders to a normal 2D window using side-by-side / anaglyph / blend modes, so contributors can develop and test the runtime without 3D-display hardware. It also serves as the canonical example of a minimal display processor implementation.

## Source layout

`src/xrt/drivers/sim_display/`:

| File | Purpose |
|---|---|
| `sim_display_device.c` | Device enumeration + init |
| `sim_display_interface.h` | Public driver interface |
| `sim_display_macos.m` | macOS-specific window setup |
| `sim_display_processor.c` | Base display processor (mode logic) |
| `sim_display_processor_d3d11.cpp` | D3D11 DP variant |
| `sim_display_processor_d3d12.cpp` | D3D12 DP variant |
| `sim_display_processor_gl.c` | OpenGL DP variant |
| `sim_display_processor_metal.m` | Metal DP variant |
| `sim_display_webcam*.{c,h}`, `sim_display_face_estimator*` | Opt-in webcam eye tracking (#1855): geometry + smoothing (`_tracking`), the capture worker (`_tracker`), the process glue (`sim_display_webcam.c`), the estimator slot |
| `shaders/` | Side-by-side / anaglyph compositing shaders |

## Display modes

sim_display supports several output modes for visualizing stereo content on a 2D screen:

- **Side-by-side (SBS)** — left view in left half, right view in right half. Default.
- **Anaglyph (red/cyan)** — for use with red/cyan glasses.
- **Blend** — interleaved or alpha-blended views for quick visual sanity checks.
- **Interlaced** — 1-pixel-period column interlace: view 0 on even panel
  columns, view 1 on odd, both sampled at the same UV. The only
  phase-sensitive mode — see the next section.

Mode is selected at driver init time
(`SIM_DISPLAY_OUTPUT=2d|anaglyph|sbs|squeezed|quad|blend|interlaced`). See
`sim_display_processor.c` for the mode dispatch and `shaders/` for the
per-mode compositing kernels.

## What sim_display does NOT validate — weave geometry

With the one exception of **interlaced** (below), none of the output modes
above is column-interlaced, so **none of them is sensitive to the woven
texture's exact physical pixel size, or to where that texture lands on the
panel**. A fullscreen triangle renders correctly at any size and any offset, so
sim_display produces a plausible image whether or not the geometry is right.

A real lenticular weaver is sensitive to both. Its interlacing phase is a
function of the target's absolute physical-pixel origin, and any resample
between the woven texture and scanout destroys the pattern outright — a
half-pixel error is visible.

What is fatal is the *resample*, not the scale setting as such. A desktop at
non-100% scale is the usual way to get one, and it is also the usual way to get
a wrong origin, because the geometry the display server reports is then in
logical rather than device pixels (#1596). But a surface that declares a buffer
equal to the region the compositor paints it into still reaches scanout 1:1 on a
scaled desktop, which is why desktop Linux now measures the buffer against its
destination and refuses to weave when they differ, rather than gating on the
scale factor (#1595, `vk_linux_update_surface_not_1to1`).

So a green sim_display run establishes plug-in discovery, the display-processor
path, session and swapchain creation, the frame loop, and that the compositor
hands the DP an atlas. It establishes **nothing** about geometric correctness.
Do not conclude from it that weaving will work.

### `SIM_DISPLAY_OUTPUT=interlaced` — making size and phase errors visible

`SIM_DISPLAY_OUTPUT=interlaced` is a phase-sensitive *proxy* for a lenticular
weave. The output pixel at panel column `X` shows view 0 when
`(X + phase) % 2 == 0` and view 1 otherwise, both sampled at the same
normalized UV — position-preserving like anaglyph, not a rearranging layout
like SBS. `phase` is the weave target's panel-relative X origin
(`canvas_offset_x`, as handed to `process_atlas`).

That gives it the same two failure modes as the real thing:

- **Any resample** — a mis-sized swapchain, a compositing display server
  scaling the surface, a buffer declared in logical rather than device pixels —
  smears the alternating columns into flat grey or a moire beat. A correct 1:1
  path stays crisp at any desktop scale.
- **A wrong origin** flips which eye lands on the even columns, so the image
  goes pseudoscopic (inverted depth) rather than merely shifting.

`SIM_DISPLAY_INTERLACE_PERIOD=N` widens the stripes to `N` pixels so the
pattern — and the phase shift as the window moves — is visible by eye. The
default is 1, because 1 is what a lenticular actually needs and what makes the
mode sensitive.

It is still a proxy. There is **no lens model** here: no slant, no pitch, no
per-view crosstalk, no per-vendor calibration. Interlaced can show you that the
geometry is wrong; it can never show you that a real vendor weave is right.
That still needs vendor hardware, or a measurement that is itself
phase-sensitive.

Backend support: implemented on the **Vulkan** and **OpenGL** display
processors. The **D3D11**, **D3D12** and **Metal** processors fall back to
anaglyph and log a one-shot warning — they share a shader-constant-buffer
layout that the phase would have to be threaded through, which has not been
done yet.

`SIM_DISPLAY_STRICT_PANEL=1` narrows that gap. It makes the Vulkan DP report,
whenever the geometry changes, the weave target it received against the panel
dimensions the driver declared (`SIM_DISPLAY_PIXEL_W`/`SIM_DISPLAY_PIXEL_H`),
plus the canvas rect the compositor asked for:

```
sim_display STRICT PANEL (#817): weave target 2880x1800, panel 2880x1800 (0.300x0.190 m)
  — target is EXACTLY panel-sized (a real weaver gets 1:1 pixels)
sim_display STRICT PANEL (#817): canvas offset (0,0) size 0x0 (panel origin);
  atlas view 1440x900 grid 2x1. ...
```

Because `SIM_DISPLAY_PIXEL_W/H` propagates all the way through (app window →
swapchain → atlas → weave target), this lets a target panel's exact geometry be
staged and verified before that hardware exists.

Two limits are worth stating plainly. The audit compares the target against the
**declared** panel, which is not the same as the **physical** panel — under a
scaled desktop the two diverge, and only the runtime's own desktop-rect resolver
(`display_desktop_rect_is_panel`, surfaced to apps as `isPanelConfirmed`) sees
it. And sim_display implements no `set_present_origin` slot at all, so in every
mode *except* interlaced a phase error is invisible in what it draws — the
audit reports the offset, and the interlaced output is what actually reacts to
it. The option is diagnostic only; it never changes what is rendered.

## Eye tracking

By default sim_display has **no tracker**: it advertises no eye-tracking mode,
leaves every rendering mode untracked, and its display processors report the
nominal viewer with `is_tracking = false`. Two opt-in environment toggles change
that. The MANAGED/MANUAL contract they follow is in
[`docs/specs/vendor/eye-tracking-modes.md`](../../specs/vendor/eye-tracking-modes.md);
the advertisement rules are in ADR-022 (Amendment 1).

- `SIM_DISPLAY_FAKE_TRACKING=1` (+ `SIM_DISPLAY_FAKE_TRACKING_PERIOD_MS=N`):
  a synthetic MANUAL tracker for exercising the tracking-state event. The
  positions stay nominal.
- `SIM_DISPLAY_WEBCAM_TRACKING=1`: real head tracking from a plain webcam
  (MANAGED), described below.

### Webcam eye tracking (#1855)

The goal is look-around on an ordinary monitor with no vendor hardware: a worker
thread reads a webcam, a face estimator finds the two pupils, and sim_display
reports the viewer's eye positions in the panel's frame.

> **Status: the plumbing has landed but the face estimator has not.** The
> default build has no estimator, so setting the toggle logs one WARN
> (`... this build has no face estimator - camera NOT opened ...`) and nothing
> else changes. No camera is opened and the advertised capability does not
> change. Which model and inference dependency to ship is the open question
> on #1855. The estimator is one interface (`sim_display_face_estimator.h`),
> so landing one is a self-contained change.

| Variable | Default | Meaning |
|---|---|---|
| `SIM_DISPLAY_WEBCAM_TRACKING` | unset | `1` turns it on. Unset means no thread, no camera, and the capture DLLs are never loaded. |
| `SIM_DISPLAY_WEBCAM_DEVICE` | first camera | A device index (`0`, `1`, …) or a case-insensitive substring of the camera's name. |
| `SIM_DISPLAY_WEBCAM_HFOV_DEG` | `70` | The camera's horizontal field of view. Distance accuracy depends on this value, so set it from the camera's spec sheet. |
| `SIM_DISPLAY_WEBCAM_OFFSET_MM` | `0,0,0` | The camera position relative to the top-centre of the panel's active area, as `x,y,z` in mm (+x right, +y up, +z towards the viewer). A typical bezel camera is about `0,8,0`. |
| `SIM_DISPLAY_WEBCAM_IPD_MM` | `63` | The inter-pupillary distance prior that the depth estimate uses. |
| `SIM_DISPLAY_WEBCAM_MIRROR` | unset | `1` if the source delivers a mirrored (selfie) image. Media Foundation does not. |

**Pipeline.** The camera is captured through the runtime's existing UVC
source: Media Foundation on Windows, with the same backend as the
`XR_DXR_stereo_camera` UVC source, and the capture DLLs are delay-loaded.
Linux uses V4L2, which is still a stub there (so Linux has no capture yet).
The tracker picks the capture mode closest to 640 px wide at up to 60 fps and
runs the estimator on every frame on its own thread. Each result is mapped,
smoothed with a one-euro filter and stored as the latest state. The display
processors read that state through a mutex that is held only for a
small-struct update, so they never wait on the camera.

**Geometry.** The model is a pinhole camera mounted at the top-centre of the
panel (plus the offset) and looking straight out, with no tilt. Depth comes
from the pupils' pixel distance under the IPD prior:
`Z = f · IPD / d_px`, where `f = (W/2) / tan(hfov/2)`. Each pupil ray is then
scaled to that depth and expressed in the display frame, in metres, with the
origin at the panel centre. The output keeps the processor's view layout: one
view gets the midpoint, two views get the pair, and four views get the pair
32 mm below and above.

**Tracking loss (MANAGED).** If no face has been seen for more than 300 ms,
`is_tracking` goes false and the eyes ease back to the nominal viewer over
500 ms instead of jumping. When the face comes back, the eyes ease from
wherever the blend was to the tracked position over 300 ms. The per-screen
status (ADR-051) reports the tracker as RUNNING or DOWN, and its change
counter moves on every tracking edge.

**Accuracy expectations.** A single plain webcam gives about a centimetre
laterally at 60 cm. Depth is the weak axis:

- a 5 mm IPD error is about an 8 % depth error;
- a yawed head shortens the pupil distance and so reads farther away;
- a wrong field of view scales all three axes;
- camera tilt is not modelled, so a camera that looks down biases the height.

That is good enough for look-around parallax on anaglyph, SBS or interlaced
output. It does not replace a calibrated vendor tracker.

**Privacy.** The camera opens only when `SIM_DISPLAY_WEBCAM_TRACKING` is set.
It opens when the first session display processor asks for eye positions,
not at plug-in load, so a process that never renders never opens it. It
closes when the last such processor is destroyed. sim_display logs one WARN
when the camera opens (name, mode, estimator) and one when it closes. Frames
never leave the worker: they are not stored, logged or sent anywhere. The
toggle is per process and is read from that process's own environment, so
for the IPC path it has to be set on `displayxr-service`.

**Limits.**

- Only MANAGED is offered.
- Multi-screen segment display processors keep their nominal viewer, because
  the camera's pose is known relative to one panel only.
- On a machine that also has a vendor tracking camera, name the webcam with
  `SIM_DISPLAY_WEBCAM_DEVICE`, because index 0 may be the vendor's camera.

## When to use

- Local development on machines without a 3D display
- CI builds and headless tests
- Reference implementation when writing a new vendor — `sim_display_processor.c` is the smallest complete DP implementation in the tree.

For the generic vendor contract, see [`docs/guides/vendor-plugin-onboarding.md`](../../guides/vendor-plugin-onboarding.md) and [`docs/specs/vendor/display-processor-interface.md`](../../specs/vendor/display-processor-interface.md).

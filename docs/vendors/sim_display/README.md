# sim_display Integration

`sim_display` is the reference simulation vendor that ships with DisplayXR. It renders to a normal 2D window using side-by-side / anaglyph / blend modes, so contributors can develop and test the runtime without 3D-display hardware. It also serves as the canonical example of a minimal display processor implementation.

## Source layout

`src/xrt/drivers/sim_display/`:

| File | Purpose |
|---|---|
| `sim_display_device.c` | Device enumeration + init |
| `sim_display_interface.h` | Public driver interface |
| `sim_display_macos.m` | macOS-specific window setup |
| `sim_display_monitor_geometry.{h,c}` | Which real monitor the simulated panel adopts (pure; desktop Linux) |
| `sim_display_processor.c` | Base display processor (mode logic) |
| `sim_display_processor_d3d11.cpp` | D3D11 DP variant |
| `sim_display_processor_d3d12.cpp` | D3D12 DP variant |
| `sim_display_processor_gl.c` | OpenGL DP variant |
| `sim_display_processor_metal.m` | Metal DP variant |
| `sim_display_webcam*.{c,h}`, `sim_display_face_estimator*` | Opt-in webcam eye tracking (#1855): geometry + smoothing (`_tracking`), the capture worker (`_tracker`), the process glue (`sim_display_webcam.c`), the estimator slot (`_none` by default, `_onnx` + `sim_webcam_onnx.cmake` with `XRT_SIM_WEBCAM_ESTIMATOR=onnx`), YuNet pre/post-processing (`sim_display_face_yunet`) |
| `shaders/` | Side-by-side / anaglyph compositing shaders |

## Display modes

sim_display supports several output modes for visualizing stereo content on a 2D screen:

- **Side-by-side (SBS)** — left view in left half, right view in right half.
- **Anaglyph (red/cyan)** — for use with red/cyan glasses. The default
  (`SIM_DISPLAY_OUTPUT` unset), so 3D on sim_display is anaglyph: red from the
  left view, green and blue from the right one. Without glasses that reads as
  red and cyan fringes, wider the further content sits from the display plane.
  In a transparent session (Vulkan) a transparent app keeps one silhouette over
  the desktop: the output alpha is where **both** views cover (#2121), so the
  strip of a silhouette's edge that only one view covers shows the desktop
  instead of a red or cyan copy. A feature thinner than its disparity (a line
  well off the display plane) vanishes the same way. An opaque session keeps the
  alpha of either view, so an opaque app that writes alpha 0 keeps its colours.
- **Blend** — interleaved or alpha-blended views for quick visual sanity checks.
- **Interlaced** — 1-pixel-period column interlace: view 0 on even panel
  columns, view 1 on odd, both sampled at the same UV. The only
  phase-sensitive mode — see the next section.

Mode is selected at driver init time
(`SIM_DISPLAY_OUTPUT=2d|anaglyph|sbs|squeezed|quad|blend|interlaced`). See
`sim_display_processor.c` for the mode dispatch and `shaders/` for the
per-mode compositing kernels.

## Panel geometry

The simulated panel takes the pixel size and physical size of a real monitor,
so the display-info pixel density matches the screen the window actually lands
on. Built-in defaults (1920x1080 px, 0.344x0.194 m) apply only when nothing can
be detected.

| Platform | Pixels | Physical size |
|---|---|---|
| Windows | Primary monitor's current mode (`EnumDisplaySettingsW`, true pixels at any DPI awareness; #369) | default |
| macOS | Main display's backing pixels | `CGDisplayScreenSize` |
| Desktop Linux | The connector's **device** mode (DRM sysfs `modes`), else the desktop mode | EDID millimetres |

On desktop Linux the monitor records come from `os_display_edid_enumerate`
(aux_os, statically linked into the plug-in): the same RandR <-> DRM-sysfs join
the plug-in loader logs as `monitor N output=... connector=...`. The picked
monitor is the **primary** one; with no X server (pure Wayland, where DRM-only
records carry no primary flag) it is the **internal panel** (`eDP` / `LVDS` /
`DSI` connector); else the first. Device pixels, not the scaled logical size,
because the service window presents device pixels 1:1: on the ds1-linux laptop
(eDP-1, 2880x1800, 300x190 mm EDID, 167 % desktop) the panel is 2880x1800 /
0.300x0.190 m, not 3456x2160 and not 1920x1080. A one-shot WARN names the pick:

```
sim_display: adopting monitor 0 (primary) output='eDP-1' connector='card1-eDP-1'
  2880x1800 px (connector mode) 300x190 mm -> panel 2880x1800 px, 0.300x0.190 m
```

`SIM_DISPLAY_PIXEL_W` / `SIM_DISPLAY_PIXEL_H` / `SIM_DISPLAY_WIDTH_M` /
`SIM_DISPLAY_HEIGHT_M` each still win over the detected value, per variable.
The nominal viewer (`SIM_DISPLAY_NOMINAL_Z_M`, default 0.60 m; 10 cm eye
height) is not derived from the panel size on any platform.

Per-monitor descriptions (multi-screen, `get_display_info_for_monitor` /
`create_dp_vk_for_screen`) already used each monitor's own EDID mm and device
mode; this is the head device's (the system-default panel's) geometry.

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

- `TRACKING` = 1 (+ `TRACKING_PERIOD_MS` = N): a synthetic MANUAL tracker for
  exercising the tracking-state event. The positions stay nominal.
- `WEBCAM_TRACKING` = 1: real head tracking from a plain webcam (MANAGED),
  described below.

Both are **settings** read through the host iface (ADR-054 decision 7, N3c):
`displayxr-cli dp set sim-display TRACKING 1` (per user; next session), or
`DXR_DP_SIM_DISPLAY_TRACKING=1` in the environment of the process loading the
runtime, which always wins. `dp options sim-display` lists every key with its
effective value and source. The former `SIM_DISPLAY_FAKE_TRACKING` /
`SIM_DISPLAY_WEBCAM_TRACKING` (and every other `SIM_DISPLAY_*`) environment
name keeps working for one release as an alias, with a deprecation WARN.

### Webcam eye tracking (#1855)

The goal is look-around on an ordinary monitor with no vendor hardware: a worker
thread reads a webcam, a face estimator finds the two pupils, and sim_display
reports the viewer's eye positions in the panel's frame.

> **Status: the estimator is a developer opt-in.** Default and release builds
> carry none (`-DXRT_SIM_WEBCAM_ESTIMATOR=stub`), so setting the toggle there
> logs one WARN (`... this build has no face estimator - camera NOT opened ...`)
> and nothing else changes: no camera opens and the advertised capability does
> not change. A build configured with `-DXRT_SIM_WEBCAM_ESTIMATOR=onnx` gets a
> real one, YuNet run through ONNX Runtime; see *Building the estimator* below.
> The estimator is one interface (`sim_display_face_estimator.h`), so another
> one can replace it without touching anything around it.

| Variable | Default | Meaning |
|---|---|---|
| `SIM_DISPLAY_WEBCAM_TRACKING` | unset | `1` turns it on. Unset means no thread, no camera, and the capture DLLs are never loaded. |
| `SIM_DISPLAY_WEBCAM_DEVICE` | first camera | A device index (`0`, `1`, …) or a case-insensitive substring of the camera's name. |
| `SIM_DISPLAY_WEBCAM_HFOV_DEG` | `70` | The camera's horizontal field of view. Distance accuracy depends on this value, so set it from the camera's spec sheet. |
| `SIM_DISPLAY_WEBCAM_OFFSET_MM` | `0,0,0` | The camera position relative to the top-centre of the panel's active area, as `x,y,z` in mm (+x right, +y up, +z towards the viewer). A typical bezel camera is about `0,8,0`. |
| `SIM_DISPLAY_WEBCAM_IPD_MM` | `63` | The inter-pupillary distance prior that the depth estimate uses. |
| `SIM_DISPLAY_WEBCAM_MIRROR` | unset | `1` if the source delivers a mirrored (selfie) image. Media Foundation does not. |
| `SIM_DISPLAY_WEBCAM_MODEL` | next to the plug-in | `onnx` builds only: the path of the face model to load instead of the one copied next to the plug-in. It must be a YuNet export with dynamic input dimensions. |

**Pipeline.** The camera is captured through the runtime's existing UVC
source: Media Foundation on Windows, with the same backend as the
`XR_DXR_stereo_camera` UVC source, and the capture DLLs are delay-loaded.
Linux uses V4L2, which is still a stub there (so Linux has no capture yet).
The tracker picks the capture mode closest to 640 px wide at up to 60 fps and
runs the estimator on every frame on its own thread. Each result is mapped,
smoothed with a one-euro filter and stored as the latest state. The display
processors read that state through a mutex that is held only for a
small-struct update, so they never wait on the camera.

**Building the estimator.** Configure with
`-DXRT_SIM_WEBCAM_ESTIMATOR=onnx` (for example
`cmake -B build -DXRT_SIM_WEBCAM_ESTIMATOR=onnx`, or add it to the configure
line `build_windows.bat` runs). Configure then downloads two pinned files into
`<build>/_deps/sim_webcam/`, verifying each SHA256, and the plug-in build copies
the runtime library and the model next to `DisplayXR-SimDisplay` (on Linux,
into `_plugins/` too; `cmake --install` puts both in the plug-in directory).
Nothing is committed, and default builds fetch nothing.

| What | Pinned source | Licence | SHA256 |
|---|---|---|---|
| ONNX Runtime 1.30.0, CPU, prebuilt | `github.com/microsoft/onnxruntime/releases/download/v1.30.0/`: `onnxruntime-win-x64-1.30.0.zip` / `onnxruntime-linux-x64-1.30.0.tgz` / `onnxruntime-osx-arm64-1.30.0.tgz` | MIT | `c6ba983b…ba949` / `a5ed5a3c…b3fd` / `6ebb5062…a7012` (full values in `sim_display/sim_webcam_onnx.cmake`) |
| YuNet face detector, `face_detection_yunet_2026may.onnx` (dynamic input shape) | `github.com/opencv/opencv_zoo/raw/26cc381e4d2594bb9f47a26eb8fd96c94a13660d/models/face_detection_yunet/face_detection_yunet_2026may.onnx` (opencv_zoo, from Shiqi Yu's libfacedetection.train) | MIT (`models/face_detection_yunet/LICENSE`) | `ebafce4e3c118d6554634be5c27ab333b4c047a9a8c3faf1d7cf93101c22f0f0` |

Only the headers, the one runtime library (`onnxruntime.dll`,
`libonnxruntime.so.1.30.0` or `libonnxruntime.1.30.0.dylib`) and ONNX Runtime's
`LICENSE` are extracted. The plug-in does **not** link ONNX Runtime. It loads it
on the worker's first frame, by full path from its own directory, so the
plug-in still loads when the library is missing, and Windows' own
`System32\onnxruntime.dll` (an older Windows ML copy) is never picked up. If the
library or the model is missing, the estimator reports itself unavailable with
one WARN, and the camera is not opened. If either fails to load, one WARN is
logged and every frame then counts as "no face". Inference never logs per
frame.

YuNet is a face *detector* that also returns five landmarks, two of them the
eye centres. It does not locate the iris, so the "pupils" are the eye centres.
For a viewer looking at the screen the difference is a millimetre or two, well
inside the depth error the IPD prior already adds. The frame's luma is reduced
to 320 px wide (aspect kept, padded to a multiple of 32) and copied into all
three input channels. The single most confident face with a score of at least
0.6 wins. On the win box a 640 x 480 frame takes a few milliseconds on two CPU
threads. The pre- and post-processing (`sim_display_face_yunet.c`) has no ONNX
Runtime dependency, so `tests_sim_webcam_tracking` covers it in every build.

Platforms: Windows x64, Linux x64 and macOS arm64 have pinned packages, and any
other platform fails at configure time. Only Windows has a working camera
source today, so the Linux and macOS builds compile and load the estimator but
do not track.

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

# Viewer-Tracking Provider Discovery Contract

**Status:** implemented (ADR-054 §4.1, N6 Part 2, #2144) — in-process and service hosting (§4a), per-screen routing (§4b).
**ABI:** `src/xrt/include/xrt/xrt_tracking_plugin.h`, major 1 (`XRT_TRACKING_PLUGIN_API_VERSION_CURRENT`).
**Siblings:** [plugin-discovery.md](plugin-discovery.md) (display processors),
[input-provider-discovery.md](input-provider-discovery.md),
[conversion-provider-discovery.md](conversion-provider-discovery.md).

A **viewer-tracking provider** measures where the viewer is — head pose and the left / right eye
positions, in display space — from a sensor the display processor does not own: a colour webcam,
a depth camera, a wearable. It is the fourth component class of ADR-054. Eye tracking does two jobs
that used to be conflated: *weaving eyes* (internal to the display processor, phase-matched to its
optics) and *look-around eyes* (the eyes `xrLocateViews` renders for). A provider only ever serves
the second job, and only on a screen whose display does not track the viewer itself.

## 1. What a provider is

- **Session-free.** It never sees an OpenXR session, a compositor or a graphics device.
- **Owns its own thread.** `start()` opens the source and starts the provider's thread, which
  pushes timestamped samples into a runtime-owned sink; `stop()` joins it.
- **Publishes samples, not a latest value.** Each sample carries its *capture* time. The trunk
  keeps them in an eye-pair ring (`util/u_eye_history.h`, the same ring the DP eye query uses,
  N6 Part 1) and samples it at the caller's target time — the frame's `displayTime` for
  `xrLocateViews`: interpolated between samples, extrapolated a bounded distance past the newest
  (50 ms), "last known, not tracking" when the newest is older than 500 ms.
- **Never pushes into a display processor.** There is no `set_viewer_eyes` slot on the DP; the
  trunk decides whose eyes the *views* follow, the DP keeps weaving from its own.

## 2. C ABI

Single exported symbol `xrtTrackingPluginNegotiate` (`XRT_TRACKING_PLUGIN_ENTRYPOINT_NAME`),
`xrt_tracking_plugin_negotiate_fn_t`. ADR-020 rules: `struct_size` first on every struct,
append-only vtables guarded by `XRT_TRACKING_PLUGIN_IFACE_HAS`, a major mismatch is rejected by the
shared loader's ABI gate before any dispatch.

| Slot | Meaning |
|---|---|
| `probe(&inst)` | Is there a **usable source** on this system? Cheap, opens nothing. `XRT_SUCCESS` = yes; `XRT_ERROR_PROBER_NOT_SUPPORTED` = cleanly no. This is the "usable source at system creation" §5 composes on. |
| `describe(inst, &info)` | What it tracks (`XRT_TRACKING_TRACKS_EYE_PAIR` / `HEAD_POSE` / `GAZE` / `CAMERA_SPACE`), typical latency, a human source string, the camera key. Optional. |
| `start(inst, sink)` | Start producing into `sink->push(sink, &sample)` from the provider's thread. Called **on demand** — the first time the trunk needs a viewer on a screen this provider serves — never at load. |
| `stop(inst)` | Stop and close the source; returns after the thread stopped pushing. |
| `get_status(inst)` | `STOPPED` / `RUNNING` / `NO_SOURCE` (camera unplugged). Non-blocking. Optional. |
| `destroy(inst)` | Free. |
| `describe_settings` | The settings it reads (ADR-054 decision 7). Optional. |

`struct xrt_tracking_viewer_sample` (display space: metres, origin at the centre of the display
surface, +X right, +Y up, +Z out of the glass toward the viewer):

| Field | |
|---|---|
| `flags` | `EYES_VALID`, `TRACKING` (lock right now; clear = last known), `HEAD_POSE_VALID`, `CAMERA_VALID` |
| `capture_ns` | Monotonic (`os_monotonic_get_ns`) time the sensor captured it |
| `left_eye[3]`, `right_eye[3]` | Eye positions |
| `head_position[3]`, `head_orientation[4]` | Head centre + orientation (x, y, z, w) |
| `confidence` | 0..1, provider scale |
| `screen_hint` | Reserved for per-screen providers (0 today) |
| `camera` | `struct xrt_tracking_camera_sample`, see §6 |

Host iface (`struct xrt_tracking_plugin_host_iface`, process-lifetime storage):
`get_setting` (the one options contract), `get_display_geometry` (panel size + nominal viewer of
the active display; `XRT_ERROR_INPUT_HOST_GEOMETRY_NOT_READY` before the runtime knows it), and
twelve reserved slots — the place an ADR-043 camera-consumer open and the N8 pose page land
without a major bump.

## 3. Discovery

### Windows — registry

`HKLM\Software\DisplayXR\TrackingProviders\<id>` with the shared row fields: `Binary` (required,
absolute), `DisplayName`, `Vendor`, `Version`, `ProbeOrder` (default 100, lower wins), `Enabled`
(DWORD, absent = 1) and a `Settings` subkey. The per-user overlay applies exactly as for the other
classes: `HKCU\Software\DisplayXR\TrackingProviders\<id>\Enabled` wins over HKLM unless
`HKLM\...\TrackingProviders\Policy\Lock` = 1. `Policy` is reserved (never a row).
`PreferredPlugin` (REG_SZ) at the root: HKCU, then HKLM (HKCU hidden under `Policy\Lock`).

### POSIX — JSON manifests

`NNN-<id>-tracking-provider.json` in the shared discovery roots (`XRT_PLUGIN_SEARCH_PATH`, then the
per-user dir, then the system dirs) — the suffix is the router (ADR-053), so the DP loader never
reads one. Schema as every class (`file_format_version` "1.0", `plugin.{id, display_name, vendor,
version, binary_path, probe_order, enabled, settings, settings_schema}`). `PreferredPlugin`:
`XRT_PREFERRED_TRACKING_PLUGIN_ID`, then the per-user `tracking-preferred` file. There is no
per-user `Enabled` store — the manifest's `enabled` is the switch.

### Android

Deferred: vendor components ship inside the runtime APK there and no Android tracking provider
exists yet (the class row has no Android prefix).

### Component settings

The one options contract (ADR-054 decision 7, `docs/specs/runtime/plugin-discovery.md`):
`DXR_TRACKING_<ID>_<KEY>` in the hosting process's environment (always wins), the per-user store,
the row's `Settings\<KEY>` / manifest `settings`. A provider never calls `getenv`. Reserved key
names for camera-based providers are listed in §6.

### Selection and lifetime

The scan is one-shot per process and **lazy**: it runs the first time the arbiter (§4) needs it,
which is only for a display that does not track, declares EXTERNAL_OK, in a process that hosts
providers. Each enabled row is loaded through the shared loader (path guard, provenance gate,
ABI gate) and probed; every provider whose probe claims a usable source stays resident, ranked
**`PreferredPlugin` first, then ProbeOrder** (ties by id). The rank-0 provider serves the screen.
`DXR_TRACKING_PROVIDERS=0` in the process skips the scan (CTS / test isolation).

The primary screen's viewer source is decided at system creation and is **static per instance**:
a provider enabled, disabled or preferred later takes effect at the next app launch (the same rule
as `dp set`) — in the service, at the next service start (§4a). Other screens follow the display
topology (§4b). The chosen provider is started on the first viewer request and stopped at system
teardown.

## 4. Arbitration (trunk policy)

Per screen, in this order (`target_viewer_arbitrate`, pure, `tests/tests_viewer_arbitration.cpp`):

1. **The display processor's own eyes, if it tracks** (`supported_eye_tracking_modes != 0`) — even
   while its tracker has lost the face. Weave and rendered views must come from the same eyes; the
   trunk never splices a second source into a screen whose weave uses tracked eyes. A provider is
   in **standby** there: enabled, outranked, no camera opened — reason "display tracks the viewer
   on this screen".
2. **The best enabled provider** — only when the display's **viewer policy** is `EXTERNAL_OK`.
3. **The nominal viewer.**

A display without tracking whose policy is `OWN` keeps its own (untracked) eyes; `FIXED` keeps the
nominal viewer ("display is FIXED"); `EXTERNAL_OK` with no usable provider gives "no source".

### The viewer-policy slot

`xrt_plugin_iface::get_viewer_policy(inst, display)` — appended after `get_claimed_cameras`
(ADR-020: `struct_size`-gated, `XRT_PLUGIN_IFACE_HAS_VIEWER_POLICY`, no ABI bump), returning
`XRT_PLUGIN_VIEWER_POLICY_OWN` (1), `EXTERNAL_OK` (2) or `FIXED` (3). A plug-in without the slot,
a NULL slot, 0 or an unknown value read as **FIXED**, so no installed display changes behaviour.
`display` is a runtime-built descriptor, NULL = the plug-in's active panel. `sim_display` declares
EXTERNAL_OK. Reference: `docs/reference/xrt_plugin_iface.md`.

### 4a. Hosting: in-process and in the service

The provider is hosted by **the process that computes the views**, once:

| Process | Hosts? | What it does |
|---|---|---|
| In-process runtime (`_handle` / `_texture` / `_hosted`, no service involved) | yes | `oxr_session.c` splices the provider's eyes into `xrLocateViews` |
| `displayxr-service` | **yes — one provider for every IPC client** | its own native system runs the same loader and the same arbitration (`target_viewer_tracking.c`); the IPC view paths splice the provider's eyes |
| IPC client (any app under `XRT_FORCE_MODE=ipc`, a workspace session, a weave present-owner) | **never** | it never creates a native system, so `xrt_system_devices::viewer_tracking` is NULL and nothing is scanned or loaded; the service's answer is used as is |

A hybrid runtime is therefore in-process when it has no service and an IPC client when it has
one, and a provider is never loaded twice for one view path. (An in-process app running *beside*
a service still hosts its own: the two are different view paths. Each starts its provider only
on its first viewer request, so the second camera open happens only when both kinds of app run
at once — see §10.)

In the service, every place that hands an IPC client eyes **for its views** asks the arbitration
(`ipc_viewer_override_eyes` → `xrt_viewer_tracking_override_eyes`, `ipc_server_handler.c`):

- the locate — `ipc_try_get_sr_view_poses` (D3D11 service: the client's render pair and the
  `XR_DXR_view_rig` raw channel) and `ipc_try_get_oop_view_poses` (macOS / desktop Linux / Android
  null + comp_multi: the head set, DP or nominal, N-view shape kept and translated onto the pair);
- the zone-scoped locate re-framed onto another screen (ADR-047 Amendment 4 addendum) — that
  screen's id;
- `compositor_get_predicted_eye_positions` — the live `isTracking` and the tracking-state event;
- `compositor_get_segment_metrics` — the per-screen table an IPC client computes per-segment views
  from (§4b). That message carries no target time: the splice samples at "now".

The splice itself (`xrt_viewer_tracking_splice`, `xrt/xrt_viewer_tracking.h`) is shared with the
in-process locate, so both paths produce identical views. A screen whose display tracks, or whose
policy is not EXTERNAL_OK, answers false everywhere and the DP's eyes stand; the **weave** always
keeps the DP's own eyes (ADR-033).

The composed advertisement (§5) is composed in the service's own `xrt_system_compositor_info` and
head rendering modes, which is exactly what `system_compositor_get_info` and the shared-memory head
carry to every client — an IPC client sees MANAGED|MANUAL + HAS_TRACKING only because the service
will deliver it.

**Lifetime in the service.** The decision is made when the service creates its system and is
static for that service run (the scan is one-shot per process; the advertisement every client
reads is static per instance). The provider starts on the first IPC locate that needs it and stops
at system teardown. The service subscribes the tracking class to its policy watch only to say,
once, that a registration / preference change applies at the next service start.

Not covered by the service splice yet: a workspace tile framed from **another presenter's** screen
(`comp_d3d11_service_get_client_workspace_view_frame`, ADR-052 D2) keeps that presenter DP's eyes,
and the `XR_DXR_weave` output eyes of a present-owner stay the DP's (they are weaving eyes).

### 4b. Per screen: read the segment DP of THAT screen

The arbitration is a per-screen question and its inputs are per-screen: *whether the display on
this screen tracks* and *its viewer policy* are read from **the display processor bound to that
screen** — the plug-in that won the monitor in the DP registry (`owning_iface` /
`owning_instance`, the plug-in a segment DP is created from, which `dp use --screen` decides), its
`get_viewer_policy(inst, <that monitor's descriptor>)`, and the screen info's
`supported_eye_tracking_modes` (`get_display_info_for_monitor`). Two different vendors' displays on
one desk (one tracking, one not) get two different answers.

`target_viewer_arbitrate_screens` (pure, `tests/tests_viewer_arbitration.cpp` `[per_screen]`)
applies §4 to every screen with the one provider:

- a provider serves **every** EXTERNAL_OK screen that has no tracking DP;
- a tracking DP (or an OWN display) keeps its own screen — the provider is in **standby** there;
- FIXED screens stay nominal; a plain monitor (no DP) is nominal ("no display processor on this
  screen").

**Display space of a non-primary screen.** A provider reports in the display space of the screen
it is calibrated to — the **primary** screen: the host's `get_display_geometry` describes the
active panel, and the camera-pose calibration (§6) is keyed by screen. Another screen receives the
sample **translated** by the offset of the primary's centre in that screen's display space
(`target_viewer_map_offset`): the vector between the two centres in desktop pixels, each screen's
part of it read at that screen's own pixel pitch (`width_m / desktop_width`; a screen without a
physical size borrows the other's), z unchanged. This is the workspace's one-layout model
(ADR-052 D2, the same assumption `comp_ws_map_pose` makes): **exact for coplanar screens that abut
the way the OS desktop says they do**, an approximation for angled or spaced screens. A screen that
cannot be mapped (an empty desktop rect, no pitch anywhere) stays nominal with the reason "no
desktop geometry to map the provider's samples onto this screen". The sample's `screen_hint` stays
0 (= the calibrated screen); a provider that serves several screens natively is a follow-up.

**When it is evaluated.** The primary screen's decision is the one made at system creation (§3,
static per instance; it is the one the advertisement is composed from). Every other screen is
re-evaluated each time the runtime **publishes a display topology** — system creation, a display
hotplug sweep (ADR-047 Amendment 8), the service's display re-probe — in
`target_instance.c::publish_screens_locked`, with one WARN per screen whose answer changed. The
lazy scan runs then only if some screen can take a provider.

**Where it is used.** `xrt_viewer_tracking::get_source / get_eyes_at` take a screen id
(`XRT_VIEWER_SCREEN_PRIMARY` = 0, or a monitor id). In-process, a locate split across screens
(`oxr_session_locate_views`, the segment table) splices each segment's eyes by its `screen_id`;
over IPC the service splices the segment table it sends (§4a). The per-screen
`xrEnumerateDisplaysDXR` capabilities stay what each screen's DP reports (only the session's
advertisement is composed) — see §10.

**Status.** `displayxr-cli status --json` reports, per screen, `eye_tracking.viewer_source`
(`dp` / `provider:<id>` / `nominal`), `viewer_reason`, and **`viewer_provider`** — what the
provider is doing for that screen:

| `viewer_provider` | Meaning |
|---|---|
| `serving` | the views follow it and it produces fresh samples |
| `idle` | chosen, not started yet (it starts on the first viewer request) |
| `paused` | chosen and started, but its source is gone (`get_status` NO_SOURCE) or its samples are stale: the views hold the last known viewer, `isTracking` false |
| `standby` | enabled but **outranked**: the display tracks (or owns) the viewer on this screen |
| `no_source` | the screen would take one (EXTERNAL_OK, no tracker) but no provider is usable |
| `none` | no provider involved (FIXED, no DP, nothing enabled) |

"standby" is never "paused": the first is a display that tracks, the second a provider that lost
its source.
## 5. The composed `XR_DXR_display_info` advertisement

The display plug-in reports only what **it** does; `sim_display` stays honest (0 modes, no
`HAS_TRACKING`) unless its own dev toggles are on. The runtime composes what the app sees:

```
advertised(screen) = DP tracks
                     OR (DP viewer policy == EXTERNAL_OK
                         AND an enabled provider has a usable source at system creation)
```

When the composed answer comes from a provider the runtime reports
`supportedModes = MANAGED | MANUAL`, `defaultMode = MANAGED`, and sets
`XRT_RENDERING_MODE_FLAG_HAS_TRACKING` on the display's 3D modes (`hardware_display_3d` or
`view_count > 1`) — which is what `isTracking` is gated on. Static per instance. **Live state**
rides `XrViewEyeTrackingStateDXR::isTracking` and `XrEventDataEyeTrackingStateChangedDXR`: no face,
camera unplugged, provider stopped or samples older than 500 ms → `is_tracking` false.

The service composes it too, in its own system compositor info, because it hosts the provider and
splices its eyes into every IPC client's views (§4a) — an IPC client is told about tracking only
because the service delivers it. The composed answer is the **primary** screen's; the per-screen
`xrEnumerateDisplaysDXR` capabilities stay each screen's DP's own (§10).

## 6. Camera-space samples and camera pose calibration

A camera-based provider MAY report, beside the display-space sample, what it measured in its
**camera** frame — `sample.camera` with `XRT_TRACKING_SAMPLE_CAMERA_VALID`:

| Field | |
|---|---|
| `camera_key` | Stable key of the camera (USB `"vvvv:pppp"`, OS device id, or a provider-chosen name) |
| `left_dir[3]`, `right_dir[3]` | Unit direction from the optical centre to each eye point (the landmarker's accurate part) |
| `left_distance_m`, `right_distance_m` | Estimated distance along it (iris-size depth, ±5–10 %) |

Same `capture_ns` as the display-space sample. Camera frame: origin at the optical centre, +X right
as the user sees it (un-mirrored), +Y up, +Z out of the lens toward the viewer. `describe()` sets
`XRT_TRACKING_TRACKS_CAMERA_SPACE` when it fills the block. Its one consumer is the camera pose
calibration below. Any provider that reports camera-space samples gets calibration for free: the
method and the solver belong to the class, not to a provider.

### Camera pose calibration

Design: `displayxr-tracking-open` `docs/design.md` §10 (mirror alignment). Solver:
`src/xrt/auxiliary/math/m_mirror_calib.{h,c}` (pure C11, `tests/tests_mirror_calib.cpp`).

**Method.** The display's front surface is a weak mirror: an eye that sees a dot overlap its own
reflection is on the display normal through that dot (closed-eye variant: the *midpoint* of the
two eyes is). Each such alignment pins the aligned eye point's display-space `(x, y)` to the dot's
known position `D_i` — pixel offset × pixel pitch from the display's reported metres — with no
depth.

**Model.** Camera pose `(R, T)`: display point = `T + R · camera point`. Sample `i` has the eye
point's camera-space unit direction `u_i`, its estimated distance `s̃_i`, and an unknown true
distance `s_i`:

```
xy( T + s_i · R · u_i ) = D_i          (two equations, one new unknown per sample)
```

For a given pose each `s_i` is eliminated in closed form (the 1-D least-squares foot,
`s_i = a·(D_i − T_xy) / |a|²` with `a = xy(R u_i)`), so the iteration runs over the pose
parameters only. The residual is the in-plane distance between the fitted eye point and its dot.

| Mode | Unknowns | Minimum | Solve |
|---|---|---|---|
| `a` | `R = I`: `T_x, T_y` | 2 samples at 2 distinct dots | closed form: each sample is a line through `D_i` along `xy(u_i)`; `T` is their least-squares meet |
| `b` | + pitch (`R = Rx(θ)`) | 3 samples; two head distances to condition θ | Levenberg–Marquardt from (a) |
| `c` | full `R` (rotation vector) + `T_x, T_y`, optional focal scale `k` | 7 samples at 3 distinct dots (9 in practice: a 3×3 grid at 15 % inset, two distances) | LM from (b) |

`k` = the camera's true focal length / the one its directions were computed with; the corrected
direction is `normalize(u_x, u_y, k·u_z)`. The reported angles follow
`R = Rz(roll)·Ry(yaw)·Rx(pitch)` (positive pitch = optical axis tilted down); the quaternion is
authoritative.

**Observability.**
- **`T_z` never enters the `x, y` equations, in any mode** — not only under `R = I`. The solver
  reports the assumed value (`--tz-mm`, default 0 = on the glass plane), which biases the viewer
  distance one-for-one by a few millimetres. Pinned by a test: data generated with `T_z = 25 mm`
  fits exactly at any assumed `T_z`.
- **One dot is degenerate** (scale ambiguity: the dot baseline is what fixes the scale). The
  solver refuses — `status: "degenerate"` with a reason — when the samples cover fewer distinct
  dots (≥ 10 mm apart) than the mode needs, when the in-plane rays are parallel, or when the
  column-scaled normal matrix at the solution is rank deficient. Above condition 10⁶ it still
  solves but flags `ill_conditioned`.
- **A fit that puts an eye point behind the camera is refused** (`behind_camera`): typically a
  wrong model (mode `a` on a strongly pitched camera) or a mirrored camera frame.
- **Accuracy is geometry-limited.** With a top-bezel camera the centre / top / bottom dots lie on
  one line through the camera, so `T_y` comes from where the left and right rays cross it. With
  0.2° direction noise and 2 mm alignment noise, five samples in mode `a` land at a median 2.3 mm
  (p95 ≈ 6 mm) over random bezel poses; ten samples, median 1.8 mm. The procedure's live check
  (and arrow-key nudge) closes the tail; rejection (`--reject-mm`, the procedure's 5 mm) drops a
  sample captured before the head settled.
- **Iris scale.** `iris_scale = median(s_i / s̃_i)` over the used samples: the user's own iris
  size against the landmarker's model, which removes the largest depth error. It is committed as
  `CAMERA_POSE_<n>_DEPTH_SCALE` (`--depth-scale`).

**Dashboard.** Components → Tracking providers → **Calibrate camera…** on a provider-served screen
opens the calibration window (`src/dashboard/README.md` § *Camera calibration window*): full screen on
that screen, the six targets of design §10 (centre; left, right, top, bottom at a 30 % inset; centre
leaning back), capture on Space once the eye midpoint has been still for 0.5 s, `solve` with mode `b`
and `--reject-mm 5`, retake of the flagged samples, the live-check dot (computed client-side from the
camera block through the solved pose and iris scale) with 1 mm arrow-key nudges, then `apply`.

**CLI** (the Dashboard drives these; it links no runtime code, ADR-051 D4):

```
displayxr-cli tracking calibrate --screen <key> --watch --json [--provider <id>] [--count N] [--interval-ms N]
displayxr-cli tracking calibrate solve --in <file|-> [--mode a|b|c] [--eye mid|left|right] [--focal]
                                       [--reject-mm X] [--tz-mm X] [--json]
displayxr-cli tracking calibrate apply --screen <key> --camera <key> --pose tx,ty,tz,qx,qy,qz,qw
                                       [--provider <id>] [--depth-scale X] [--focal-scale X] [--slot N] [--machine] [--json]
```

- **Watch** hosts the provider headlessly in-process (the `selftest` path: discovery, display
  info, the arbitration; the provider is started with a CLI-owned sink), and prints NDJSON: one
  `hello` line, then one `sample` line per pushed sample (latest-wins; `--interval-ms` throttles).
  It stops on Ctrl-C, on EOF of a *piped* stdin (the Dashboard closing its end), or after
  `--count` samples; without `--watch`, after one. Exit 3 with an `error` line when no provider is
  resident (`no_provider`), it fails to start (`start_failed`), or it pushes nothing in 5 s
  (`no_samples`). Reading the service's provider instead is a follow-up (§10 item 5).

```json
{"type":"hello","screen":"<key>","provider":"sim-tracking","source":"scripted sweep (simulated webcam)",
 "camera_key":"sim-tracking:webcam0","tracks_camera_space":true,"viewer_source":"provider:sim-tracking",
 "viewer_reason":"","display":{"width_m":0.60,"height_m":0.34,"pixel_width":3840,"pixel_height":2160},"note":"..."}
{"type":"sample","seq":12,"capture_ns":270407621817000,"tracking":true,"confidence":1,
 "display":{"left_eye":[x,y,z],"right_eye":[x,y,z]},
 "camera":{"key":"sim-tracking:webcam0","left_dir":[x,y,z],"left_distance_m":0.60,"right_dir":[x,y,z],"right_distance_m":0.60}}
{"type":"error","error":"no_provider","reason":"..."}
```

  `camera` is `null` on a sample without the camera-space block; `display` in `hello` is `null`
  when the display info could not be read.
- **Solve** input: an array of pairs, or `{"mode","eye","camera_key","screen","samples":[...]}`
  (flags override the file). A pair is `{"dot_m":[x,y], "sample": <a watch sample line>}` (the
  Dashboard records the line it captured on Space), or `"sample": <its camera object>`, or an
  already-chosen eye point `{"dot_m":[x,y], "dir":[x,y,z], "distance_m": d}`. `eye` picks the
  aligned point from the per-eye block (default `mid`, the closed-eye variant). Output (exit 0 ok,
  2 refused, 1 bad input):

```json
{"ok":true,"status":"ok","reason":"","mode":"b","eye":"mid","camera_key":"sim-tracking:webcam0","screen":"<key>",
 "pose":{"t_m":[0.035,0.183,0],"q":[0.0785,0,0,0.9969],"pitch_deg":9.0,"yaw_deg":0,"roll_deg":0,"focal_scale":1,
         "tz_observable":false},
 "setting":"0.035000,0.183000,0.000000,0.078459,0.000000,0.000000,0.996917",
 "rms_mm":0.0,"max_mm":0.0,"iris_scale":1.0,"params":3,"rank":3,"condition":115.4,"ill_conditioned":false,
 "iterations":7,"sample_count":10,"used_count":10,"distinct_dots":5,
 "samples":[{"index":0,"dot_m":[0,0],"residual_mm":0.0,"distance_m":0.534,"estimated_distance_m":0.534,"rejected":false}]}
```

  On refusal `ok` is false, `status` is `invalid_input` / `too_few_samples` / `degenerate` /
  `behind_camera`, `pose` is `null`, `samples` is empty and `reason` says why.
- **Apply** writes the reserved settings through the same store as `tracking set` (per user;
  `--machine` = HKLM on Windows). Slot: the one already holding this camera + screen, else the
  first free `n` in 0..15 (`--slot` overrides). `--depth-scale` / `--focal-scale` are written when
  given and cleared otherwise. `--provider` defaults to the row the loader ranks first. `--json`
  prints `{"ok","provider","slot","store","keys":{...}}`.

**Reserved setting keys** (per camera and per screen; `n` = 0..15):

| Key | Value |
|---|---|
| `CAMERA_POSE_<n>` | `"tx,ty,tz,qx,qy,qz,qw"`: metres, display space; camera → display |
| `CAMERA_POSE_<n>_CAMERA` | the camera key it applies to |
| `CAMERA_POSE_<n>_SCREEN` | the screen key it applies to |
| `CAMERA_POSE_<n>_DEPTH_SCALE` | optional: multiplies the provider's distance estimate (the solved iris scale) |
| `CAMERA_POSE_<n>_FOCAL_SCALE` | optional: the solved `k` (mode `c` with `--focal`) |

A provider applies the slot for the camera it reads and the screen it serves: display eye =
`T + R · (depth_scale · s̃ · normalize(u_x, u_y, k·u_z))`; with no slot it uses its own prior (a
top-centre bezel camera, `T_y = H/2 + 8 mm`, `R = I`). **This release:** the host does not pass a
provider the key of the screen it serves (per-screen routing is §10 item 1), so a provider takes
the first slot whose `_CAMERA` matches; `_SCREEN` is recorded for when routing lands, and
`calibrate --screen` is echoed, not routed.

**Test bed.** `sim_tracking` with `CAMERA=1` simulates a webcam (§8): it measures the scripted eyes
from the hidden `CAMERA_TRUE_POSE` and reports them through its calibrated `CAMERA_POSE_<n>` (else
the prior). `tests_tracking_calibrate_e2e` loads the real plug-in with a hidden pose
(35, 183, 0) mm + 9° pitch, places the eye midpoint on the normals of five dots at two distances,
solves, commits `CAMERA_POSE_0`, and checks that the reported eyes equal the scripted ones (about
105 mm off through the prior, under 0.2 mm after). `tracking_calibrate_cli_solve` and
`tracking_calibrate_cli_watch` run the two CLI verbs on the same setup (POSIX: manifest discovery).

## 7. Cameras: what a provider may open

- A provider on a **plain colour webcam** opens it itself, on demand (§3), subject to the OS camera
  permission and the runtime's tracker-camera denylist (ADR-043 Amendment 5: never a display's
  claimed tracker camera).
- A provider on **the display's shared stereo camera** is an ADR-043 camera consumer (follow-up,
  §10). Being a consumer does **not** keep the display's tracker running: the shared display stream
  carries frames **only while the display itself is tracking a viewer** (a 3D session on that
  screen). Such a provider therefore works only during 3D sessions on that screen — exactly where
  the display's own eyes win the viewer role anyway — unless the display vendor adds a camera-only
  wake (frames without a weaver). That makes the shared camera a hands source, not a viewer source:
  the viewer role is served from a plain webcam (on a display that does not track, there is no
  shared tracker stream to consume).

## 8. In-tree reference provider: `sim_tracking`

`src/xrt/drivers/sim_tracking/` → `DisplayXR-SimTracking.{dll,dylib,so}`, id `sim-tracking`,
ProbeOrder 200. Two sources, picked by `SOURCE`:

- `script` (default) — a scripted viewer with **no camera**, the hardware-free CI source; with
  `CAMERA=1` it also simulates a webcam for the §6 calibration (the test bed);
- `webcam` — the #1855 webcam eye tracker (moved here from `sim_display`): a plain colour webcam,
  a face estimator for the two pupils, a pinhole + IPD-prior depth model, one-euro smoothing and
  the MANAGED loss collapse. It pushes display-space eye samples **and** the camera-space block
  (`XRT_TRACKING_SAMPLE_CAMERA_VALID`: camera key — USB `vvvv:pppp`, else the OS device id —
  per-eye unit direction and distance from the optical centre, the raw unsmoothed measurement of
  the newest face frame), so it is the reference input for the §6 calibration. `describe()`
  reports `XRT_TRACKING_TRACKS_CAMERA_SPACE` and the `camera_key`. Like the test bed, it honours
  the §6 calibration: when a `CAMERA_POSE_<n>` slot names its camera key, every measurement is
  mapped to display space as `T + R · (depth_scale · s̃ · normalize(u_x, u_y, k·u_z))` before
  smoothing; with none it uses its own bezel-offset model (`WEBCAM_OFFSET_MM`, `WEBCAM_*`).

Opt-in: `probe()` declines unless `ENABLE` = 1. With `SOURCE=webcam` it also declines unless a
face estimator is built in **and** a camera matching `WEBCAM_DEVICE` is enumerated (it never
opens the camera to decide; the camera opens on `start()`, on the provider's own worker thread).
Settings (`displayxr-cli tracking options sim-tracking`):

| Key | Default | |
|---|---|---|
| `ENABLE` | 0 | opt-in |
| `SOURCE` | script | `script` / `webcam` |
| `SCRIPT` | sweep | `sweep` (head sweeps left-right) / `offset` (holds still) |
| `PERIOD_MS`, `AMPLITUDE_M` | 4000, 0.10 | sweep |
| `OFFSET_X_M` / `_Y_M` / `_Z_M` | 0 | from the nominal viewer (host geometry) |
| `IPD_MM` | 63 | |
| `LOSS_PERIOD_MS` | 0 | > 0: tracking off for half of every period (exercises the state event) |
| `RATE_HZ` | 60 | push rate (webcam: how often the tracker is polled; one push per analysed frame while tracking, every tick while lost so the collapse animates) |
| `CAMERA` | 0 | 1: simulate a webcam — camera-space samples + display output through `CAMERA_POSE_<n>` (§6) |
| `CAMERA_KEY` | `sim-tracking:webcam0` | the simulated camera's key |
| `CAMERA_TRUE_POSE` | the bezel prior | hidden ground truth `"tx,ty,tz,qx,qy,qz,qw"` the camera measures from |
| `CAMERA_DEPTH_BIAS` | 1 | reported distance = true × this (a simulated per-user iris error) |
| `WEBCAM_ESTIMATOR` | auto | `auto` (the built-in estimator) / `none` / the built-in name (`yunet-onnx`) |
| `WEBCAM_MODEL` | beside the plug-in | face model path (onnx builds) |
| `WEBCAM_DEVICE` | first camera | index (`0`, `1`, …) or a case-insensitive substring of the name |
| `WEBCAM_HFOV_DEG` | 70 | horizontal field of view (distance accuracy depends on it) |
| `WEBCAM_IPD_MM` | 63 | IPD prior for the monocular depth |
| `WEBCAM_OFFSET_MM` | 0,0,0 | uncalibrated camera position from the top-centre of the panel, `x,y,z` mm (a `CAMERA_POSE_<n>` for the camera's key replaces this model, §6) |
| `WEBCAM_MIRROR` | 0 | the source delivers a mirrored image |

**Which estimator is built in is a build-time choice.** The default build (and every release)
compiles the stub estimator and fetches nothing, so `SOURCE=webcam` declines in probe with one
WARN. `-DXRT_SIM_WEBCAM_ESTIMATOR=onnx` fetches ONNX Runtime + the YuNet face model (both pinned
by SHA256), loads them at run time from the plug-in's own directory (never linked) and copies
them next to both plug-ins. Details: `docs/vendors/sim_display/README.md` § *Webcam eye tracking*.

**One core, two hosts.** The camera worker, estimator and geometry
(`sim_tracking/sim_webcam_*`, `sim_face_*`) read their settings only through a small callback,
`sim_webcam_settings::get(key) -> string`. `sim_tracking` satisfies it from its host iface;
`sim_display` satisfies it from its settings shim for the **one-release deprecation alias**
`WEBCAM_TRACKING` (and the legacy `SIM_DISPLAY_WEBCAM_*` names): with it on, sim_display keeps
the old behaviour — it tracks in-process, OWN-like, advertising MANAGED, and so wins its own
screen over any provider (§4 rule 1) — and logs one WARN pointing to `sim-tracking SOURCE=webcam`.
Both hosts use the same `WEBCAM_*` key names.

**Camera claims (ADR-043 Amendment 5).** sim_display's `get_claimed_cameras` declares the webcam
whenever its alias is on, as before. On the provider path the camera is declared by the provider
itself (`describe().camera_key`); the trunk does not yet fold an active provider's camera into the
service's claim set (§10 item 10), so with only `SOURCE=webcam` on, the service's UVC SBS
source is not told the webcam is taken. Do not opt the same webcam into `stereo-cameras.json`.

**macOS camera permission.** macOS has no capture backend yet (`aux_os_uvc` builds its stubs
there), so `SOURCE=webcam` declines on macOS today. When an AVFoundation backend lands, access is
gated by the **hosting process's** `NSCameraUsageDescription` and TCC grant: in-process (the only
hosting this release) that is the app's own permission, exactly as for the sim_display alias; a
standalone provider process (§10 item 1) will need its own usage string and grant.

Dev trees stage it next to sim-input (`scripts/build_macos.sh`, `scripts/build_linux.sh`); the CI
end-to-end arm is `tests_oxr_view_space_viewer_tracking`.

## 9. Diagnostics

- `displayxr-cli tracking list [--json]` — rows, Enabled (+ source), provenance, PreferredPlugin,
  the row ranked first. `tracking enable|disable|use|prefer|reset|options|set|unset` mirror
  `input`.
- `displayxr-cli selftest --json` → `tracking_providers`: `registered`, `enabled`,
  `scan_declined`, `scan_failed`, `scan_untrusted`, `ranked` (registration ids), `active_id`,
  `viewer_source` (`dp` / `provider:<id>` / `nominal`), `viewer_reason`, `display_viewer_policy`,
  `composed_eye_tracking_modes`. Never a verdict: a provider in standby or absent is normal. The
  Windows N4 conformance step asserts selected / Enabled=0 falls back / Preferred wins / ABI
  fixture rejected / a tracking display wins.
- `displayxr-cli info --json` → the same `tracking_providers` object (the Dashboard's Components page reads it).
- `displayxr-cli status --json` → per screen `eye_tracking.viewer_source`, `viewer_reason`,
  `viewer_policy` (the display's policy for that screen) and `viewer_provider` (§4b) — from the service over DIAG when it is reachable (its live provider
  state), else headless (evaluated, nothing started).
- Logs: one WARN per process for the primary decision (`viewer tracking (ADR-054 N6): primary screen
  viewer source = ...`, plus "hosted in the service, shared by every IPC client" there), one WARN
  per screen whose answer changed at a topology publish (`screen 0x... viewer source = ...`, with
  the mapping offset for a non-primary screen), one when the provider starts on demand, and in the
  service one the first time an IPC client's views come from it.

## 10. Follow-ups

Done since the first release: **service hosting** (§4a) and **per-screen routing** through each
screen's own DP with the desktop-geometry mapping (§4b).

1. **Out-of-process hosting** (ADR-053 §5 hosting contract): the sink becomes the N8 seqlock pose
   page; the ABI does not change. It also ends the "in-process app beside a service opens the
   camera a second time" case (§4a).
2. **Provider as an ADR-043 camera consumer** (with the §7 constraint) — through a reserved host
   slot.
3. **`screen_hint` contract** for a provider that measures several screens natively (one camera per
   screen, or a calibrated multi-screen rig): today every sample is in the primary's display space
   and the trunk maps it (§4b); a provider-supplied hint would bypass the mapping for its screen.
4. **Camera-pose calibration** (§6): the solver, `tracking calibrate [solve|apply]`, `--watch
   --json`, the `sim_tracking` camera test bed and the Dashboard calibration window are **done**;
   open: the service-side watch (read the service's running provider rather than hosting a second one),
   `_SCREEN` matching once providers are passed their screen key (item 3), and replacing the
   coplanar desktop-layout assumption of §4b with a per-screen pose.
5. **Service live re-selection**: today a tracking policy change applies at the next service start
   (the scan is one-shot, the advertisement static per instance).
6. **Per-screen composed `xrEnumerateDisplaysDXR` caps** for a non-primary screen a provider serves
   (today only the session advertisement is composed; `XRT_SCREEN_FLAG_TRACKED` feeds workspace
   policy, so composing it is a separate decision).
7. **Workspace tiles on a secondary presenter** (`comp_d3d11_service_get_client_workspace_view_frame`)
   and the controller's FRAME_TICK viewer: still the presenter DP's eyes.
8. ~~`sim_tracking` webcam source~~ — **done**: `SOURCE=webcam` (§8), honouring `CAMERA_POSE_<n>`;
   sim_display's `WEBCAM_TRACKING` is a one-release alias over the same core, removed next release.
9. Installer rows: the Windows runtime installer ships `DisplayXR-SimTracking.dll` (allowlisted, N4b)
   but writes no `TrackingProviders` row yet; the Linux / macOS packages do not ship it yet.
   `versions.json` for the open provider. (The Dashboard card landed: Components → Tracking providers.)
10. **Provider camera claims**: fold the active provider's `describe().camera_key` into the
    service's ADR-043 Amendment 5 refusal set (today only the display plug-in's
    `get_claimed_cameras` feeds it).

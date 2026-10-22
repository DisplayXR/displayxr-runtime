# Viewer-Tracking Provider Discovery Contract

**Status:** implemented (ADR-054 §4.1, N6 Part 2, #2144) — in-process hosting, primary screen.
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

The viewer source is decided at system creation and is **static per instance**: a provider
enabled, disabled or preferred later takes effect at the next app launch (the same rule as
`dp set`). The chosen provider is started on the first viewer request and stopped at system
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

### Per screen: read the segment DP of THAT screen

The arbitration is a per-screen question and its inputs are per-screen: *whether the display on
this screen tracks* and *its viewer policy* must be read from **the display processor bound to that
screen** — the segment DP the runtime created for it (multi-screen M3/M6, `create_dp_*_for_screen`,
`get_display_info_for_monitor`, `get_viewer_policy(inst, <that screen's descriptor>)`) — never only
from the primary DP. Two different vendors' displays on one desk (one tracking, one not) must get
two different answers.

**This release** evaluates the **primary screen only** (the active plug-in's panel): the in-process
`xrLocateViews` path that a single-screen window takes. A locate split across screens
(`seg != NULL` in `oxr_session.c`) keeps each segment DP's own eyes, and the status snapshot reports
every non-primary screen as "dp" when it tracks, else "nominal" with the reason "viewer-tracking
providers serve the primary screen only (for now)". Per-screen routing through the segment DPs is
the first follow-up (§9).

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

The service process does not host providers in this release (§9), so it never composes either —
an IPC client is never told about tracking the service would not deliver.

## 6. Camera-space samples and camera-pose calibration (follow-up)

A camera-based provider MAY report, beside the display-space sample, what it measured in its
**camera** frame — `sample.camera` with `XRT_TRACKING_SAMPLE_CAMERA_VALID`:

| Field | |
|---|---|
| `camera_key` | Stable key of the camera (USB `"vvvv:pppp"`, OS device id, or a provider-chosen name) |
| `left_dir[3]`, `right_dir[3]` | Unit direction from the optical centre to each eye point (the landmarker's accurate part) |
| `left_distance_m`, `right_distance_m` | Estimated distance along it (iris-size depth, ±5–10 %) |

Same `capture_ns` as the display-space sample. Camera frame: origin at the optical centre, +X right
as the user sees it (un-mirrored), +Y up, +Z out of the lens toward the viewer. `describe()` sets
`XRT_TRACKING_TRACKS_CAMERA_SPACE` when it fills the block.

Nothing consumes it yet. It is the input of the **mirror-alignment camera-pose calibration**
(design: `displayxr-tracking-open` `docs/design.md` §10): `displayxr-cli tracking calibrate
--screen <key> --watch --json` streams these samples as NDJSON for the Dashboard's calibration
window, `displayxr-cli tracking calibrate solve` runs a runtime-side solver (camera pose `(R, T)` in
display space, per-sample residuals, iris scale), and the result is committed as provider
settings. Reserved setting keys, **per camera and per screen**: `CAMERA_POSE_<n>` =
`"tx,ty,tz,qx,qy,qz,qw"` (metres, display space), with `CAMERA_POSE_<n>_CAMERA` = the camera key
and `CAMERA_POSE_<n>_SCREEN` = the screen key it applies to. A provider applies the pose for the
camera it reads and the screen it serves; until one is set it uses its own prior (a top-centre
bezel camera). Any provider that reports camera-space samples gets calibration for free — the
method and the solver belong to the class, not to a provider.

## 7. Cameras: what a provider may open

- A provider on a **plain colour webcam** opens it itself, on demand (§3), subject to the OS camera
  permission and the runtime's tracker-camera denylist (ADR-043 Amendment 5: never a display's
  claimed tracker camera).
- A provider on **the display's shared stereo camera** is an ADR-043 camera consumer (follow-up,
  §9). Being a consumer does **not** keep the display's tracker running: the shared display stream
  carries frames **only while the display itself is tracking a viewer** (a 3D session on that
  screen). Such a provider therefore works only during 3D sessions on that screen — exactly where
  the display's own eyes win the viewer role anyway — unless the display vendor adds a camera-only
  wake (frames without a weaver). That makes the shared camera a hands source, not a viewer source:
  the viewer role is served from a plain webcam (on a display that does not track, there is no
  shared tracker stream to consume).

## 8. In-tree reference provider: `sim_tracking`

`src/xrt/drivers/sim_tracking/` → `DisplayXR-SimTracking.{dll,dylib,so}`, id `sim-tracking`,
ProbeOrder 200. A scripted viewer with **no camera**, the hardware-free CI source. Opt-in:
`probe()` declines unless `ENABLE` = 1. Settings (`displayxr-cli tracking options sim-tracking`):

| Key | Default | |
|---|---|---|
| `ENABLE` | 0 | opt-in |
| `SCRIPT` | sweep | `sweep` (head sweeps left-right) / `offset` (holds still) |
| `PERIOD_MS`, `AMPLITUDE_M` | 4000, 0.10 | sweep |
| `OFFSET_X_M` / `_Y_M` / `_Z_M` | 0 | from the nominal viewer (host geometry) |
| `IPD_MM` | 63 | |
| `LOSS_PERIOD_MS` | 0 | > 0: tracking off for half of every period (exercises the state event) |
| `RATE_HZ` | 60 | push rate |

The #1855 webcam tracker core still lives in `sim_display` (`SIM_DISPLAY_WEBCAM_TRACKING`,
unchanged; when on, sim_display tracks and so wins its own screen). Moving it into `sim_tracking`
as `SCRIPT=webcam` is a follow-up.

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
- `displayxr-cli status --json` → per screen `eye_tracking.viewer_source` + `viewer_reason`.
- Logs: one WARN per process for the decision (`viewer tracking (ADR-054 N6): primary screen viewer
  source = ...`) and one when the provider starts on demand.

## 10. Follow-ups

1. **Per-screen routing** through each screen's segment DP (§4), and a `screen_hint` contract for
   providers that serve more than one screen.
2. **Service hosting**: splice the provider into the service's view computation
   (`ipc_try_get_sr_view_poses`) and then compose there; until then the service never loads one.
3. **Out-of-process hosting** (ADR-053 §5 hosting contract): the sink becomes the N8 seqlock pose
   page; the ABI does not change.
4. **Provider as an ADR-043 camera consumer** (with the §7 constraint) — through a reserved host
   slot.
5. **Camera-pose calibration** (§6): `tracking calibrate --watch --json`, the solver, the Dashboard
   window.
6. **`sim_tracking` SCRIPT=webcam**: move the #1855 webcam core out of `sim_display`.
7. Dashboard tracking-provider card (the N3e shape), installer rows (the reference provider is not
   registered by the runtime installer yet), `versions.json` for the open provider.

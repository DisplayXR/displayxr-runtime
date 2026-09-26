# Stereo camera source — browser integration, vendor plug-in notes, phased plan

**Status:** design (2026-09-25); **R1 implemented** on `feat/stereo-camera-r1` (2026-09-26),
**R2 implemented** on `feat/stereo-camera-r2` (2026-09-26, stacked) — see §E; maintainer decisions
for R1 in §G. **L1** (Leia provider) is on `displayxr-leia-plugin` `feat/stereo-camera-l1`. Decision: [ADR-043](../adr/ADR-043-stereo-camera-source.md).
Extension: [`XR_DXR_stereo_camera`](../specs/extensions/XR_DXR_stereo_camera.md). Consumer
driving it: the web SDK's 3D call module (`@displayxr/inline3d/call`, RFC 0002 on
`displayxr-web` branch `feat/call-p1`, §4 *Capture*).

This page holds what the vendor-neutral spec deliberately does not: how the DisplayXR Browser
turns a runtime camera into a web camera, how the first vendor plug-in (Leia SR) sources frames,
the Android story, and the plan.

---

## A. The page-facing contract (goal)

A page uses **standard web APIs only, plus one capability hint**:

```js
const devices = await navigator.mediaDevices.enumerateDevices();
// the runtime camera appears as an ordinary videoinput: label "3D Camera (DisplayXR)"
const stream = await navigator.mediaDevices.getUserMedia({ video: { deviceId: id } });
const track = stream.getVideoTracks()[0];
track.getSettings();
// { width: 1280, height: 480, frameRate: 30, facingMode: 'user', ...,
//   displayxrStereo: { layout: 'side-by-side', rectified: true, baselineMm: 50, horizontalFovDeg: 68 } }
```

- **The hint is one non-standard dictionary member, `displayxrStereo`, on `MediaTrackSettings`**
  (mirrored in `getCapabilities()`), present only on DisplayXR stereo tracks. Absent ⇒ treat the
  track as whatever the page already assumes. Values are **coarsened** — integer mm, integer
  degrees — because exact calibration is a device fingerprint (spec §7.5); a page that needs
  `f_px` derives it from `width/2` and `horizontalFovDeg`.
- **No selection API.** The call module's existing `camera: 'auto'` rule (aspect > 2.5 ⇒ stereo
  SBS) already picks a 1280×480 track. With the hint it can stop guessing: prefer a device whose
  track reports `displayxrStereo`, and fill its `hello` (`rectified`, `baselineMm`, `hfovDeg`)
  from the hint instead of assuming `rectified: false`. That is the entire SDK change.
- **The frame is never mirrored** and is left-eye-left. Self-view mirroring (mirror each half AND
  swap halves) stays the page's, as RFC 0002 §3 already says.
- **Permission, indicator, device picker are Chromium's own**, unchanged: the runtime camera is a
  camera like any other to the permission model.
- Other browsers (stock Chrome/Edge) never see this camera; the SDK treats such senders as mono
  (lifted on the receiver). A system-wide virtual webcam is out of scope (ADR-043 alternative B).

## B. DisplayXR Browser — Windows

### B.1 Where it plugs in

Chromium enumerates and opens cameras in its **video capture service**
(`media/capture/video/…`, `VideoCaptureDeviceFactory` → `VideoCaptureDevice`). The integration
is a **second device factory composed in front of the platform one**:

| Piece | Where | Notes |
|---|---|---|
| `DisplayXRStereoCameraDeviceFactory` | new files, `components/displayxr/capture/` | `GetDevicesInfo()` = `xrEnumerateStereoCamerasDXR`; one descriptor per camera: display name `"3D Camera (DisplayXR)"` (plus the plug-in's `displayName` when more than one), `device_id = "dxr-stereo:" + persistentId`, `facing = USER`, one format `2·eyeW × eyeH @ maxFrameRate, NV12` |
| Composition hook | one small hunk in `media/capture/video/create_video_capture_device_factory.cc` | wraps the platform factory; the only upstream-file edit (goes into `docs/integration-points.md`) |
| **Hide the busy duplicate** | same wrapper | drops the platform (MediaFoundation) descriptor whose symbolic link matches a camera's `platformDeviceHint` — otherwise the page also sees the physical device that fails with "Device in use" |
| `DisplayXRStereoCameraDevice : VideoCaptureDevice` | `components/displayxr/capture/` | `AllocateAndStart` → create + start a RECTIFIED (else RAW) NV12 shared-memory stream; a capture thread waits on the stream's wake handle, acquires, and calls `Client::OnIncomingCapturedData` with the pinned slot (a copy into Chromium's pool happens there), `reference_time`/`timestamp` from `captureTime` (QPC domain = `base::TimeTicks` on Windows). `StopAndDeAllocate` → stop + destroy. SUSPENDED ⇒ stop delivering (the track stays live and shows its last frame, like a paused webcam); INSTANCE_LOST ⇒ `Client::OnError` |
| Hint | Blink `MediaTrackSettings` / `MediaTrackCapabilities` IDL + the `VideoCaptureFormat`/device-info plumbing that already carries per-device metadata to the renderer | the one Blink-visible change; lives beside the existing inline-3D Blink surface |

A greyscale source is delivered as NV12 with neutral chroma — every downstream stage (WebRTC
encoders, `<video>`, `VideoFrame`) takes NV12 natively, and a grey image costs the chroma planes
nothing to compress.

### B.2 Runtime connection and process

The capture device needs an OpenXR instance with `XR_DXR_stereo_camera`, **no session**. Which
process opens it depends on where the pinned Chromium runs its video capture service on Windows
(**verify at the pin**; it has moved between the browser process and an unsandboxed utility
process across milestones):

- **Medium-IL process** (browser process, or an unsandboxed utility) → it opens its own runtime
  connection, exactly like the browser process's existing snap/query instance.
- **Sandboxed process** → the browser process brokers a connection the same way it already does
  for the GPU process (browser#103: `xrWeaveExportIpcConnectionDXR` + `instance_declare_peer`,
  opener IL ≥ target IL). The shared-memory transport is chosen precisely so a Low-IL consumer can
  map it (the service labels the section for the declared peer).

### B.3 Consent and privacy

- **Per origin:** Chromium's normal camera permission prompt, site settings, tab/omnibox in-use
  indicator. Nothing new.
- **Per executable (runtime):** the browser installer registers `chrome.exe` (path + Authenticode
  signer) as a **consent-delegating** client of the runtime (spec §7.1), so the runtime does not
  prompt a second time per page. An unsigned/dev build is not delegating: the runtime prompts once
  for it from the tray.
- **OS camera privacy:** if Windows camera access is off globally or for desktop apps, the runtime
  refuses (`PERMISSION_INSUFFICIENT`) and the device reports an error — the same outcome a normal
  webcam would give, so users are not surprised by a camera that ignores the OS switch.
- **Runtime indicator** (tray badge naming `chrome.exe`) in addition to Chromium's; runtime kill
  switch hides the device entirely.
- **Fingerprinting:** `device_id` is Chromium's per-origin hash of `dxr-stereo:<persistentId>`
  (itself a keyed hash per executable), the hint is coarsened, and raw calibration is never exposed
  to pages.

## C. Android

### C.0 Field facts (NP02J / K68 tablet, measured 2026-09-26)

**Camera topology.** Camera2 reports six ids on the tablet: 0 and 2 are back lenses, 1 and 3 front
lenses, and **4 = back and 5 = front are LOGICAL multi-cameras — the stereo pairs**. The logical
ids are hidden from `getCameraIdList()` and are opened by hard-coded id. Physical lens per eye:
front L = 3, R = 1; back L = 2, R = 0.

**The browser cannot reach them** (DisplayXR/displayxr-browser-pvt#175): `enumerateDevices()`
lists no video inputs, `getUserMedia` only opens the front single camera, and
`facingMode: 'environment'` fails with `NotFoundError`.

**How the vendor's own app gets stereo — and what CNSDK is, and is not.** The vendor app uses
`LeiaCameraBuilder(ctx).setStereoMode(true).setFront(b).setConvergenceMode(AUTO)
.setPreviewSurfaceView(surfaceTexture)` and receives a ready SBS image (2560×720) in a
`SurfaceTexture`. **`LeiaCameraBuilder` is not part of CNSDK:** it is a separate, private camera
wrapper over plain Camera2. It opens the one logical camera (5 front / 4 back) with an output per
physical lens, and its own shader does the SBS pack and the rectification. **CNSDK supplies only
the calibration** (per-camera rotations Rx/Ry/Rz from its display-config service, config v4.0;
the `/sdcard/rect.js` file the vendor app also reads is absent on the unit) **and in-app face
tracking.**

**What the pair delivers.** Two separate 1280×720 NV12/YUV streams, one per lens, at exactly
30.00 fps, hardware-synchronised (identical timestamps per pair), about 97–100 ms from exposure to
the app. The frames arrive **raw**: not side-by-side, not rectified, not mirrored. For viewer
left/right the front eyes need mirroring. The residual vertical misalignment is about −2 px
(median), and the roll about 0.09°.

**Exclusivity — the constraint that decides the design.** Opening the front pair (camera 5)
**evicts the head-tracking service's camera** (it held camera 3 at 10 fps). The tracker then stays
wedged ("Camera has been invalidated") and never reacquires it. The rear pair does not conflict.
The vendor's answer is **in-app tracking**: the capturing process feeds a third stream (640×480,
left lens) to CNSDK in-app mode. CNSDK's frame hand-off runs face detection **on the caller's
thread** (~119 ms). Called on the camera thread, that dragged the pair to 24 fps, so it must run
on a worker behind a latest-frame slot.

**Windows, same week:** on two SR machines the eye tracker holds the stereo camera exclusively
(`getUserMedia` → `NotReadableError`). The design (plug-in source, service owner) follows from
that on both platforms.

### C.1 Runtime + plug-in — design consequence

On Android the provider lives **inside the runtime APK** (the vendor plug-in ships there, ADR-038).
It is not Chromium, and it is not a separate app. Because the front pair and the tracker cannot
coexist as two Camera2 clients:

- **The runtime service is the ONE owner of the front pair, and it runs tracking in-process from
  that same capture.** Three outputs of the logical camera: the two 1280×720 NV12 eye streams, and a
  640×480 left-lens stream that goes to CNSDK in-app tracking on a **worker thread behind a
  latest-frame slot**, never on the camera thread. The runtime's eye tracking on the tablet then
  comes from this capture, and camera streams to clients are passengers on it (the Windows model,
  reversed: there the tracker owns the device and the plug-in reads its frames).
- **Nothing else may open the pair behind the tracker's back**: not a page, not another plug-in, not
  an app through Camera2. Doing so evicts the capture that tracking depends on, and the display
  loses viewer tracking for the whole call.
- **Transport:** AHardwareBuffer per eye (NV12). A 2×1280×720 pair at 30 Hz is ≈ 83 MB/s through a
  CPU ring, so this is where the optional `AHARDWAREBUFFER` transport (spec §5.3) becomes the
  default. Rectification runs in the runtime (R2's `u_stereo_rectify`, fed with the CNSDK
  rotations) or in the consumer from the calibration.
- **Contract gaps this exposes (A1):** the R1 plug-in frame is ONE side-by-side image in CPU
  memory. The tablet produces two per-lens hardware buffers that need mirroring. A1 either packs
  them in the plug-in (a GPU blit into one SBS AHB, un-mirroring on the way), or appends a
  per-eye layout to `xrt_plugin_stereo_camera_frame` (ADR-020 append-only: a layout field + per-eye
  AHB handles). The second is preferred. The calibration slot stays as is: CNSDK's per-camera
  rotations become `R`, and the baseline comes from the display config.
- Consent: CAMERA permission of the peer uid + the runtime app's consent, and
  `android_package_is_visible` for the foreground rule (spec §7), are the platform-neutral code R3
  adds.

### C.2 Browser

The browser's Android arm is in the patch series (the runtime fd is brokered by the
browser-process Java via `DXR_IPC_FD`, patches 0083+). The camera lands on top of it the same way
as Windows: a DisplayXR device factory composed in front of `VideoCaptureDeviceFactoryAndroid`, fed
over the existing runtime connection (AHB transport). Chromium's own Android camera permission
request covers the uid check the runtime makes. It fixes browser-pvt#175 for the stereo pair
without the browser ever opening camera 5.

## D. Leia SR plug-in (Windows) — sourcing notes

These live here, not in the spec, because they are one vendor's implementation of the slots.
The implementation belongs in `displayxr-leia-plugin` (`src/drv_leia/`), per ADR-019.

| Topic | Finding | Plug-in design |
|---|---|---|
| Channel | The SR SDK's `sr-shared-memory-utilities` exposes `getSREyetrackerRawCamera{Event,Mutex,Memory}()` (named `Global\SREyetrackerRawCamera*` objects, ACL: authenticated users, default Medium label — unreachable from a Low-IL sandbox, fine for the service). The eye tracker's MediaFoundation camera thread writes the newest MJPEG under the mutex, then signals | Read under the mutex; copy out; decode outside the lock |
| Frame layout | A reader-defined header `{u64 rawImageFormat (0 = SBS MJPEG), u64 compressedSize, u64 height, u64 width}` followed by one JPEG of the full SBS frame. No sequence number, no timestamp, no SDK header for the struct | Validate every field (size bound, format 0, JPEG header dims = header dims); new-frame detection by (size, hash of head/tail bytes); stamp **arrival** time; `time_is_exposure = false` |
| Wake-up | The event is **auto-reset** (`CreateEvent(…, FALSE, FALSE, …)`), one per box. Two waiting readers steal each other's frames — the vendor's own apps included | **Do not wait on it.** Poll at ~2× the source rate (≈ 60 Hz); the service provides per-stream wake-ups downstream |
| Liveness | Frames flow only while the tracker runs, i.e. once some weave context brought it up | `stereo_camera_open` holds an SR context with tracking started (keep-alive) for as long as a camera is open; report WAITING → AVAILABLE on first frame, SUSPENDED when frames stop > 1 s |
| Content | 1280×480 @ 30, greyscale | Decode straight to GRAY8 (libjpeg-turbo gray colourspace or WIC) — half the work of RGB; set `MONOCHROME` |
| Calibration | Per device in `%ProgramData%\Simulated Reality\Devices\<serial>\{intrinsics.yml, extrinsics.yml}` (OpenCV FileStorage: `M1 D1 M2 D2`, `R T`; norm(T) in mm). The SDK's own resolver keys `Devices/<serial>`; the v2 C API exposes `srLensGetSerialNumber` | Resolve the **active** device's serial from the SDK session the plug-in already holds, open that folder only, parse the small YAML subset itself (no OpenCV dependency). Missing/unreadable ⇒ no `CALIBRATED` (RAW only) and a WARN naming the serial — never a fallback to another folder |
| Duplicate device | The same camera is a MediaFoundation device that `getUserMedia` sees as "in use" | Fill `platform_device_hint` with its MF symbolic link if the SDK/config reveals it (open question) |
| Exceptions | SR SDK throws as routine control flow | try/catch around every SDK call, as everywhere in the plug-in |

## E. Phased plan

| Phase | Repo | Scope | Exit criterion |
|---|---|---|---|
| **R0** | runtime | This design: ADR-043, spec, this page | Maintainer sign-off on the open questions below |
| **R1** ✅ | runtime | **Implemented 2026-09-26 (see "R1 as built" below).** Header `XR_DXR_stereo_camera.h` + `index.json` note (together — the catalog lint requires both); `xrt_plugin_iface` slots + `XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA` (appended after `create_dp_d3d11_lift`, so **lands after ADR-042's PR**); platform-neutral camera manager (thread, refcount + linger, 3-slot pinned ring, per-stream wake handles, decimation, NV12/BGRA conversion); IPC + OpenXR entry points; sim_display fake; `displayxr-cli camera list/calib/probe`; selftest check | `camera probe` on the sim fake writes frames on Windows + Linux CI; ring/pinning unit tests (mirror `tests_lift_mailbox`) |
| **R2** ✅ | runtime | **Implemented 2026-09-26 (see "R2 as built" below).** `u_stereo_rectify` (Bouguet, valid-region crop, LUT remap) with golden tests against OpenCV-generated fixtures; RECTIFIED output | sim fake: row error < 0.5 px, disparity = f·B/Z within 0.5 px — **measured 0.061 px worst block, 0.063 px worst disparity error** |
| **R3** | runtime | Privacy: OS consent check, consent store + tray prompt, delegating-client registration, visibility/lock suspension, indicator, kill switches | Manual matrix: allow/deny/revoke, OS switch off, lock screen, background app — each blocks frames |
| **L1** | leia-plugin (Windows) | Slots over the SR raw-camera channel per §D; calibration by active serial; keep-alive; repin runtime (feature-macro repin, `downstream-pins.json` `features` track) | On a panel box, incl. the multi-folder box: `camera probe --rectified` rows aligned; the vendor call app running at the same time still gets every frame; tracking/weave unaffected (frame-time + tracking-state logs) |
| **B1** | browser (Windows) | §B device factory + duplicate hiding + hint + delegating registration in the installer; web SDK: prefer the hint in `camera:'auto'`, fill `hello` from it | RFC 0002 P1 call between two panel laptops sends rectified SBS from the tracker camera **while both are tracking** |
| **A1** | leia-plugin (Android) + runtime | §C.1: the runtime owns the front pair and runs CNSDK in-app tracking from the same capture (worker thread, latest-frame slot); per-eye AHB transport + per-eye frame layout; Android consent + visibility | `camera probe` on the tablet via the runtime app **while the weave tracks from the same capture**; pair at 30.00 fps (not 24) |
| **B2** | browser (Android) | §C.2 | Tablet ↔ laptop 3D call, both directions stereo |

R1–R3 need no hardware; L1 and B1 can proceed in parallel once R1's header exists (B1 develops
against the sim fake).

### R1 as built

- **Header + catalog:** `openxr/XR_DXR_stereo_camera.h`, type values `1004999290–300` (the registry
  holds a reserved row for lift's `270–289`); `index.json` note in the `capture` group.
- **Plug-in slots:** six `stereo_camera_*` slots + `XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA`, after a
  one-pointer placeholder for lift's `create_dp_d3d11_lift`. **Merge order: after the lift PR**
  (`feat/lift-ext`); the rebase replaces the placeholder with lift's field in place. The offsets are
  identical either way, and `tests_stereo_camera` asserts the adjacency.
- **Service:** `ipc/server/ipc_server_stereo_camera.c`. It is platform-neutral (Windows D3D11
  service, macOS/Linux service, Android service). One thread per open camera, open on the first
  start, 2 s linger, per-stream 3-slot rings in shared memory, conversion + decimation, and a
  per-stream wake (Windows: an auto-reset event, handed out SYNCHRONIZE-only; POSIX: a pipe).
  Windows hands the section out read-only (`FILE_MAP_READ` duplicate).
- **IPC:** 12 `stereo_camera_*` calls in `proto.json`. Streams are owned by the connection.
- **OpenXR:** nine entry points. An in-process instance enumerates zero.
- **sim_display fake:** `SIM_DISPLAY_FAKE_STEREO_CAMERA=1` (spec §10).
- **CLI:** `displayxr-cli camera list | calib | probe`; `selftest` gains `stereo_camera_caps`.
- **Privacy hooks (deny by default):** `DXR_STEREO_CAMERA_DEV_ALLOW=1` in the service environment
  is the only way to start a stream or read calibration in R1. `DXR_STEREO_CAMERA=0` is the kill
  switch. RAW is refused to `PRESENT_OWNER` (the browser).
- **Deferred beyond R1:** state-change events (structs defined, not delivered — they need a
  server→client instance-event channel), the GPU transports (designed in the manager's file
  comment: D3D11 texture ring + fence as the `XR_DXR_weave` output export; AHB / dma-buf ring +
  sync_file as the #1699 weave path), a read-only POSIX section, and a distorted/misaligned fake
  (all with R2/R3/A1).

### R2 as built

- **`auxiliary/util/u_stereo_rectify.{h,c}`**, no OpenCV. Three layers so a GPU path replaces only
  the last: geometry (`u_stereo_rectify_compute`: OpenCV's `stereoRectify(CALIB_ZERO_DISPARITY,
  alpha = 0)` in C), float maps (`u_stereo_rectify_build_map`, what a GPU backend uploads as an
  RG32F texture), and the CPU backend (a fixed-point bilinear LUT over the whole SBS image; NV12 =
  full-res Y LUT + half-res UV LUT). RADTAN5 / RADTAN8 / KB4. Calibration at a different size than
  the frames is rescaled. The alpha = 0 crop is verified on every border pixel of the real maps.
- **Service:** a per-camera `struct scam_rectifier` built at manager create from the plug-in's
  RAW calibration (WARN line with f, principal point, baseline, zoom, build time). It runs once
  per source frame on the camera thread with the lock released, only while a started stream wants
  RECTIFIED; RAW streams read the original. `get_calibration(RECTIFIED)` returns the same
  geometry (one pinhole for both eyes, pure +x baseline); `horizontalFovDeg` is the rectified one.
  The browser is refused (`INPUT_UNSUPPORTED`) a camera the rectifier rejected rather than being
  given the RAW-flagged fallback. INFO stats add `rectify ms/frame`.
- **Fake:** `SIM_DISPLAY_FAKE_STEREO_CAMERA_DISTORT=1` — the raw distorted, vertically misaligned,
  rolled pair with exact ground truth (spec §10).
- **Tests** (`tests_stereo_rectify`, fixtures in `tests/fixtures/` from
  `gen_stereo_rectify_fixtures.py`, OpenCV 4.10, offline): R1/R2 to 1e-12, principal point to the
  converged reference within 1e-4 px (and OpenCV's own P1 within 0.75 px — OpenCV stops
  `undistortPoints` at 5 iterations), maps to 3e-5 px, no black pixel, and end to end on the fake:
  raw |Δy| median 2.9 px → rectified worst block 0.061 px; disparity 11.33 / 37.77 px ground truth
  vs 11.31 / 37.79 measured (worst block 0.063 px off).
- **CLI:** `camera calib --rectified` prints P1/P2 and `Z = f·B/d`; every probe reports row
  alignment and the dominant disparities as depths; `probe --rectified` exits 5 unless rows align.
  Against the fake: |Δy| median 0.016 px, depths read back 2.004 m and 0.599 m.
- **Perf:** 0.94 ms GRAY8 / 1.36 ms NV12 / 2.25 ms BGRA8 per 1280×480 frame, M1 Pro, -O2, one
  thread (3 ms GRAY8 in a Debug service). At 30 Hz that is ~3 % of one core for the Leia GRAY8
  source; **budget: ≤ 2 ms per frame on the camera thread**, above which the rows split across
  threads (`u_stereo_rectify_lut_apply_rows`) or the GPU backend takes over. Setup (geometry + both
  LUTs) 9–18 ms, once per calibration; LUT memory 6 bytes per output sample (3.7 MB + 0.9 MB).
- **Not in R2:** the GPU backend (seam in place), per-frame recalibration (`calibrationGeneration`
  stays 0), and a rectifier for a camera whose calibration changes at runtime.

## F. Risks

- **Undocumented vendor channel.** The raw-camera shared memory is a reader-defined contract with
  no version, sequence, or timestamp; a tracker update can change it silently. Mitigated by strict
  validation + RAW/SUSPENDED degradation, but it should become a documented SDK surface.
- **Privacy exposure.** Frames from a camera the user thinks of as "the eye tracker" reach web
  pages. The consent/indicator work (R3) is not optional polish; it gates B1 shipping.
- **Tracker side effects.** Holding tracking alive for a call changes power/thermal behaviour and
  keeps the lens in its tracked state; the tracker may lower its rate or exposure for its own needs
  mid-call (the source reports it; consumers must cope).
- **Quality ceiling.** 640×480 greyscale per eye is a real 3D pair but not an HD call image; the
  product may still prefer the HD mono webcam + receiver-side lift on some machines. The call
  module should let the user choose.
- **Chromium churn.** `media/capture` is a moderately active area; the composition hook is one
  hunk, and the device + factory are new files, to keep rebase conflicts small.
- **Latency/sync.** Arrival timestamps hide tracker-internal latency; A/V sync relies on WebRTC's
  own jitter handling. Fine for calls, not for measurement.
- **ABI merge order.** The iface slots append after ADR-042's `create_dp_d3d11_lift`. R1 holds
  lift's offset with a same-size placeholder, so an out-of-order merge still yields the right
  offsets, but the intended order is lift first and the camera PR rebased onto it.
- **Android: the pair and the tracker cannot share Camera2.** Opening the front logical camera
  evicts the vendor tracker, which then never reacquires (§C.0). The mitigation is architectural:
  the runtime owns both, and tracking runs in-process from the same capture (§C.1). It adds
  CNSDK's in-app face detection (~119 ms per hand-off) to the runtime's CPU budget on a worker.
  **Vendor asks (CNSDK / head-tracking service):** (1) the head-tracking service must reacquire its
  camera when another client releases it; today it stays wedged ("Camera has been invalidated")
  until reboot. (2) Can the tracker run from the pair's own frames (the 1280×720 eye streams, or a
  downscaled copy), so that no third stream is needed? (3) Expose the pair's calibration through a
  documented CNSDK call, rather than the display-config service plus an absent `/sdcard/rect.js`.

## G. Open questions & maintainer decisions

**Decided for R1 (maintainer defaults, 2026-09-26):**

- **Q1 → stereo-only for now.** Names and struct shapes stay open for other camera kinds
  (`XrStereoCameraPropertiesDXR::viewCount`, always 2 in v1, and next chains).
- **Q2 → plug-in iface slots** behind `XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA` (ADR-020 append-only).
  A separate camera-provider plug-in type waits for a camera vendor that is not a display vendor.
- **Q3 → service clients only.** In-process instances enumerate zero; there is no side IPC
  connection.
- **Q9 → raw frames are not exposed to pages in R1.** Pages get RECTIFIED, or a RAW frame that
  says so (`frame.output`) until R2's rectifier lands. The service refuses a RAW stream to
  `PRESENT_OWNER` clients (the browser).
- **Q6 is answered by the field facts (§C.0):** CNSDK has no frames to share. The pair is plain
  Camera2 and the vendor's camera wrapper is a separate library, so on Android the runtime owns
  the pair and runs tracking from it (§C.1). The remaining CNSDK asks are in §F.
- Q4, Q5, Q7 and Q8 remain open (consent UX ownership, SR SDK asks, browser topology, hint shape).

**Original questions:**

1. **Name and scope.** `XR_DXR_stereo_camera` (stereo only, SBS output) — or a broader
   `XR_DXR_camera_source` that also carries mono tracker cameras? The design is stereo-only; a
   view-count field could be added before the header freezes.
2. **Plug-in iface vs a camera-provider plug-in type (ADR-034 style).** Proposed: iface slots now,
   provider type only when a non-display camera vendor appears. Agree?
3. **In-process apps.** Proposed: service-only (in-process enumerates zero). Should an in-process
   runtime instead open a side IPC connection just for the camera?
4. **Consent UX ownership.** Is a runtime tray prompt + consent store acceptable product-wise, or
   should delegating-client registration (browser only) be the sole v1 path and native apps wait?
5. **Vendor asks (SR SDK):** a versioned raw-camera header with sequence number + exposure
   timestamp; a broadcast/manual-reset or per-reader wake; an API for the active device's camera
   calibration (instead of reading ProgramData files); confirmation that the calibration folder is
   named by the same serial `srLensGetSerialNumber` returns; the MF symbolic link of the tracker
   camera (for duplicate hiding); and whether tracking can be held alive without a weaver.
6. **Vendor asks (CNSDK):** can the head-tracking service share the front pair's frames (and
   calibration) with a trusted client? Without it Android ships the documented difference (§C.1).
7. **Browser process topology** at the current pin: where does the video capture service run on
   Windows, and is it sandboxed (decides own-connection vs brokered, §B.2)?
8. **Hint name/shape.** `MediaTrackSettings.displayxrStereo` as proposed, or a vendor-prefixed
   standard-track proposal later? Should it be exposed in `getCapabilities()` pre-permission (no —
   proposed post-permission only)?
9. **Default output.** RECTIFIED whenever calibrated (proposed), with RAW available only to native
   clients — or expose RAW to pages too (fingerprinting cost)?

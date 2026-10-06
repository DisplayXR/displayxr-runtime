# Stereo camera on Android tablets — design proposal

**Status:** proposal (2026-10-06), design only — nothing here is built. Decision record:
[ADR-043](../adr/ADR-043-stereo-camera-source.md). Extension:
[`XR_DXR_stereo_camera`](../specs/extensions/XR_DXR_stereo_camera.md). Parent plan:
[stereo-camera-source.md](stereo-camera-source.md) §C (Android) and its phases A1/B2, which this
page replaces with a gated plan. Browser issue: DisplayXR/displayxr-browser-pvt#175 (the Android
browser's `enumerateDevices()` lists no cameras).

**Goal.** The DisplayXR Browser on an Android 3D tablet offers the tablet's stereo camera pair to
web pages as an ordinary `getUserMedia` video device that delivers a rectified side-by-side
frame with the `displayxrStereo` hint, so the web SDK's call module sends a real stereo pair, as
it already does on Windows. The display must keep tracking the viewer throughout the call.

**How to read the citations.** Runtime code paths marked **r3:** are on
`origin/feat/stereo-camera-r3` at `b567f3203` (PR #1819, stacked R1→R2→R3, not yet on `main`).
Unmarked runtime paths are on `main`. **browser:** = `displayxr-browser-pvt` (`origin/main` unless
**B1:** = `origin/feat/stereo-camera-b1`). **plug-in:** = `displayxr-leia-plugin` `origin/main`.
**web:** = `displayxr-web` `origin/main`. *Measured* = verified on a tablet by an earlier
hands-on session (2026-09-26, recorded in [stereo-camera-source.md](stereo-camera-source.md)
§C.0 on r3); *inferred* = reasoning, not checked; *vendor-unconfirmed* = an assumption about
the vendor SDK or tracking service that nothing in code or docs settles.

---

## 0. Recommendation in one screen

1. **Ship the rear pair first.** The rear stereo pair does not conflict with head tracking
   (measured). It exercises the whole Android pipeline (plug-in Camera2 source → service
   rectifier → ring → browser device → page) without touching the hard problem.
2. **Front pair, near term: the runtime satellite process that weaves for the browser owns the
   pair and drives the vendor's in-app face tracking from the same capture** (option **R**,
   §1.2). This is the only option that keeps the browser's 3D tracked during a call without a
   new vendor-service API. Its costs: other apps' head tracking pauses while the pair is open,
   in-process 3D apps cannot use the camera, and it needs three vendor confirmations (in-app
   tracking fed with external frames licenses in a service process; the tracking service
   reacquires its camera when we release the pair; the in-app pose matches the service pose).
3. **Front pair, end state: the vendor tracking service owns the pair and shares frames**
   (option **V**). It is the only option consistent with
   [OEM requirement R7](../specs/vendor/oem-android-platform-requirements.md#r7--camera-arbitration-through-the-tracking-service)
   ("the tracking camera is opened by the tracking service and only the tracking service") and
   with multi-app tracking. Request it from the vendor now; when it lands, the plug-in becomes a
   passenger exactly as on Windows and option R is retired. The OpenXR surface does not change.
4. **Browser:** a DisplayXR `VideoCaptureDeviceFactory` composed in front of the Android platform
   factory, with its own `CAMERA_CONSUMER` connection from the process that runs video capture.
   CPU NV12 first, AHardwareBuffer later. This is independent of #175, but #175 still needs fixing
   for ordinary cameras.
5. **Privacy:** on Android, identity comes from the **package and signing certificate** of the
   peer uid. The runtime's executable-path identity does not work there (§4.1). The OS while-in-use
   camera rule enforces the foreground rule for us, provided the owner process does **not** run a
   camera-type foreground service. The runtime still owns the "who receives frames" indicator and
   the kill switch.

---

## 1. Ownership: who opens the Camera2 pair

### 1.1 What constrains the choice

| # | Fact | Source |
|---|---|---|
| F1 | The pair is a Camera2 **logical** camera: front = id 5, back = id 4, hidden from `getCameraIdList()` and opened by hard-coded id. Physical lenses: front L = 3, R = 1; back L = 2, R = 0. Each delivers 1280×720 NV12 at 30.00 fps, the two eyes hardware-synchronised (identical timestamps), ~97–100 ms exposure-to-app, raw (not packed, not rectified) | Measured, NP02J |
| F2 | Opening the **front** pair evicts the vendor head-tracking service's camera (it held physical id 3, the front-left lens, at 10 fps). The service then stays wedged ("Camera has been invalidated") and does not reacquire. Restarting the browser, which re-requests tracking, brought tracking back | Measured, NP02J |
| F3 | The rear pair does not conflict with tracking | Measured, NP02J |
| F4 | The vendor's own stereo app reaches the pair through a private camera wrapper over plain Camera2, not through the vendor SDK. The vendor SDK supplies calibration and face tracking, not frames | Measured |
| F5 | Feeding a 640×480 left-lens stream from the same logical camera into the vendor SDK's in-app face tracking worked in a standalone probe app. The hand-off runs face detection **on the caller's thread** (~119 ms); on the camera thread it dragged the pair to 24 fps, so it must run on a worker behind a latest-frame slot | Measured (probe app). The vendor SDK header says the external-frame sink works only with the in-app tracking runtime, and that tracking must be re-enabled for a newly attached sink to apply (*inferred from the header text; not run in a service process*) |
| F6 | The plug-in uses the **in-service** tracking runtime today, deliberately: in-app tracking "failed to license on the Lume Pad ('Invalid Device')" | plug-in: `src/drv_leia_android/leia_cnsdk.cpp:1097-1102` |
| F7 | The plug-in contains no Android camera code | plug-in: no `ACamera`/`AImageReader`/`Camera2` hit on `origin/main` |
| F8 | The Android browser reaches the runtime **out of process**. The browser process's Java calls the runtime's `Client.blockingConnect` and passes the socket fd down. The GPU process adopts it via `DXR_IPC_FD` and opens a `PRESENT_OWNER` session (`XR_DXR_weave`) on that connection | browser: `patches/0084-…runtime-fd-co.patch` (`DisplayXrRuntimeConnector.java`), `patches/0085-…GPU-pro.patch` (`gpu_main.cc` `setenv("DXR_IPC_FD")`), `patches/0088-…present-owner-weave-session…patch`; runtime: [android-ipc-fd-adoption.md](../specs/runtime/android-ipc-fd-adoption.md) |
| F9 | That connection lands in a **satellite** compositor process (`:dxr0..3`), one per client package. A second connection from the same package is routed to the **same** satellite ("reuse") | `src/xrt/ipc/android/src/main/java/org/freedesktop/monado/ipc/SlotBroker.kt:54-67`; manifest `src/xrt/ipc/android/src/main/AndroidManifest.xml:55-78` |
| F10 | The satellite is bound with `BIND_IMPORTANT \| BIND_ABOVE_CLIENT \| BIND_INCLUDE_CAPABILITIES`, so it inherits the binding app's foreground standing, including while-in-use capabilities (*Android semantics inferred from the flag*) | `src/xrt/ipc/android/src/main/java/org/freedesktop/monado/ipc/Client.java:532-554` |
| F11 | Each satellite is its own IPC server process with its own plug-in instance, its own vendor-SDK core (one core per process) and therefore its own camera manager. Nothing coordinates two satellites today. The lease pattern that would (`u_overlay_lease`) has no broker backend yet | `src/xrt/auxiliary/util/u_overlay_lease.h:1-40`; [android-concurrent-multi-app.md](android-concurrent-multi-app.md) |
| F12 | In-process (Architecture A) apps run their own vendor core in their own process, tracked by the vendor tracking service | [ADR-036](../adr/ADR-036-android-per-window-compositor-instances.md) D2; plug-in F6 |
| F13 | An in-process instance enumerates zero cameras, and a declared `CAMERA_CONSUMER` is always routed to the service | spec §2; r3: `src/xrt/targets/openxr/target.c:203-210`, `src/xrt/state_trackers/oxr/oxr_instance.c:572-573` |

F2 against R7 is the whole problem. Whoever holds the front pair holds the only camera the
tracker can use. And the tracker serves **every** 3D app on the device, not just the caller.

### 1.2 Options

| | Option | Who opens the pair | How tracking survives | Verdict |
|---|---|---|---|---|
| **R** | **Runtime satellite owns the pair, feeds in-app tracking** | The satellite that serves the camera consumer's package. That is the same process that weaves the browser (F8, F9) | The satellite's own vendor core switches from in-service to in-app tracking, fed by a third, low-res left-lens stream on a worker (F5). Only the cores **in that process** are tracked. Other processes' cores lose the tracking service's poses while the pair is open | **Recommended near term** for the browser call. Feasible with existing vendor-SDK surface **if** the three V-asks below confirm |
| **V** | **Vendor tracking service owns the pair, shares frames** | The vendor tracking service: it opens the logical camera instead of the single lens, keeps tracking from it, and hands eye frames (AHardwareBuffer over binder) to an authorised client. The plug-in reads them like the Windows tracker channel | Unchanged for every client: the service never lets go of the camera | **Recommended end state.** R7-consistent and multi-app. Needs a new vendor-service API plus a firmware update on the fleet |
| **V′** | Runtime owns the pair, pushes frames *into* the tracking service | The runtime satellite | The tracking service accepts external frames (cross-process sink) and keeps broadcasting poses to all clients | Fallback if V is refused. Keeps multi-app tracking, but splits one device across two owners and still needs a new vendor-service API — V is cleaner for the same vendor cost |
| **T** | Time-slice: release tracking for the call | Runtime or browser | It doesn't — the display drops to 2D for the whole call. And today the service does not come back (F2) | **Rejected.** A 3D call that turns the sender's own display 2D defeats the purpose ([ADR-043](../adr/ADR-043-stereo-camera-source.md) alternative A, same reasoning) |
| **C** | Chromium opens Camera2 itself; the runtime only arbitrates | The browser process | Not at all, unless the browser also feeds vendor tracking. That puts vendor code and hidden camera ids in the browser | **Rejected** ([ADR-043](../adr/ADR-043-stereo-camera-source.md) alternative D). Chromium cannot even see ids 4/5 (F1) |

**Why R is acceptable for now, and where it stops.**

- *Within the call it is correct.* In practice a call in the browser is the foreground task, and
  the browser's weave runs in the satellite that owns the camera (F8, F9), so the display that
  matters stays tracked.
- *Outside the call it is not:* a second visible 3D app (freeform multi-window) goes untracked
  for the duration, and an **in-process** app that asks for the camera breaks its own tracking:
  its weave runs in its own process (F12), while the camera would be opened by a satellite (F13).
  So under R the runtime **refuses the front pair** (`XR_ERROR_STEREO_CAMERA_BUSY_DXR`) to a
  consumer whose package has no live weave in the owning satellite, and native in-process apps
  wait for V. The rear pair has no such restriction (F3).
- *The deal-breaker to rule out first* is F2's "never reacquires". Under R, every call ends with
  the tracking service evicted. If it cannot be brought back by the client re-requesting tracking
  (the browser-restart observation in F2 suggests it can), R would leave the device untracked
  until a reboot. That is phase A0's experiment (§7).

**Cross-satellite arbitration (needed by R, harmless under V).** Two satellites must not both
open the pair (F11). Add a **camera lease** with the `u_overlay_lease` shape: a pure, host-tested
policy function plus a broker-backed mechanism in `SlotBrokerService`. The first started stream
takes the lease; another satellite enumerates the camera as `SUSPENDED` and refuses start with
`BUSY`. Until the broker backend exists (the overlay lease's own step A3), the lease is
process-local, and a single-call product is safe because only the browser's satellite has a
consumer.

### 1.3 Vendor capabilities each option requires

| Capability | R | V | V′ | Confirmed? |
|---|---|---|---|---|
| In-app face tracking fed with external frames, at runtime, **in a service process** (no Activity) | required | — | — | **Vendor-unconfirmed.** Header present; worked in an Activity probe (F5); in-app licensing failed on Lume Pad 2 (F6) |
| Switching a live core from in-service to in-app tracking and back **without re-creating the interlacer** (no weave interruption) | required | — | — | **Vendor-unconfirmed**; the header says tracking must be re-enabled for a newly attached sink to apply |
| Same face/eye coordinate frame from in-app as from in-service (no pose jump at hand-over) | required | — | — | **Vendor-unconfirmed** |
| Tracking service **reacquires its camera** after another client releases it | required | — | required | **Fails today** (F2); a client re-request recovered it once (F2) |
| Per-lens calibration: intrinsics + pose of each front/back lens | required | required | required | **Partly confirmed.** The vendor SDK's C API exposes per-camera intrinsics (fx, fy, cx, cy, 8 distortion coefficients, mirrored flag), `rotation_deg`, `translation_mm`, sensor orientation and facing, from the device config (format v4). Whether the tablets' configs **populate** intrinsics (rather than rotations only), and the config-index ↔ Camera2-id mapping, are **unconfirmed** |
| Tracking service opens the logical pair and keeps tracking from it | — | required | — | **Vendor-unconfirmed** |
| Frame export from the tracking service to an authorised client (AHardwareBuffer + fence, per-client wake, sequence + exposure timestamp) | — | required | — | **Does not exist** (F4) |
| External frames pushed into the tracking service | — | — | required | **Does not exist** |
| Tracking at a minimum rate from a third, low-res stream does not starve the pair (bus/ISP) | required | required | required | Measured fine once the hand-off left the camera thread (F5) |

---

## 2. Consumer path into Chromium on Android

### 2.1 Where the frames go

The Windows design (B1: `docs/stereo-camera.md` lines 48–91) composes
`displayxr::MaybeWrapWithStereoCameras` in front of the platform factory, in
`services/video_capture/video_capture_service_impl.cc` (patch `0243`). The video-capture process
opens its own runtime connection as `CAMERA_CONSUMER` and copies NV12 from the shared-memory ring
into `Client::OnIncomingCapturedData`. On Android:

| Question | Answer |
|---|---|
| Which process runs video capture? | On Android, upstream Chromium runs the video capture service **in the browser process** (`kRunVideoCaptureServiceInBrowserProcess` defaults on there). *Inferred from upstream, not verified at the fork's pin (`scripts/config.env` `CHROMIUM_TAG=155.0.8059.26`); verify first in B2.* |
| How does it reach the runtime? | The browser process already has a `Context` and the runtime `Client` (F8). The capture device does its own `Client.blockingConnect` (or a second fd from `DisplayXrRuntimeConnector`) and creates an `XrInstance` in the browser process, declaring `XrStereoCameraClientInfoDXR{CONSUMER_ONLY}`. **Same package → same satellite** (F9), so the consumer lands where the browser weaves, which is exactly what option R needs. *Inferred:* no browser-process `XrInstance` exists today (the browser process holds a socket, the GPU process the instance), so the loader's one-instance-per-process constraint B1 works around does not bite |
| Device factory | The same wrapper, now also on Android: `MaybeWrapWithStereoCameras` is gated `#if BUILDFLAG(IS_WIN)` today (B1 patch `0242`) and the portable list/hide/classification code (`displayxr_stereo_camera_devices.*`, `_xr.*`) carries over unchanged. New: `displayxr_stereo_camera_{runtime,device}_android.cc` |
| Frame delivery, phase 1 (CPU) | `OnIncomingCapturedData` with `PIXEL_FORMAT_NV12`, 2560×720 (2.76 MB/frame, ≈ 83 MB/s at 30 Hz), one copy from the ring into Chromium's pool. Wake = the stream's POSIX pipe (r3: `ipc_server_stereo_camera.c` — the `#ifndef XRT_OS_WINDOWS` `wake.fds`), read-only memfd section (r3 R3: `ASharedMemory_setProt(PROT_READ)`) |
| Frame delivery, phase 2 (GPU) | `AHARDWAREBUFFER` transport: three AHBs per stream handed over the socket once; per-frame slot + sync fd. Chromium side: `OnIncomingCapturedExternalBuffer` with an AHB-backed `GpuMemoryBufferHandle` (*inferred to exist on Android at the pin; verify*) |
| Timestamps | `captureTime` is `CLOCK_MONOTONIC` on Android, which is the domain of `base::TimeTicks`. With F1's per-pair sensor timestamps, `captureTimeIsExposure` can be **true** on Android (Windows cannot) |
| Duplicate hiding | Not needed for ids 4/5: they are hidden from `getCameraIdList()` (F1). The physical front lens a page *can* open (id 1, "camera 1, facing front", #175) is the **right** lens of the pair (F1). Opening it while option R holds the pair fails, and it should: list it after the 3D camera |
| Labels / selection | `3D Camera (DisplayXR)` (front) and `3D Camera, rear (DisplayXR)` (back), `facing` user/environment. The web SDK's fast path matches the label (`/stereo|\b3d\b|\bsbs\b|dual/`) and then requires width/height > 2.5 (web: `js/camera/capture.js:53-54,139-161`, `js/camera/geometry.js:7,10`). 2560×720 passes (3.56). It has **no `facingMode` preference** today, so with two 3D cameras it would open whichever matches first — the SDK should prefer `facingMode:'user'` for calls (web follow-up) |

### 2.2 Relation to #175

#175 is a separate, Chromium-side defect: the platform factory returns no video inputs at all,
even with a stream open. No root cause is recorded (browser issue #175, no comments). The
DisplayXR factory **does not depend on it**: it enumerates from the runtime, not from Camera2.
So B2 can ship the 3D camera while #175 is open. #175 must still be fixed for ordinary webcams.
B2 should **not** paper over it by listing the platform cameras itself.

Chromium's own Android permission flow is a prerequisite either way. The browser package must
hold `android.permission.CAMERA` before Chromium opens any capture device, and the runtime checks
the same permission on the peer uid (§4). *Inferred from upstream; the fork has no camera-permission
patch (browser: no hit for `uses-permission`/CAMERA in `patches/`).*

---

## 3. Rectification, calibration, output format

### 3.1 Calibration source

In order of preference, all inside the plug-in (ADR-019):

1. **Vendor device config** (the vendor SDK's per-camera record: intrinsics, distortion,
   `rotation_deg`, `translation_mm` relative to the display, `sensorOrientation`, facing). The
   plug-in composes `rotation_right_from_left` / `translation_right_from_left_mm` from the two
   lenses' poses, in OpenCV's `x_R = R·x_L + T` convention (spec §9). Today the plug-in reads only
   camera 0's translation (plug-in: `leia_cnsdk.cpp:805`).
2. **Camera2 `CameraCharacteristics` of the physical lenses**: `LENS_INTRINSIC_CALIBRATION`,
   `LENS_DISTORTION`, `LENS_POSE_ROTATION`, `LENS_POSE_TRANSLATION`, `LENS_POSE_REFERENCE`. These
   are vendor-neutral and would let a second vendor reuse the whole path. Whether this OEM
   populates them is unknown; phase A0 reads them without opening a camera.
3. **Nominal fallback**: focal length from `LENS_INFO_AVAILABLE_FOCAL_LENGTHS` ÷ pixel pitch from
   `SENSOR_INFO_PHYSICAL_SIZE`, principal point at the centre, no distortion, rotation identity,
   baseline from the device config. Report it as `CALIBRATED` only if the A0 gate shows that
   online refinement brings the rows under 0.5 px. Otherwise report RAW only.

The measured raw misalignment is small: about −2 px vertical (median) and 0.09° roll (F1, front
pair). That makes 3 viable. `u_stereo_vrefine`'s model is `dy = a + b·(y − cy)`, with no x (roll)
term (r3: [stereo-camera-source.md](stereo-camera-source.md) "R2 as built"). 0.09° of roll is
about ±1 px at the eye's left and right edges (640 px × tan 0.09°). That is at the gate, so A1
measures the edge columns separately and adds a roll term only if they fail.

### 3.2 Do `u_stereo_rectify` + `u_stereo_vrefine` apply unchanged?

**Geometry: yes.** Bouguet with no OpenCV dependency, RADTAN5/RADTAN8/KB4, rescaling calibration
given at a different size than the frames (Camera2 calibration often describes the full active
array, not 1280×720 — rescale *and crop* by the stream's `SCALER_CROP_REGION`; *inferred*).
NV12 is already a supported LUT format.

**Cost: not unchanged.** Measured on an M1 Pro at -O2, one thread (r3, "R2 as built"):
1.36 ms per 1280×480 NV12 frame; vrefine 2.7 ms per measurement, plus an 8 ms rebuild.

| | Pixels/frame | M1 Pro (scaled) | Tablet SoC (*inferred* 1.5–2.5× slower per core) |
|---|---|---|---|
| Windows SR source, NV12 | 2·640×480 = 0.61 MP | 1.36 ms (measured) | — |
| Tablet pair, NV12 | 2·1280×720 = 1.84 MP | ≈ 4.1 ms | ≈ 6–10 ms |

That is above the 2 ms camera-thread budget R2 set for itself. So on Android:

- **CPU v1:** split rows across two threads (`u_stereo_rectify_lut_apply_rows`, which exists for
  this) and keep vrefine on its worker. Measure in A1; the gate is ≤ 8 ms per frame wall-clock and
  the pair still at 30.00 fps.
- **GPU v2:** the `scam_rectifier` seam is designed for it (r3: `ipc_server_stereo_camera.c`
  header comment, "A GPU backend uploads `u_stereo_rectify_build_map()` as an RG32F texture").
  Vulkan compute in the satellite, remapping the two per-lens AHBs straight into the AHB ring
  (§6). That also removes the per-eye → SBS packing copy.
- LUT memory scales too: about 6 bytes per output sample → ≈ 11 MB + 2.8 MB (*scaled from R2's
  3.7 + 0.9 MB*). Fine.

### 3.3 Output — what the web SDK expects

From B1 (`docs/stereo-camera.md` lines 15–35) and web (`docs/camera.md`, `js/camera/capture.js`
96–117, `js/call/wire.js` 121–158), what a page needs is:

- one **full-width L|R side-by-side** frame, left eye left, **never mirrored**, NV12;
- aspect > 2.5;
- a `displayxrStereo` hint: `{layout:'side-by-side', rectified:true, baselineMm, horizontalFovDeg}`
  (whole mm and whole degrees), from which the `hello` message is filled.

The Android output is therefore **2560×720 NV12 RECTIFIED**, i.e. the same contract at a different
size. The web SDK needs no change for the frame. It does need a `facingMode` preference (§2.1).
The `baselineMm` hint is 25 (NP02J front), 45 (NP02J rear), 15 / 25 (Lume Pad 2 front / rear) per
the device owner's figures. The calibrated |T| is authoritative and the hint just rounds it.

**Eye order is a gate, not an assumption.** F1 labels the front lenses L = 3, R = 1 without
saying from whose side, and §C.0 notes that "for viewer left/right the front eyes need mirroring".
The spec convention (§4) is: left half = the lens on the camera's own left, seen from behind the
camera looking at the scene. For a user-facing pair, that is the lens on the **user's right**.
Two mechanisms already settle this: r3 commit `8c590f808` WARNs when a plug-in's calibration
puts the right camera at −x, and the Windows plug-in auto-swaps halves from the sign of `T.x`
(plug-in `feat/stereo-camera-l1`, `leia_stereo_camera.cpp`). A1's gate requires **positive
disparity** on a near object, and the frames are never mirrored in the runtime.

---

## 4. Consent and privacy on Android

Spec §7 / R3 assume the OS cannot see the consumer (the tracker opened the device). On Android
under option R, **the runtime is the OS-visible camera client**. Android's controls therefore
apply to the runtime package, and they reach the browser only through the runtime. What changes:

### 4.1 Identity — the R3 code does not work on Android as written

R3 keys consent, the delegating list, "Allow once" and `persistentId` on the **verified peer
executable path**. On Android the peer's executable is the zygote's `app_process64` for every
app (*inferred*), or unreadable across uids. The path comes from `/proc/<pid>/exe` under
`XRT_OS_LINUX`, which also matches Android (r3:
`src/xrt/ipc/server/ipc_server_peer_creds.c:248-266`). An empty path is refused at step 3
(spec §7.1). Either every app shares one consent entry, or every app is refused.

**Proposal:** on Android the consent subject is **(package name, signing-certificate SHA-256)**
of the `SO_PEERCRED` uid (`PackageManager.getPackagesForUid` + `hasSigningCertificate`). Shared-uid
packages fall back to the uid string, as `SlotBroker.resolveCallerPackage` already does
(`SlotBroker.kt:147-164`). The store keeps the `u_camera_consent` policy unchanged, behind its
environment vtable. `persistentId` = HMAC(secret, device ‖ package ‖ cert) (spec §7.5).

### 4.2 The decision tree mapped (spec §7.1)

| Step | Desktop (R3) | Android |
|---|---|---|
| 1 Sharing off | env / tray toggle | env (`debug.dxr.*` property) / runtime settings toggle; also the **OS sensor-privacy camera toggle** (Android 12+), which blanks the *runtime's* capture. Report it as `SUSPENDED` with a reason, not as a broken camera |
| 2 Dev override | `DXR_STEREO_CAMERA_DEV_ALLOW=1` | the same, as a debuggable-build-only property |
| 3 Unverified peer | empty exe → refuse | no package for the uid → refuse |
| 4 Delegating client | installer-written list | a list **built into the runtime APK**, keyed by package + certificate digest (an APK cannot be modified by another installer). The DisplayXR Browser is the v1 entry; its per-origin prompt is the consent |
| 5 OS camera switch | Windows ConsentStore | **The peer uid must hold `CAMERA`** (`checkPermission(CAMERA, pid, uid)`) and its camera app-op must not be ignored (`AppOpsManager.checkOpNoThrow`) → else `PERMISSION_INSUFFICIENT` |
| 6–7 Stored decision / Allow once | registry / JSON | JSON in the runtime app's private storage (the POSIX store, `u_file_*_in_config_dir`), keyed per §4.1 |
| 8 Prompt | tray dialog | **Deferred.** A service starting a consent Activity runs into background-activity-launch rules (*inferred*). v1 Android = delegating clients + stored decisions only (set from the runtime's settings Activity or the CLI-equivalent), and everything else `CONSENT_REFUSED`. Native-app prompting waits for a demand |

### 4.3 What Android already enforces, and what the runtime keeps

| Concern | Android does it | The runtime still owns |
|---|---|---|
| Camera only while the user is looking | While-in-use: a background uid's camera client is disconnected. Under option R the owner is the satellite, which holds while-in-use **only because the foreground browser bound it with `BIND_INCLUDE_CAPABILITIES`** (F10). So when the browser leaves the foreground, the OS takes the camera from the satellite. *Inferred; A1 gate checks it.* **Consequence:** the satellite must **not** declare `foregroundServiceType="camera"` (today it declares `connectedDevice\|mediaPlayback`, `AndroidManifest.xml:55-78`). That would let it keep the camera in the background, and the runtime would then have to re-implement the foreground rule itself | Spec §7.2 on Android: today `pid_has_visible_window()` falls to `return true` off Windows/macOS (r3: `ipc_server_stereo_camera.c:601-614`), so the runtime has no rule of its own. Keep the OS as the gate and treat a camera disconnect as `SUSPENDED`. The spec/roadmap's `android_package_is_visible` suggestion is **wrong**: that host slot answers *package visibility* (`<queries>`), not foreground (`xrt_plugin.h:566-605`) |
| Lock screen | The foreground app stops, so the same disconnect applies | nothing extra |
| In-use indicator | Status-bar privacy dot (Android 12+), attributed to the **runtime** package, not the browser. Chromium shows its own tab indicator | A notification "3D camera in use by <browser>" with a **Stop** action (= spec §7.4 "Stop camera sharing"). This is the runtime's in-use indicator on Android, and the only place a user sees *who* receives the frames |
| Runtime's own `CAMERA` permission | Required; it is a runtime permission, so the user grants it to the **runtime app** once (from its Activity) | Request it at first use, through the runtime's settings Activity; until it is granted, report the cameras as `UNAVAILABLE` |
| Fingerprinting | — | Unchanged (§7.5, keyed per §4.1); the browser coarsens the hint |

Under option V the tracking service is the OS-visible client and none of this changes on the
runtime side, except that the runtime indicator matters more: the OS dot would then name the
tracking service.

---

## 5. Front vs rear, rotation, device generations

**Which camera a call uses.** The front pair (`USER_FACING`). The rear pair is "show what I'm
looking at": it is enumerated as a second camera without `USER_FACING`, which maps to
`facing: environment`. It needs no tracking hand-over (F3), so it ships first.

**Rotation.** The baseline is fixed in the device frame and horizontal in the device's natural
orientation, which on NP02J is landscape (`ROTATION_0` = landscape). Rules:

- **The runtime publishes frames upright in the device's natural orientation, always** (the
  plug-in applies `sensorOrientation`). The spec should say so explicitly; the stereo layout is
  only defined in that orientation. If a lens's sensor orientation is 90°/270°, each eye
  rotates to 720×1280 before packing (*unknown which; A0 reads it*).
- **Reverse landscape (180°):** rotating the whole SBS frame by 180° is exactly right. It swaps
  the halves *and* turns each eye, which matches the lens that is now physically on the left.
  The browser device applies it, as Chromium already does for platform cameras (*inferred*).
- **Portrait (90°/270°): there is no horizontal stereo.** The baseline is vertical, so rectified
  disparity is vertical, which no receiver can show. Proposed: the **browser device** switches
  the track to **mono** (one eye, rotated upright, 720×1280) and drops `displayxrStereo`. The web
  SDK then classifies it as mono (aspect < 2.5) and receivers lift it, as for any 2D sender.
  This is a policy in the browser device; the runtime is unchanged. *Owner decision* (§8.1 Q3).
  A WebRTC resolution change mid-call is ordinary.
- The SDK must re-read track settings on a resolution change (web follow-up; today the hint is
  read at open, `js/camera/capture.js:96-117`).

**The two generations.**

| | NP02J (Nubia Pad 3D II) | Lume Pad 2 |
|---|---|---|
| Baselines | front 25 mm, rear 45 mm | front 15 mm, rear 25 mm |
| Pair topology (logical ids, 1280×720 NV12 ×2) | **Measured** (F1) | **Not measured** — ids, sizes, sync unknown |
| In-app tracking (needed by option R) | worked in a probe app (F5) | **failed to license** in the plug-in (F6) — option R may be unavailable; rear pair still fine |
| Disparity of a face at 0.6 m, *inferred* f ≈ 1000 px at 1280 px width | ≈ 42 px front | ≈ 25 px front |

15 mm is still a usable call stereo base at arm's length, but noisier. vrefine's ≥ 50 matches gate
and the receiver's auto-convergence (web#92) both operate in that range. Expect Lume Pad 2 to ship
**rear-only** unless the in-app licensing is fixed, or V lands.

---

## 6. Extension and plug-in ABI impact

**`XR_DXR_stereo_camera` (OpenXR surface): usable as is for phases A1–B2.**

- Enumerate, calibration (RAW/RECTIFIED), NV12, shared-memory transport, `USER_FACING` (absent =
  rear), `captureTimeIsExposure`, events and results all fit unchanged.
- **Spec text** changes, no struct changes:
  - §6: allow a plug-in to **own** the device when it also sources tracking from that capture (option R). Today's "never opens the device away from the tracker" is a Windows-shaped rule.
  - §3/§5: frames are upright in the device's natural orientation.
  - §7.1/§7.2: the Android rows of §4 above, replacing the `android_package_is_visible` line.
  - §2: the satellite / camera-lease note.
- **AHB transport (phase A4):** the enum value `XR_STEREO_CAMERA_TRANSPORT_AHARDWAREBUFFER_DXR`
  exists (spec §5.1), but its carrier does not. Add:
  - `XrStereoCameraStreamTransportAhbDXR` (chained on `XrStereoCameraStreamInfoDXR`): `slotCount`, and the AHBs delivered over the socket.
  - `XrStereoCameraFrameAhbDXR` (chained on `XrStereoCameraFrameDXR`): `AHardwareBuffer* buffer` for the acquired slot, and `int acquireFenceFd` (the caller closes it).
  - Type values come from a new block allocated at implementation time; R1–R3 use `1004999300–316`.
  - Spec version bump; old consumers keep shared memory.

**Plug-in iface (ADR-020, append-only).**

- **Phase A1 needs no ABI change.** The plug-in packs the two per-lens NV12 images into one CPU
  SBS frame (`struct xrt_plugin_stereo_camera_frame`, `xrt_plugin.h:738-753`). That is one copy,
  ≈ 2.8 MB per frame. Calibration fits `struct xrt_plugin_stereo_camera_calibration` (`:719-732`).
  `stereo_camera_open` "doubles as a tracker keep-alive" (`:1092-1098`), which is where option R's
  plug-in performs the tracking hand-over, internally. The plug-in instance that owns the camera is
  the one whose DP weaves in that process (*inferred from the slot taking
  `struct xrt_plugin_instance *`; verify in A3*).
- **Zero-copy (A4) cannot grow the frame struct.** `xrt_plugin_stereo_camera_frame` has **no
  `struct_size`**: the runtime allocates it and the plug-in fills it, so a field appended there
  would be written past the end by a new plug-in on an old runtime. So append, at the end of
  `struct xrt_plugin_iface`, after `stereo_camera_close`:

  ```c
  //! Optional; used when non-NULL and struct_size covers it. Same rules as
  //! stereo_camera_wait_frame. XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_FRAME2.
  uint32_t (*stereo_camera_wait_frame2)(struct xrt_plugin_stereo_camera *cam, int64_t timeout_ns,
                                        struct xrt_plugin_stereo_camera_frame2 *out);

  struct xrt_plugin_stereo_camera_frame2 {
  	uint32_t struct_size;            // set by the runtime
  	uint32_t layout;                 // XRT_PLUGIN_STEREO_CAMERA_LAYOUT_SBS_CPU | _PER_EYE_AHB
  	uint64_t sequence; int64_t time_ns; bool time_is_exposure;
  	uint32_t width, height, format;  // per-eye extent when PER_EYE_AHB
  	const uint8_t *planes[2]; uint32_t pitches[2];   // SBS_CPU
  	struct AHardwareBuffer *eye_buffer[2];           // PER_EYE_AHB, owned by the plug-in until release
  	int acquire_fence_fd;            // -1 = ready; the runtime closes it
  };
  ```

  `stereo_camera_release_frame` keeps its meaning. No `XRT_PLUGIN_API_VERSION_CURRENT` bump.
  `downstream-pins.json`'s `features` track picks up the new macro.
- **No other slot is needed** for R or V: the hand-over is plug-in-internal. Option V is
  a different *source* behind the same slots, which is the point of ADR-043 decision 1.
- **Runtime-internal, not ABI:** the camera lease (§1.2) and the Android consent environment
  (§4).

---

## 7. Phases

Each phase lists its gate, a rough size, and what it needs (**tablet** / **vendor** / neither).
Every tablet phase follows the pad-lock protocol: one session at a time, the serial pinned, and
the head-tracking service's state checked before and after. A camera probe has wedged tracking
before (F2).

| Phase | Scope | Gate (checkable) | Size | Needs |
|---|---|---|---|---|
| **A0a** — cheapest de-risk, read-only | A throwaway APK (or `dumpsys media.camera`) that **opens no camera**. It dumps `CameraCharacteristics` for ids 0–5 (sensor orientation, `LENS_POSE_*`, `LENS_INTRINSIC_CALIBRATION`, `LENS_DISTORTION`, logical-multi-camera sync type, stream configurations incl. whether 2×1280×720 + 640×480 is a supported logical combination) and logs the vendor device-config camera records via the plug-in's existing config read. | A table in this doc: for each of the 4 lenses, whether intrinsics and pose exist and where; sensor orientation; config-index ↔ Camera2-id map. **Proves:** whether calibration exists vendor-neutrally (§3.1 source 2) or only in the vendor config, and whether eyes need rotating. Cannot disturb tracking | ½ day | tablet |
| **A0b** — the deal-breaker | The same probe, in a **bound service** (not an Activity): open the front pair with the third stream, attach the vendor in-app external-frame sink on a worker, track for 60 s, close, then re-request tracking from the tracking service (disable/enable face tracking, else re-bind). | (1) In-app tracking licenses and reports faces **from a service process**. (2) The pair stays at 30.00 fps. (3) After close, the tracking service is serving faces again **within 5 s, no reboot, no app restart** (`dumpsys media.camera` shows it as the client again). **Proves:** option R is viable on NP02J, or kills it before any runtime code. Repeat on Lume Pad 2 for (1) | 1–2 days | tablet; vendor if (1) or (3) fails |
| **A1** — rear pair, end to end in the runtime | Plug-in Android Camera2 source (NDK `ACameraManager`, logical id from the vendor config rather than hard-coded), CPU SBS pack, calibration per §3.1, enumerate rear as non-`USER_FACING`; service unchanged except threaded row-split rectify; a minimal `CAMERA_CONSUMER` test client APK as the Android stand-in for `displayxr-cli camera probe` | On NP02J rear: rectified row residual median ≤ 0.5 px, edge columns ≤ 1 px; positive disparity on a near object; 30.00 fps source, ≤ 8 ms rectify per frame; head tracking and weave of a running 3D app unaffected (tracking state logs before/during/after). Sim fake + host tests stay green | 1–1.5 weeks | tablet |
| **A2** — Android privacy | §4: package+cert identity, the peer-uid `CAMERA` check, the built-in delegating list, the in-use notification + Stop, sharing toggle, OS disconnect → `SUSPENDED`, runtime `CAMERA` request; spec §7 Android rows | `tests_camera_consent` extended with an Android environment fake (identity, permission denied, app-op ignored); device matrix: browser denied CAMERA → `PERMISSION_INSUFFICIENT`; browser backgrounded → frames stop within 1 s; lock screen → stop; Stop action → `STREAM_ENDED` | 1 week | tablet (matrix only) |
| **B2a** — browser, rear pair | B1's factory on Android; capture-service process verified at the pin; browser-process `CAMERA_CONSUMER` instance; CPU NV12; labels/facing; rotation policy (§5); results mapped as B1 | A page on the tablet lists `3D Camera, rear (DisplayXR)`, opens 2560×720 with `displayxrStereo{rectified:true, baselineMm:45}`; a call tablet → laptop shows a rectified, correctly-ordered rear stereo pair; rotating to portrait switches to mono without ending the call | 1–1.5 weeks + one Android build-box run | tablet |
| **A3** — front pair, option R | Plug-in hand-over: open front pair → in-app tracking from the third stream on a worker → close → restore in-service tracking; camera lease (process-local first); refuse front pair to a package with no live weave in the satellite | With the browser weaving in 3D: front pair at 30.00 fps **while the weave stays tracked** (tracking state + eyeball); no visible pose jump at hand-over; within 5 s of stop, an in-process 3D app is tracked again; a second package's start → `BUSY` | 1–2 weeks | tablet + **vendor** (A0b outcomes) |
| **B2b** — the product gate | Front camera in the browser + web SDK `facingMode:'user'` preference + hint re-read on resize | Tablet ↔ SR laptop 3D call, **both directions stereo, both displays tracked**, 10 minutes, no tracking loss on either side after hang-up | 3–5 days | tablet |
| **A4** — performance | AHB transport (§6 structs), `stereo_camera_wait_frame2`, Vulkan-compute rectifier into the AHB ring; Chromium `OnIncomingCapturedExternalBuffer` path | Camera-thread CPU per frame ≤ 2 ms (R2's budget) with rectification on GPU; end-to-end exposure → page latency not worse than CPU path; ABI test pins the new slot order | 2–3 weeks | tablet |
| **V** (parallel) | Vendor tracking service owns the pair and exports frames (§1.3 V column); plug-in switches source; option R removed | Everything in B2b, **plus** a second visible 3D app stays tracked during the call, and `dumpsys media.camera` shows exactly one client (R7) | vendor-sized | **vendor** + firmware |

A0a and A0b come first because they decide between "R works on NP02J" and "rear-only until V".
That decides whether A3 is scheduled at all. A1, A2 and B2a are useful in either outcome.

---

## 8. Open questions

### 8.1 For the project owner (decisions)

1. Accept option R (runtime satellite owns the front pair; other apps' tracking pauses during a call) as the near-term front-camera path, with V as the end state — yes/no?
2. Ship rear-only on a device where in-app tracking cannot run (Lume Pad 2 today) — yes/no?
3. In portrait, should the browser send mono (one eye, upright) rather than a stereo pair that cannot be shown — yes/no?
4. Android v1 consent = the built-in delegating list (DisplayXR Browser by package + certificate) plus stored decisions, with no runtime prompt for native apps — acceptable?
5. Is the runtime app's own Android `CAMERA` permission prompt (asked once, from the runtime's Activity) acceptable product-wise, given that it is a second prompt beside Chromium's?
6. Should the front pair be refused to native in-process apps until option V lands (they would otherwise break their own tracking) — yes/no?
7. File the option-V request to the vendor now, before A0b's result — yes/no?

### 8.2 For the vendor SDK / tracking-service team

1. Does in-app face tracking with an external-frame sink license and run inside a bound **service** process (no Activity) on NP02J and on Lume Pad 2?
2. Can a live core switch from in-service to in-app tracking and back without re-creating its interlacer?
3. Do in-app and in-service tracking report faces in the same coordinate frame and units?
4. Can the tracking service reacquire its camera automatically when another client releases it (today it stays "Camera has been invalidated")?
5. Until then, what client call reliably makes the tracking service reopen its camera without a reboot?
6. Do the tablets' device configs populate per-lens intrinsics and distortion, or only rotations and translations?
7. Which config camera index corresponds to which Camera2 physical id (front L/R, back L/R), and is that mapping stable across firmware?
8. Is the stereo pair's logical camera id published anywhere (config or API), or must it be hard-coded?
9. What frame size and format does in-app tracking need — must it be a separate 640×480 stream, or can it take a downscaled copy of an eye stream?
10. Could the tracking service open the logical pair itself and share eye frames (AHardwareBuffer + fence + exposure timestamp) with an authorised client (option V)?
11. If not, could it accept frames pushed from another process and keep serving poses to all clients (option V′)?

---

## 9. What this page corrects or adds to the existing docs

- **"The runtime service owns the front pair"** (stereo-camera-source §C.1 on r3) → more
  precisely, *the satellite process that weaves for the consumer's package*, plus a cross-satellite
  lease (§1.2). The Android "service" is not one process (F9, F11).
- **[OEM requirement R7](../specs/vendor/oem-android-platform-requirements.md#r7--camera-arbitration-through-the-tracking-service)
  says the runtime never touches the camera.** Option R breaks that knowingly, while a call is
  live. R7's text should gain a sentence about the camera-call exception, or R should be retired
  as soon as V exists.
- **Spec §7.1 "`android_package_is_visible` for §7.2"** is a category error: that host slot is
  package visibility, not foreground (§4.3).
- **R3's executable-path identity** does not identify Android apps (§4.1).
- **Calibration** is richer than "rotations only": the vendor SDK's C API carries per-camera
  intrinsics (§1.3). Whether the devices populate them is A0a's question.

## 10. Not verified (by design or by reach)

- Nothing here was run on a device; no `adb` was used to write it. Every *measured* fact is from
  the 2026-09-26 session.
- Where Chromium 155 runs the video capture service on Android, and whether its Android capture
  client accepts AHB-backed external buffers.
- The Android semantics of `BIND_INCLUDE_CAPABILITIES` for camera while-in-use, and the
  background-activity-launch rules for a consent Activity (both *inferred*, §4).
- `/proc/<pid>/exe` content for an app peer on Android (*inferred* `app_process64` or unreadable).
- Every vendor-SDK behaviour in §1.3 marked vendor-unconfirmed. Vendor-SDK API presence was
  checked against a local 0.10.71 checkout of the vendor SDK, which is not the vendor's main
  branch.
- Lume Pad 2's camera topology (only NP02J was probed).
- Tablet CPU cost of rectification (scaled from M1 Pro numbers, §3.2).

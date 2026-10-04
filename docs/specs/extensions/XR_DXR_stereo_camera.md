# XR_DXR_stereo_camera — Stereo Camera Source

| Field | Value |
|---|---|
| **Extension Name** | `XR_DXR_stereo_camera` |
| **Spec Version** | 1 |
| **Extension Type** | Instance extension, service path only (an in-process instance enumerates zero cameras) |
| **Header** | [`src/external/openxr_includes/openxr/XR_DXR_stereo_camera.h`](../../../src/external/openxr_includes/openxr/XR_DXR_stereo_camera.h) (+ its `index.json` catalog note) |
| **Status** | **R1 implemented** (runtime, hardware-free): header, plug-in slots, service camera manager, IPC, OpenXR entry points, sim_display fake, `displayxr-cli camera`, selftest check. **R2 implemented**: the service-side rectifier (`u_stereo_rectify`, CPU) — RECTIFIED frames + rectified calibration from any CALIBRATED source, golden-tested against OpenCV, and a distorted sim fake with ground truth. **Not yet:** consent / indicator / foreground rule (R3 — deny-by-default hooks in place), GPU transports (the rectifier has the seam), state-change events, the Leia provider (L1), the browser (B1). Provisional type values `1004999300–310` (relocated from `290–300` when `XR_DXR_weave` v14 took `290`; `XR_DXR_lift` holds `270–289`), pending Khronos registry |
| **R1 decisions** | stereo-only (names/structs kept open: `viewCount`, next chains) · camera provider = `xrt_plugin_iface` slots · service clients only (in-process enumerates zero) · raw frames never reach web pages (`RAW` refused to `PRESENT_OWNER` clients) — see [roadmap §G](../../roadmap/stereo-camera-source.md#g-open-questions--maintainer-decisions) |
| **Decision record** | [ADR-043](../../adr/ADR-043-stereo-camera-source.md) |
| **Plug-in contract** | appended `xrt_plugin_iface` camera slots, `XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA` (§9) |
| **Browser / Android integration + plan** | [roadmap/stereo-camera-source.md](../../roadmap/stereo-camera-source.md) |

## 1. What it is

Many 3D displays carry a **stereo camera** — usually the one the display's eye tracker looks
through. It is exactly the camera a 3D video call wants, and on several classes of hardware an
application cannot open it: the vendor's tracker holds the device **exclusively** while it
tracks, so the OS camera API fails with "device in use"; when it does open, it is a raw,
unrectified pair with no calibration attached.

`XR_DXR_stereo_camera` makes such a camera a **runtime-provided source**:

1. **The vendor plug-in produces frames; the service owns them; clients consume them.** The
   plug-in reads the camera by whatever private channel its stack offers (a tracker's shared
   memory, a vendor camera service) and hands the runtime plain frames plus calibration. It
   never opens the device away from the tracker.
2. **One capture, many consumers.** The service opens each camera once and fans frames out to
   every authorised stream. Nothing a consumer does can starve the eye tracker or another
   consumer.
3. **Rectified by default, using the ACTIVE device's calibration.** The plug-in answers for the
   device it is bound to; the service rectifies with a vendor-neutral routine. A consumer that
   wants raw pixels + calibration can ask for them.
4. **Frames are privacy-gated by the runtime**, not just by the consumer: authorised peers only,
   visible/foreground clients only, an always-on in-use indicator, and a user kill switch (§7).
5. **Pre-weave, never woven.** A stereo camera frame is ordinary side-by-side content. Showing it
   in 3D is the weave path's job (ADR-007) and is the consumer's choice.

```c
uint32_t n = 0;
xrEnumerateStereoCamerasDXR(instance, systemId, 0, &n, NULL);
XrStereoCameraPropertiesDXR cams[4] = {{XR_TYPE_STEREO_CAMERA_PROPERTIES_DXR}, ...};
xrEnumerateStereoCamerasDXR(instance, systemId, n, &n, cams);

XrStereoCameraStreamCreateInfoDXR ci = {XR_TYPE_STEREO_CAMERA_STREAM_CREATE_INFO_DXR, NULL,
    cams[0].cameraId, XR_STEREO_CAMERA_OUTPUT_RECTIFIED_DXR, XR_STEREO_CAMERA_FORMAT_NV12_DXR,
    XR_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY_DXR, /*maxFrameRate*/ 30.0f};
xrCreateStereoCameraStreamDXR(instance, &ci, &stream);
xrStartStereoCameraStreamDXR(stream);
// consumer thread: wait on the stream's wake handle, then
XrStereoCameraFrameDXR f = {XR_TYPE_STEREO_CAMERA_FRAME_DXR};
if (xrAcquireStereoCameraFrameDXR(stream, &f) == XR_SUCCESS) { /* f.slot is pinned until the next acquire */ }
```

## 2. Availability

| Situation | Result |
|---|---|
| In-process instance (any platform) | `xrEnumerateStereoCamerasDXR` → `XR_SUCCESS`, count 0. The single owner of a camera is the service; an in-process runtime opening it too would recreate the exclusive-device fight this extension exists to end |
| Service whose plug-in has no camera slots (or `sim_display` without its fake) | count 0 |
| Service with a camera source | one entry per camera, with its `state` (§3) |
| `DXR_STEREO_CAMERA=0` in the **service's** environment, or the user kill switch (§7.4) | count 0 — indistinguishable from "no camera", on purpose |

Instance-level, not session-level: a capture component (a browser's video-capture service) has
no compositor session and needs none. Calls travel on the instance's IPC connection; streams
belong to that connection and die with it.

### 2a. Declaring a camera-only client (spec v2, R3)

A process that will only ever read a camera — a browser's video-capture utility, `displayxr-cli
camera` — chains `XrStereoCameraClientInfoDXR` with
`XR_STEREO_CAMERA_CLIENT_CONSUMER_ONLY_BIT_DXR` on `XrInstanceCreateInfo`:

```c
XrStereoCameraClientInfoDXR ci = {XR_TYPE_STEREO_CAMERA_CLIENT_INFO_DXR, NULL,
                                  XR_STEREO_CAMERA_CLIENT_CONSUMER_ONLY_BIT_DXR};
XrInstanceCreateInfo ii = {XR_TYPE_INSTANCE_CREATE_INFO, &ci, ...};   // enabledExtensionNames includes XR_DXR_stereo_camera
```

The service admits such an instance as the **`CAMERA_CONSUMER` client class** (quota 4): it may
call only the camera entry points — `xrCreateSession` is refused with
`XR_ERROR_PERMISSION_INSUFFICIENT` — and it is **not a panel owner**, so it never counts toward
the `PRESENT_OWNER` quota even when its executable is the browser's (the quota counts distinct
owner *executables*; a camera-only sibling of an admitted owner is simply not in the count). The
declaration outranks the `XR_DXR_weave`-derived present-owner claim: a capture utility that
enables both is still a camera consumer. A hybrid runtime always routes a declared consumer to
the service (an in-process instance has no cameras). Without the declaration the instance is
classed as before (APP, or PRESENT_OWNER when it enables `XR_DXR_weave`).

## 3. Enumerating cameras

```c
typedef XrFlags64 XrStereoCameraFlagsDXR;
// the camera is the eye tracker's; frames exist only while tracking runs (§6)
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING_BIT_DXR = 0x1;
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_USER_FACING_BIT_DXR             = 0x2;
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_CALIBRATED_BIT_DXR              = 0x4; // calibration available → RECTIFIED supported
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_NATIVELY_RECTIFIED_BIT_DXR      = 0x8; // the source already delivers a rectified pair
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_MONOCHROME_BIT_DXR              = 0x10; // luma only (e.g. an IR / grey tracking sensor)

typedef enum XrStereoCameraStateDXR {
    XR_STEREO_CAMERA_STATE_AVAILABLE_DXR = 1,   // frames flowing or will flow on start
    XR_STEREO_CAMERA_STATE_WAITING_DXR   = 2,   // source up, no frames yet (tracker warming up, §6)
    XR_STEREO_CAMERA_STATE_SUSPENDED_DXR = 3,   // temporarily no frames: tracker stopped, privacy lock, session locked
    XR_STEREO_CAMERA_STATE_UNAVAILABLE_DXR = 4, // failed / removed
} XrStereoCameraStateDXR;

typedef struct XrStereoCameraPropertiesDXR {
    XrStructureType            type;
    void* XR_MAY_ALIAS         next;
    uint64_t                   cameraId;          // service-lifetime id, used by every other call
    char                       persistentId[64];  // stable per device + per consumer executable (§7.5); never the raw serial
    char                       displayName[128];  // plug-in supplied, human-readable ("Built-in 3D camera")
    char                       platformDeviceHint[256]; // the OS's id of the physical device this source reads, if any
                                                  // (so a capture stack can hide the raw duplicate); "" if none
    XrStereoCameraFlagsDXR     flags;
    XrStereoCameraStateDXR     state;
    XrExtent2Di                eyeExtent;         // native per-eye size (e.g. 640x480)
    float                      maxFrameRate;      // Hz the source delivers: MEASURED by the service after its
                                                  // first open, else the vendor's value; 0 = not known yet
    float                      baselineMm;        // |T| of the pair; 0 if uncalibrated
    float                      horizontalFovDeg;  // per eye, rectified; 0 if uncalibrated
    XrStereoCameraFormatFlagsDXR supportedFormats;       // GRAY8 / NV12 / BGRA8 bits the service will produce
    XrStereoCameraTransportFlagsDXR supportedTransports; // SHARED_MEMORY always; GPU transports optional (§5.3)
} XrStereoCameraPropertiesDXR;

XrResult xrEnumerateStereoCamerasDXR(XrInstance instance, XrSystemId systemId,
    uint32_t capacityInput, uint32_t* countOutput, XrStereoCameraPropertiesDXR* cameras);
```

`XrEventDataStereoCameraStateChangedDXR {cameraId, state}` is queued on every state change,
and `XrEventDataStereoCamerasChangedDXR` when the set changes (plug-in hot-plug, device swap).
Re-enumerate on the latter; `cameraId`s of vanished cameras are never reused within a service
lifetime.

## 4. Calibration

```c
typedef enum XrStereoCameraDistortionModelDXR {
    XR_STEREO_CAMERA_DISTORTION_NONE_DXR = 0,
    XR_STEREO_CAMERA_DISTORTION_RADTAN5_DXR = 1,   // OpenCV k1 k2 p1 p2 k3
    XR_STEREO_CAMERA_DISTORTION_RADTAN8_DXR = 2,   // + k4 k5 k6 (rational)
    XR_STEREO_CAMERA_DISTORTION_KB4_DXR = 3,       // fisheye
} XrStereoCameraDistortionModelDXR;

typedef struct XrStereoCameraIntrinsicsDXR {
    XrExtent2Di  imageExtent;        // the image these numbers describe
    float        fx, fy, cx, cy;     // pixels
    XrStereoCameraDistortionModelDXR model;
    float        coefficients[8];
} XrStereoCameraIntrinsicsDXR;

typedef struct XrStereoCameraCalibrationDXR {
    XrStructureType            type;
    void* XR_MAY_ALIAS         next;
    XrStereoCameraOutputDXR    output;          // IN: which output the numbers should describe
    XrStereoCameraIntrinsicsDXR eye[2];         // RAW: native + distortion. RECTIFIED: post-rectification, model NONE, equal fy/cy
    XrPosef                    rightFromLeft;   // RAW: the pair's extrinsics. RECTIFIED: pure +x translation of baselineMm/1000
    float                      baselineMm;
} XrStereoCameraCalibrationDXR;

XrResult xrGetStereoCameraCalibrationDXR(XrInstance instance, uint64_t cameraId,
    XrStereoCameraCalibrationDXR* calibration);
```

**Whose calibration.** The plug-in returns the calibration of **the device it is bound to** —
the same physical unit whose display it weaves for and whose tracker it reads — keyed by that
device's own identity (serial). It never picks "the first calibration folder" on the machine:
one field box carried eleven. A plug-in that cannot resolve the active device's calibration
reports the camera without `CALIBRATED`, and the service offers RAW only.

**Rectification** (when not `NATIVELY_RECTIFIED`) is the service's, vendor-neutral
(`auxiliary/util/u_stereo_rectify.{h,c}`, R2): Bouguet rectification to a **parallel** pair
with zero disparity at infinity, cropped to the valid region (no black corners), per-eye size
unchanged; applied as a per-pixel remap LUT built once per (calibration, size). **No convergence
shear is baked in.** Placing the subject at the display plane is the receiver's decision
(a per-eye crop offset of `f_px · baseline / (2 · subjectZ)`), so the source stays a physically
honest parallel pair.

> **R2 as built.** The geometry is OpenCV's `stereoRectify(CALIB_ZERO_DISPARITY, alpha = 0)`
> re-implemented in C (no OpenCV dependency), golden-tested against OpenCV 4.10 fixtures
> (`tests/fixtures/`, generated offline): R1/R2 agree to 1e-12, the principal point matches the
> converged reference to 1e-4 px (OpenCV itself stops `undistortPoints` after 5 iterations and
> lands a fraction of a pixel off), the maps agree with `initUndistortRectifyMap` to 3e-5 px. Two
> deliberate differences: the alpha = 0 zoom is **verified on every border pixel** of the real
> maps and nudged up until no output pixel samples outside its raw image (OpenCV's 9-point
> inner rectangle leaves a few black pixels on strong lenses) — at most +0.13 % focal on the
> fixtures; and calibration given at a different size than the frames (`image_width/height` ≠ the
> eye size) is rescaled pixel-centre-aware. Lens models: RADTAN5, RADTAN8 (rational), KB4
> (fisheye; same Bouguet geometry). The service rectifies **once per source frame** on the camera
> thread (outside its lock) and only while some started stream wants RECTIFIED; RAW streams read
> the original. `xrGetStereoCameraCalibrationDXR(RECTIFIED)` returns that same geometry: both
> eyes `fx = fy = f` with one principal point, model NONE, and `rightFromLeft` = identity
> rotation + `(+baseline, 0, 0)` m — so frames and numbers cannot disagree. The browser
> (`PRESENT_OWNER`) is refused a stream on a calibrated camera the rectifier rejected, instead of
> getting the RAW-flagged fallback native clients get. CPU cost per 1280×480 frame (M1 Pro, -O2):
> **0.94 ms GRAY8, 1.36 ms NV12, 2.25 ms BGRA8**; setup (geometry + both LUTs) 9–18 ms, once.

**Eye order and mirroring.** Left half = the lens on the camera's own left (as seen from behind
the camera, looking at the scene) = the remote viewer's left eye. Frames are **never mirrored**;
a self-view mirror is the consumer's job (mirror each half AND swap halves).

## 5. Streams and frames

### 5.1 Create, start, stop

```c
typedef enum XrStereoCameraOutputDXR { XR_STEREO_CAMERA_OUTPUT_RECTIFIED_DXR = 1, XR_STEREO_CAMERA_OUTPUT_RAW_DXR = 2 } XrStereoCameraOutputDXR;
typedef enum XrStereoCameraFormatDXR { XR_STEREO_CAMERA_FORMAT_GRAY8_DXR = 1, XR_STEREO_CAMERA_FORMAT_NV12_DXR = 2, XR_STEREO_CAMERA_FORMAT_BGRA8_DXR = 3 } XrStereoCameraFormatDXR;
typedef enum XrStereoCameraTransportDXR {
    XR_STEREO_CAMERA_TRANSPORT_SHARED_MEMORY_DXR = 1,   // required; every platform
    XR_STEREO_CAMERA_TRANSPORT_D3D11_TEXTURE_DXR = 2,   // optional (Windows)
    XR_STEREO_CAMERA_TRANSPORT_AHARDWAREBUFFER_DXR = 3, // optional (Android)
} XrStereoCameraTransportDXR;

typedef struct XrStereoCameraStreamCreateInfoDXR {
    XrStructureType type; const void* XR_MAY_ALIAS next;
    uint64_t cameraId;
    XrStereoCameraOutputDXR output;         // RECTIFIED needs CALIBRATED or NATIVELY_RECTIFIED
    XrStereoCameraFormatDXR format;         // a MONOCHROME source in NV12 = luma + neutral chroma
    XrStereoCameraTransportDXR transport;
    float maxFrameRate;                     // ≤ 0 = the source's rate; the service decimates, never interpolates
} XrStereoCameraStreamCreateInfoDXR;

XrResult xrCreateStereoCameraStreamDXR(XrInstance, const XrStereoCameraStreamCreateInfoDXR*, XrStereoCameraStreamDXR*);
XrResult xrStartStereoCameraStreamDXR(XrStereoCameraStreamDXR stream);
XrResult xrStopStereoCameraStreamDXR(XrStereoCameraStreamDXR stream);
XrResult xrDestroyStereoCameraStreamDXR(XrStereoCameraStreamDXR stream);
```

Create allocates; **start is the authorisation point** (§7) and the point the service opens the
source if no other stream holds it open. Stop releases the source when the last started stream
stops (after a short linger, default 2 s, so a start/stop flurry during a call's device switch
does not bounce the plug-in). The output layout is always **one side-by-side image**,
`2·eyeWidth × eyeHeight`, left eye left — the shape every stereo consumer (WebRTC SBS, the weave
batch layout) already takes.

### 5.2 Acquire

```c
typedef struct XrStereoCameraFrameDXR {
    XrStructureType type; void* XR_MAY_ALIAS next;
    uint64_t     frameIndex;        // source frame counter, monotonic from 1; gaps = frames this stream missed
    uint32_t     slot;              // which ring slot holds it; PINNED until this stream's next acquire
    XrExtent2Di  extent;            // full SBS extent
    XrStereoCameraFormatDXR format;
    uint32_t     rowPitch[2];       // plane pitches (NV12: Y, UV)
    uint64_t     planeOffset[2];    // byte offsets of the planes inside the slot
    XrTime       captureTime;       // see timeDomain
    XrBool32     captureTimeIsExposure; // TRUE = sensor/exposure time; FALSE = arrival time at the plug-in
    XrStereoCameraOutputDXR output; // what this frame is (RAW when rectification is unavailable)
    uint32_t     calibrationGeneration; // bumps when calibration/rectification changed; re-query §4
} XrStereoCameraFrameDXR;

XrResult xrAcquireStereoCameraFrameDXR(XrStereoCameraStreamDXR stream, XrStereoCameraFrameDXR* frame);
```

Returns the newest frame **newer than the last one this stream acquired**, or the success code
`XR_STEREO_CAMERA_FRAME_NOT_READY_DXR`. Latest-wins: frames the consumer did not acquire in time
are skipped, never queued (the ring never builds a backlog; `frameIndex` gaps say how many).

### 5.3 Transport

**Shared memory (required).** On the first `xrStartStereoCameraStreamDXR` the stream exposes,
through `XrStereoCameraStreamTransportDXR` (chained on a `xrGetStereoCameraStreamInfoDXR` call):

- a read-only section (Windows) / sealed memfd (Android, POSIX) holding a 3-slot ring sized for
  the stream's extent+format, mapped by the consumer once;
- a **per-stream wake handle** (Windows auto-reset event duplicated into the consumer / an
  eventfd), signalled once per published frame. Per-stream on purpose: a wake object shared by
  several consumers lets one consumer's wait steal another's wake-up (§6).

```c
typedef struct XrStereoCameraStreamTransportDXR {   // chained on XrStereoCameraStreamInfoDXR
    XrStructureType type; void* XR_MAY_ALIAS next;
    uint64_t sectionHandle;     // HANDLE (Windows) / fd (POSIX, Android), duplicated into the caller; caller closes
    uint64_t sectionSize;
    uint32_t slotCount;         // 3
    uint64_t slotStride;        // bytes between slots
    uint64_t wakeHandle;        // HANDLE / eventfd, per stream
} XrStereoCameraStreamTransportDXR;

XrResult xrGetStereoCameraStreamInfoDXR(XrStereoCameraStreamDXR stream, XrStereoCameraStreamInfoDXR* info);
XrResult xrGetStereoCameraStreamStatsDXR(XrStereoCameraStreamDXR stream, XrStereoCameraStreamStatsDXR* stats);
```

(`XrStereoCameraFormatFlagsDXR` / `XrStereoCameraTransportFlagsDXR` are `XrFlags64` with one bit
per enum value above.)

The service writes only into a slot that is neither the latest nor pinned by any stream; a slot
stays byte-stable while pinned. A 1280×480 NV12 frame is 0.9 MB — 27 MB/s at 30 Hz — so the CPU
path is the simple, universal, low-integrity-friendly default (a section can carry a Low-IL
label; a GPU share cannot be imported by every sandbox).

**GPU transports (optional, per camera).** `D3D11_TEXTURE` follows the `XR_DXR_weave` output
contract (NT shared handle or legacy DXGI handle for Low-IL callers, a shared fence, handles
handed out on first acquire and on reallocation, NULL otherwise) over the same 3-slot pinning
rule. `AHARDWAREBUFFER` hands out the ring's AHBs once over the IPC socket. Worth it only for
large colour sources (a 2×1280×720 tablet pair); v1 implementations may omit both.

### 5.4 Timestamps

`captureTime` is on the runtime clock (`XrTime`; on Windows the QPC domain, so a consumer maps
it to its own monotonic clock with `xrConvertTimeToWin32PerformanceCounterKHR` or the Android /
POSIX `timespec` equivalent). A source that carries no exposure timestamp (the first real one
does not) is stamped at **arrival in the plug-in**, and says so with
`captureTimeIsExposure = XR_FALSE`. `xrGetStereoCameraStreamStatsDXR` reports source rate,
delivered rate, drops, and plug-in → publish latency — the same shape as lift stream stats.

## 6. Coexistence with eye tracking

When `SHARED_WITH_EYE_TRACKING` is set:

- **The tracker keeps the device; the camera stream is a passenger.** The plug-in reads the
  frames the tracker already publishes. It never opens, reconfigures, or re-times the device.
  Frame size, rate and exposure are whatever tracking needs; a consumer asking for more gets
  `maxFrameRate`-decimated tracker frames, not a new mode.
- **Frames exist only while tracking runs.** A started stream on an idle tracker asks the
  plug-in to hold its tracker up (the same keep-alive a weaving session gives it); the camera
  reports `WAITING` until the first frame, and `SUSPENDED` whenever tracking stops (lens off,
  tracker restart, user disabled tracking). Consumers show "camera paused", they do not fail.
- **Never a destructive consumer of a shared channel.** If the vendor channel has a single
  auto-reset wake object shared by every reader, the plug-in must not wait on it (each wake
  would be stolen from another reader, e.g. the vendor's own apps); it polls the channel at
  ~2× the source rate and detects new frames by content/sequence, and the service fans out with
  its own per-stream wake handles (§5.3).
- **Eye tracking has priority.** A plug-in that must choose (CPU budget, bus bandwidth) sheds
  camera delivery first; the weave never waits on the camera.
- **Tracking is not a camera-privacy exemption.** The eye tracker's own use of the camera is the
  vendor's business; every *frame leaving the tracker for a client* goes through §7.

## 7. Privacy

> **R3 status: as built** (`src/xrt/ipc/server/ipc_server_stereo_camera.c`, policy in
> `src/xrt/auxiliary/util/u_camera_consent.{h,c}` + the platform store
> `u_camera_consent_store.c`, UI in `targets/service/service_tray_win.c` and
> `ipc/server/ipc_server_macos_appkit.m`). Developer guide:
> [`docs/guides/stereo-camera-consent.md`](../../guides/stereo-camera-consent.md). The vendor
> plug-in never sees any of this.

The frames bypass the OS camera stack (the tracker, not the consumer, opened the device), so OS
camera permissions and in-use indicators do not see this consumer. The runtime therefore enforces
the equivalent itself. All checks are made on the **OS-derived peer** of the connection
(`GetNamedPipeClientProcessId` / `SO_PEERCRED`, with the brokered-connection declaration rules of
[service-architecture §4.1a](../../architecture/service-architecture.md#41a-declared-peer-identity-for-a-brokered-connection-browser103-rc-1)),
never on client-asserted fields.

### 7.1 Authorisation (at `xrStartStereoCameraStreamDXR` and `xrGetStereoCameraCalibrationDXR`)

The decision tree, first hit wins, evaluated with the manager lock released and one evaluation
at a time (so two apps never race two prompts):

| # | Check | Outcome |
|---|---|---|
| 1 | Sharing off: `DXR_STEREO_CAMERA=0` in the service environment, or the user's **"Share the 3D camera with apps"** toggle (tray / menu bar, persisted in the store) | `XR_ERROR_STEREO_CAMERA_DISABLED_DXR` (and zero cameras enumerated) |
| 2 | `DXR_STEREO_CAMERA_DEV_ALLOW=1` in the service environment — development override, one WARN per service run; **never** on a user's machine | allowed |
| 3 | The peer executable could not be verified (`""`) | `XR_ERROR_STEREO_CAMERA_CONSENT_REFUSED_DXR` |
| 4 | The executable is a registered **consent-delegating** client (a browser that shows its own per-origin prompt + in-use indicator): machine list written by its installer (`HKLM\Software\DisplayXR\CameraConsent\Delegating`, `/etc/displayxr/camera-delegating.json`, `/Library/Application Support/DisplayXR/camera-delegating.json`) or the user list (`displayxr-cli camera trust <exe>`) | allowed, no prompt, no store entry; never RAW (§7.5) |
| 5 | The OS camera privacy switch denies this executable (Windows `CapabilityAccessManager\ConsentStore\webcam`: machine policy, user global, desktop-app class, per-app entry; no equivalent is readable on macOS / Linux) | `XR_ERROR_PERMISSION_INSUFFICIENT` |
| 6 | The stored per-app decision (`HKCU\Software\DisplayXR\CameraConsent\Apps\<path>`, `camera_consent.json` `apps`) | Allow → allowed; Deny → `CONSENT_REFUSED` |
| 7 | "Allow once" already granted to this same process (executable + pid) | allowed |
| 8 | The **tray prompt**: *"<app> wants to use the 3D camera — Allow / Allow once / Deny"* (Windows: a top-most dialog from the tray thread; macOS: a floating panel from the menu-bar item — never a modal that would park the service's run loop). **Allow** and **Deny** are written to the store; **Allow once** is remembered for the process. The call blocks for up to 60 s; unanswered / dismissed, or no prompt available (`DXR_STEREO_CAMERA_PROMPT=0`, headless Linux service) | `CONSENT_REFUSED` — retryable, asked again next time |

A refused stream stays created and may be started again (e.g. after `displayxr-cli camera allow`).
The policy is pure (two injected vtables) and unit-tested with fakes in `tests_camera_consent`.

**Android** (A1, not built): CAMERA permission of the peer uid + the runtime app's consent
activity per package; `android_package_is_visible` for §7.2.

### 7.2 Foreground rule

Frames are published to a stream only while its client is **visible**, checked per published
frame with a 250 ms cache:

- **Window-bearing classes (APP, PRESENT_OWNER):** the verified peer pid must own a visible,
  non-minimised top-level window (Windows `EnumWindows`; macOS `NSRunningApplication` not hidden
  and not activation-prohibited; desktop Linux has no display connection in the service and is
  always visible — the OS lock below is the gate there). Otherwise the stream is `SUSPENDED` for
  that client: no new frames, pinned slot cleared so the last image does not linger.
- **Delegating clients** follow their own visibility rule (a capturing tab keeps its indicator;
  closing it stops the track). **`CAMERA_CONSUMER` and `DIAG`** have no window by contract and
  are exempt — they were granted explicit consent instead.
- **Session lock:** every stream is suspended while the OS session is locked or switched away
  (Windows `WM_WTSSESSION_CHANGE` lock / console or remote disconnect, plus `SM_REMOTESESSION` at
  start; macOS `com.apple.screenIsLocked` and `NSWorkspaceSessionDidResignActive`), and resumes on
  unlock. Enumerated and event-reported state is `SUSPENDED` meanwhile (§7.6).

### 7.3 Indicator

While any stream is started, the service shows an in-use indicator: Windows tray icon with a red
dot, tooltip and menu line *"3D camera in use by <app>"* (executable base names), one balloon on
the transition; macOS menu-bar icon with a red dot, tooltip and menu line. The indicator is owned
by the runtime because the camera LED, if any, is lit by tracking and says nothing about who is
receiving frames.

### 7.4 Kill switches

- **User, immediate:** tray / menu-bar **"Stop camera sharing"** ends every started stream now
  (each gets `XrEventDataStereoCameraStreamEndedDXR` with `USER_STOPPED`; acquire and start on it
  return `XR_ERROR_STEREO_CAMERA_STREAM_ENDED_DXR`; "Allow once" grants are forgotten). The app
  may create and start a new stream, which goes through §7.1 again.
- **User, persistent:** **"Share the 3D camera with apps"** (checkmark) — off ⇒ zero cameras
  enumerated, every stream ended with `DISABLED`, start refused with
  `XR_ERROR_STEREO_CAMERA_DISABLED_DXR`. Stored with the consent.
- **Admin / dev:** `DXR_STEREO_CAMERA=0` in the service environment (zero cameras).
- `displayxr-cli camera stop-all | sharing on|off` drive the same two switches over IPC.

### 7.5 Fingerprinting

Calibration and serials identify a device uniquely. `persistentId` is
**HMAC-SHA-256(secret, device identity ‖ consumer executable)** rendered as `dxrcam-` + 32 hex
chars, where the secret is 32 random bytes generated once per user and kept in the consent
store (`Secret` / `"secret"`). The same device gives every executable a different id, the same
executable a different id on every install, and nothing about the serial is derivable from it.
Calibration is returned only to a client that has passed §7.1 for that camera; RAW output is
refused to `PRESENT_OWNER` and to every delegating client (both expose cameras to third-party
content). A browser must still coarsen what it gives pages (roadmap §B.3).

### 7.6 Events

The service queues, per connection (16 deep, oldest dropped), and the client drains from
`xrPollEvent` (no session needed):

- `XrEventDataStereoCameraStateChangedDXR` — the camera's **effective** state to every connection
  with a stream on it: the source state, or `SUSPENDED` while the session is locked / sharing is
  off.
- `XrEventDataStereoCameraStreamEndedDXR` — a stream the service ended (§7.4), with the handle
  and the reason.
- `XrEventDataStereoCamerasChangedDXR` — reserved (the camera set is fixed for a service run).

## 8. Errors

| Result | When |
|---|---|
| `XR_STEREO_CAMERA_FRAME_NOT_READY_DXR` (success) | no frame newer than the last acquired |
| `XR_ERROR_VALIDATION_FAILURE` | struct out of contract (unknown camera, format/transport not in the supported bits, RECTIFIED on an uncalibrated camera) |
| `XR_ERROR_PERMISSION_INSUFFICIENT` | the OS camera privacy switch denies the app (§7.1 step 5); RAW asked by a browser / delegating client; a client class that may not use cameras (or `xrCreateSession` from a `CAMERA_CONSUMER`) |
| `XR_ERROR_STEREO_CAMERA_CONSENT_REFUSED_DXR` | §7.1: the user / the stored decision refused, the prompt was unanswered or unavailable; retryable after consent. A browser maps it to `NotAllowedError` |
| `XR_ERROR_STEREO_CAMERA_DISABLED_DXR` | §7.4: sharing is off (user toggle or `DXR_STEREO_CAMERA=0`) |
| `XR_ERROR_STEREO_CAMERA_BUSY_DXR` | the plug-in could not open the source and its retry is pending; retry later. A browser maps it to `NotReadableError` |
| `XR_ERROR_STEREO_CAMERA_STREAM_ENDED_DXR` | start / acquire on a stream the service ended (§7.4); destroy it and create a new one |
| `XR_ERROR_FEATURE_UNSUPPORTED` | GPU transport requested on a platform without it |
| `XR_ERROR_LIMIT_REACHED` | more than 8 streams per camera service-wide |
| `XR_ERROR_RUNTIME_FAILURE` | transient service refusal on a healthy pipe; retry |
| `XR_ERROR_INSTANCE_LOST` | IPC connection gone (weave §4b recovery) |

## 9. Plug-in contract (ADR-020 append-only)

Appended to `struct xrt_plugin_iface` (after `create_dp_d3d11_lift`, ADR-042), gated by
`struct_size`, announced by `#define XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA 1`, no ABI bump.
**As implemented:** the six slots are the last members of the iface, after `create_dp_d3d11_lift`
(ADR-042) and `get_platform_state` (ADR-045), both of which were on `main` first;
`tests_stereo_camera` pins the order. The authoritative
header is `src/xrt/include/xrt/xrt_plugin.h`; the plug-in-facing rules are in
[`docs/reference/xrt_plugin_iface.md`](../../reference/xrt_plugin_iface.md#a-stereo-camera-the-stereo_camera_-slots-adr-043).
Two refinements over the sketch below: `wait_frame` returns an `enum xrt_plugin_stereo_camera_wait`
(`OK` / `TIMEOUT` / `SUSPENDED` / `ERROR`) rather than an `xrt_result_t`, so no new
`xrt_result_t` values were needed; and the extrinsics are **OpenCV's `x_R = R·x_L + T`** (T in
mm, `T = (−B, 0, 0)` for a right camera at +x) — the service converts them to the client's
`rightFromLeft` pose (the right camera in the left camera's frame, metres). On the
**plug-in iface, not a display-processor vtable**: a camera is a sensor, not a weaver; it must be
graphics-API-neutral (Windows D3D11 service, Android Vulkan runtime) and must not share a lifetime
with a DP that is recreated on presenter changes.

```c
struct xrt_plugin_stereo_camera_info {
	uint32_t struct_size;
	char display_name[128];
	char device_identity[128];       // stable device key (serial); the SERVICE hashes it, never exports it
	char platform_device_hint[256];  // OS id of the physical device read, or ""
	uint32_t flags;                  // XRT_PLUGIN_STEREO_CAMERA_* (shared-with-tracking, user-facing, calibrated, rectified, mono)
	uint32_t eye_width, eye_height;
	float max_frame_rate;            // only if KNOWN (vendor value / measured earlier); 0 = unknown, never a guess
	uint32_t native_format;          // GRAY8 / NV12 / BGRA8 — the plug-in decodes its transport (e.g. JPEG) itself
};

struct xrt_plugin_stereo_camera_calibration {   // RAW calibration of the ACTIVE device
	uint32_t struct_size;
	uint32_t image_width, image_height;          // per eye
	double k[2][4];                              // fx fy cx cy
	uint32_t distortion_model;                   // NONE / RADTAN5 / RADTAN8 / KB4
	double distortion[2][8];
	double rotation_right_from_left[3][3];
	double translation_right_from_left_mm[3];
};

struct xrt_plugin_stereo_camera_frame {
	uint64_t sequence;               // monotonic per open
	int64_t  time_ns;                // os_monotonic domain
	bool     time_is_exposure;
	uint32_t width, height;          // full SBS
	uint32_t format;
	const uint8_t *planes[2];
	uint32_t pitches[2];             // valid until release_frame
};

// appended to struct xrt_plugin_iface:
uint32_t     (*stereo_camera_enumerate)(struct xrt_plugin_instance *inst, uint32_t cap,
                                        struct xrt_plugin_stereo_camera_info *out);
xrt_result_t (*stereo_camera_get_calibration)(struct xrt_plugin_instance *inst, uint32_t index,
                                              struct xrt_plugin_stereo_camera_calibration *out);
xrt_result_t (*stereo_camera_open)(struct xrt_plugin_instance *inst, uint32_t index,
                                   struct xrt_plugin_stereo_camera **out);   // also = tracker keep-alive
xrt_result_t (*stereo_camera_wait_frame)(struct xrt_plugin_stereo_camera *cam, int64_t timeout_ns,
                                         struct xrt_plugin_stereo_camera_frame *out); // TIMEOUT / SUSPENDED / OK
void         (*stereo_camera_release_frame)(struct xrt_plugin_stereo_camera *cam);
void         (*stereo_camera_close)(struct xrt_plugin_stereo_camera *cam);
```

Rules: `wait_frame` is called from **one runtime-owned camera thread per open camera** and may
block up to `timeout_ns`; it never throws (C boundary). Enumerate/calibration are cheap and
callable from any thread. `SUSPENDED` from `wait_frame` maps to the camera state (§3). The
plug-in owns decoding its transport and resolving calibration for its own device; the runtime
owns threads, fan-out, rectification, format conversion, transport, consent and indicator.

| Runtime owns | Plug-in owns |
|---|---|
| camera thread, open refcount, linger | reading frames from its stack without taking the device |
| rectification (`u_stereo_rectify`), format conversion, decimation | decoding (JPEG, YUY2 …) to GRAY8/NV12/BGRA8 |
| rings, wake handles, pinning, GPU transports | frame sequence + best timestamp it has |
| authorisation, foreground rule, indicator, kill switch | calibration of **its active device**, by device identity |
| events, stats, CLI | tracker keep-alive while open; shedding camera before tracking |

## 10. sim_display fake

> **R1 as built** (`drivers/sim_display/sim_display_stereo_camera{,_pattern}.c`): the pair is
> **undistorted and aligned** and the camera reports `NATIVELY_RECTIFIED` (+ `CALIBRATED`,
> `SHARED_WITH_EYE_TRACKING`, `USER_FACING`, and `MONOCHROME` for gray8), so the RECTIFIED output
> path runs end to end today. The scene is a random-dot stereogram: background at 2.0 m and a bar
> at 0.6 m, which at the defaults (640 px per eye, 68° HFOV → fx 474.4 px, 50 mm) are **12 px and
> 40 px** of disparity, plus an 8-digit frame counter in both halves. Knobs, read by the SERVICE:
> `SIM_DISPLAY_FAKE_STEREO_CAMERA=1`, `_SIZE=WxH` (per eye), `_FPS=N`, `_FORMAT=gray8|nv12|bgra`,
> `_SUSPEND_PERIOD_MS=N`, `_ADVERTISED_FPS=N` (what enumerate claims, default = `_FPS`; `0` = unknown —
> exercises the service's measured-rate path), `_BASELINE_MM=N` (default 50; `120` reproduces the Leia SR
> tracking camera: the bar at 0.6 m is ~95 px).
>
> **R2 as built:** `SIM_DISPLAY_FAKE_STEREO_CAMERA_DISTORT=1` renders the same scene through two
> RAW cameras with known ground truth (`sim_stereo_camera_truth_init`): different per-eye
> intrinsics and principal points, RADTAN5 barrel lenses (k1 −0.21 / −0.17), and each camera
> turned by half of (pitch 0.35°, yaw 0.25°, roll 0.45°) in opposite senses — 0.7° of relative
> pitch, 0.9° of relative roll. The raw pair measures |Δy| median 2.6 px, p90 5.3 px. The
> rotation split is symmetric (left `Sᵀ`, right `S`), which makes Bouguet recover **exactly** the
> virtual parallel pair the scene is defined in, so the ground truth after rectification is
> exact: rows aligned and disparity `f_rect · B / Z`. The camera then drops `NATIVELY_RECTIFIED`
> and its calibration slot returns the truth (RADTAN5, `R = S²`, `T = −S·(B, 0, 0)`, B = `_BASELINE_MM`).

`SIM_DISPLAY_FAKE_STEREO_CAMERA=1` makes sim_display advertise one camera
(`SHARED_WITH_EYE_TRACKING | USER_FACING | CALIBRATED | MONOCHROME`, 640×480 per eye, 30 Hz,
baseline 50 mm) producing a **synthetic SBS pattern**: a textured plane and a bar at known depths,
rendered with a known synthetic calibration that includes radial distortion and a small vertical
misalignment + roll between the eyes, plus a frame counter burned into each half. So:

- the descriptor's rate is testable: with `_ADVERTISED_FPS=0` the camera lists "rate unknown" until a
  stream has run ~60 frames, then reports the measured rate (field finding: the Leia SR tracking
  camera was hard-coded to advertise 30 Hz and actually ran 58–64 Hz);
- rectification is testable exactly — after `RECTIFIED`, feature rows must align (|Δy| < 0.5 px)
  and the bar's disparity must equal `fx·B/Z`;
- `SIM_DISPLAY_FAKE_STEREO_CAMERA_SUSPEND_PERIOD_MS=N` square-waves SUSPENDED/AVAILABLE, like the
  fake-tracking knob, to exercise consumers' pause handling;
- `SIM_DISPLAY_FAKE_STEREO_CAMERA_FORMAT=nv12|bgra` switches the native format.

The whole path (enumerate → consent → start → acquire → rectify) then runs in CI without hardware.

## 11. Probe and diagnostics

```
displayxr-cli camera list [--json]                    # properties + state of every camera
displayxr-cli camera calib <id> [--raw|--rectified] [--json]
displayxr-cli camera probe [<id>] [--raw] [--format gray8|nv12|bgra8] [--fps F]
                              [--frames N] [--seconds S] [--out DIR]
                                                      # rate, index gaps, SBS layout, block
                                                      # disparities, service stats; --out writes cam_<n>.png
```

As built in R1, e.g. against the sim fake on macOS:

```
stream 1 on camera 1: 1280x480 NV12 SBS (RECTIFIED), cap 30.0 Hz, ring 3 x 921600 B (section 2764800 B)
received 150 frames (index 1..150, 0 source frames not delivered to this stream), 150 wakes, 0 not-ready
measured delivery rate: 29.95 Hz over 4.98 s
block disparity (left x - right x): 12 px (38/54 blocks), 40 px (16/54 blocks), range 12..40 px
service stats: source 30.49 Hz, delivered 30.49 Hz, published 155, skipped 4, acquired 150, mean latency 12.417 ms
```

R2 adds row alignment to every probe (2-D block matching of the last frame, textured 32×32
blocks: SIGNED Δy median, |Δy| median / p90 / count above 1 px, and the dominant disparities
converted to depth with the RECTIFIED calibration), `camera calib <id> --rectified` prints P1/P2
and `Z = f·B/d`, and `probe --rectified` insists on RECTIFIED output and exits 5 unless the median
|Δy| ≤ 0.5 px **and** the signed median is within ±0.5 px (a constant vertical offset fails even
when the scatter is small).

**Matching (after the first SR-hardware run).** Zero-mean NCC with parabolic sub-pixel refinement
(coarse pass on every second pixel, full-resolution refinement), blind to the two sensors'
gain/offset differences. The disparity window is **baseline-aware**: `f·B / 0.4 m` from the
camera's own calibration (quarter eye width uncalibrated, clamped to [64, 0.6 · eye]); the
vertical window is ±24 px. Blocks with peak NCC < 0.90, or whose peak sits **on** either
window's bound, are counted and excluded — a bound hit is a clamp, not a measurement. (The first
SR run read "d = 64, |dy| = 10.000" because a face at 0.6 m on the 120 mm tracker is ~95 px and
both fixed windows clamped.) Knobs: `--max-disparity N`, `--max-dy N`, `--min-ncc C`.
Against the DISTORTED fake (macOS, Debug service), default 50 mm and the SR-like 120 mm:

```
$ displayxr-cli camera calib 1 --rectified
  left  eye: 640x480 fx 453.181 fy 453.181 cx 320.426 cy 239.364 model 0
  P2 = [453.181 0 320.426 -22659.030; 0 453.181 239.364 0; 0 0 1 0]   (P2[0][3] = f * Tx, Tx = -50.000 mm)
$ displayxr-cli camera probe --rectified --seconds 2
row alignment (RECTIFIED): NCC 32x32 blocks, search d -8..64 px, dy +-24 px, accept NCC >= 0.90
  266 textured block(s): 255 accepted, 11 below NCC 0.90, 0 AT the disparity bound, 0 AT the dy bound
  dy (right - left) median +0.001 px SIGNED; |dy| median 0.016 px, p90 0.038 px, 1 block(s) > 1 px
  disparity mode 1: 11.30 px (213 blocks) -> Z = f*B/d = 2.005 m
  disparity mode 2: 37.81 px (40 blocks) -> Z = f*B/d = 0.599 m
--rectified: PASS — rows aligned
$ displayxr-cli camera probe --raw --seconds 2
  dy (right - left) median +0.019 px SIGNED; |dy| median 2.015 px, p90 4.026 px, 100 block(s) > 1 px
# service with SIM_DISPLAY_FAKE_STEREO_CAMERA_BASELINE_MM=120:
$ displayxr-cli camera probe --rectified --seconds 2
row alignment (RECTIFIED): NCC 32x32 blocks, search d -8..136 px, dy +-24 px, accept NCC >= 0.90
  266 textured block(s): 233 accepted, 33 below NCC 0.90, 0 AT the disparity bound, 0 AT the dy bound
  dy (right - left) median +0.000 px SIGNED; |dy| median 0.011 px, p90 0.031 px, 1 block(s) > 1 px
  disparity mode 1: 27.17 px (191 blocks) -> Z = f*B/d = 2.002 m
  disparity mode 2: 90.68 px (40 blocks) -> Z = f*B/d = 0.600 m
--rectified: PASS — rows aligned
```

(The blocks above 1 px on the rectified pair are mismatches at the bar's occlusion edges and on
the frame counter, which is burned in raw space.)

**Open: a +1.7 px signed Δy on real SR data after rectification.** The `[vshift]` tests in
`tests_stereo_rectify` pin what each candidate cause looks like on the 120 mm distorted fake, so
the raw frames + calibration dumps can be matched to one: a per-eye principal-point error Δcy is
a *constant* Δy ≈ −(f_rect/fy)·Δcy with small spread (flipped row convention, a crop taken for a
scale, a one-eye ±0.5 px centre convention); a transposed R is a large, roll-shaped Δy (spread
> 1 px), so a clean constant offset is *not* that; the calibration-size rescale and the plug-in's
half swap (`R' = Rᵀ`, `T' = −RᵀT`) are exact; an identity calibration maps as a pure scale about
(cx, cy) — no half-pixel offset.

Runs as a DIAG IPC client, non-elevated. `probe` goes through the full consent path (so its
first run on a box raises the prompt — which is how a developer checks the prompt). `selftest`
gains `stereo_camera_caps`: zero cameras passes; malformed properties (zero extent, CALIBRATED
without a calibration, baseline ≤ 0 on a calibrated camera) fail. Service logs: one WARN per
camera state change and per stream start/stop with the peer executable; INFO stats every 5 s.

## 12. Consumers

| Consumer | Path | Uses |
|---|---|---|
| DisplayXR Browser (Windows, Android) | capture component → service, consent-delegating | exposes each camera to pages as an ordinary `getUserMedia` video device + one capability hint — [roadmap §B](../../roadmap/stereo-camera-source.md) |
| Native call / capture apps | app → service | enumerate, calibration, RECTIFIED NV12 over shared memory |
| `displayxr-cli camera` | DIAG IPC | §11 |

## 13. Version history

| Version | Change |
|---|---|
| 2 (R3) | Consent and privacy as built (§7): `XrStereoCameraClientInfoDXR` + `XR_STEREO_CAMERA_CLIENT_CONSUMER_ONLY_BIT_DXR` (the `CAMERA_CONSUMER` class, §2a); distinct results `XR_ERROR_STEREO_CAMERA_CONSENT_REFUSED_DXR` / `_DISABLED_DXR` / `_BUSY_DXR` / `_STREAM_ENDED_DXR`; `XrEventDataStereoCameraStreamEndedDXR`; the state-changed event is now delivered; `persistentId` keyed with a per-user secret; read-only consumer section on every platform; `DXR_STEREO_CAMERA_DEV_ALLOW` becomes a documented dev override, `DXR_STEREO_CAMERA_PROMPT=0` disables the prompt. Everything appended at `1004999311–316`; nothing renumbered |
| 1 (R2) | No API change. RECTIFIED output and rectified calibration now come from the service's rectifier for any CALIBRATED, non-natively-rectified source; the browser is refused a camera it cannot rectify; sim_display `_DISTORT=1` fake; `camera probe` row alignment + `--rectified` |
| 1 | R1 implements: enumerate, calibration (RAW; RECTIFIED for natively rectified sources), streams with start/stop, latest-wins acquire over a pinned 3-slot shared-memory ring with per-stream wake handles, stream stats, plug-in slots, sim_display fake, CLI, selftest. Deferred: state-change events (structs defined, not delivered), GPU transports, rectifier (R2), consent/indicator (R3). Design scope: Enumerate + state events, calibration (raw / rectified), streams with start/stop, latest-wins acquire over a pinned 3-slot shared-memory ring with per-stream wake handles, optional GPU transports, runtime-enforced consent / foreground / indicator, plug-in iface slots, sim_display fake, CLI. |

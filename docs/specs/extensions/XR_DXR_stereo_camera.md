# XR_DXR_stereo_camera — Stereo Camera Source

| Field | Value |
|---|---|
| **Extension Name** | `XR_DXR_stereo_camera` |
| **Spec Version** | 1 (design) |
| **Extension Type** | Instance extension, service path only (an in-process instance enumerates zero cameras) |
| **Header** | `src/external/openxr_includes/openxr/XR_DXR_stereo_camera.h` — **not written yet.** It lands with the first implementation PR, together with its `docs/specs/extensions/index.json` note (the catalog lint joins notes to headers, so neither can land alone) |
| **Status** | **Design** — not implemented. Provisional type-value block proposed: `1004999290–299` (after `XR_DXR_lift`'s `1004999270–280`), pending Khronos registry |
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
    float                      maxFrameRate;      // what the source can deliver, Hz
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
(`u_stereo_rectify`, planned in `auxiliary/util`): Bouguet rectification to a **parallel** pair
with zero disparity at infinity, cropped to the valid region (no black corners), per-eye size
unchanged; applied as a per-pixel remap LUT built once per (calibration, size). **No convergence
shear is baked in.** Placing the subject at the display plane is the receiver's decision
(a per-eye crop offset of `f_px · baseline / (2 · subjectZ)`), so the source stays a physically
honest parallel pair.

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

The frames bypass the OS camera stack (the tracker, not the consumer, opened the device), so OS
camera permissions and in-use indicators do not see this consumer. The runtime therefore enforces
the equivalent itself. All checks are made on the **OS-derived peer** of the connection
(`GetNamedPipeClientProcessId` / `SO_PEERCRED`, with the brokered-connection declaration rules of
[service-architecture §4.1a](../../architecture/service-architecture.md#41a-declared-peer-identity-for-a-brokered-connection-browser103-rc-1)),
never on client-asserted fields.

### 7.1 Authorisation (at `xrStartStereoCameraStreamDXR`)

| Check | Windows | Android |
|---|---|---|
| OS camera privacy | the peer executable's webcam consent in `CapabilityAccessManager\ConsentStore\webcam` (global + per-app desktop entry) must not be Deny | the peer uid must hold `android.permission.CAMERA` (granted, not just declared) |
| DisplayXR consent | the peer executable (path + Authenticode signer) is in the user's allow list (`HKCU\Software\DisplayXR\StereoCamera\Consent`), else the service raises a first-use prompt from its tray ("*App* wants to use the 3D camera — Allow / Deny"); a Deny is remembered | the runtime app's consent activity, first use per package |
| Consent-delegating clients | a registered browser that shows its own per-origin permission prompt and in-use indicator (roadmap §B) is marked **delegating** at install time; the runtime's check is then per executable, the browser's per origin | same, per package |

Failure is `XR_ERROR_PERMISSION_INSUFFICIENT`; the stream stays created and may be started again
(e.g. after the user allows it).

### 7.2 Foreground rule

Frames are published to a stream only while its client is **visible**: for an OpenXR app, a
session in `VISIBLE` or `FOCUSED`; on Android, the peer package visible to the user (the plug-in
host's `android_package_is_visible`). Otherwise the stream is `SUSPENDED` for that client — it
receives no new frames, and its pinned slot is cleared on suspension so the last image does not
linger. A **delegating** client (the browser's capture component) has no session; its visibility
rule is the browser's own (a capturing tab keeps its indicator; closing it stops the track), and
the runtime additionally suspends every stream while the OS session is locked or switched away.

### 7.3 Indicator

While any stream is started, the service shows an in-use indicator (Windows tray badge + tooltip
naming the consumer executables; Android an ongoing notification from the runtime app), in
addition to whatever the consumer shows. The indicator is owned by the runtime because the camera
LED, if any, is lit by tracking and says nothing about who is receiving frames.

### 7.4 Kill switches

User: a tray / runtime-app toggle "Share the 3D camera with apps" (off ⇒ zero cameras
enumerated). Admin/dev: `DXR_STEREO_CAMERA=0` in the service environment.

### 7.5 Fingerprinting

Calibration and serials identify a device uniquely. `persistentId` is a keyed hash of (device
identity, peer executable), never the serial; calibration is returned only to a client that has
passed §7.1 for that camera. A browser must coarsen what it gives pages (roadmap §B.3).

## 8. Errors

| Result | When |
|---|---|
| `XR_STEREO_CAMERA_FRAME_NOT_READY_DXR` (success) | no frame newer than the last acquired |
| `XR_ERROR_VALIDATION_FAILURE` | struct out of contract (unknown camera, format/transport not in the supported bits, RECTIFIED on an uncalibrated camera) |
| `XR_ERROR_PERMISSION_INSUFFICIENT` | §7.1 refused; retryable after consent |
| `XR_ERROR_FEATURE_UNSUPPORTED` | GPU transport requested on a platform without it |
| `XR_ERROR_LIMIT_REACHED` | more than 8 streams per camera service-wide |
| `XR_ERROR_RUNTIME_FAILURE` | transient service refusal on a healthy pipe; retry |
| `XR_ERROR_INSTANCE_LOST` | IPC connection gone (weave §4b recovery) |

## 9. Plug-in contract (ADR-020 append-only)

Appended to `struct xrt_plugin_iface` (after `create_dp_d3d11_lift`, ADR-042), gated by
`struct_size`, announced by `#define XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA 1`, no ABI bump. On the
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
	float max_frame_rate;
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

`SIM_DISPLAY_FAKE_STEREO_CAMERA=1` makes sim_display advertise one camera
(`SHARED_WITH_EYE_TRACKING | USER_FACING | CALIBRATED | MONOCHROME`, 640×480 per eye, 30 Hz,
baseline 50 mm) producing a **synthetic SBS pattern**: a textured plane and a bar at known depths,
rendered with a known synthetic calibration that includes radial distortion and a small vertical
misalignment + roll between the eyes, plus a frame counter burned into each half. So:

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
displayxr-cli camera probe <id> [--raw] [--format gray8|nv12|bgra8] [--seconds S] [--out DIR]
                                                      # writes cam_<n>.png, prints rate, drops, latency
```

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
| 1 (design) | Enumerate + state events, calibration (raw / rectified), streams with start/stop, latest-wins acquire over a pinned 3-slot shared-memory ring with per-stream wake handles, optional GPU transports, runtime-enforced consent / foreground / indicator, plug-in iface slots, sim_display fake, CLI. |

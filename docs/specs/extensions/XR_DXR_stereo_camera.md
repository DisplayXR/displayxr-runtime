# XR_DXR_stereo_camera — Stereo Camera Source

| Field | Value |
|---|---|
| **Extension Name** | `XR_DXR_stereo_camera` |
| **Spec Version** | 8 |
| **Extension Type** | Instance extension, service path only (an in-process instance enumerates zero cameras) |
| **Header** | [`src/external/openxr_includes/openxr/XR_DXR_stereo_camera.h`](../../../src/external/openxr_includes/openxr/XR_DXR_stereo_camera.h) (+ its `index.json` catalog note) |
| **Status** | **R1 implemented** (runtime, hardware-free): header, plug-in slots, service camera manager, IPC, OpenXR entry points, sim_display fake, `displayxr-cli camera`, selftest check. **R2 implemented**: the service-side rectifier (`u_stereo_rectify`, CPU) — RECTIFIED frames + rectified calibration from any CALIBRATED source, golden-tested against OpenCV, and a distorted sim fake with ground truth. **Not yet:** consent / indicator / foreground rule (R3 — deny-by-default hooks in place), GPU transports (the rectifier has the seam), state-change events, the Leia provider (L1), the browser (B1). **Amendment 4 implemented** (2026-10-07): a vendor-neutral, service-owned source for plain UVC side-by-side stereo webcams (§9a) — Windows Media Foundation; desktop Linux V4L2 since 2026-10-18 (§9a; no SBS webcam has run on it yet). Provisional type values `1004999300–310` (relocated from `290–300` when `XR_DXR_weave` v14 took `290`; `XR_DXR_lift` holds `270–289`), pending Khronos registry |
| **R1 decisions** | stereo-only (names/structs kept open: `viewCount`, next chains) · camera provider = `xrt_plugin_iface` slots · service clients only (in-process enumerates zero) · raw frames never reach web pages (`RAW` refused to `PRESENT_OWNER` clients) — see [roadmap §G](../../roadmap/stereo-camera-source.md#g-open-questions--maintainer-decisions) |
| **Decision record** | [ADR-043](../../adr/ADR-043-stereo-camera-source.md) |
| **Spec v8** (2026-11) | **Implemented** (runtime, hardware-free): capture modes with service-side downscaling (§5.5), focus / metering / exposure controls with one holder per camera and the per-frame capture state (§5.6), stills over a read-only shared-memory section (§5.7), plug-in slots (§9b), sim_display fake, `displayxr-cli camera modes / set / still`. **Not yet:** the `AHardwareBuffer` still transport; the Leia provider's implementation of the §9b slots. Known limits: §5.8 |
| **Plug-in contract** | appended `xrt_plugin_iface` camera slots, `XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA` (§9); v8 slots `XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_MODES` / `_CONTROLS` / `_STILL` (§9b) |
| **Second source** | the service's own UVC side-by-side webcam source, opt-in per device (§9a, ADR-043 Amendment 4) |
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
3. **Rectified by default, using each camera's own device calibration.** The plug-in answers,
   per camera, for the device behind it; the service rectifies with a vendor-neutral routine. A consumer that
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
| Service with an opted-in UVC side-by-side webcam (`stereo-cameras.json`, §9a) | one more entry per claimed webcam, after the plug-in's; no plug-in needed |
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

**Declining delegation (spec v3).** A registered consent-delegating client (§7.1 row 6) that is
running with its own permission prompt bypassed — automation, a test harness, an auto-grant
switch such as a browser's `--use-fake-ui-for-media-stream` — sets
`XR_STEREO_CAMERA_CLIENT_DECLINE_DELEGATION_BIT_DXR` in the same `flags`. For that instance the
runtime skips row 6: the executable needs a stored decision or the runtime's own prompt, like any
other app (§7.1). The bit only ever restricts, so it needs no verification; it combines freely
with `CONSUMER_ONLY`. Unknown bits are ignored.

## 3. Enumerating cameras

```c
typedef XrFlags64 XrStereoCameraFlagsDXR;
// the camera is the eye tracker's; frames exist only while tracking runs (§6)
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_SHARED_WITH_EYE_TRACKING_BIT_DXR = 0x1;
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_USER_FACING_BIT_DXR             = 0x2;
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_CALIBRATED_BIT_DXR              = 0x4; // calibration available (does NOT by itself promise RECTIFIED; see RECTIFIABLE)
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_NATIVELY_RECTIFIED_BIT_DXR      = 0x8; // the source already delivers a rectified pair
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_MONOCHROME_BIT_DXR              = 0x10; // luma only (e.g. an IR / grey tracking sensor)
// v7: a RECTIFIED stream of this camera delivers rectified frames — the source is natively
// rectified, or the service built a rectifier for it. Set by the runtime only. A CALIBRATED
// camera without it is RAW-only (its geometry was rejected, e.g. a vertical baseline): a
// consumer that takes only rectified frames should not list it.
static const XrStereoCameraFlagsDXR XR_STEREO_CAMERA_RECTIFIABLE_BIT_DXR             = 0x20;

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
    float                      baselineMm;        // |T| of the pair; 0 if uncalibrated (a §9a UVC camera: the
                                                  // CONFIGURED nominal value, else 0)
    float                      horizontalFovDeg;  // per eye, rectified; 0 if uncalibrated (§9a: as baselineMm)
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

**The list is live (spec v4).** The service re-queries its camera sources (the plug-in, §9, and
the UVC source, §9a) during its run: on a client's `xrEnumerateStereoCamerasDXR` (at most once a
second) and every ~2 s while any instance that has enumerated is connected. The result is merged
so that nothing a client holds moves:

- a camera is matched by its source + device identity (what `persistentId` is derived from), so
  an unchanged camera keeps its **index, `cameraId` and `persistentId`**;
- a camera that appears later (an SR tracker that starts after the service, a display plugged
  in) is **appended** with a new `cameraId`;
- a camera its source no longer lists **stays in the list, in place, as `UNAVAILABLE`**: no
  other camera is renumbered, its streams end (`XrEventDataStereoCameraStreamEndedDXR`, reason
  `SOURCE_LOST`), a new start on it fails with `XR_ERROR_STEREO_CAMERA_BUSY_DXR`, and if it is
  listed again it comes back with the same `cameraId`;
- `displayName` / `platformDeviceHint` follow the source (a plug-in that learns which screen a
  camera belongs to renames it), and so does its display (spec v5, below). Geometry, format and
  flags are fixed at first enumeration for the service's run.

Any of these changes — and the sharing toggle switching the enumerated set to zero or back —
queues one `XrEventDataStereoCamerasChangedDXR` to every instance that has enumerated (or
created a stream) and is still connected. A client re-enumerates on it; a browser fires
`devicechange`. A client that cannot poll events can still call enumerate periodically: the
throttle makes that cheap, and the ids make a diff trivial. A display topology change (a screen
plugged in or removed, or the active plug-in changing) wakes the background re-enumeration at
once instead of at its next ~2 s tick.

**Which display a camera belongs to (spec v5).** Chain `XrStereoCameraDisplayDXR` on a
camera's `XrStereoCameraPropertiesDXR::next` and `xrEnumerateStereoCamerasDXR` fills it:

```c
typedef struct XrStereoCameraDisplayDXR {
    XrStructureType type;      // XR_TYPE_STEREO_CAMERA_DISPLAY_DXR (1004999317)
    void* XR_MAY_ALIAS next;
    uint64_t displayId;        // XrDisplayDXR::displayId (XR_DXR_display_info v22), 0 = unknown
} XrStereoCameraDisplayDXR;
```

`displayId` is the id `xrEnumerateDisplaysDXR` reports for the screen the camera is built into
or mounted on, so an app can offer "the camera on this screen" or group cameras per screen
without parsing `displayName`. It is 0 when the plug-in does not know, and for a camera that
belongs to no display (a plain stereo webcam, §9a). It follows the source like the name: a
change of it is a change of the list (one `XrEventDataStereoCamerasChangedDXR`). A runtime older
than v5 leaves the struct untouched, so set `displayId` to 0 before the call. It is chained, not
appended to `XrStereoCameraPropertiesDXR`, because growing a public struct would break apps
built against the older size.

**How the published frame is turned (spec v7).** Chain `XrStereoCameraOrientationDXR` on a
camera's `XrStereoCameraPropertiesDXR::next` and `xrEnumerateStereoCamerasDXR` fills it:

```c
#define XR_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN_DXR 0xFFFFFFFFu
typedef struct XrStereoCameraOrientationDXR {
    XrStructureType type;              // XR_TYPE_STEREO_CAMERA_ORIENTATION_DXR (1004999318)
    void* XR_MAY_ALIAS next;
    uint32_t frameOrientationDegrees;  // 0 / 90 / 180 / 270, or ..._UNKNOWN_DXR
} XrStereoCameraOrientationDXR;
```

`frameOrientationDegrees` is the **clockwise** rotation that makes the published side-by-side
frame (the whole image, as `xrAcquireStereoCameraFrameDXR` delivers it) upright in the device's
**natural orientation** (display rotation 0). It has exactly the meaning of Android Camera2's
`SENSOR_ORIENTATION`, applied to the published frame rather than to the sensor: if the source
already turned its eyes, the value is what remains after that turn. It is **fixed per camera and
never follows the current display rotation**; a consumer combines it with the rotation it is
held in (e.g. Chromium's user-facing `(S + R) % 360`, rear `(S − R) % 360`). A frame that is
already upright in the natural orientation reports 0.

- It does not change what "left" means: the left half is always the lens on the camera's own
  left (§4), and the published frame is the orientation in which that pair is side by side.
- `XR_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN_DXR` (`0xFFFFFFFF`) = the runtime does not know:
  the source did not say (a plug-in that predates the field, or does not fill it), and the UVC
  source of §9a. A consumer then uses its own rule (on Android, the CDD rule that a camera's long
  edge follows the screen's long edge).
- **The consumer MUST set `frameOrientationDegrees` to
  `XR_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN_DXR` before the call.** A runtime older than v7
  leaves the chained struct untouched; a consumer that zero-initialises it would read 0
  ("upright in the natural orientation") from such a runtime, which is wrong on every
  portrait-natural tablet. With the pre-set, "old runtime" and "not known" are the same answer.
- A change of it (a source that learns it late) is a change of the list, like the display: one
  `XrEventDataStereoCamerasChangedDXR`.

What the Leia Android plug-in reports (the reference case): a portrait-natural tablet publishes
its pairs unturned in the sensor's landscape, so the front pair reports **270** and the rear pair
**90** (their sensor orientations); a phone-class device whose front lenses are stacked along
the sensor's y axis turns each eye by the sensor orientation before packing, so its pair is
upright in natural portrait and reports **0**.

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
    XrPosef                    rightFromLeft;   // the RIGHT camera's pose in the LEFT camera's frame (below)
    float                      baselineMm;
} XrStereoCameraCalibrationDXR;

XrResult xrGetStereoCameraCalibrationDXR(XrInstance instance, uint64_t cameraId,
    XrStereoCameraCalibrationDXR* calibration);
```

**Extrinsics convention (`rightFromLeft`, fixed — shipped consumers depend on these numbers).**
`rightFromLeft` is the **pose of the right camera expressed in the left camera's frame**, in metres;
both are OpenCV camera frames (+x right in the image, +y down, +z forward). As a transform it maps
right-camera coordinates *into* left-camera coordinates — so read the name as "right, relative to
left"; it is the **inverse** of the "aFromB maps B into A" naming habit. With `R_q` the rotation
matrix of `orientation` and `C` = `position`:

| | equation |
|---|---|
| where the right camera's centre is, in the left frame | `C` |
| a point `x_L` in the left camera's frame, in the right camera's frame | `x_R = R_qᵀ (x_L − C)` |
| the same, in OpenCV `stereoCalibrate` terms (`x_R = R x_L + T`) | `R = R_qᵀ`, `T = −R_qᵀ C` (metres; ×1000 for mm) |

A plug-in reports OpenCV `R`, `T` (mm, §9); the service converts with `orientation = quat(Rᵀ)`,
`position = −Rᵀ T / 1000`. **Example (the tablet, and any pair whose right lens is to the right of
the left one): `C = (+baseline, ≈0, ≈0)` — `rightFromLeft.position.x > 0`**, i.e. OpenCV
`T.x = −baseline`. RECTIFIED: identity orientation, `C = (+baselineMm/1000, 0, 0)` exactly. Pinned by
`tests_stereo_camera_manager` ("rightFromLeft is the right camera's pose in the left frame": a 5°
extrinsic yaw, both equations checked against OpenCV's on an arbitrary point).

**Pixel convention (everywhere in this extension, plug-in and service alike).** Pixel centres at
integer coordinates (OpenCV): the first pixel's centre is (0, 0) and an image `W` wide has its
centre at `(W − 1)/2`. Every rescale, crop and mode derivation the service applies (§5.5) uses this
convention; a plug-in that reports principal points in another (e.g. an edge-origin) convention is
off by ½ px times the scale.

**Whose calibration.** For each camera the plug-in returns the calibration of **the device
behind that camera** — the physical unit whose tracker it reads (a plug-in driving several
displays lists one camera per device) — keyed by that device's own identity (serial). It never
picks "the first calibration folder" on the machine: one field box carried eleven. A plug-in
that cannot resolve a camera's calibration reports that camera without `CALIBRATED`, and the
service offers RAW only.

**An uncalibrated UVC webcam (§9a)** is the one exception to "RAW only": the service models it
as an ideal parallel pinhole pair from its configured HFOV and baseline. It is reported without
`CALIBRATED`; `xrGetStereoCameraCalibrationDXR(RAW)` fails (`XR_ERROR_FEATURE_UNSUPPORTED`: a
nominal model is not a measurement of the device); RECTIFIED is offered and returns the numbers
of the frames actually delivered — the nominal pinhole while they are passed through, the
corrected geometry once the online refinement has aligned the rows (the frame's
`calibrationGeneration` moves then).

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
> thread (outside its lock) and only while some started stream wants RECTIFIED (with only RAW
> streams started, just the frames the online refinement measures, §5.9); RAW streams read
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
    XrStereoCameraOutputDXR output;         // RECTIFIED needs CALIBRATED or NATIVELY_RECTIFIED, or a
                                            // nominal-model UVC camera the service aligns (§9a)
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

**Colour.** NV12 frames are BT.601 full range (JPEG), on every source: the contract the browser's
camera device applies, and why a grey sensor's values can be NV12 luma unchanged.

**Shared memory (required). On the first `xrStartStereoCameraStreamDXR` the stream exposes,
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

### 5.5 Capture modes (spec v8)

A v7 camera delivers one fixed size. A v8 camera lists the per-eye sizes its source can run, and a
stream asks for one:

```c
uint32_t n = 0;
xrEnumerateStereoCameraModesDXR(instance, cameraId, 0, &n, NULL);
XrStereoCameraModeDXR modes[16];                  // each {XR_TYPE_STEREO_CAMERA_MODE_DXR}
xrEnumerateStereoCameraModesDXR(instance, cameraId, n, &n, modes);

XrStereoCameraStreamModeInfoDXR mi = {XR_TYPE_STEREO_CAMERA_STREAM_MODE_INFO_DXR, NULL,
                                      {1920, 1080}};       // a STREAM mode's eyeExtent
XrStereoCameraStreamCreateInfoDXR ci = {XR_TYPE_STEREO_CAMERA_STREAM_CREATE_INFO_DXR, &mi, ...};
```

| `XrStereoCameraModeDXR` | |
|---|---|
| `eyeExtent` | per-eye size; a frame of the mode is `2·width × height`, side by side |
| `maxFrameRate` | the highest rate the source runs the mode at, Hz. **0 = not known yet**: a source may not know a mode's real rate before running it (a device's HAL table claimed 30 Hz where the sensor gave 29.80 and 27.4), so a plug-in lists 0 rather than a guess; always 0 for a still-only size. **Once a mode has run, the runtime lists its measured rate** here, and a stream's `XrStereoCameraStreamStatsDXR::sourceFrameRate` reports the measured rate while it runs. A consumer never divides by it unchecked (the CLI prints "rate unknown" / JSON `null`) |
| `flags` | `DEFAULT_BIT` (0x1): the mode a stream gets without the chained struct — exactly one mode, its `eyeExtent` equals `XrStereoCameraPropertiesDXR::eyeExtent`, and it is listed first. `STREAM_BIT` (0x2): a stream may ask for it. `STILL_BIT` (0x4): a still may be taken at that size (§5.7) |

- **Every camera lists at least one mode** — its default. A camera whose source lists no modes
  (a plug-in that predates v8, the UVC source of §9a, an app entry) reports exactly that one, so a
  consumer never needs a v7 fallback path beyond "the runtime is older than v8".
- **One horizontal field of view.** Every mode of a camera has the default mode's **horizontal**
  field of view, about the same optical axis, with square pixels; its **vertical** extent may differ —
  a centred vertical crop or extension of the default. The reference case is the tablet: its default
  is the centre 16:9 cut (1280×720) of a 4:3 sensor (1280×960), and the camera app wants the full 4:3
  for stills and may stream it. One calibration (the default's) therefore describes every mode, with
  `s = width / width_default`:

  ```
  f' = f·s     cx' = (cx + ½)·s − ½     cy' = (cy + ½ − H_default/2)·s + H/2 − ½     distortion unchanged
  ```

  (pixel centres at integer coordinates, as everywhere in this spec; in an edge-origin convention the
  same rule reads `cy' = (cy − H_default/2)·s + H/2`). The service drops, with one WARN per camera, a
  listed mode that is not expressible this way: a relative height `(H/W) / (H_default/W_default)`
  outside [½, 2], or — for a calibrated camera — a derived principal point outside the mode's frame.
  Sizes alone cannot reveal a mode that crops HORIZONTALLY; listing one is a plug-in contract
  violation (§9b).
- **Absent = default.** Without `XrStereoCameraStreamModeInfoDXR` the stream runs the default mode,
  exactly as in v7. With it, `eyeExtent` must equal a `STREAM` mode's (`XR_ERROR_VALIDATION_FAILURE`
  otherwise). The stream's frames, ring and `XrStereoCameraStreamInfoDXR::extent` are then
  `2·width × height` of that mode, and `maxFrameRate` on the create info still caps its rate.
- **Calibration of a mode.** Chain the same `XrStereoCameraStreamModeInfoDXR` on
  `XrStereoCameraCalibrationDXR::next` and `xrGetStereoCameraCalibrationDXR` returns the numbers of
  frames of that per-eye size (`eye[i].imageExtent` = it). Absent = the default mode's, as before.
  RAW numbers are the device calibration carried to the mode by the rule above; RECTIFIED numbers
  are those of the rectifier the source currently runs, carried through the same centred-rows crop
  and scale its frames go through (or, when the source runs a mode that cannot serve that size, the
  geometry the service builds for the mode itself) — so frames and numbers never disagree, and
  `calibrationGeneration` moves when they change (below).

**One capture, several modes (principle 2).** Two streams of one camera may ask for different
modes. The service decides what the source runs:

1. **The source runs the smallest STREAM mode that serves every started stream** — at least as wide
   as each and at least as tall relative to its width (the default mode when none is started). For
   streams of one aspect this is simply the largest mode any of them asks for. Every other stream is
   served from it: **the centred rows** of the stream's aspect (rows = H·W_source/W, even), **then
   downscaled by the service** per eye (integer box pre-reduction, then bilinear, each half sampling
   only its own half), before the format conversion — **never letterboxed, never stretched**, never
   scaled up. A 16:9 stream on a source running 4:3 gets the middle 16:9 band; while the source runs a
   mode that cannot serve a stream (during a switch), that stream simply gets no frames.
   **A start no single listed mode can serve together with the started streams is refused with
   `XR_ERROR_STEREO_CAMERA_BUSY_DXR`** (e.g. a 1920×1080 16:9 stream while a 4:3 stream runs and no
   4:3 mode is 1920 wide); it may be started once the other stream stops.
2. **A mode change is a reconfigure, not a restart.** When the wanted mode changes (a larger stream
   starts, the largest one stops) the service asks the open source to switch
   (`stereo_camera_set_mode`, §9b). The source keeps its open state, sequence numbers keep counting,
   and the streams that keep their mode see at most a **reconfigure gap**: the plug-in contract
   allows 500 ms without frames per switch, and the camera's state does not leave `AVAILABLE` for a
   gap shorter than the 1 s suspension threshold. Streams are never ended or restarted by a switch.
3. **The rectifier follows the source.** A service-rectified camera rebuilds its maps for the new
   source size (once per switch, on the camera thread, ~10–20 ms at 1280×720 per eye), keeping the
   online vertical correction (§4), and bumps `calibrationGeneration`: a RECTIFIED consumer re-reads
   its calibration on the next frame, as it already does for the refinement.
4. **A source may refuse.** A source that cannot switch (an error, or a `SHARED_WITH_EYE_TRACKING`
   camera that will not leave the tracker's mode) fails the switch; the service stays on the mode it
   runs and **ends every started stream whose mode it does not reach** with
   `XR_STEREO_CAMERA_STREAM_END_REASON_SOURCE_BUSY_DXR` (one WARN). A tracking-shared plug-in should
   rather not list a mode it will refuse: §6 still holds — tracking decides the device.
5. A source without a mode switch (a plug-in that lists modes but has no `stereo_camera_set_mode`)
   is listed with its default mode only.

### 5.6 Controls (spec v8)

Capabilities, per camera, chained on `XrStereoCameraPropertiesDXR::next` and filled by
`xrEnumerateStereoCamerasDXR`:

| `XrStereoCameraControlCapabilitiesDXR` | |
|---|---|
| `supportedControls` | `XrStereoCameraControlFlagsDXR`: `FOCUS_MODE_BIT` 0x1, `FOCUS_DISTANCE_BIT` 0x2, `METERING_POINT_BIT` 0x4, `EXPOSURE_COMPENSATION_BIT` 0x8. **0 = the camera has no controls** (and what an older runtime leaves in a zero-initialised struct) |
| `supportedFocusModes` | one bit per `XrStereoCameraFocusModeDXR` (`1 << value`) |
| `minFocusDistanceDiopters` | nearest focus, dioptres (1/m); **0 = fixed focus** |
| `focusDistanceCalibration` | `UNCALIBRATED` (0, relative steps) / `APPROXIMATE` (1) / `CALIBRATED` (2) — Camera2's `LENS_INFO_FOCUS_DISTANCE_CALIBRATION` |
| `exposureCompensationMinEv` / `MaxEv` / `StepEv` | the EV range and its step; step 0 = no compensation |
| `defaultFocusDistanceDiopters` | what the source holds when no focus control is set: a **fixed distance in dioptres** (0 = infinity), or `XR_STEREO_CAMERA_FOCUS_DISTANCE_UNKNOWN_DXR` (−1) when the source focuses by itself or does not say. Pre-set it to −1 before the call. It sits in what was the struct's tail padding: the size is unchanged |

Controls are set **on a stream**, and apply to its camera:

```c
XrStereoCameraControlsDXR c = {XR_TYPE_STEREO_CAMERA_CONTROLS_DXR};
c.validControls = XR_STEREO_CAMERA_CONTROL_FOCUS_MODE_BIT_DXR | XR_STEREO_CAMERA_CONTROL_METERING_POINT_BIT_DXR;
c.focusMode = XR_STEREO_CAMERA_FOCUS_MODE_AUTO_ONCE_DXR;     // tap to focus
c.meteringPoint = (XrVector2f){0.4f, 0.6f};                  // in the LEFT eye of THIS stream's output
xrSetStereoCameraControlsDXR(stream, &c);
...
xrReleaseStereoCameraControlsDXR(stream);                     // back to the camera's defaults
```

| Field (`validControls` bit) | Meaning |
|---|---|
| `focusMode` | `CONTINUOUS_AUTO` (1): the source focuses by itself; `AUTO_ONCE` (2): one search at the metering point, then hold — **each call that sets it starts a new search**; `MANUAL` (3): focus at `focusDistanceDiopters`; `LOCKED` (4): hold the current distance |
| `focusDistanceDiopters` | the MANUAL distance, `0` (infinity) … `minFocusDistanceDiopters` |
| `meteringPoint` | the metering / focus point, **normalised in the left eye of the stream's output** (0,0 = top-left, 1,1 = bottom-right of the left half). For a RECTIFIED stream the service maps it through the rectifier to the raw left eye before the source sees it, so the point is the pixel the app tapped |
| `exposureCompensationEv` | EV, within the advertised range, rounded to the step |

Rules:

- **Only fields whose bit is set change**; the others keep their current value. A bit the camera does
  not support, or a focus mode not in `supportedFocusModes`, is `XR_ERROR_FEATURE_UNSUPPORTED`; a
  camera with no controls at all (`supportedControls == 0`) is `XR_ERROR_FEATURE_UNSUPPORTED` for any
  call. A value out of range (distance, point outside [0, 1], EV outside the range, an unknown enum)
  is `XR_ERROR_VALIDATION_FAILURE`.
- **Multi-lens sources and focus.** A pair's own autofocus is often unusable (each lens would
  search on its own and the two would disagree), so a source **may advertise only `MANUAL` and
  `LOCKED`** (with `FOCUS_DISTANCE`). Its default, with no focus control set, is then a
  source-chosen fixed distance, exposed as `defaultFocusDistanceDiopters`. A consumer that wants
  tap-to-focus on such a camera drives `MANUAL` from **its own distance estimate** (e.g. the
  disparity under the tapped point of a rectified pair: `Z = f·B/d`, dioptres `1/Z`), not
  `AUTO_ONCE`.
- **One controller per camera.** The first stream that sets controls **holds** them. Another stream
  of the same camera — any connection — gets `XR_ERROR_STEREO_CAMERA_CONTROL_HELD_DXR` until the
  holder calls `xrReleaseStereoCameraControlsDXR`, stops, ends (§7.4) or is destroyed (its connection
  closing included) — **or its frames are withheld** (below). When the hold ends the camera returns to
  its defaults (every field unset), so a call's preview never inherits another app's manual focus.
- **The hold does not survive the background.** The moment the holder's frames are withheld — its
  app goes to the background or loses its visible window (§7.2, including the Android front-camera
  rule), the session locks, sharing is switched off — it loses the hold and the camera returns to its
  defaults. A foreground app is therefore never blocked by a background one: its
  `xrSetStereoCameraControlsDXR` succeeds even if the old holder has not yet had a frame withheld
  (the service checks the holder's state on the spot). The holder's connection is told with the
  existing `XrEventDataStereoCameraStateChangedDXR`: `SUSPENDED` when its frames stop (§7.6 — v8
  makes this guaranteed for the foreground rule too, which before only stopped the frames), the
  camera's state again when they resume. **A client that set controls re-applies them after a
  `SUSPENDED` → `AVAILABLE` transition** (and treats any `XR_ERROR_STEREO_CAMERA_CONTROL_HELD_DXR`
  as "someone else in front holds them now"). Re-applying is idempotent, except that setting
  `AUTO_ONCE` again starts a new search.
- **Same gate as frames.** Controls need a **started** stream (`XR_ERROR_CALL_ORDER_INVALID`
  otherwise): start is the authorisation point (§7.1), so controls need exactly the consent a
  stream does. A stream whose frames are withheld right now — the session is locked, sharing is off,
  or its client is not visible (§7.2, including the Android front-camera rule) — gets
  `XR_ERROR_PERMISSION_INSUFFICIENT`: an app in the background never moves the lens under the app in
  front. An ended stream gets `XR_ERROR_STEREO_CAMERA_STREAM_ENDED_DXR`.
- Controls are applied by the camera thread between two frames (≤ one frame period, at most 100 ms),
  and re-applied after the source re-opens while the hold lasts.

**Reported state.** Chain `XrStereoCameraCaptureResultDXR` on `XrStereoCameraFrameDXR::next`
(zero-initialised: an older runtime leaves it untouched) and each acquired frame carries the state
of its capture:

| Field (`validFields` bit) | |
|---|---|
| `focusDistanceDiopters` (`FOCUS_DISTANCE` 0x1) | where the lens was, in the unit `focusDistanceCalibration` says |
| `focusState` (`FOCUS_STATE` 0x2) | `SEARCHING` 1 / `LOCKED` 2 / `FIXED` 3 (a fixed-focus lens) / `FAILED` 4 (the last search did not converge) / `UNKNOWN` 0 |
| `exposureTime` (`EXPOSURE_TIME` 0x4) | ns |
| `sensitivityIso` (`SENSITIVITY` 0x8) | ISO |
| `eyeCaptureTime[2]` (`EYE_CAPTURE_TIMES` 0x10) | start of exposure of each eye, on the runtime clock like `captureTime` (two sensors are rarely exposed at the same instant). **Set only when the source stamps each lens itself**: a source whose HAL stamps both lenses with one value leaves the bit clear. Consumers **must not** treat equal per-eye times, or missing ones, as evidence that the two lenses were exposed together |

A field the source did not report has its bit clear. The result travels on the acquire reply, never
in the ring.

### 5.7 Stills (spec v8)

A still is **one pair delivered once**, at a still size (a mode with `STILL_BIT`), in its own
section — not a frame of the stream:

```c
XrStereoCameraStillRequestDXR rq = {XR_TYPE_STEREO_CAMERA_STILL_REQUEST_DXR, NULL,
    {0, 0} /* the largest still size */, XR_STEREO_CAMERA_OUTPUT_RECTIFIED_DXR, XR_STEREO_CAMERA_FORMAT_NV12_DXR};
uint64_t id = 0;
xrRequestStereoCameraStillDXR(stream, &rq, &id);
// ... XrEventDataStereoCameraStillReadyDXR {stream, id} arrives from xrPollEvent
XrStereoCameraCaptureResultDXR res = {XR_TYPE_STEREO_CAMERA_CAPTURE_RESULT_DXR};
XrStereoCameraCalibrationDXR cal = {XR_TYPE_STEREO_CAMERA_CALIBRATION_DXR, &res};
XrStereoCameraStillDXR st = {XR_TYPE_STEREO_CAMERA_STILL_DXR, &cal};
xrAcquireStereoCameraStillDXR(stream, id, &st);   // st.sectionHandle: map read-only, close it when done
xrReleaseStereoCameraStillDXR(stream, id);
```

- **Request.** On a **started** stream, with the same gate as controls (§5.6: CALL_ORDER_INVALID /
  PERMISSION_INSUFFICIENT / STREAM_ENDED). `eyeExtent` is a `STILL` mode's, or `{0, 0}` for the
  largest; another size is `XR_ERROR_VALIDATION_FAILURE`. `output` follows the stream rules: RAW is
  refused to a present-owner / delegating client (`XR_ERROR_PERMISSION_INSUFFICIENT`), RECTIFIED on
  a camera the service cannot rectify is delivered RAW and flagged (`output`). `format` is any of
  the camera's `supportedFormats`. A camera that lists no still size (its source has no still
  support, §9b) returns `XR_ERROR_FEATURE_UNSUPPORTED`. **One still per stream at a time**: a request
  while the previous one is pending or not yet released is `XR_ERROR_LIMIT_REACHED`. `requestId` is
  unique within the service run.
- **Completion is an event.** `XrEventDataStereoCameraStillReadyDXR {stream, requestId}` or
  `XrEventDataStereoCameraStillFailedDXR {stream, requestId, result}`, on the connection's event
  queue (§7.6). `result`: `XR_ERROR_STEREO_CAMERA_BUSY_DXR` (the source failed or refused the still,
  the source closed, or no still within 10 s), `XR_ERROR_PERMISSION_INSUFFICIENT` (at delivery the
  stream's frames were withheld — locked, sharing off, not visible — so the still is dropped, never
  delivered late), `XR_ERROR_STEREO_CAMERA_STREAM_ENDED_DXR` (the stream stopped or ended first).
- **Acquire** fills `XrStereoCameraStillDXR`: the full SBS `extent`, `format`, `output`, plane
  pitches and offsets, the capture time, `sourceFlags`, and a **new read-only section handle**
  (Windows `HANDLE` / POSIX fd, the caller closes it) of `sectionSize` bytes holding exactly one
  image. `XR_STEREO_CAMERA_STILL_SOURCE_LOSSY_BIT_DXR` (0x1) in `sourceFlags` says the pixels were
  **lossy-compressed before the runtime got them**: on a device that streams YUV, a full-resolution
  pair can only come from one JPEG output per lens (a second YUV output aborts the HAL), which the
  plug-in decodes. Such a RAW still is raw in geometry (unrectified, lens distortion intact), not in
  pixel fidelity. `sourceFlags` sits in what was the struct's tail padding: its size is unchanged;
  zero-initialise it. Chained
  `XrStereoCameraCaptureResultDXR` gets the still's capture state; chained
  `XrStereoCameraCalibrationDXR` gets the calibration of exactly these pixels (its `output` is
  overwritten with the still's). Acquire may be called again (another handle to the same memory) until
  release. Before the ready event: `XR_ERROR_CALL_ORDER_INVALID`; an unknown or failed id:
  `XR_ERROR_VALIDATION_FAILURE`.
- **Release** frees the runtime's copy; a mapping the consumer already made stays valid (the memory
  lives until the last handle and mapping close). Releasing an id the runtime no longer holds
  (failed, already released) is a no-op `XR_SUCCESS`; releasing a still still in flight cancels it
  (what the source delivers is discarded, no event). Destroying the stream releases its still.
- **The running stream.** Taking a still may make the source reconfigure (a larger sensor mode, a
  separate still pipeline). The stream's frames may then **pause for a documented gap** — the
  plug-in contract allows up to 1 s — and resume by themselves; the camera's state stays
  `AVAILABLE` while a still is in flight, and the stream is never ended or restarted for it.
  Controls (focus, exposure) in force apply to the still.
- **A tracking-shared camera** (§6) may refuse a still (`STILL_FAILED`, BUSY) or **downsize** it to
  protect tracking: the still's `extent` is what was actually captured, possibly smaller than asked,
  and its calibration describes that size.
- Pixels are produced exactly like stream frames: the plug-in hands over a RAW pair in its native
  format, the service rectifies it (a rectifier for the still size, built on first use and kept per
  camera), converts it and copies it into a fresh section. Transport is **shared memory only** in v8;
  an `AHardwareBuffer` still (Android, zero-copy for 12 MP pairs) is the next step and its struct
  type value (1004999469) is held back for it.

### 5.9 Online vertical refinement for a RAW consumer (spec v8)

A device's stored calibration drifts: the service measures the rows it delivers and corrects
them online (R2 refinement: `dy(y) = a + b (y − cy)`, right minus left, fitted on ≥ 50 row
matches over ≥ 3 frames, every 250 ms for 5 s after an open or an update, then one window every
30 s; clamped to |a| ≤ 6 px, |b| ≤ 2 px / 100 px). RECTIFIED frames carry the correction. A
consumer that rectifies **RAW** frames itself (a camera app on its GPU) reads it by chaining
`XrStereoCameraVerticalRefinementDXR` (`XR_TYPE_STEREO_CAMERA_VERTICAL_REFINEMENT_DXR` =
1004999470) on `XrStereoCameraCalibrationDXR::next` (either output; also on the calibration chained
to a still):

```c
typedef struct XrStereoCameraVerticalRefinementDXR {
    XrStructureType type; void* next;
    XrStereoCameraVerticalRefinementFlagsDXR flags;  // VALID_BIT 0x1
    float verticalOffset;                // A, normalised rectified units
    float verticalSlope;                 // B, dimensionless
    XrQuaternionf rectifyingRotation[2]; // rectified-from-raw per eye (OpenCV R1, R2)
    XrExtent2Di measuredEyeExtent;       // the rectified per-eye size measured at
    float measuredFocalLength;           // its rectified f, px
    float residualPx;                    // signed median dy of the latest fitted window
    uint32_t matchCount, windowCount, updateCount;
} XrStereoCameraVerticalRefinementDXR;
```

**How it composes with the RAW calibration.**

1. *Frame.* The correction lives in the service's rectified frame: eye `e`'s raw rays rotated by
   `rectifyingRotation[e]` (`ray_rect = R(q_e) · ray_raw`). These are exactly OpenCV
   `stereoRectify`'s R1 / R2 for the RAW calibration of §4 (zero disparity; **any alpha** — the
   crop changes f / cx / cy, never the rotations; golden-tested to 1e-12). A consumer whose own
   rectification uses those rotations applies the numbers directly; one that rectifies some other
   way uses these rotations instead of its own (or re-expresses D in its frame).
2. *What, and its sign.* In that frame's normalised coordinates `(x, y) = (X/Z, Y/Z)` (+y down)
   the uncorrected pair leaves the **right** eye's rows displaced from the **left** eye's by
   `D(y) = verticalOffset + verticalSlope · y` (a scene point at row `y_L` in the left image sits
   at `y_L + D` in the right).
3. *Which eye.* **Both, split symmetrically**: the corrected output row `y` of eye `e` samples the
   uncorrected rectified row `y_src = y (1 + s_e B/2) + s_e A/2`, `s_left = −1`, `s_right = +1`
   (`A` = offset, `B` = slope; `x` unchanged). Equivalently a per-eye vertical affine
   `diag(1, 1 + s_e B/2, 1)` plus a row shift in normalised coordinates, applied after the
   rotation. Rows then agree to second order (B², AB ≈ 1e-5).
4. *Units and other modes.* Normalised units are mode-independent: the same `A`, `B` serve every
   mode (the rotations do not depend on the image size). In pixels of any rectified image with
   focal `f` and principal row `cy`: `dy(v) = f·A + B·(v − cy)`, pixel centres at integers (§4
   pixel convention). `measuredEyeExtent` / `measuredFocalLength` say where it was measured
   (`f·A` there is the offset in those pixels); they are informational, not a rescale input.
5. *Validity.* `VALID_BIT` clear = nothing fitted on this camera yet, the refinement is off
   (`DXR_STEREO_CAMERA_REFINE=0`), or the camera is natively rectified / not rectified by the
   service (no RAW-referred correction exists): every field then reads 0 except
   `rectifyingRotation`, which is identity. Set = at least one window was fitted
   (`windowCount ≥ 1`); `A = B = 0` then means "measured and aligned". Confidence:
   `matchCount` (≥ 50 to fit), `residualPx` (the latest window's signed median dy on frames that
   carried the correction in force *then*: right after the first update it is the uncorrected
   misalignment, later the residual; |residualPx| ≤ 0.2 = converged), `updateCount`.
6. *Change.* A new correction is swapped in between two frames and **moves the frame's
   `calibrationGeneration`** — RAW streams too: re-read the calibration (with this struct
   chained) when it moves.

**RAW-only consumers are measured too.** The refinement is driven by the camera thread on the
service's rectified image; when only RAW streams are started, the service rectifies a source frame
**only when the estimator is due** (the cadence above; one remap of ~1 ms per measured frame on an
M1 Pro, i.e. ≤ 4 frames/s while settling and 3 frames per 30 s afterwards) and never publishes it.
A lingering source with no started stream is not measured. Measured on this Mac (sim fake,
distorted, `_DY=2.6`, one RAW GRAY8 stream, `camera probe --raw` then `camera calib --raw`):
`D = +0.005455 − 0.000060·y` = **+2.485 px at 640×480, f 455.54** (the fake's +2.60 px raw
misalignment at raw fy ≈ 471, i.e. +2.51 px at the rectified f), residual +0.010 px over 456
matches, 9 windows, 1 update; rotations `R1 q(0.003054 0.002182 0.003927 0.999985)`,
`R2 q(−0.003054 −0.002182 −0.003927 0.999985)`. Pinned by `tests_stereo_camera_manager`
("the vertical refinement runs for a RAW-only consumer and is exposed": UVC fake, RAW only →
VALID with `f·A` = +2.41 px for the fake's +2.4 px, the RAW frames themselves still +2.40 px).

The UVC nominal model (§9a) has no RAW calibration; its rectifying rotations are identity and its
"rectified" frames are the source frames while passed through, so a consumer of its RAW frames can
apply the correction with the RECTIFIED calibration's `f` / `cy`.

### 5.8 Known limits (v8, as built)

- **A large RECTIFIED still is slow today.** The service rectifies a still on the camera's own
  thread, building a remap table for the still size on first use (cached per camera): the first
  1280×960-per-eye still on the distorted sim fake took 1.2–1.4 s in a Debug build, and a 12 MP pair's
  table is ~180 MB while it exists. That camera's stream frames stall meanwhile. **A RAW still stays
  cheap** — no rectifier, one copy (or format conversion) into its section — and is the intended path
  for a camera app that rectifies on its own GPU with the still's calibration. Moving still
  rectification off the camera thread is a follow-up.
- **Per-stream downscale runs under the manager lock**, one pass per stream (like the format
  conversion it precedes). Measured on this Mac, distorted fake, a 1280×960 RECTIFIED stream plus a
  default 640×480 RECTIFIED stream served downscaled from it: Debug build ~7.5 Hz and ~9 Hz; **Release
  build 30.0 Hz and 29.8 Hz (the full source rate), mean source-to-publish latency 26–28 ms** — so
  the Debug rates are a Debug artefact, not a design limit. Sharing one scaled image per target size
  and scaling outside the lock are follow-ups.
- Centred rows are rounded to an even count (NV12): when `H·W_source/W` is not an integer the served
  stream's numbers carry `fx ≠ fy` by at most one row in `H`; they always describe the frames exactly.
- Shared memory only; the `AHardwareBuffer` still (and stream) transport is the next step.

## 6. Coexistence with eye tracking

When `SHARED_WITH_EYE_TRACKING` is set:

- **The tracker keeps the device; the camera stream is a passenger.** The plug-in reads the
  frames the tracker already publishes. It never opens, reconfigures, or re-times the device.
  Frame size, rate and exposure are whatever tracking needs; a consumer asking for more gets
  `maxFrameRate`-decimated tracker frames, not a new mode. (Spec v8: a tracking-shared plug-in lists
  only the modes it will run without hurting tracking, may refuse a mode switch — §5.5 rule 4 — and
  may refuse or downsize a still, §5.7. Controls on such a camera are the plug-in's to accept: it
  advertises only those that leave tracking intact.)
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
| 4 | The OS camera privacy switch denies this executable (Windows `CapabilityAccessManager\ConsentStore\webcam`: machine policy, user global, desktop-app class, per-app entry; no equivalent is readable on macOS / Linux) — **for every client, delegating ones included** | `XR_ERROR_PERMISSION_INSUFFICIENT` |
| 5 | A stored per-app **Deny** (`HKCU\Software\DisplayXR\CameraConsent\Apps\<path>` = 2, `camera_consent.json` `apps`) — it beats a delegating registration, an installer's included | `CONSENT_REFUSED` |
| 6 | The executable is a registered **consent-delegating** client (a browser that shows its own per-origin prompt + in-use indicator): machine list written by its installer (`HKLM\Software\DisplayXR\CameraConsent\Delegating`, `/etc/displayxr/camera-delegating.json`, `/Library/Application Support/DisplayXR/camera-delegating.json`) or the user list (`displayxr-cli camera trust <exe>`) — **unless** the client set `XR_STEREO_CAMERA_CLIENT_DECLINE_DELEGATION_BIT_DXR` (§2a), or the entry is not trusted (§7.1.1). A skipped row 6 falls through to rows 7–9 exactly as for an unregistered app | allowed, no prompt, no store entry; never RAW (§7.5) |
| 7 | A stored per-app **Allow** | allowed |
| 8 | "Allow once" already granted to this same process (executable + pid) | allowed |
| 9 | The **tray prompt**: *"<app> wants to use the 3D camera — Allow / Allow once / Deny"* (Windows: a top-most dialog from the tray thread; macOS: a floating panel from the menu-bar item — never a modal that would park the service's run loop). **Allow** and **Deny** are written to the store; **Allow once** is remembered for the process. The call blocks for up to 60 s; unanswered / dismissed, or no prompt available (`DXR_STEREO_CAMERA_PROMPT=0`, headless Linux service) | `CONSENT_REFUSED` — retryable, asked again next time |

**Delegation means exactly "no runtime prompt and no stored per-app decision needed" — nothing
more.** It never outranks a refusal: the kill switches (row 1), the OS switch (row 4) and a stored
Deny (row 5) apply to a delegating client exactly as to any other app. The OS switch matters most
for it: the service, not the client, opens the camera, so the OS never sees the browser as a
camera user and nothing else would enforce the user's switch. Each refusal keeps its own result
(`DISABLED` / `PERMISSION_INSUFFICIENT` / `CONSENT_REFUSED`), so a browser can map them (§8).

`displayxr-cli camera trust <exe>` over a stored Deny for the same executable **clears the Deny**
and says so — `trust` is the user's newer, explicit word on that executable, as `allow` replacing
a Deny is. An installer's machine-wide registration never touches the user's store: there the
Deny keeps winning. `camera deny` on a delegating executable notes that the Deny wins.

This table is the one statement of the order; `u_camera_consent.h`, the developer guide and the
roadmap refer to it. One consumer does not enter it through these entry points: an input provider
inside the service opening the shared camera through the input host iface
(`docs/specs/runtime/input-provider-discovery.md` §4d) is judged by the `component` verdict of
ADR-043 Amendment 6 — row 1 and the session-lock suspend still apply, a provider the provenance
gate admitted as signed / allowlisted / platform-trusted is allowed without a prompt, and any other
provider is evaluated per executable through this table like an app.

#### 7.1.1 Trusting a delegating entry (spec v3)

An entry names an executable by full path, and a path is only an identity where nobody but an
administrator can put a different file there.

- **Admin-protected path** — on Windows the OS-derived executable path is under
  `%ProgramFiles%`, `%ProgramFiles(x86)%` or `%SystemRoot%` (`SHGetKnownFolderPath`
  `FOLDERID_ProgramFiles` / `_ProgramFilesX86` / `_Windows`; ASCII-case-insensitive, at a
  separator boundary, no `..` component): the path alone is trusted, as before.
- **User-writable path** — anywhere else (a per-user install under `%LOCALAPPDATA%`, a dev
  tree, Downloads): the entry must also record the **required signer**, and the executable must
  carry a **valid Authenticode signature** whose leaf signer's subject CN equals it
  (ASCII-case-insensitive, surrounding spaces ignored). Verification is `WinVerifyTrust`
  (`WINTRUST_ACTION_GENERIC_VERIFY_V2`, no UI, `WTD_REVOKE_NONE`, `WTD_CACHE_ONLY_URL_RETRIEVAL`
  — it never blocks a stream start on the network). The signer is stored as a sibling `REG_SZ`
  in the same `Delegating` key, named `<valueName>.signer` (`displayxr-cli camera trust <exe>
  --signer "<CN>"` writes it; an installer registering a per-user install writes it next to
  its entry). A `*.signer` value is never itself an entry.
- An entry that fails — no signer recorded, no valid signature, or another signer — is **not
  applied**: row 6 is skipped exactly as if the executable were not registered (stored decision
  / prompt), with one WARN per executable per service run naming the reason. The executable
  stays *registered* for the RAW refusal (§7.5): an untrusted entry never widens anything.
- **POSIX:** delegation is path-only for now — no signer is recorded and no code-signature check
  exists yet (macOS `SecStaticCode` / Linux are a follow-up).
- Known limit of the path rule: a few folders under `%SystemRoot%` (e.g. `Temp`, `Tasks`) and an
  install directory whose ACL an installer loosened are writable all the same. The path rule is
  the cheap gate for the common case; an installer that registers such a path should record a
  signer anyway.

**User-level entries are announced.** An entry in the *user* list (`HKCU`, the user JSON —
written by `displayxr-cli camera trust` or by anything else running as the user) is honoured,
but the first time it actually allows a given executable in a service run the service logs a
WARN and raises a notice (Windows: a tray balloon, *"3D camera: per-user trust used"*; macOS:
the WARN only, for now), once per executable per service run, so a silent self-registration
cannot go unnoticed. A machine-level (installer, `HKLM` / system JSON) entry is silent. When the
same path is in both lists the machine-level entry is the one evaluated.

**Foreground rule.** Only a stream whose start was allowed *by* row 6 follows its client's own
visibility rule (§7.2); a registered client that declined delegation, or whose entry was not
trusted, is an ordinary window-bearing app there too.

A refused stream stays created and may be started again (e.g. after `displayxr-cli camera allow`).
The policy is pure (two injected vtables) and unit-tested with fakes in `tests_camera_consent`.

#### 7.1.2 Android (A2, as built)

On Android the runtime **is** the OS-visible camera client: a display plug-in that sources the
device's own pair opens it with Camera2 inside the runtime satellite that serves the consumer
(stereo-camera-android §1.2, option R; design in draft PR [#1834](https://github.com/DisplayXR/displayxr-runtime/pull/1834)). Every app's
executable is the zygote's `app_process`, so the identity is not a path. It is a **subject**
derived from the connection's `SO_PEERCRED` uid by the runtime's Java side
(`StereoCameraConsent.peerIdentity`): `android:<package>@<SHA-256 of the current APK signing
certificate>` (`util/u_camera_consent_android.h`). A shared-uid set is named by its shared-user
name. The policy itself is unchanged and keyed on the subject. Stored decisions, "Allow once"
and `persistentId` (§7.5) therefore follow package **and** certificate: a reinstall signed by
someone else inherits nothing.

| # | Android |
|---|---|
| 1 | `debug.xrt.DXR_STEREO_CAMERA=0` (the kill switch, read in the runtime's process) or the sharing toggle in the store |
| 2 | `debug.xrt.DXR_STEREO_CAMERA_DEV_ALLOW=1`: developer override, logged loudly at service start and once per run at first use. It also admits an unverifiable peer (row 3), so a debug-signed build of a trusted package works. It does **not** lift the front-camera rule (§7.2): a `USER_FACING` camera still needs a live weave. Never on a user's device |
| 3 | No single valid package for the uid, no readable signing certificate, several signers, or **a package on the built-in trusted list signed with a different certificate** (a rebuild cannot pass for the listed client). All of these are refused with `CONSENT_REFUSED`, without a prompt |
| 4 | The peer package must hold `CAMERA` (`PackageManager.checkPermission`), and its camera app-op must be `MODE_ALLOWED` or `MODE_FOREGROUND` ("only while using the app" is enough). Otherwise `XR_ERROR_PERMISSION_INSUFFICIENT`. This applies to the trusted browser too |
| 5, 7 | Stored decisions in `<files dir>/displayxr/camera_consent.json` (app-private, the POSIX store format) |
| 6 | **The built-in trusted list** (`u_camera_consent_android_builtin_trusted`): entries are package **and** certificate digest, compiled into the runtime. Any delegating list in the JSON store is ignored on Android. v1 list: `org.chromium.chrome` signed with the DisplayXR Browser release certificate, SHA-256 `d52153793c7a403f3cd733032ed43314f26a7748f6d5c0a2b5e12847ff0df5b2` (`CN=DisplayXR Browser, O=DisplayXR, C=US`), and — since v2.63 — the same package signed with the browser's own build-lane (CI) certificate, SHA-256 `32a2fc74d731105859e5a85df16d95f102d85b22099b8064c5d8915c61dad1e0`, so a tablet carrying a CI artifact between releases keeps its 3D camera. Both are entries of package AND digest: any other signer of that package is still refused outright. Its own per-origin prompt is the consent. The decline bit (§2a) is honoured |
| 9 | A runtime-owned dialog (`CameraConsentActivity`, not exported). It names the app label **and** package, and offers **Allow / Only this time / Don't allow**. If a background activity start is blocked, a heads-up notification opens it on tap. Dismissing it, or 60 s without an answer, refuses this time and stores nothing |
| after 9 | The runtime package must hold `CAMERA` itself. If it does not, the first allowed start shows Android's permission request from the same activity (or the DisplayXR app's **3D camera** card). If the user refuses, the start returns `XR_ERROR_PERMISSION_INSUFFICIENT` |

**Revoking.** The DisplayXR app's **3D camera** card lists every stored decision; a tap forgets
it, and the app is asked again next time. adb, guarded by `android.permission.DUMP` (the shell
holds it, no third-party app can):

```
adb shell am broadcast -a org.displayxr.stereo_camera.CONSENT --es op list \
    -n <runtime package>/org.freedesktop.monado.ipc.StereoCameraConsentStore\$Receiver
... --es op revoke --es package com.example.app     # every certificate of that package
... --es op revoke-all
```

### 7.2 Foreground rule

Frames are published to a stream only while its client is **visible**, checked per published
frame with a 250 ms cache:

- **Window-bearing classes (APP, PRESENT_OWNER):** the verified peer pid must own a visible,
  non-minimised top-level window (Windows `EnumWindows`; macOS `NSRunningApplication` not hidden
  and not activation-prohibited; desktop Linux has no display connection in the service and is
  always visible — the OS lock below is the gate there). Otherwise the stream is `SUSPENDED` for
  that client: no new frames, pinned slot cleared so the last image does not linger.
- **Delegating clients** follow their own visibility rule (a capturing tab keeps its indicator;
  closing it stops the track) — only when the stream's start was allowed by delegation (§7.1.1). **`CAMERA_CONSUMER` and `DIAG`** have no window by contract and
  are exempt — they were granted explicit consent instead.
- **Android:** the runtime keeps its own foreground rule. Do not rely on the OS: on a retail
  Android 12 tablet the camera service did not take the camera from the satellite when its client
  went to the background, and a reopen in the background streamed at full rate (measured). While a
  stream is started, the satellite polls its own process importance. A satellite serves one
  package, bound with `BIND_IMPORTANT | BIND_ABOVE_CLIENT`, so it is `IMPORTANCE_FOREGROUND`
  (`BOUND_TOP`) exactly while that package is the top app. Anything lower suspends every stream in
  that satellite (`SUSPENDED`, no frames), and they resume on return. The satellite still has
  **no** `foregroundServiceType="camera"`. A source that had delivered and then lost its device to
  the OS (where the OS does enforce while-in-use, or sensor privacy) is also reported `SUSPENDED`,
  not `UNAVAILABLE`, while the service retries, and its started streams are kept.
  **Front camera (option R):** a `USER_FACING` camera
  starts only for a consumer whose uid presents woven 3D in the same runtime process: a weave
  submit within the last second, a visible session, or an `XR_DXR_weave` present-owner connection
  while the satellite's own importance says its app is on top and the screen is unlocked. The last
  case is a call page: its only 3D content is the camera preview, so it cannot submit a weave
  before it has frames. A start waits up to 3 s for one of these to become true (a browser asks
  ~100 ms before its first weave when it returns from the background). Otherwise the start returns
  `XR_ERROR_STEREO_CAMERA_BUSY_DXR`. A started front stream gets no frames, and its connection is
  told `SUSPENDED`, while that stops being true. While the front pair is open it is also what keeps
  the display tracking the viewer, so an app in the background can never take it. The rule holds
  under the developer override too (§7.1.2 row 2). Rear cameras have no such rule.
- **Session lock:** every stream is suspended while the OS session is locked or switched away
  (Windows `WM_WTSSESSION_CHANGE` lock / console or remote disconnect, plus `SM_REMOTESESSION` at
  start; macOS `com.apple.screenIsLocked` and `NSWorkspaceSessionDidResignActive`; Android
  `ACTION_SCREEN_OFF`, then `ACTION_USER_PRESENT`, or `ACTION_SCREEN_ON` with no keyguard), and
  resumes on unlock. Enumerated and event-reported state is `SUSPENDED` meanwhile (§7.6).

### 7.3 Indicator

While any stream is started, the service shows an in-use indicator: Windows tray icon with a red
dot, tooltip and menu line *"3D camera in use by <app>"* (executable base names), one balloon on
the transition; macOS menu-bar icon with a red dot, tooltip and menu line. The indicator is owned
by the runtime because the camera LED, if any, is lit by tracking and says nothing about who is
receiving frames. Android: an ongoing notification *"3D camera in use — Used by <app label>"* with
a **Stop** action (= §7.4 "Stop camera sharing"), posted by the runtime process that serves the
stream. The status-bar privacy dot names the runtime, not the consumer, so the notification is
the only place the user sees who receives the frames.

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
- `XrEventDataStereoCameraStreamEndedDXR` — a stream the service ended (§7.4; reasons `USER_STOPPED`, `DISABLED`, `SOURCE_LOST`, `SOURCE_BUSY`), with the handle
  and the reason.
- `XrEventDataStereoCamerasChangedDXR` (spec v4) — the enumerated set changed (§3, "The list
  is live"): to every connection that has enumerated or created a stream. Coalesced: at most one
  is pending per connection.
- `XrEventDataStereoCameraStillReadyDXR` / `XrEventDataStereoCameraStillFailedDXR` (spec v8) — a
  still the connection requested completed (§5.7). Never coalesced.
- Spec v8: a connection whose stream's frames stop under the **foreground rule** (§7.2) is now also
  told `XrEventDataStereoCameraStateChangedDXR` `SUSPENDED`, and the camera's state when they resume
  (the lock and the sharing toggle already were). It is the signal that a controls hold was lost
  (§5.6).

## 8. Errors

| Result | When |
|---|---|
| `XR_STEREO_CAMERA_FRAME_NOT_READY_DXR` (success) | no frame newer than the last acquired |
| `XR_ERROR_VALIDATION_FAILURE` | struct out of contract (unknown camera, format/transport not in the supported bits, RECTIFIED on a camera the service cannot rectify); (v8) a mode or still size the camera does not list, a control value out of range, an unknown still id |
| `XR_ERROR_PERMISSION_INSUFFICIENT` | the OS camera privacy switch denies the app (§7.1 row 4, delegating clients included); RAW asked by a browser / delegating client; a client class that may not use cameras (or `xrCreateSession` from a `CAMERA_CONSUMER`); (v8) controls or a still on a stream whose frames are withheld right now (§7.2), a RAW still to a browser / delegating client |
| `XR_ERROR_STEREO_CAMERA_CONSENT_REFUSED_DXR` | §7.1: the user / the stored decision refused, the prompt was unanswered or unavailable; retryable after consent. A browser maps it to `NotAllowedError`. A registered delegating client gets it too when its delegation was skipped — it declined (`DECLINE_DELEGATION_BIT`, §2a) or its entry is untrusted (§7.1.1) — and no stored Allow / prompt answer allowed it |
| `XR_ERROR_STEREO_CAMERA_DISABLED_DXR` | §7.4: sharing is off (user toggle or `DXR_STEREO_CAMERA=0`) |
| `XR_ERROR_STEREO_CAMERA_BUSY_DXR` | the plug-in could not open the source and its retry is pending; retry later. A browser maps it to `NotReadableError`. (v8) a start no single listed mode can serve together with the camera's started streams (§5.5) |
| `XR_ERROR_STEREO_CAMERA_STREAM_ENDED_DXR` | start / acquire on a stream the service ended (§7.4); destroy it and create a new one |
| `XR_ERROR_FEATURE_UNSUPPORTED` | GPU transport requested on a platform without it; (v8) a control the camera does not have, a still on a camera without still sizes |
| `XR_ERROR_LIMIT_REACHED` | more than 8 streams per camera service-wide; (v8) a still requested while the stream's previous still is pending or unreleased |
| `XR_ERROR_STEREO_CAMERA_CONTROL_HELD_DXR` (v8) | another stream holds this camera's controls (§5.6) |
| `XR_ERROR_CALL_ORDER_INVALID` (v8) | controls or a still on a stream that is not started; a still acquired before its ready event |
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

struct xrt_plugin_stereo_camera_calibration {   // RAW calibration of THIS camera index's device
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
block up to `timeout_ns`; it never throws (C boundary). `enumerate` is called again during the
plug-in's life (spec v4, §3): on a client's enumerate (at most once a second) and every ~2 s
while a client listens — keep it cheap and log only when the set changes. Indices stay stable
while a device is listed and are never handed to another device; a device that appears may be
appended at any time; one that goes away may be dropped from the list (the service reports it
`UNAVAILABLE` and never opens its index) or kept at its index (an append-only registry; its
`open` waits / fails). No change counter or callback slot is needed: the service compares by
`device_identity`.

`xrt_plugin_stereo_camera_info::monitor_id` (appended, spec v5, #1961; announced by
`XRT_PLUGIN_STEREO_CAMERA_INFO_HAS_MONITOR_ID`) is the `xrt_display_descriptor::monitor_id` of
the screen the camera belongs to, as this plug-in claimed it, or 0. Write it only when the
runtime's `struct_size` covers it; a plug-in built against an older header simply leaves it 0.
Apps see it as `XrStereoCameraDisplayDXR::displayId`.

`xrt_plugin_stereo_camera_info::frame_orientation` (appended, spec v7; announced by
`XRT_PLUGIN_STEREO_CAMERA_INFO_HAS_FRAME_ORIENTATION`) is an
`enum xrt_plugin_stereo_camera_frame_orientation`: `UNKNOWN = 0`, `_0 = 1`, `_90 = 2`,
`_180 = 3`, `_270 = 4` — the clockwise turn that makes the frame the plug-in delivers upright in
the device's natural orientation, after any turn the plug-in applied itself (§3). The encoding
is offset on purpose so that **0 is unknown**: a plug-in built against an older header, or one
that does not write the field, reads as unknown and the runtime reports
`XR_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN_DXR`; a value outside the enum is treated the same.
Same `struct_size` gate as `monitor_id`. The plug-in knows the value (the sensor's mounting and
whether it turns the frame before packing); the runtime never derives it. Enumerate/calibration are cheap and
callable from any thread. `SUSPENDED` from `wait_frame` maps to the camera state (§3). The
plug-in owns decoding its transport and resolving calibration for its own device; the runtime
owns threads, fan-out, rectification, format conversion, transport, consent and indicator.

| Runtime owns | Plug-in owns |
|---|---|
| camera thread, open refcount, linger | reading frames from its stack without taking the device |
| rectification (`u_stereo_rectify`), format conversion, decimation | decoding (JPEG, YUY2 …) to GRAY8/NV12/BGRA8 |
| rings, wake handles, pinning, GPU transports | frame sequence + best timestamp it has |
| authorisation, foreground rule, indicator, kill switch | calibration of **each camera's own device**, by device identity |
| re-enumeration, stable ids, the cameras-changed event | listing devices as they come and go |
| events, stats, CLI | tracker keep-alive while open; shedding camera before tracking |

## 9b. Plug-in contract, spec v8 (modes, controls, stills)

Eight more slots appended to `struct xrt_plugin_iface` after `get_setting_status` (ADR-020: gated
by `struct_size`, no ABI bump), announced by `XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_MODES`,
`_CONTROLS` and `_STILL`. **Each slot is optional on its own**; a missing one is the v7 behaviour
for that feature. Every slot that takes an open `struct xrt_plugin_stereo_camera *` is called from
that camera's single runtime-owned camera thread — the one that calls `wait_frame` — never
concurrently with it, so a plug-in needs no locking of its own for them. The authoritative header is
`src/xrt/include/xrt/xrt_plugin.h`; `tests_stereo_camera` pins the slot order.

```c
// Modes (§5.5). Mode 0 MUST be the default (= info.eye_width x eye_height), else the list is ignored.
struct xrt_plugin_stereo_camera_mode { uint32_t struct_size; uint32_t eye_width, eye_height;
                                       float max_frame_rate; uint32_t flags; /* MODE_STREAM | MODE_STILL */ };
uint32_t     (*stereo_camera_enumerate_modes)(inst, uint32_t index, uint32_t capacity,
                                              struct xrt_plugin_stereo_camera_mode *out);   // camera closed, any thread
xrt_result_t (*stereo_camera_set_mode)(cam, uint32_t mode_index);                        // reconfigure the OPEN camera

// Controls (§5.6).
struct xrt_plugin_stereo_camera_control_caps { uint32_t struct_size; uint32_t supported_controls,
    supported_focus_modes; float min_focus_distance_diopters; uint32_t focus_distance_calibration;
    float exposure_compensation_min_ev, exposure_compensation_max_ev, exposure_compensation_step_ev;
    uint32_t default_focus; float default_focus_distance_diopters; };   // appended: XRT_PLUGIN_STEREO_CAMERA_CONTROL_CAPS_HAS_DEFAULT_FOCUS
struct xrt_plugin_stereo_camera_controls { uint32_t struct_size; uint32_t valid_controls, focus_mode,
    focus_trigger; float focus_distance_diopters, metering_x, metering_y, exposure_compensation_ev; };
xrt_result_t (*stereo_camera_get_control_caps)(inst, uint32_t index, struct xrt_plugin_stereo_camera_control_caps *out);
xrt_result_t (*stereo_camera_set_controls)(cam, const struct xrt_plugin_stereo_camera_controls *controls);

// Per-frame capture state (§5.6), called after WAIT_OK and before release_frame.
struct xrt_plugin_stereo_camera_capture_result { uint32_t struct_size; uint32_t valid_fields;
    float focus_distance_diopters; uint32_t focus_state; int64_t exposure_time_ns;
    uint32_t sensitivity_iso, reserved; int64_t eye_time_ns[2]; };
xrt_result_t (*stereo_camera_get_capture_result)(cam, struct xrt_plugin_stereo_camera_capture_result *out);

// Stills (§5.7).
struct xrt_plugin_stereo_camera_still_request { uint32_t struct_size; uint32_t eye_width, eye_height, reserved;
                                                uint64_t request_id; };
struct xrt_plugin_stereo_camera_still { uint32_t struct_size; uint32_t width, height, format;
    const uint8_t *planes[2]; uint32_t pitches[2]; int64_t time_ns; uint32_t time_is_exposure,
    flags; };   // flags: was `reserved` (same offset); XRT_PLUGIN_STEREO_CAMERA_STILL_HAS_FLAGS
xrt_result_t (*stereo_camera_request_still)(cam, const struct xrt_plugin_stereo_camera_still_request *req);
uint32_t     (*stereo_camera_poll_still)(cam, struct xrt_plugin_stereo_camera_still *out,
                                         struct xrt_plugin_stereo_camera_capture_result *out_result);  // NONE / READY / FAILED
void         (*stereo_camera_release_still)(cam);
```

Rules a plug-in must keep:

- **Modes.** List mode 0 = the default. Every other mode must have the **default's horizontal field
  of view about the same optical axis, with square pixels**; its vertical extent may differ — a
  centred vertical crop or extension of the default (e.g. the full 4:3 sensor, 1280×960, beside a
  16:9 default cut from its centre, 1280×720). Do **not** list a mode that crops horizontally, is
  off-centre, or has non-square pixels: the runtime derives every mode's calibration from the one
  you return for the default (`s = w / w_default`, `f' = f·s`, `cx' = (cx + ½)·s − ½`,
  `cy' = (cy + ½ − h_default/2)·s + h/2 − ½`, distortion unchanged) and would silently describe such
  a mode wrongly (it drops only modes whose relative height is outside [½, 2] or whose derived
  principal point leaves the frame). List only modes the source can really run now — a
  tracking-shared camera lists none that would hurt tracking. A camera opens in mode 0;
  the runtime calls `set_mode` right after `stereo_camera_open` (before the first `wait_frame`) when
  it wants another, so **configure the capture session lazily**. `set_mode` on an open camera is a
  reconfigure: keep the sequence counting, pause frames at most 500 ms, deliver frames of the new size
  from then on (a frame of the old size still in flight is fine: the runtime sizes everything from
  the frame). An error leaves the camera where it was. A plug-in that lists several STREAM modes
  without `set_mode` is listed with mode 0 only. STILL-only sizes need no `set_mode`.
- **Controls.** `get_control_caps` advertises only what the source does; the runtime never sends an
  unadvertised control. `set_controls` always receives the **whole** wanted state (including
  `valid_controls == 0` the moment the holding app is backgrounded, locked out or stops — apply it
  promptly: the camera must be back on its defaults for whoever is in front): a field whose bit
  is clear means "the source's own default", and `valid_controls == 0` (the app released, stopped or
  left) means back to the defaults. AUTO_ONCE is edge-triggered by `focus_trigger`: start a new search
  whenever it changes. The metering point is normalised in the **raw left eye of the frame the plug-in
  delivers** (the runtime maps the app's rectified point back). Re-sent after each open while held.
- **Capture state.** `get_capture_result` describes the frame the last `wait_frame` returned (focus
  distance in the unit the caps declare, focus state, exposure, ISO, per-eye exposure start on the
  `os_monotonic_get_ns` clock); set a `valid_fields` bit only for what is known. Set
  `XRT_PLUGIN_STEREO_CAMERA_RESULT_EYE_TIMES` **only when each lens is stamped on its own**; a HAL
  that stamps both lenses identically leaves it clear.
- **Mode rates.** `max_frame_rate` only when you really know it; 0 = not known yet (do not copy a
  HAL table you cannot trust). The runtime measures each mode once it runs.
- **Focus on a pair.** If the source's own autofocus cannot drive a pair, advertise only `MANUAL` +
  `LOCKED` (+ the distance control) and say what you hold with no control set:
  `default_focus = XRT_PLUGIN_STEREO_CAMERA_DEFAULT_FOCUS_FIXED` and
  `default_focus_distance_diopters` (`_AUTO` when the source focuses by itself; leaving 0 = unknown).
  Write them only when `struct_size` covers them.
- **Lossy stills.** Set `XRT_PLUGIN_STEREO_CAMERA_STILL_LOSSY_SOURCE` in the still's `flags` when its
  pixels were lossy-compressed before you decoded them (e.g. a per-lens JPEG output).
- **Stills.** `request_still` returns at once; the runtime polls `poll_still` after every
  `wait_frame` while one is outstanding (one at a time per camera). READY hands over ONE RAW SBS pair
  in a native format (GRAY8 / NV12 / BGRA8), of the requested size **or smaller** (protecting
  tracking) with the same horizontal field of view (its calibration is derived by the §5.5 rule from
  its actual size), planes valid until `release_still`; FAILED ends the request. The
  stream's frames may pause for at most 1 s around a still and must resume by themselves. The runtime
  gives up after 10 s (and closes nothing).

| Runtime owns (v8) | Plug-in owns (v8) |
|---|---|
| which mode the source runs (largest started), downscaling smaller streams, the rectifier per source size | listing modes it can run, switching on `set_mode` |
| control ownership, consent / foreground gating, range checks, rectified → raw point mapping | applying the whole control state, AF triggers |
| per-frame result transport, still sections, still rectification + conversion, events, timeouts | capture state per frame, capturing the still pair (or refusing / downsizing it) |

## 9a. Service-owned UVC side-by-side source (ADR-043 Amendment 4)

A plain stereo webcam that sends **one side-by-side image** over standard UVC belongs to no
display vendor, so it gets no plug-in: the **service** reads it with its own vendor-neutral
capture code and exposes it like any other camera — same entry points, same consent path and
`CAMERA_CONSUMER` rules, same foreground rule, indicator, kill switches, linger, rings and
rectifier. Code: `auxiliary/util/u_stereo_uvc.{h,c}` (config, matching, SBS split, nominal
model, calibration files, the fake backend — pure, tested on every OS by `tests_stereo_uvc`),
`auxiliary/os/os_uvc_capture*.{h,c,cpp}` (the OS call), and the `src_*` dispatch in
`ipc_server_stereo_camera.c` (the only place the manager tells the two sources apart).

**Frame orientation (spec v7): UNKNOWN.** A desktop stereo webcam has no natural orientation the
service can know — it sits on a monitor however the user mounted it — so the source claims
nothing and consumers apply their own default (on the desktop, none).

**Opt-in only.** With no config the source enumerates nothing and touches no device. The SERVICE
reads, once at start:

| Where | |
|---|---|
| Windows | `%LOCALAPPDATA%\DisplayXR\stereo-cameras.json` |
| Linux / macOS | `$XDG_CONFIG_HOME/displayxr/stereo-cameras.json`, else `~/.config/displayxr/stereo-cameras.json` |
| override | `DXR_STEREO_CAMERA_UVC_CONFIG=<path>` in the service's environment; `DXR_STEREO_CAMERA_UVC=0` turns the source off |

```jsonc
{"uvc": [{
   "match": "Eyes",             // friendly-name substring, case-insensitive  } at least one;
   "vid_pid": "2bdf:0281",      // USB VID:PID, hex                           } both = both must hold
   "name": "Stereo webcam",     // display name ("" = the device's own)
   "layout": "sbs-half",        // "sbs-full": each half one full-res eye; "sbs-half": each half an eye squeezed 2:1
   "eyes": "lr",                // "lr" = the device's left lens in the left half; "rl" = swapped
   "mode": "3840x2160@60",      // capture mode; omitted = the largest, then fastest, the device lists
   "eye_size": "1280x720",      // per-eye output; omitted = see "Frames"
   "baseline_mm": 60,           // nominal lens separation (omitted: 60 internally, reported 0)
   "hfov_deg": 90,              // nominal per-eye horizontal FOV (omitted: 70 internally, reported 0)
   "calibration": "C:/cal/eyes.yaml"   // optional OpenCV-style calibration (below)
}]}
```

(The file itself is strict JSON — no comments.) Up to 4 entries; any malformed value fails the
whole file (logged, nothing claimed): a typo must never claim a different camera. **No built-in
allowlist** ships: a device is listed only with a public spec of its SBS layout, and none was
available.

**Which device.** Entries are resolved in order, one device each: the first present device that
matches and is not already claimed. A **name-only** entry matching several devices claims none
(a vendor prefix shared by a display's tracking camera and the webcam must not pick one by
enumeration order). **A device a display plug-in reports as its camera is never claimed**: plug-in
cameras are enumerated first, and a device whose OS id, or whose USB VID:PID, equals a plug-in
camera's `platformDeviceHint` (e.g. `"04f2:b70c"`) is skipped.

**Frames.** The backend delivers decoded NV12 or YUY2 (Windows: an async `IMFSourceReader` with
advanced video processing; the reader keeps only the newest sample) — preferably already at the
output size, 2 × eye width × eye height (below). The source splits each frame into an NV12 SBS image of two
square-pixel eyes, LEFT lens left: SBS_FULL eyes are `(W/2) × H`, SBS_HALF eyes `(W/2) × (H/2)`
(a 2:1-squeezed half carries that much information; the vertical 2:1 is an exact two-row
average), both capped at 1280 px wide with the aspect kept, unless `eye_size` says otherwise
(integer box pre-reduction + bilinear). Timestamps are arrival time (`captureTimeIsExposure`
FALSE). Flags: `USER_FACING`, plus `CALIBRATED` with a calibration file; never
`SHARED_WITH_EYE_TRACKING`. `platformDeviceHint` = the OS device id (Windows: the MF symbolic
link), so a capture stack can hide the raw webcam it also sees. The device is opened on the first
started stream and closed after the 2 s linger; an open that fails (another app holds it) makes
the camera `UNAVAILABLE`, and a start meanwhile gets `XR_ERROR_STEREO_CAMERA_BUSY_DXR`.

**Calibration.** None (the usual case): the nominal model of §4 — fx = fy =
`(eyeWidth/2) / tan(hfov/2)`, principal point `((w−1)/2, (h−1)/2)`, no distortion, R = I,
T = (−baseline, 0, 0) mm. RECTIFIED frames are the delivered frames unchanged (no remap, no
zoom) until the online vertical refinement (§4, R2) — which runs on them exactly as on a
calibrated camera — folds a correction in; from then on they go through the corrected LUT and
the rectified calibration describes that geometry. A `calibration` file makes the camera
`CALIBRATED`: OpenCV FileStorage YAML (`K1: !!opencv-matrix ... data: [...]`) or JSON, keys
`K1`/`M1`, `D1`, `K2`/`M2`, `D2`, `R` (3×3 or a rotation vector), `T` (mm; |T| < 1 is taken as
metres), optional `image_width` / `image_height` (default: the raw per-eye size `mode/2 × mode`,
i.e. the squeezed half for SBS_HALF — an anisotropic K is fine), optional
`distortion_model: fisheye` for 4-coefficient KB4; 4–5 coefficients are RADTAN5, 8 RADTAN8. A
file that cannot be read or parsed is logged and the nominal model is used.

**Backends.** Windows: Media Foundation (`mf`, `mfplat`, `mfreadwrite`, `mfuuid`, `d3d11`, `dxgi`,
linked only into the service and `displayxr-cli`, via `aux_os_uvc`). **Decode path** — a 4K60 MJPEG
webcam is the expensive case (first hardware run: 11.3 Hz, mostly a CPU split of the full 4K frame):

| Path | What runs | When |
|---|---|---|
| **Hardware** (default) | a D3D11 device (`VIDEO_SUPPORT`, multithread-protected) on the adapter that has a hardware MJPEG decoder MFT (matched by PCI vendor; the most dedicated memory wins, `DXR_STEREO_CAMERA_UVC_ADAPTER=<substring>` picks), handed to the reader as `MF_SOURCE_READER_D3D_MANAGER` + `MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS`; NV12 is requested **at the output size**, so the GPU decodes **and** downscales (half-SBS 3840×2160 → 2560×720) — with an anamorphic output pixel aspect (`PAR_out = PAR_src · src_w·out_h / (src_h·out_w)`, 1:2 here), because the video processor otherwise preserves the display aspect and **letterboxes** (the first real-webcam run got a centred ~1280 px picture between black bars); readback is **pipelined** over a ring of 3 staging NV12 textures: each read queues a `CopySubresourceRegion` + event query and delivers the newest copy that has already finished (waiting, by polling — never a blocking `Map` that would hold the device lock MF's decoder needs — on the older one only when two are in flight), one `Map`, copy out, `Unmap`, the sample returned at once. A synchronous copy measured 22–25 ms per frame on the real webcam (the copy waits behind the decode) and capped it at 36 Hz | an adapter with a hardware MJPEG decoder exists, and MF actually inserted a hardware decoder (checked after the topology is built; a software decoder behind the device manager is reopened on the software path) |
| **Software** (fallback, one WARN with the reason) | the plain reader; MF's multithreaded MJPEG decoder + video processor, also asked for the output size | no capable adapter, the hardware open failed, or `DXR_STEREO_CAMERA_UVC_DECODER=software` |

`DXR_STEREO_CAMERA_UVC_DECODER=hardware` refuses the fallback (the open fails instead). The service
logs which ran, per open: `uvc capture (Media Foundation): started 3840x2160@60.00 — decoder NVIDIA
MJPEG Video Decoder MFT (hardware), HARDWARE (GPU decode + scale) path, reader output NV12
2560x720`, and at close the frame count and mean readback. `displayxr-cli camera uvc-devices
--decoder` prints the policy, the decoders registered and the adapter it would use, and
`displayxr-cli camera uvc-decode-bench <file>` runs a media file (a 4K MJPEG AVI) through the
identical open / read / split path with requests paced to the file's frame rate — no camera.
The bench also checks the picture: per eye, the mean luma of the outer 5 % columns against the
centre, and exits 4 on dark side bars (`tests_uvc_capture_mf` asserts the same through the real
backend on a generated YUY2 AVI, both policies). On the development laptop (RTX 3080 + Intel iGPU,
a bright 3840×2160@60 half-SBS MJPEG file, out 2560×720, no bars on any path): hardware Intel
60.0 Hz (readback 0.7 ms), hardware NVIDIA 51–53 Hz (readback 6–7 ms — a file lets the
asynchronous NVIDIA MFT decode ~5× ahead of the paced requests and the copies queue behind that
backlog; a 60 Hz camera has none), software with MF scaling 59.7 Hz (0 superseded, CPU-heavy); the source's own 4K → 2 × 1280×720 resample,
used only when no scaler is available, costs ~25 ms (was ~110 ms), a pre-scaled frame 0.3 ms.

Desktop Linux: **V4L2** (`os_uvc_capture_v4l2.c`, kernel uapi headers only, linked into the same two
processes plus the sim_display plug-in). *Enumerate* reads sysfs + udev's database only — no
`/dev/video*` node is opened to list devices (capture vs metadata node from udev's
`ID_V4L_CAPABILITIES`, else the sysfs `index`; USB VID:PID + product from the interface's parent;
the name is the V4L2 card name, or `<USB product> (<card>)` when the kernel's 31-byte card name
lost the product, so a `match` written against the Windows friendly name works on both; USB nodes
are listed before platform/ISP nodes so the 16-device cap never drops a webcam; the id is
`/dev/videoN`). *List modes* opens the matched node for `ENUM_FMT` / `ENUM_FRAMESIZES` /
`ENUM_FRAMEINTERVALS` and closes it — no format set, no stream. *Open*: the exact size at the
closest rate, NV12 > YUYV > MJPEG on ties (Windows' rule), `S_FMT` + `S_PARM`, 4 mmap'd buffers,
`STREAMON`; *read* polls, drains every completed buffer and keeps the newest (latest-wins without a
reader thread); NV12 / YUYV planes point into the dequeued buffer until the next read. MJPEG is
decoded on the CPU (`os_uvc_mjpeg.c`): **libturbojpeg** (`dlopen`'d — no new `DT_NEEDED`, the
`.deb` soname list is unchanged) straight to the JPEG's Y/Cb/Cr planes with DCT-domain downscaling
towards the source's output size, else a built-in **stb_image** decoder (full size, slow; one WARN
at open naming the `libturbojpeg` package); frames without Huffman tables (UVC's AVI1 convention)
get the Annex K tables inserted. `DXR_STEREO_CAMERA_UVC_DECODER=hardware` refuses MJPEG modes
(there is no hardware decoder on Linux), `auto` = `software`. Kernel `CLOCK_MONOTONIC` buffer
timestamps are used as-is (`time_is_exposure` when the driver flags start-of-exposure). Unplug:
`POLLERR`/`POLLHUP` + `ENODEV` → `READ_ERROR` (one WARN) → the manager ends the stream, as on
Windows. `uvc-decode-bench` takes a concatenated `.mjpeg` stream there. Tests:
`tests_uvc_capture_v4l2` (enumeration from a fake sysfs/udev tree, format selection, the decoder
incl. DHT insertion and both decoders, the real backend on a `file:` stream). **Validated on
hardware** only against a mono laptop webcam forced through an `sbs-full` entry (ds1-linux,
2026-10-18): enumeration opened no node, 2880×1800 MJPEG at the camera's own ~16.7 Hz
(low light) with libturbojpeg (18 ms/frame decode, 0 superseded) vs 8.9 Hz with the built-in
decoder (97 ms/frame), 640×480 YUYV raw at 16.7 Hz, frames correct in the probe PNGs. **Not
validated:** a real SBS stereo webcam (no `--rectified` pass is possible on a mono camera),
hot-unplug, NV12-native devices, the sim_display webcam tracker on Linux. Other platforms: none. A config with `"fake": true`
(or `{"disparity": 0.04, "dy": 0.005, "pixel": "nv12"|"yuy2", "fps": 30}`, fractions of the eye
width / height) REPLACES real capture with a synthetic device ("DisplayXR synthetic SBS camera",
id `dxr-fake-uvc`) that renders a known disparity and a known right-eye vertical offset in any
layout — the hardware-free path through the whole manager (`tests_stereo_camera_manager`, and
`camera probe --rectified` against a service on any OS).

**Tracker-camera denylist (ADR-043 Amendment 5).** Before any config entry is matched, the source
marks unavailable every device that (1) a plug-in camera names in `platform_device_hint`, (2) the
active display plug-in declares through `xrt_plugin_iface::get_claimed_cameras` (USB `"vvvv:pppp"`
or OS device ids; optional slot, ADR-020 append-only, `XRT_PLUGIN_IFACE_HAS_CLAIMED_CAMERAS`), or
(3) is on the runtime's small built-in list (a few VID:PIDs plug-ins already reported as tracker
cameras; the generic names "tracking camera" / "eye track"). The denylist wins over every entry,
an explicit `vid_pid` included: such a device is never claimed or opened, and each entry that
would have matched logs `stereo camera (uvc): config entry N matches "<name>" (VID:PID …, <id>)
— REFUSED: <reason>`. `camera uvc-devices` marks built-in refusals.

**Diagnostics.** (See also the decode-path tools above.) `displayxr-cli camera list` tags each camera `(source: plugin|uvc)` (`"source"` in
`--json`; the wire carries it in the former `reserved` word of the properties — the OpenXR struct
is unchanged). `displayxr-cli camera uvc-devices [--modes]` runs locally (no service): the config
path and parse result, every capture device present with its VID:PID and OS id, which entry
claims it, ambiguous entries, and with `--modes` the capture modes of the CLAIMED devices only and
the per-eye output each would give (the device object is created to read its media types, never
started). Service log: one WARN per claimed device (`stereo camera (uvc): CLAIMED ...`), the
nominal-model line (`NOMINAL model (uncalibrated) — RECTIFIED = the frames as delivered ...`),
then the usual per-camera, per-stream and refinement lines.

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

**Spec v8 (as built).** The fake also implements every §9b slot, with no extra knob:

- **Modes:** the configured size (default, STREAM + STILL), half of it (STREAM), double (STREAM +
  STILL, capped at 4096 px), and the other aspect at the same horizontal field of view: a default
  wider than 4:3 (`_SIZE=640x360`, the tablet's shape) adds the full 4:3 sensor as a STREAM mode
  (640×480) and a double-size 4:3 STILL size (1280×960); a 4:3 default (the 640×480 default) adds its
  centred 16:9 cut as STREAM modes (640×360, 1280×720). `set_mode` re-renders the scene at the new
  size; fx scales with the width and, on `_DISTORT=1`, every mode's lenses are the default's carried
  by the §5.5 rule (`sim_stereo_camera_truth_init_ex`), so the derived calibration is exact.
- **Controls:** all four, every focus mode, nearest focus 10 D (`APPROXIMATE`), EV −2…+2 in ⅓ steps.
  The fake lens moves towards its target with a 150 ms time constant and reports `SEARCHING` until it
  is within 0.01 D: MANUAL goes to the asked distance, AUTO_ONCE / CONTINUOUS_AUTO to the depth under
  the metering point (the bar at 0.6 m = 1.667 D, else the background at 2 m = 0.5 D), LOCKED holds.
  EV scales the reported exposure time (16.7 ms · 2^EV), ISO 100, right eye exposed 0.2 ms after the
  left.
- **Stills:** rendered at the still size after a 200 ms capture latency during which `wait_frame`
  returns no frame (the documented pause), through the same raw lenses on the `_DISTORT=1` fake.

On this Mac (Debug service, `DXR_DP_SIM_DISPLAY_STEREO_CAMERA=1`): `camera set focus-mode manual
focus 4 ev 1` shows the focus settle 0.5 → 3.41 → 3.90 → 3.98 → 4.00 D (LOCKED) with the exposure
doubled; `camera set focus-mode once point 0.5,0.5` settles on the bar at 1.66 D; a `--mode
1280x960` probe sees 24 / 79 px disparities (2.00 m / 0.60 m from the mode's own calibration), a
`--mode 320x240` probe 6 / 20 px. On `_DISTORT=1`, a 1280×960 `--rectified` probe and a concurrent
default `--rectified` probe (served downscaled from the 1280×960 rectified source) both PASS row
alignment; `camera still` returns a 2560×960 RECTIFIED still with |dy| median 0.014 px and depths
1.998 m / 0.600 m from its own calibration, and a RAW still keeps the 2 px raw misalignment.
With `_SIZE=640x360 _DISTORT=1` (16:9 default, 4:3 sensor): a 640×480 `--rectified` probe and a
concurrent default (16:9) `--rectified` probe make the source run 640×480; the 16:9 stream, served
from its centred rows, PASSes (|dy| median 0.016 px, 2.005 m / 0.599 m from the 16:9 numbers), as
does the 4:3 one; `camera still --size 1280x960` returns a RECTIFIED 4:3 still with |dy| median
0.013 px and 1.998 m / 0.600 m from its own (derived) calibration.

## 11. Probe and diagnostics

```
displayxr-cli camera list [--json]                    # properties + state + source (plugin|uvc) + display +
                                                      # frame orientation (v7; JSON frameOrientationDeg, null = unknown)
displayxr-cli camera uvc-devices [--modes] [--decoder]  # §9a: UVC webcam config + devices + decode path (local)
displayxr-cli camera uvc-decode-bench <file>          # §9a: a media file through the webcam decode path
displayxr-cli camera calib <id> [--raw|--rectified] [--json]
displayxr-cli camera probe [<id>] [--raw] [--format gray8|nv12|bgra8] [--fps F]
                              [--frames N] [--seconds S] [--out DIR] [--mode WxH]
                                                      # rate, index gaps, SBS layout, block
                                                      # disparities, service stats; --out writes cam_<n>.png
                                                      # v8: --mode WxH streams that mode (depth from its calibration)
displayxr-cli camera modes [<id>] [--json]            # v8: modes + control capabilities
displayxr-cli camera set [<id>] <control> <value> ... [--seconds S] [--mode WxH]
                                                      # v8: focus-mode auto|once|manual|locked, focus <D>,
                                                      # point <x>,<y>, ev <EV>; takes the controls, prints the
                                                      # per-frame capture state (exit 9 = another stream holds them)
displayxr-cli camera still [<id>] [--size WxH] [--raw] [--format gray8|nv12|bgra8] [--mode WxH] [--out DIR]
                                                      # v8: one still end to end: latency, layout, capture state,
                                                      # calibration, its own row alignment + depth; --out writes still_<id>.png
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
when the scatter is small). Spec v8: `camera calib` also prints the §5.9 vertical refinement (`D(y)` normalised and in px at the
measured size, residual / matches / windows / updates, R1 / R2 quaternions; `--json`: `verticalRefinement`).

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

`selftest`'s `stereo_camera_caps` also checks the v8 answers of each plug-in camera (mode 0 = the
default, even non-zero sizes, sane rates, STILL sizes only with the still slots, control capabilities
consistent with themselves and with `set_controls` being there); absence never fails.

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
| 8 | A camera app, not only a call (appended; no public struct changes size; nothing renumbered). **Modes** (§5.5): `xrEnumerateStereoCameraModesDXR` → `XrStereoCameraModeDXR` (`XR_TYPE_STEREO_CAMERA_MODE_DXR` = 1004999319; flags `DEFAULT` / `STREAM` / `STILL`), every camera lists at least its default; `XrStereoCameraStreamModeInfoDXR` (1004999460) chained on the stream create info (absent = the default mode, v7 unchanged) or on the calibration query (numbers for that size); every mode has the default's horizontal field of view about the same axis with a possibly different vertical extent (a 4:3 mode beside a 16:9 default), its calibration derived from the default's (scale by width, principal point kept about the centre); the source runs the smallest mode serving every started stream, the others get their centred rows downscaled by the service (never letterboxed or stretched), a start no single mode can serve with the started streams is `BUSY`, a switch is a reconfigure gap (≤ 500 ms) and never ends a stream unless the source refuses it (then `SOURCE_BUSY` for the streams it cannot serve). **Controls** (§5.6): `XrStereoCameraControlCapabilitiesDXR` (1004999461) chained on the properties; `xrSetStereoCameraControlsDXR` (`XrStereoCameraControlsDXR`, 1004999462: focus mode, focus distance in dioptres, metering point in the stream's left eye, EV) / `xrReleaseStereoCameraControlsDXR`; one holder per camera (`XR_ERROR_STEREO_CAMERA_CONTROL_HELD_DXR` = −1004999468), started streams only, frames-withheld streams refused, and the hold is lost the moment the holder's frames are withheld (the client is told `SUSPENDED`, re-applies on `AVAILABLE`); `XrStereoCameraCaptureResultDXR` (1004999463) chained on a frame or a still. **Stills** (§5.7): `xrRequestStereoCameraStillDXR` (`XrStereoCameraStillRequestDXR`, 1004999464) → `XrEventDataStereoCameraStillReadyDXR` (1004999466) / `…StillFailedDXR` (1004999467) → `xrAcquireStereoCameraStillDXR` (`XrStereoCameraStillDXR`, 1004999465: a read-only section + layout, chained capture result + calibration) / `xrReleaseStereoCameraStillDXR`; one still per stream at a time; the stream may pause (≤ 1 s) and resumes by itself; a tracking-shared camera may refuse or downsize. Shared memory only; the `AHardwareBuffer` still transport is the next step (1004999469 held back). Hardened against the first real source (Leia Android): `XrStereoCameraStillDXR::sourceFlags` with `XR_STEREO_CAMERA_STILL_SOURCE_LOSSY_BIT_DXR` (a per-lens JPEG still), `XrStereoCameraControlCapabilitiesDXR::defaultFocusDistanceDiopters` (a pair's fixed default focus; −1 = not fixed / unknown) — both in what was tail padding, no size change; a mode's `maxFrameRate` 0 = not known yet, measured once it runs; per-eye capture times only when each lens is stamped on its own; a pair may advertise only `MANUAL` + `LOCKED`. Plug-in contract §9b: eight appended slots (`XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA_MODES` / `_CONTROLS` / `_STILL`), no ABI bump **Refinement for RAW consumers** (§5.9): `XrStereoCameraVerticalRefinementDXR` (`XR_TYPE_STEREO_CAMERA_VERTICAL_REFINEMENT_DXR` = 1004999470) chained on the calibration query; the refinement now also runs with only RAW streams started. The `rightFromLeft` convention is stated with its equations (§4) — no number changed. |
| 7, as built | No API change. NV12/BGRA conversions are BT.601 full range (§5.3). |
| 7 | `XR_STEREO_CAMERA_RECTIFIABLE_BIT_DXR` = `0x20` (appended): set by the runtime exactly when a RECTIFIED stream of the camera delivers rectified frames (natively rectified, or the service's rectifier accepted the calibration at registration). It is the pre-start signal a rectified-only consumer needs: `CALIBRATED` stays set on a pair whose geometry the rectifier rejected, and the RECTIFIED calibration query is a consent point, so neither can be used to filter a camera list. A plug-in's own value of the bit is ignored. Also `XrStereoCameraOrientationDXR` (`XR_TYPE_STEREO_CAMERA_ORIENTATION_DXR` = 1004999318), chained on `XrStereoCameraPropertiesDXR::next`: the clockwise rotation (0/90/180/270) that makes the published frame upright in the device's natural orientation, Camera2 `SENSOR_ORIENTATION` semantics, or `XR_STEREO_CAMERA_FRAME_ORIENTATION_UNKNOWN_DXR` (`0xFFFFFFFF`); the consumer pre-sets UNKNOWN (§3). Plug-in contract (§9): appended `xrt_plugin_stereo_camera_info::frame_orientation` (0 = unknown, `XRT_PLUGIN_STEREO_CAMERA_INFO_HAS_FRAME_ORIENTATION`); the UVC source reports UNKNOWN (§9a). `displayxr-cli camera list` prints it. No public struct changes size, nothing renumbered |
| 6 (A2, Android) | No API change. The Android rows of §7: the consent identity is the peer uid's package + signing-certificate digest, the trusted list is built into the runtime, and the consent dialog, the in-use notification with Stop, the lock watch, and "the OS took the camera" = `SUSPENDED` are runtime-owned. A front camera needs a live weave from the same app (`BUSY` otherwise), with or without the developer override |
| 6 | `XR_STEREO_CAMERA_STREAM_END_REASON_SOURCE_BUSY_DXR` = 4 (appended, nothing renumbered). A started stream whose source cannot start — the device is held by another process, errors on its first read (Media Foundation's shape for a busy UVC device), or (UVC source) delivers nothing within 5 s of opening — ends with this reason and the camera reads `UNAVAILABLE` until the service's retry succeeds; a new start meanwhile is `XR_ERROR_STEREO_CAMERA_BUSY_DXR` (a browser maps both to `NotReadableError`). Before this the stream stayed started at 0 fps with no error (runtime #2221). A source that stops after it delivered is unchanged (its streams wait through the retry). |
| 5 | `XrStereoCameraDisplayDXR` (`XR_TYPE_STEREO_CAMERA_DISPLAY_DXR` = 1004999317), chained on `XrStereoCameraPropertiesDXR::next`: the `XrDisplayDXR::displayId` of the screen a camera belongs to, 0 = unknown (§3). Plug-in contract (§9): appended `xrt_plugin_stereo_camera_info::monitor_id` (`XRT_PLUGIN_STEREO_CAMERA_INFO_HAS_MONITOR_ID`). A display topology change wakes the re-enumeration at once. `displayxr-cli camera list` prints the display. Nothing renumbered, no struct changes size (#1961) |
| 4 | No struct change. The camera list is live (§3): the service re-enumerates its sources during its run (on enumerate, at most once a second, and every ~2 s while a client listens), keeping each camera's index / `cameraId` / `persistentId`; new cameras are appended, vanished ones stay listed as `UNAVAILABLE` (streams end with `SOURCE_LOST`, start is `BUSY`) and come back with their id. `XrEventDataStereoCamerasChangedDXR` is delivered (it was reserved) to every instance that enumerated. Plug-in contract (§9): `enumerate` may be called repeatedly; calibration is per camera index, not "the active device" (#1961) |
| 3 (Amendment 4) | No API change. The service's own vendor-neutral UVC side-by-side source (§9a): opt-in per device by `stereo-cameras.json`, never a plug-in's camera; an uncalibrated pair is reported without `CALIBRATED`, its RAW calibration is refused, its RECTIFIED output is a pass-through aligned by the online refinement — so RECTIFIED now requires a camera the service can rectify rather than `CALIBRATED \| NATIVELY_RECTIFIED`. `displayxr-cli camera list` shows the source; new `camera uvc-devices` |
| 3 | Delegation hardening: `XR_STEREO_CAMERA_CLIENT_DECLINE_DELEGATION_BIT_DXR` (`0x2`) — a delegating client opts out of row 6 for one instance (§2a); a delegating entry for a user-writable executable path is applied only with a recorded signer its valid Authenticode signature matches (`<valueName>.signer`, Windows; POSIX path-only) and a user-level entry's first use per executable per service run is announced (§7.1.1). Appended; nothing renumbered |
| 2 (R3) | Consent and privacy as built (§7): `XrStereoCameraClientInfoDXR` + `XR_STEREO_CAMERA_CLIENT_CONSUMER_ONLY_BIT_DXR` (the `CAMERA_CONSUMER` class, §2a); distinct results `XR_ERROR_STEREO_CAMERA_CONSENT_REFUSED_DXR` / `_DISABLED_DXR` / `_BUSY_DXR` / `_STREAM_ENDED_DXR`; `XrEventDataStereoCameraStreamEndedDXR`; the state-changed event is now delivered; `persistentId` keyed with a per-user secret; read-only consumer section on every platform; `DXR_STEREO_CAMERA_DEV_ALLOW` becomes a documented dev override, `DXR_STEREO_CAMERA_PROMPT=0` disables the prompt. Everything appended at `1004999311–316`; nothing renumbered |
| 1 (R2) | No API change. RECTIFIED output and rectified calibration now come from the service's rectifier for any CALIBRATED, non-natively-rectified source; the browser is refused a camera it cannot rectify; sim_display `_DISTORT=1` fake; `camera probe` row alignment + `--rectified` |
| 1 | R1 implements: enumerate, calibration (RAW; RECTIFIED for natively rectified sources), streams with start/stop, latest-wins acquire over a pinned 3-slot shared-memory ring with per-stream wake handles, stream stats, plug-in slots, sim_display fake, CLI, selftest. Deferred: state-change events (structs defined, not delivered), GPU transports, rectifier (R2), consent/indicator (R3). Design scope: Enumerate + state events, calibration (raw / rectified), streams with start/stop, latest-wins acquire over a pinned 3-slot shared-memory ring with per-stream wake handles, optional GPU transports, runtime-enforced consent / foreground / indicator, plug-in iface slots, sim_display fake, CLI. |

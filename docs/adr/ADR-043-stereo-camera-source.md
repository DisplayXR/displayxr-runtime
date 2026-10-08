# ADR-043: A display's stereo camera is a plug-in-provided source, owned by the service and privacy-gated by the runtime

**Status:** Proposed (2026-09-25) · **R1 implemented** 2026-09-26 (runtime, hardware-free; maintainer
defaults recorded in the roadmap §G: stereo-only, iface slots, service clients only, no raw frames
to pages) · **R2 implemented** 2026-09-26 (the service-side rectifier, decision 2 below) · **Amendment 4**
2026-10-07 (a vendor-neutral UVC side-by-side source owned by the service) · introduces
[`XR_DXR_stereo_camera`](../specs/extensions/XR_DXR_stereo_camera.md) · appends optional
camera slots to `xrt_plugin_iface` under the [ADR-020](ADR-020-plugin-abi-compatibility-policy.md)
append-at-end rule · related: [ADR-019](ADR-019-vendor-plugin-aux-boundary.md),
[ADR-034](ADR-034-input-provider-plugins.md), [ADR-035](ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md),
[ADR-042](ADR-042-vendor-2d3d-conversion-supersedes-default.md) (vendor 2D→3D conversion — the same shape in reverse) ·
integration + plan: [roadmap/stereo-camera-source.md](../roadmap/stereo-camera-source.md)

## Context

The web SDK's 3D call module (`@displayxr/inline3d/call`, RFC 0002 in `displayxr-web`) sends
full-width L|R side-by-side video when the sender has a stereo camera. Most 3D laptops and
monitors have one — the camera their eye tracker looks through. Field facts (2026-09-25, two
machines):

- **While the vendor eye tracker runs, it holds the camera exclusively.** `getUserMedia` fails
  with `NotReadableError: Device in use`. Since tracking runs whenever the display is in 3D, a
  page effectively never gets the camera on the machines that most need it.
- **When the tracker was idle**, `getUserMedia` opened it as ONE device: 1280×480 @ 30 Hz =
  640×480 per eye, **greyscale, raw, unrectified** (visible vertical misalignment), with no
  calibration attached.
- **The vendor's own call application gets frames anyway** — not from the device, from the
  tracker: the vendor SDK publishes each tracker frame into named shared memory (a small local
  header `{format, compressedSize, height, width}` followed by one SBS JPEG), and frames only
  flow once a weave context has brought the tracker up. It rectifies with OpenCV from per-device
  `intrinsics.yml`/`extrinsics.yml` files. Two defects in that application are instructive:
  it picks the **first** device calibration folder on the machine (one test box had eleven), so a
  correct design must key calibration by the **active** device; and the SDK's new-frame event is
  **auto-reset**, so two readers waiting on it steal each other's wake-ups.
- 3D **tablets** carry a front stereo pair (15 mm and 25 mm baselines on two models, parallel)
  owned by the vendor's camera/tracking service on Android. Measured 2026-09-26 (roadmap §C.0):
  the pair is a hidden Camera2 **logical** camera (front 5, back 4) that delivers two raw 1280×720
  NV12 streams at 30 fps. Opening the front one **evicts the vendor tracker, which never
  reacquires**. The vendor camera SDK (CNSDK) supplies only calibration and in-app face tracking,
  not frames. The DisplayXR Browser cannot see the pair at all (browser-pvt#175).

Everything a correct stereo camera needs — the private frame channel, the device's calibration,
keeping the tracker alive — is vendor knowledge. The runtime's plug-in model (ADR-019) exists
exactly so that knowledge lives in one vendor DLL and nowhere else.

## Decision

1. **The vendor plug-in is the camera source.** Optional slots appended to `xrt_plugin_iface`
   (not a DP vtable — a camera is a sensor, graphics-API-neutral, and must outlive DP
   re-creation) let a plug-in enumerate stereo cameras, return the **active device's**
   calibration, and deliver decoded frames (GRAY8/NV12/BGRA8) from its own stack **without
   taking the device from the tracker**. Opening a camera doubles as a tracker keep-alive.
2. **The service owns the camera.** One runtime-owned thread per open camera, one capture fanned
   out to every authorised stream, latest-wins pinned rings, per-stream wake handles. Rectification
   (Bouguet, parallel, zero disparity at infinity, **no convergence shear**) and format conversion
   are vendor-neutral runtime code. In-process runtimes enumerate nothing: one owner per camera.
3. **Clients consume through `XR_DXR_stereo_camera`**, an instance-level extension (a capture
   component has no session): enumerate with properties and state, calibration (raw/rectified),
   start/stop, acquire newest frame with timestamps over shared memory (optional GPU transports).
4. **The runtime enforces camera privacy itself**, because these frames bypass the OS camera
   stack: OS camera consent of the OS-derived peer, a DisplayXR per-executable consent (a
   registered browser delegates per-origin consent to its own prompt), visible/foreground-only
   delivery, a runtime-owned in-use indicator, a user kill switch, and hashed ids against
   fingerprinting.
5. **The DisplayXR Browser exposes each camera as an ordinary `getUserMedia` device** with one
   capability hint, so pages (the call module) need no DisplayXR-specific capture API.

## Alternatives considered

| Alternative | Verdict |
|---|---|
| **A. `getUserMedia` only** (page opens the physical camera; pause the tracker if needed) | **Rejected.** Fails exactly where it matters (exclusive tracker). Pausing tracking to free the device kills the weave's viewer tracking for the whole display during a call — the 3D experience the call is for. Still yields raw, uncalibrated pairs. Remains the path for plain USB stereo cameras that no plug-in claims (RFC 0002 §4 row 1). |
| **B. Bridge to the vendor's own app** (the vendor call app or a vendor helper re-publishes frames, e.g. as a virtual webcam) | **Rejected.** Couples DisplayXR to a vendor application's lifetime and install, adds a second privileged reader of the tracker (the auto-reset-event contention above), and a virtual webcam driver is a kernel/DirectShow-filter install per vendor with no consent model of ours. |
| **C. Plug-in source via the service** (this ADR) | **Accepted.** Vendor code stays in the plug-in (ADR-019); one owner, fan-out, runtime-enforced privacy; the same shape as `XR_DXR_lift` reversed (plug-in produces, service owns, client consumes). |
| **D. Vendor code in the browser** (the Chromium fork reads the vendor shared memory, decodes, rectifies) | **Rejected — violates vendor isolation.** Vendor names/paths/SDK headers in a vendor-agnostic browser; every vendor would need a browser patch; the browser's GPU/utility processes are sandboxed (Low IL, restricted token) and cannot open the vendor's named objects anyway; and no other client could reuse it. |
| **E. A separate "camera provider" plug-in type** (ADR-034 style) | **Deferred.** Right for a camera vendor that is not a display vendor. Today every source is a display's own tracker camera, whose calibration and keep-alive are the display plug-in's; a second plug-in type would split one device across two DLLs. The OpenXR surface does not change if a provider type is added later. |
| **F. Plug-in rectifies** | **Rejected as the default.** Rectification is vendor-neutral math; doing it once in the runtime gives every vendor correct, identical, testable (sim_display) output. A plug-in whose source is already rectified says so (`NATIVELY_RECTIFIED`). |
| **G. Bake a convergence shear into the source** (as the vendor call app does) | **Rejected.** Convergence depends on subject distance and is the receiver's choice (RFC 0002: per-eye crop offset `f·B/(2Z)`); the source stays a physically honest parallel pair. |

## Consequences

- **+** Pages get the tracker camera during tracking, rectified, with honest calibration, through
  standard web APIs; the eye tracker is never disturbed.
- **+** Calibration is the active device's by construction; the "first folder" class of bug
  cannot happen in a consumer.
- **+** Hardware-free end to end: sim_display's fake camera (synthetic distorted, misaligned SBS
  with known depths) makes rectification and the privacy flow CI-testable.
- **+** One more place where DisplayXR adds, not subtracts, platform privacy: indicator and
  consent exist even though the OS cannot see this consumer.
- **−** The runtime takes on camera privacy UX (tray prompt, indicator, consent store) it did not
  have. It must be right the first time; a leak here is a headline.
- **−** The first real source rides an **undocumented vendor shared-memory contract** (header
  defined by readers, no sequence number or timestamp, auto-reset wake). The plug-in must poll
  defensively until the vendor SDK formalises it (roadmap open questions).
- **−** Frame quality is the tracker's: greyscale, 640×480, tracker-chosen rate/exposure.
- **−** Another service thread, and a CPU copy per frame (≈ 27 MB/s at 1280×480 NV12 30 Hz).
- **−** On Android the source cannot be a passenger on the tracker: the pair and the tracker cannot
  both hold Camera2. So the runtime (the vendor plug-in in the runtime APK, ADR-038) must own the
  front pair and run the vendor's in-app tracking from the same capture, on a worker thread
  (roadmap §C.1). That is more vendor code inside the runtime APK, and more CPU, than on Windows.

## Amendment 1 (2026-10-03, R3 as built) — what changed from the decision text

- **A camera-only client class.** The decision assumed the browser's capture component would
  be classed as the browser (PRESENT_OWNER, by enabling `XR_DXR_weave`). In the field the
  video-capture *utility* process is a separate OS process with no window and no weave, and
  classing it PRESENT_OWNER spent a panel-owner slot. R3 adds `CAMERA_CONSUMER`, declared
  explicitly (`XrStereoCameraClientInfoDXR`), session-less by contract and outside the
  present-owner quota. Consent is still keyed by the verified executable, so the browser's
  installer registration (delegating) covers the utility too.
- **Distinct refusal results instead of one `PERMISSION_INSUFFICIENT`.** A browser must map
  "the user said no" to `NotAllowedError` and "the device cannot be read" to
  `NotReadableError`; one code could not carry both. `PERMISSION_INSUFFICIENT` now means the OS
  camera switch (or a class that may not use cameras); consent, sharing-off, busy and
  service-ended each have their own `XrResult`.
- **The foreground rule is window visibility of the peer pid, not session state.** The spec
  said "a session in VISIBLE / FOCUSED"; a camera consumer may have no session at all, and the
  service already has the peer pid. Window-bearing classes need a visible top-level window;
  delegating, camera-consumer and diagnostic clients are exempt (they were granted explicit
  consent). The OS-lock suspension is unconditional.
- **The prompt blocks the caller (≤ 60 s) rather than returning "pending".** A capture
  device's `AllocateAndStart` wants a final answer, and the stream handle stays valid either
  way; an unanswered prompt is a refusal the app may retry. Unanswered is never persisted.
- **Two user kill switches, not one.** "Stop camera sharing" ends every started stream now
  (apps see `STREAM_ENDED`); "Share the 3D camera with apps" is the persistent toggle the
  decision described (zero cameras enumerated).
- **The dev override stayed** (`DXR_STEREO_CAMERA_DEV_ALLOW=1`) as a documented, loud
  development switch below the sharing-off checks — the R1 text had it as a stand-in to be
  deleted.

## Amendment 2 (2026-10-06) — delegation never outranks a refusal

As built in R3 (PR #1819) a registered consent-delegating client was allowed at the fourth step,
**before** the OS camera privacy switch and before a Deny the user had stored for that executable
— so once an installer registered a browser, it would get the camera with the OS switch off and
a stored Deny silently ignored. That contradicted §B.3 of the roadmap and the pre-R3 spec, where
OS consent applied to every consumer and delegation replaced only the DisplayXR prompt. No rationale
for the order was recorded, and the plausible one ("the browser enforces the OS switch itself")
does not hold: the service, not the browser, opens the camera, so the OS never sees the browser as
a camera user.

The order is now: sharing off / `DXR_STEREO_CAMERA=0` → (dev override) → no identity → **OS
camera switch** → **stored Deny** → delegating client → stored Allow → Allow once → prompt
(spec §7.1 is normative). Delegation means exactly "no runtime prompt and no stored per-app
decision needed". `displayxr-cli camera trust` over a stored Deny clears it and says so (the
user's newer explicit word); an installer's registration never touches the user's store.

## Amendment 3 (2026-10-06, spec v3) — delegation hardening

Delegation trusted a path, any path, and could not be turned off by the client that held it.
Three changes close that (spec §2a, §7.1, §7.1.1 are normative):

- **A user-writable executable path needs a signer.** A path is only an identity where nobody
  but an administrator can put a different file there. On Windows an entry whose executable is
  not under `%ProgramFiles%`, `%ProgramFiles(x86)%` or `%SystemRoot%` is applied only when it
  records a signer CN (`<valueName>.signer`) and the executable carries a valid Authenticode
  signature by exactly that signer (`WinVerifyTrust`, never touching the network). Otherwise the
  entry is skipped as if absent — stored decision / prompt — with one WARN per executable per
  run. Admin-protected paths keep path-only trust. POSIX stays path-only for now (no signature
  check exists there yet; recorded as a known gap).
- **Per-user delegation is announced.** A user-level entry (`HKCU` / the user JSON) can be
  written by anything running as the user. It is still honoured — it is how a developer or a
  per-user install registers — but its first use per executable per service run raises a WARN
  and a tray balloon. Installer (machine-level) entries stay silent.
- **A client can decline delegation.** `XR_STEREO_CAMERA_CLIENT_DECLINE_DELEGATION_BIT_DXR` on
  `XrStereoCameraClientInfoDXR`: a browser running with its own prompt bypassed (automation,
  auto-grant switches) must not let the runtime take that missing prompt as consent. The bit
  only restricts, so the claim is not verified; the runtime then requires a stored decision or
  its own prompt.

A skipped delegation (declined or untrusted) also drops the foreground-rule exemption — the
client is an ordinary window-bearing app for that stream — but never the RAW refusal, which keys
on registration alone, so an untrusted entry can only ever narrow what a client gets.

## Amendment 4 (2026-10-07) — a vendor-neutral source for plain UVC side-by-side stereo webcams

Decision 1 made the display plug-in the only camera source, and Alternative A left "plain USB
stereo cameras that no plug-in claims" to `getUserMedia`. That leaves a gap the plug-in model
cannot fill: an external stereo webcam that outputs one side-by-side image over ordinary UVC
works with **any** display, belongs to **no** display vendor, and a page reading it through
`getUserMedia` gets raw, unaligned pairs with no calibration, no consent of ours and no
`XR_DXR_stereo_camera` path for native apps. Nothing about reading it is vendor knowledge: the
device is a standard UVC camera; the only device-specific facts are its SBS layout and eye order.

**Decision.** The service gains its **own** camera source, next to the plug-in's:

1. **Runtime-owned, vendor-neutral capture code** (`auxiliary/util/u_stereo_uvc.{h,c}` for
   config, matching, the SBS split and the nominal model — pure, tested on every OS;
   `auxiliary/os/os_uvc_capture*.{h,c,cpp}` for the OS call: Media Foundation on Windows, a
   V4L2 stub on Linux to follow). It has exactly the six-operation shape of the plug-in camera
   slots, and the camera manager dispatches on a per-camera source tag in one place; consent,
   `CAMERA_CONSUMER` rules, the foreground rule, the in-use indicator, linger, rings,
   rectification and refinement are one code path for both sources. No vendor names, no SDK.
2. **Opt-in per device, never automatic.** Nothing is claimed without the user's per-user
   `stereo-cameras.json` (`%LOCALAPPDATA%\DisplayXR\`, `$XDG_CONFIG_HOME/displayxr/`;
   `DXR_STEREO_CAMERA_UVC_CONFIG` overrides the path, `DXR_STEREO_CAMERA_UVC=0` turns the source
   off). An entry matches by USB VID:PID and/or a friendly-name substring and states layout
   (`sbs-full` | `sbs-half`), eye order, capture mode, per-eye output size, nominal baseline and
   HFOV, optionally a calibration file. A **name-only** entry that matches more than one present
   device claims nothing. The built-in allowlist is **empty**: no device is listed without a
   public spec of its SBS layout.
3. **A plug-in's camera is never claimed.** Plug-in cameras are enumerated first; a device whose
   OS id or USB VID:PID equals a plug-in camera's `platform_device_hint` is skipped — the
   display's tracker camera stays the plug-in's and its eye tracker's.
4. **Uncalibrated is the normal case, and it is reported honestly.** Without a calibration file
   the camera has no `CALIBRATED` flag; the service models it as an ideal parallel pinhole pair
   (fx = fy from the configured HFOV, centred principal point, no distortion, R = I,
   T = (−baseline, 0, 0)). RAW calibration is refused (`FEATURE_NOT_SUPPORTED`): a nominal model
   is not a measurement of the device. RECTIFIED is offered anyway — it is a **pass-through**
   (frames as delivered, described by the nominal pinhole, no remap, no zoom) until the online
   vertical refinement (R2) has measured the rows and folded a correction in, from which point
   the ordinary corrected LUT path runs. `baselineMm` / `horizontalFovDeg` are reported only
   when the user configured them (0 = unknown, as before). A calibration file (OpenCV `K1 D1 K2
   D2 R T`, YAML or JSON) makes the camera `CALIBRATED` and the R2 rectifier does the rest.
5. **The device is open only while a stream is started** (plus the 2 s linger), exactly like a
   plug-in source; there is no tracker to keep alive. Enumeration never activates a device.

**What changes for consumers.** Nothing in the API (spec version unchanged): one rule is relaxed
— RECTIFIED no longer requires `CALIBRATED | NATIVELY_RECTIFIED`, it requires a camera the
service can rectify, which now includes a nominal-model UVC pair. A consumer that gates
RECTIFIED on `CALIBRATED` simply does not ask for it. The browser's capture component sees an
ordinary camera with a rectified output and a `platformDeviceHint` naming the physical webcam, so
it can hide the raw duplicate the OS also exposes. `displayxr-cli camera list` tags each camera
`plugin` or `uvc`; `camera uvc-devices [--modes]` shows the config and which present device it
claims (modes only for claimed devices, never started).

**Alternatives.** *A V4L2/MF virtual camera that re-publishes rectified frames* — rejected for the
reason Alternative B was: a driver install with no consent model of ours. *Letting each display
plug-in carry webcam support* — rejected: a webcam is not a display's, and every vendor would
duplicate the same capture code (ADR-019 isolates vendor code; this is the opposite case). *A
camera-provider plug-in type* (Alternative E) remains the right home for a **camera vendor's**
proprietary device (custom transport, factory calibration channel); a standards-compliant UVC
SBS webcam needs no vendor code, so it lives in the runtime.

**Consequences.** **+** Any display + any SBS UVC stereo webcam, no plug-in, same privacy model.
**+** The fake backend (`"fake"` in the config) runs the whole manager hardware-free
(`tests_stereo_camera_manager`). **−** MJPEG decoding the plug-in path does not have. A 4K60
webcam is decoded on the GPU by default (a hardware MJPEG decoder MFT on the adapter that has one,
via a DXGI device manager, scaled to the output size before it crosses to the CPU), with MF's
software decoder as the logged fallback; the first hardware run, before that path, delivered
11.3 Hz. **−** Linux has no backend yet (V4L2 is a TODO). **−** A
loose `match` could claim the wrong webcam; the ambiguity rule and `uvc-devices` mitigate, the
config is still the user's statement of which device is a stereo pair.

# ADR-043: A display's stereo camera is a plug-in-provided source, owned by the service and privacy-gated by the runtime

**Status:** Proposed (2026-09-25) · **R1 implemented** 2026-09-26 (runtime, hardware-free; maintainer
defaults recorded in the roadmap §G: stereo-only, iface slots, service clients only, no raw frames
to pages) · introduces
[`XR_DXR_stereo_camera`](../specs/extensions/XR_DXR_stereo_camera.md) · appends optional
camera slots to `xrt_plugin_iface` under the [ADR-020](ADR-020-plugin-abi-compatibility-policy.md)
append-at-end rule · related: [ADR-019](ADR-019-vendor-plugin-aux-boundary.md),
[ADR-034](ADR-034-input-provider-plugins.md), [ADR-035](ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md),
ADR-042 (vendor 2D→3D conversion; in flight on branch `feat/lift-ext`, not yet on `main`) (the same shape in reverse) ·
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

# Stereo camera source — browser integration, vendor plug-in notes, phased plan

**Status:** design (2026-09-25). Decision: [ADR-043](../adr/ADR-043-stereo-camera-source.md).
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

### C.1 Runtime + plug-in

On the tablets the front stereo pair (15 mm and 25 mm baselines on the two current models,
parallel) belongs to the vendor's head-tracking service; the plug-in (`drv_leia_android`) talks
to it through the vendor camera SDK (CNSDK), which today gives it **face/eye data, not images**.

- **If CNSDK can share frames** (an AHardwareBuffer or buffer stream of the pair the tracker
  already captures, with the pair's calibration): the Android plug-in implements the same slots;
  the runtime service's camera thread, rings (memfd), consent (CAMERA permission of the peer uid +
  runtime-app consent) and visibility rule (`android_package_is_visible`) are the platform-neutral
  code of spec §5–7. This is the target, and the **first open question to the vendor**.
- **If it cannot yet — documented platform difference:** Android enumerates no runtime stereo
  camera. We do **not** have the plug-in or the browser open the front cameras through Camera2
  while tracking runs: Camera2 evicts the lower-priority client, which is the tracker, and the
  weave would lose viewer tracking for the whole call. Pages see the tablet's ordinary cameras via
  `getUserMedia`; the call module sends mono (lifted on the receiver).
- Frames on this path are likely **colour and larger** (per-eye 1280×720 class), which is where
  the optional `AHARDWAREBUFFER` transport earns its keep.

### C.2 Browser

The browser's Android arm is in the patch series (runtime fd brokered by the browser-process Java
via `DXR_IPC_FD`, patches 0083+). The camera lands on top of it the same way as Windows: a
DisplayXR device factory composed in front of `VideoCaptureDeviceFactoryAndroid`, fed over the
existing runtime connection, NV12 from the shared-memory ring. Chromium's own Android camera
permission request covers the uid check the runtime makes.

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
| **R1** | runtime | Header `XR_DXR_stereo_camera.h` + `index.json` note (together — the catalog lint requires both); `xrt_plugin_iface` slots + `XRT_PLUGIN_IFACE_HAS_STEREO_CAMERA` (appended after `create_dp_d3d11_lift`, so **lands after ADR-042's PR**); platform-neutral camera manager (thread, refcount + linger, 3-slot pinned ring, per-stream wake handles, decimation, NV12/BGRA conversion); IPC + OpenXR entry points; sim_display fake; `displayxr-cli camera list/calib/probe`; selftest check | `camera probe` on the sim fake writes frames on Windows + Linux CI; ring/pinning unit tests (mirror `tests_lift_mailbox`) |
| **R2** | runtime | `u_stereo_rectify` (Bouguet, valid-region crop, LUT remap) with golden tests against OpenCV-generated fixtures; RECTIFIED output | sim fake: row error < 0.5 px, disparity = f·B/Z within 0.5 px |
| **R3** | runtime | Privacy: OS consent check, consent store + tray prompt, delegating-client registration, visibility/lock suspension, indicator, kill switches | Manual matrix: allow/deny/revoke, OS switch off, lock screen, background app — each blocks frames |
| **L1** | leia-plugin (Windows) | Slots over the SR raw-camera channel per §D; calibration by active serial; keep-alive; repin runtime (feature-macro repin, `downstream-pins.json` `features` track) | On a panel box, incl. the multi-folder box: `camera probe --rectified` rows aligned; the vendor call app running at the same time still gets every frame; tracking/weave unaffected (frame-time + tracking-state logs) |
| **B1** | browser (Windows) | §B device factory + duplicate hiding + hint + delegating registration in the installer; web SDK: prefer the hint in `camera:'auto'`, fill `hello` from it | RFC 0002 P1 call between two panel laptops sends rectified SBS from the tracker camera **while both are tracking** |
| **A1** | leia-plugin (Android) + runtime | Only if CNSDK shares frames (§C.1): Android slots, memfd/AHB transport, Android consent + visibility | `camera probe` on the tablet via the runtime app |
| **B2** | browser (Android) | §C.2 | Tablet ↔ laptop 3D call, both directions stereo |

R1–R3 need no hardware; L1 and B1 can proceed in parallel once R1's header exists (B1 develops
against the sim fake).

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
- **ABI merge order.** The iface slots append after ADR-042's `create_dp_d3d11_lift`; merging out
  of order would put two branches on the same offset.

## G. Open questions for the maintainer

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

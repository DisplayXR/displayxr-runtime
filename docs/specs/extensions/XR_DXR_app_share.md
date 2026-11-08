# XR_DXR_app_share — 3D App Share

| Field | Value |
|---|---|
| **Extension Name** | `XR_DXR_app_share` |
| **Spec Version** | 1 |
| **Extension Type** | Instance extension, **requires** [`XR_DXR_stereo_camera`](XR_DXR_stereo_camera.md); service path only for the consumer (an in-process instance enumerates zero sources, as for cameras) |
| **Header** | [`src/external/openxr_includes/openxr/XR_DXR_app_share.h`](../../../src/external/openxr_includes/openxr/XR_DXR_app_share.h) (+ its `index.json` catalog note) |
| **Status** | **Accepted design (ADR-061, 2026-11-03) — portable half implemented** (app entries in the camera manager, share consent + `share.*` settings, IPC, state tracker, `DXR_APP_SHARE_FAKE`, `displayxr-cli share`); **no service tap yet** — every real app entry is listed `UNAVAILABLE` "no share tap on this platform" until the Windows D3D11 tap lands. Phases in [ADR-061](../../adr/ADR-061-3d-screen-share.md#phases-and-effort). Provisional type values `1004999430–432` (decade `430–439`), pending the Khronos registry |
| **Decision record** | [ADR-061](../../adr/ADR-061-3d-screen-share.md) |
| **Builds on** | [ADR-043](../../adr/ADR-043-stereo-camera-source.md) (the stereo camera stack), [ADR-030](../../adr/ADR-030-crop-before-dp-zero-copy-only-when-swapchain-equals-atlas.md) (crop), [ADR-041](../../adr/ADR-041-fixed-view-count-with-per-frame-activity.md) (view count), [ADR-044](../../adr/ADR-044-colour-contract-per-backend.md) (colour), [ADR-049](../../adr/ADR-049-in-process-lift-rides-the-service.md) (the in-process link), [ADR-059](../../adr/ADR-059-d3d11-hub-compositor-windows.md) (the in-process tap point) |

## 1. What it is

A consumer — in practice the DisplayXR Browser's video-capture utility process, on behalf of a
web call — receives a **running DisplayXR app's pre-weave left and right views** as one
side-by-side (SBS) video stream, so the call sends the app in 3D and every receiver weaves it on
its own display. The woven output is never shared: it is useless to any other display (ADR-007).

The extension adds **no entry point**. An app is a third **source kind** of the stereo camera
stack, next to the display plug-in's camera and the service's UVC webcam source
([`XR_DXR_stereo_camera`](XR_DXR_stereo_camera.md) §9, §9a). Everything a stream is — create,
start, stop, the 3-slot shared-memory ring, the per-stream wake handle, stats, state and
end events, the kill switch, the in-use indicator — is the stereo camera's, unchanged. This
document specifies only what differs.

## 2. Enabling and enumeration

A consumer enables **both** `XR_DXR_stereo_camera` and `XR_DXR_app_share` at
`xrCreateInstance`. Enabling `XR_DXR_app_share` alone fails with
`XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED`. A camera-only process keeps chaining
`XrStereoCameraClientInfoDXR` with `CONSUMER_ONLY` (stereo camera §2a); nothing else is declared:
enabling the extension **is** the declaration, and the state tracker records it on the
connection (an internal `xrt_application_info` client flag, see ADR-061 decision 3).

`xrEnumerateStereoCamerasDXR` then lists, **after every camera**, one entry per shareable app:

| Field | Value for an app entry |
|---|---|
| `flags` | `APP_CONTENT` (`0x40`, defined in this header) \| `NATIVELY_RECTIFIED` \| `RECTIFIABLE`. Never `CALIBRATED`, `USER_FACING`, `SHARED_WITH_EYE_TRACKING`, `MONOCHROME` |
| `displayName` | the app's `XrApplicationInfo::applicationName` (falls back to the executable's file name) |
| `persistentId` | keyed hash of (app executable path, instance ordinal, consumer executable) with the per-user secret of stereo camera §7.5 — stable across runs for "the first model viewer", distinct for a second concurrent copy, different for every consumer |
| `platformDeviceHint` | `""` |
| `viewCount` | 2 (the stream is always a pair; the app's own count is in the chained struct) |
| `eyeExtent` | the stream's fixed per-eye size from the user setting `share.resolution` (§8): 1280×720 (`720p`, default) or 1920×1080 (`1080p`, the maximum) |
| `maxFrameRate` | 30 for `720p`, up to 60 for `1080p`; the service decimates the app's commit rate, never interpolates |
| `baselineMm`, `horizontalFovDeg` | 0 — not a camera; the geometry is display-referred (§4) |
| `supportedFormats` | `NV12` only |
| `supportedTransports` | `SHARED_MEMORY` only (`D3D11_TEXTURE` stays specified-not-implemented, ADR-061 decision 5) |
| `state` | `AVAILABLE` while the app commits frames; `SUSPENDED` after 1 s without a commit (minimised, paused, hidden by its own choice); `UNAVAILABLE` when the app cannot be tapped (§3, listed with a reason so it can be diagnosed — ADR-061 decision 18) or has exited |

Chain `XrAppShareSourcePropertiesDXR` on `XrStereoCameraPropertiesDXR::next` to read the app's
process id, executable name, application name, window rect, per-view content size, view
count / active view count, the current `zeroDisparityShiftFrac` and the source atlas's encoding
state. A runtime fills it only for an `APP_CONTENT` entry; test the flag, not the struct.

**Which apps are listed.** Every connection with a running session — service-composited apps
(under a workspace controller, `XRT_FORCE_MODE=ipc`, `DISPLAYXR_WORKSPACE_SESSION=1`) and
in-process apps reached through their in-process service link (`ipc_client_service_link`, the ADR-049 lift link generalised — ADR-049 Amendment 1, ADR-061 decisions 9 and 16) — **except**:

- `CONTROLLER`, `DIAG`, `CAMERA_CONSUMER` and `PRESENT_OWNER` clients. A present-owner is a
  browser: its pages carry other origins' content, which no one consented to share;
- the consumer's own process;
- a session that opted out (§5).

**Lifetime of an entry (an exception to stereo camera spec v4, for app entries only).** Cameras
keep their index and `cameraId` for the service's life and stay listed `UNAVAILABLE` when gone.
Apps come and go far more often, so an app entry:

- keeps one `cameraId` for as long as its app runs, and gets the **same** `cameraId` back if the
  same (executable, instance ordinal) starts again within the same service run;
- is listed `UNAVAILABLE` for 10 s after its app exits (so a started stream ends with
  `SOURCE_LOST` and the consumer sees the state change), then **leaves the list**;
- is always listed after every camera, so a camera's index never moves. App entries' indices
  may: **consumers address app entries by `cameraId`**.

`XrEventDataStereoCamerasChangedDXR` is delivered on every change of the app set or of an app
entry's labels, exactly as for cameras. The service rescans on client connect, session begin and
end, and a 1 Hz tick while any consumer has enumerated.

## 3. Sources (informative: where the frames come from)

| App class | Tap | Covered |
|---|---|---|
| Service-composited (`_ipc`, any app under a workspace controller, forced IPC) | the D3D11 service, after the per-client atlas is final in `compositor_layer_commit` | Windows v1; macOS P6 |
| In-process D3D11, D3D12, Vulkan on Windows, whose frames ride the D3D11 fill arm (`compositor/xbridge/`, ADR-039 / ADR-059) | the app process copies its pre-weave content into an NT-shared ring the service imports, over a sessionless share link | Windows v1 |
| In-process sessions the fill arm declines (OpenGL; a shared-texture session without `DXR_HUB=1`; a kill-switched split; `DXR_LEGACY_STANDALONE`) | none | listed `UNAVAILABLE` (§9) |
| macOS / Linux in-process | none in v1 | not listed — the session never dials the service to share (§3a) |

Both taps produce the same frame: the views are taken from the atlas the display processor
would weave, cropped to the active mode's content dimensions (ADR-030); with `viewCount > 2` the
middle pair; with one active view (ADR-041) that view in both halves; scaled to fit `eyeExtent`
preserving aspect, letterboxed black; alpha dropped; Y-flipped when the source atlas is
bottom-up; converted to NV12, BT.601 full range (§8). A LINEAR atlas is encoded first (ADR-044).

### 3a. In-process apps: the service link — as built (portable half)

Informative; ADR-061 decisions 9 and 16, ADR-049 Amendment 1. The Windows ring and the service's
import are the remaining half (the hook contracts below are what it fills).

**The link.** Every in-process session holds at most one sessionless `APP` connection to the
service, its *service link* (`ipc_client_service_link.c`), named `<exe> (in-process)`. Its
capabilities are fixed when it is created: `lift` when the instance enabled `XR_DXR_lift`, `share`
only where a tap can exist — a Windows D3D11, D3D12 or Vulkan in-process session, the tiers that
feed the D3D11 fill arm — and while sharing is on in the app's environment (`DXR_APP_SHARE`,
default on; `0` = it never offers itself). OpenGL and macOS / Linux sessions never declare it, so
they spend no connection on sharing. A `share` link's worker thread dials once the session runs
**and** its compositor reported its tap state (a tap, or the reason it has none) — in the
background, never launching the service, re-dialling a failed connect every 5 s — and registers
(`service_link_register`): capabilities, executable and pid (diagnostics only — the listed pid and
executable are OS-derived from the connection), the `XrApplicationInfo` name, the §5 opt-out, the
graphics tier (D3D11 / D3D12 / Vulkan / GL / Metal), the adapter LUID its ring would live on (zero
off Windows), whether its compositor can tap, and whether the session is running. It re-registers
on every change; `xrEndSession` sends "not running". All of that, and the share poll, run on the
link's worker: no OpenXR call and no compositor frame ever waits on the service for them. A session
that also lifts uses the same connection.

**Listing.** The service lists a registered `share` link with a running session as an app entry of
source `app-link` — `AVAILABLE` only when the compositor can tap, the LUID matches the adapter the
service publishes, and the service can import a ring; otherwise `UNAVAILABLE` with the reason:
`graphics API has no tap` (OpenGL), the fill arm's own reason (a shared-texture session without
`DXR_HUB=1`, a kill-switched split, `DXR_LEGACY_STANDALONE`), `renders on another adapter`, or
`no tap` (Windows until its tap lands, or a service without an importer). An opted-out session is
not listed.
Closing the link (the app exits) ends a running stream `SOURCE_LOST` and starts the entry's
10-second linger.

**The share protocol.**

| Message | Direction | What |
|---|---|---|
| `service_link_share_poll` | app → service | `NONE`, `SHARE_REQUESTED` (with the stream's per-eye extent, rate and a stream generation that bumps on every start) or `SHARE_STOPPED` (once, then `NONE`). The app's compositor calls it every frame through `u_share_link_poll`; the IPC round trip happens at most every 250 ms, so an idle link costs a few small calls a second and nothing per frame |
| `service_link_share_publish_ring` + `service_link_share_publish_fence` | app → service | the ring for one stream generation: exactly one NT-shared texture per slot (three), each holding the left view in its left half and the right view in its right half at their content size (unscaled — the service scales and letterboxes), plus — when the ring says `has_fence` — one shared `ID3D11Fence`. A ring is imported at once without a fence, or once its fence arrived; a declared fence that does not arrive within 2 s, or an import that fails, ends the stream `SOURCE_LOST` and the app hears `SHARE_STOPPED`. A new ring (a new generation) replaces the old one, e.g. when the app's content size changes; a ring for an older stream generation is dropped |
| `service_link_share_frame_ready` | app → service | slot *N* of ring *G* is finished once the fence reaches value *V*; latest-wins. The reply is the poll's answer; `SHARE_STOPPED` is reported to the compositor exactly once across poll and reply |

A refusal (an unknown link, a malformed ring, a full link table) comes back as
`XRT_ERROR_OUTPUT_REQUEST_FAILURE` / `XRT_ERROR_FEATURE_NOT_SUPPORTED` /
`XRT_ERROR_CLIENT_LIMIT_REACHED` — never `XRT_ERROR_IPC_FAILURE`, which the link reserves for a dead
pipe (and which would drop the session's lift streams with it).

**Hook surfaces** (where the Windows half plugs in):

- *App side*, `compositor/xbridge/comp_xbridge_share_tap.h`:
  `xbridge_share_tap_begin / _commit / _end`, called by the ADR-059 D3D11 fill arm once its
  pre-weave atlas is final. They reach the link only through `util/u_share_link.h`, a
  process-wide sink the session installs at `xrBeginSession` (the compositor never links IPC).
  Built on every platform; the body reports "no tap" until the Windows ring lands.
- *Service side*, `struct ipc_server_stereo_camera_platform`: `share_link_adapter_luid`,
  `share_link_import`, `share_link_convert`, `share_link_release_frame`, `share_link_unimport` —
  the same NV12 pass as the service-composited tap (§3, decision 6). Import, convert and unimport
  come as a set: a partial table is refused whole (logged). All `NULL` = no importer: every
  `app-link` entry is `no tap`.

## 4. Frames and their metadata

Frames are the stereo camera's `XrStereoCameraFrameDXR`: `extent` = 2·`eyeExtent`, `format`
NV12, `output` `RECTIFIED`, `captureTimeIsExposure` `XR_FALSE` (the time is the tap's).

**`RECTIFIED` means display-referred here** (v1; a distinct `OUTPUT_DISPLAY_REFERRED` output is deferred to spec v2, ADR-061 decision 20). For a camera, RECTIFIED is a parallel pair with zero
disparity at infinity. An app's views are the runtime's off-axis (Kooima) projection: row-aligned
like a rectified pair, but with **zero disparity at the sender's display plane**. A consumer that
applies a camera convergence crop (`f·B/(2Z)`, RFC 0002) to an `APP_CONTENT` stream doubles the
convergence. Receivers branch on the source kind (the browser's `displayxrStereo.source`, §7).

Chain `XrAppShareFrameInfoDXR` on `XrStereoCameraFrameDXR::next` at acquire:

| Field | Meaning |
|---|---|
| `contentRect` | the content inside one eye half, left-half coordinates; outside it is letterbox |
| `mono` | `XR_TRUE`: one view in both halves (a 2D mode) |
| `contentGeneration` | bumps whenever `contentRect`, `mono` or the app's view layout changed |
| `zeroDisparityShiftFrac` | the horizontal disparity, as a fraction of the delivered per-eye width (`eyeExtent.width`, what the receiver decodes; right minus left), of a point on the sender's display plane. **0 in v1, always.** A receiver shifts each eye by half of it, in opposite directions, to put the sender's display plane at its own zero-disparity plane |
| `sourceCommitTime` | when the app's frame was committed (runtime clock) |

The service sends no measured disparity: synthetic content has a known zero plane, and an app that
uses the runtime's FOVs and poses (or `XR_DXR_view_rig`, whose camera rigs map the convergence
plane onto the display) has it at the display by construction. An app that ignores the runtime's
FOVs is out of contract and its zero plane is wherever it put it; the runtime cannot detect that.

The views follow the sender's tracked eyes (look-around and IPD included): a receiver sees the
sender's viewpoint move, as with any camera that moves.

## 5. The shared app's opt-out

An app that must never be shared (a password manager, a medical viewer) enables
`XR_DXR_app_share` and chains on `XrSessionCreateInfo`:

```c
XrSessionShareabilityDXR sh = {XR_TYPE_SESSION_SHAREABILITY_DXR, NULL, XR_FALSE};
XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO, &sh, 0, systemId};
```

The session is then never enumerated, and a stream of it that is already running ends with
`SOURCE_LOST`. Absent (or `shareable = XR_TRUE`) = shareable. The opt-out is runtime-enforced: it
travels on the session's IPC connection or in the registration of the in-process service link (§3a), which then never offers
frames. It is not a substitute for the prompt (§6): an app without the struct is shareable only
after the user said so for that start.

## 6. Consent and privacy

App share has **its own consent evaluation**; the camera table (stereo camera §7.1) does not
apply to it. The differences are deliberate: no OS camera is involved, and the content belongs to
an app the consumer does not own.

At `xrStartStereoCameraStreamDXR` on an app entry, in order:

1. **Sharing off** — the "Share the 3D camera with apps" toggle off, the "Stop camera sharing"
   kill switch, or `DXR_STEREO_CAMERA=0` → `XR_ERROR_STEREO_CAMERA_DISABLED_DXR`. One switch
   stops cameras and shares alike; `DXR_APP_SHARE=0` in the service's environment turns off app
   sources alone (count 0, indistinguishable from "no shareable app").
2. **No verifiable consumer identity** → `XR_ERROR_STEREO_CAMERA_CONSENT_REFUSED_DXR`.
3. **The source app opted out or is gone** → `XR_ERROR_STEREO_CAMERA_BUSY_DXR`.
4. **The runtime's share prompt**, every start: *"Share <applicationName> with <consumer
   executable>? Deny / Allow"*. Allow → the stream starts; Deny or no answer within 60 s →
   `XR_ERROR_STEREO_CAMERA_CONSENT_REFUSED_DXR`. The prompt blocks the start, as the camera prompt
   does (ADR-043 Amendment 1).

What is **not** in the list, on purpose:

- **No delegation.** A registered consent-delegating client (a browser) still gets the runtime's
  prompt: the browser's per-origin camera prompt cannot name the app being shared, and the app is
  not the browser's to give away. `XR_STEREO_CAMERA_CLIENT_DECLINE_DELEGATION_BIT_DXR` is moot.
- **No stored decision by default.** The user setting `share.prompt` = `always` (default) |
  `per-app-run` (one Allow covers the same consumer and the same app until that app exits — the
  documented booth / kiosk setting). There is no "always allow" and no stored Deny.
- **Not lifted by `DXR_STEREO_CAMERA_DEV_ALLOW`.** Only `DXR_APP_SHARE_DEV=1` (§10) skips it.
- **No OS camera privacy switch:** no camera is opened.

While a share runs:

- **Indicator:** the runtime's in-use indicator reads *"Sharing <applicationName>"*; stopping it
  from there ends the stream with `USER_STOPPED`.
- **The shared app need not be in the foreground.** Its state follows its commits (§2).
- **The consumer's visibility rule is the camera's** (stereo camera §7.2): a `CAMERA_CONSUMER`
  is exempt; another class needs a visible window.
- **Session lock / switched user:** `SUSPENDED`, as for cameras.
- **App exit:** the stream ends with `SOURCE_LOST`; a browser ends the page's track (`ended`).
- **Calibration:** `xrGetStereoCameraCalibrationDXR` on an app entry returns
  `XR_ERROR_FEATURE_UNSUPPORTED`; the display-referred geometry is §4's metadata.
- **`RAW` output:** refused (`XR_ERROR_FEATURE_UNSUPPORTED`) — there is nothing raw about it.

## 7. The DisplayXR Browser mapping (informative)

The browser's capture component (browser-pvt patches 0370–0375) already exposes each service
camera as an ordinary `videoinput` device from the unsandboxed video-capture utility process. For
app entries it adds:

| Page-visible | Value |
|---|---|
| `MediaDeviceInfo.label` | `DisplayXR: <displayName> (3D)` |
| `deviceId` | `dxr-app:` + `persistentId` (cameras keep `dxr-stereo:`) |
| `track.getSettings().displayxrStereo` | `{ source: 'app', appName, zeroDisparityShiftFrac, ... }` beside the existing camera fields |

The page's own picker lists these devices; there is no `getDisplayMedia` integration and no
Chromium picker change. **App devices are listed only once the page holds camera permission**
(ADR-061 decision 17): before that a page sees no `dxr-app:` device at all, not even an unlabelled
one, so the device count never reveals how many DisplayXR apps are running. A page that wants
"share an app" asks for the camera first, then filters `enumerateDevices()` on the `dxr-app:`
prefix. A started track ends when the stream ends
(app exit, kill switch, Deny on a later restart).

## 8. Stream parameters

- **Format:** NV12, BT.601 full range (JPEG), the colour contract the browser applies to every
  runtime camera device; receivers decode it as any webcam. Planes and pitches per
  `XrStereoCameraFrameDXR`.
- **Size:** fixed per entry (`eyeExtent`), letterboxed; it never follows the app's window. A
  **user setting** in v1, `share.resolution`: `720p` (default, 2×1280×720 @ 30 Hz, ~83 MB/s through
  the ring) | `1080p` (2×1920×1080, the maximum, up to 60 Hz; ~187 MB/s at 30 Hz). A consumer's
  `maxFrameRate` can only lower the rate. A consumer-requested size is deferred to spec v2.
- **Latency:** the readback is a non-blocking staging ring; a frame reaches the ring one to three
  app commits after it was rendered. The source app never waits for the tap.

## 9. Errors and states, summary

| Situation | Result |
|---|---|
| `XR_DXR_app_share` enabled without `XR_DXR_stereo_camera` | `XR_ERROR_EXTENSION_DEPENDENCY_NOT_ENABLED` at `xrCreateInstance` |
| In-process consumer instance | zero app entries (and zero cameras) |
| App cannot be tapped (§3 row 3), or on another adapter than the service's (ADR-061 decision 9) | entry `UNAVAILABLE`; start → `XR_ERROR_STEREO_CAMERA_BUSY_DXR` |
| No commit for 1 s | `SUSPENDED`; frames resume with the next commit |
| Share prompt denied / unanswered | `XR_ERROR_STEREO_CAMERA_CONSENT_REFUSED_DXR` |
| Sharing off | `XR_ERROR_STEREO_CAMERA_DISABLED_DXR` |
| App exited / opted out mid-stream | stream ends, `SOURCE_LOST` |
| `RAW`, a format other than NV12, a GPU transport, calibration | `XR_ERROR_FEATURE_UNSUPPORTED` |

## 10. Diagnostics and tests

- `DXR_APP_SHARE_DEV=1` (**service** environment; loud WARN at service start): app entries are
  listed in the plain camera enumeration **without** `XR_DXR_app_share` and started **without** the
  share prompt, so a shipping browser sees them as ordinary 3D cameras. Development only — it is
  how the ADR-061 P1 spike runs before any browser change.
- `DXR_APP_SHARE_FAKE=1` (service environment): a synthetic app source owned by the service — an
  SBS frame with a known disparity ramp and a zero-disparity marker — so the manager, consent,
  metadata and the browser path are testable hardware-free in CI (`tests_stereo_camera_manager`,
  `tests_camera_consent`). Service-side, like the UVC source's `"fake"` backend; no plug-in slot.
  `DXR_APP_SHARE_FAKE=link` instead runs a synthetic **in-process registrant** over the service
  link's core (§3a): it registers, answers `SHARE_REQUESTED` with a CPU NV12 ring and posts
  `frame_ready` at the stream rate, so the in-process arm (enumerate → consent → start → ring →
  frames → acquire → stop) runs headless too (`tests_stereo_camera_manager [app_share_link]`);
  `both` lists both fakes.
- `displayxr-cli share list` / `share probe <id> [--frames N]` — aliases of `camera list` /
  `camera probe` restricted to app entries (`cli_cmd_camera.c`); `camera list` tags each entry
  `plugin`, `uvc`, `app` or `app-link`.

## 11. Version history

| Version | Change |
|---|---|
| 1 | Initial: app entries in the stereo camera enumeration, `XrAppShareSourcePropertiesDXR`, `XrAppShareFrameInfoDXR`, `XrSessionShareabilityDXR`, `XR_STEREO_CAMERA_APP_CONTENT_BIT_DXR`. Design accepted (ADR-061), not implemented |
| 2 (planned) | `XR_STEREO_CAMERA_OUTPUT_DISPLAY_REFERRED_DXR`; a consumer-requested stream size (ADR-061 decisions 20, 21) |

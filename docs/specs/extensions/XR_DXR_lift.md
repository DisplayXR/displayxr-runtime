# XR_DXR_lift — 2D→3D Conversion Service

| Field | Value |
|---|---|
| **Extension Name** | `XR_DXR_lift` |
| **Spec Version** | 3 |
| **Extension Type** | Instance extension (conversion in the service — Windows/D3D11; IPC sessions, and in-process D3D11/D3D12 sessions on Windows over a lift-only connection (ADR-049); advertised on every desktop platform, where a service without a module reports `supportedModes = 0`) |
| **Header** | `src/external/openxr_includes/openxr/XR_DXR_lift.h` (canonical; auto-syncs to `displayxr-extensions`) |
| **Status** | Provisional (`1004999270–285` block, pending Khronos registry) |
| **Decision record** | [ADR-042](../../adr/ADR-042-vendor-2d3d-conversion-supersedes-default.md); viewpoint policy, app rig and auxiliary depth: [ADR-048](../../adr/ADR-048-lift-viewpoint-policy.md) (+ Addendum A) · in-process: [ADR-049](../../adr/ADR-049-in-process-lift-rides-the-service.md) |
| **Plug-in contract** | `src/xrt/include/xrt/xrt_dp_lift.h` + the lift slots of `xrt_display_processor_d3d11` (`XRT_DP_D3D11_HAS_LIFT`), [`xrt_plugin_iface.md` § lift](../../reference/xrt_plugin_iface.md#turning-2d-into-3d-the-lift-slots-adr-042-xr_dxr_lift) |

## 1. What it is

A display vendor may ship a **2D→3D conversion module** with its plug-in: monocular depth,
stereo synthesis, N-view synthesis, or photo → Gaussian splats. `XR_DXR_lift` exposes that
module to apps **generically**, exactly the way `XR_DXR_weave` exposes the vendor's weaver: the
caller learns what the module can do and what it produced, never which model runs.

Four ideas carry the whole extension:

1. **A READY vendor module supersedes the open default** (ADR-042). A consumer that ships its
   own open converter — the DisplayXR Browser's and the web SDK's depth estimator + view
   generator — uses the runtime's module whenever `xrGetLiftPropertiesDXR` reports it READY for
   the mode it needs. The vendor module is calibrated for the panel it is plugged into.
2. **Lift never weaves.** Results are pre-weave SBS / N-view pixels, a depth map, or a splat
   blob. Weaving stays the display processor's job on the ordinary weave path (ADR-007).
3. **Asynchronous, one frame behind.** Conversion runs on a runtime-owned thread. A submit is a
   snapshot into a latest-wins mailbox; an acquire returns the newest finished result. Nothing
   on the caller's frame path — and nothing on the service's weave, render or IPC threads —
   ever waits for the model.
4. **Geometry comes from the weave rect.** When lifted content is woven
   (`XrWeaveSubmitLiftRectsDXR`), the service weaves the latest result at the rect's *current*
   position every frame. Drag, resize and scroll are exact and real-time; only the depth lags.

```c
XrLiftPropertiesDXR props = {XR_TYPE_LIFT_PROPERTIES_DXR};
xrGetLiftPropertiesDXR(session, &props);                 // cheap; kicks activation
if (props.state == XR_LIFT_STATE_READY_DXR && (props.supportedModes & XR_LIFT_MODE_SBS_BIT_DXR)) {
    XrLiftStreamCreateInfoDXR ci = {XR_TYPE_LIFT_STREAM_CREATE_INFO_DXR, NULL,
                                    XR_LIFT_MODE_SBS_DXR, XR_LIFT_CONTENT_HINT_VIDEO_DXR, 1.0f};
    xrCreateLiftStreamDXR(session, &ci, &stream);
    // per frame:
    xrSubmitLiftFrameDXR(stream, &submit, &frameId);       // never waits for the model
    XrLiftResultDXR res = {XR_TYPE_LIFT_RESULT_DXR};
    if (xrAcquireLiftResultDXR(stream, &res) == XR_SUCCESS) { /* newer result */ }
}
```

## 2. Availability, states and discovery

| Situation | `xrGetLiftPropertiesDXR` |
|---|---|
| In-process D3D11 / D3D12 session on Windows (engine apps, e.g. the Unity display provider) | the service's module, reached over a lift-only connection the session opens in the background on its first lift call (ADR-049): `ACTIVATING` while it connects, then the rows below; `UNAVAILABLE` when no service is running (it is never launched for this) or the service refuses the connection (e.g. a from-source runtime against a different installed service) |
| Any other in-process session (GL, Vulkan, Metal; any non-Windows platform) | `XR_ERROR_FEATURE_UNSUPPORTED` |
| Service without a module (macOS, Linux, Android, or a Windows plug-in without lift slots, sim_display) | `XR_SUCCESS`, `supportedModes = 0`, `UNAVAILABLE` |
| First query on a Windows service with a module | `ACTIVATING` (the service is bringing its lift device + the plug-in's lift DP up in the background) |
| Module loading (model weights, licence check, engine build) | `ACTIVATING` — poll ≤ 2 Hz, do not fall back permanently |
| Module up | `READY`, `supportedModes` = the module's bits, `backend` = its name |
| Module failed / `DXR_LIFT=0` on the service | `UNAVAILABLE` |

The first `xrGetLiftPropertiesDXR` (or `xrCreateLiftStreamDXR`) of the service's lifetime starts
activation; the call itself never waits. `typicalLatency` is the module's own estimate
(submit → result); the measured value per stream is in `xrGetLiftStreamStatsDXR` (§7).

`maxStreams` is **service-wide**, not per session. `maxViews` bounds `XrLiftOptionsDXR::viewCount`
for NVIEW. `depthSemantics` says whether a DEPTH result is relative (larger = farther, no unit)
or metric (metres).

**Streams may be created while ACTIVATING** (their frames wait, latest-wins, until READY). A
mode not in `supportedModes` of a READY module is `XR_ERROR_FEATURE_UNSUPPORTED`; creating past
`maxStreams` is `XR_ERROR_LIMIT_REACHED`.

## 3. Streams, modes and result layouts

| Mode | Result | Acquire with | Layout |
|---|---|---|---|
| `DEPTH` | depth map | `xrAcquireLiftResultDXR` | one channel (`format` tells: `R32_FLOAT`, `R8_UNORM`, …) at the module's inference resolution |
| `SBS` | stereo pair, **not woven** | `xrAcquireLiftResultDXR` | two views side by side, left view left; `viewCount = 2` |
| `NVIEW` | N views, **not woven** | `xrAcquireLiftResultDXR` | `viewCount` views side by side in ONE row, view 0 leftmost |
| `GAUSSIANS` | 3D Gaussian splats | `xrAcquireLiftBlobDXR` | a binary little-endian reference-3DGS PLY, or a PlayCanvas SOG container (`format`) |

The tile size is the module's inference resolution, not necessarily the input size — always read
`extent` / `viewCount`. `contentHint` VIDEO lets a module keep temporal state across frames;
PHOTO asks for quality over latency. GAUSSIANS streams take PHOTO only. `inputScale` in (0, 1]
asks the module to convert at a reduced resolution — an advisory latency lever.

## 4. Submitting frames

```c
XrLiftOptionsDXR opt = {XR_TYPE_LIFT_OPTIONS_DXR, NULL,
    XR_LIFT_CONVERGENCE_AUTO_DXR, /*strength*/ 1.0f, /*inpaint*/ XR_TRUE,
    XR_LIFT_VIEWPOINT_SOURCE_TRACKED_DXR, /*viewCount*/ 2, /*viewpoints*/ NULL, /*focalPx*/ 0.0f};
XrLiftFrameSubmitInfoDXR submit = {XR_TYPE_LIFT_FRAME_SUBMIT_INFO_DXR, &opt,
    sharedHandle, /*inputIsDxgi*/ XR_FALSE, {w, h}, sourceTime};
xrSubmitLiftFrameDXR(stream, &submit, &frameId);
```

**Non-blocking.** The service acquires the input's keyed mutex (key 0 = "caller done writing",
the weave contract; 4 ms budget), **snapshots** `extent` (the top-left sub-rect of
`inputTexture`) into the stream's mailbox with one blit on its own GPU, releases the mutex, and
returns. The caller may overwrite the texture immediately. The call costs one IPC round trip +
one blit — the same order as a weave submit, never the model's latency.

**Latest wins.** The mailbox has two input slots. While the module converts one frame, the other
slot always accepts a new one; a frame still waiting when the next arrives is **dropped**
(counted in `framesDropped`), never queued. A slow module therefore lags by one frame instead
of building a backlog.

**Handle kinds** are XR_DXR_weave v3's: a D3D11 NT shared handle, or a legacy global DXGI
handle with `inputIsDxgi = XR_TRUE` (Low-integrity callers, #743). RGBA8 or BGRA8. The service
caches the import per stream (NT handles by kernel-object identity, DXGI handles by value).

**`frameId`** is per stream, monotonic from 1. `0` means the frame was **not taken** because the
module is not up yet (still ACTIVATING); submit again next frame.

**Options** (`XrLiftOptionsDXR`, optional; omitted = the stream's last options, initially the
module defaults):

| Field | Meaning |
|---|---|
| `convergence` | RELATIVE depth placed at the display plane, in [0, 1] over the frame's depth range (0 = nearest content on the glass, 1 = farthest, 0.5 = mid). Negative = AUTO. >1 is clamped. The plug-in maps it to its model's units and calibrates it for its panel. |
| `strength` | disparity scale; 1 = the module's calibrated budget, 0 = flat |
| `inpaint` | fill disocclusions (XR_TRUE) or leave them |
| `viewpointSource` | TRACKED (the runtime passes the panel's predicted tracked eyes) or EXPLICIT (`viewpoints`, `viewCount` display-space positions in metres) |
| `viewCount` | views for NVIEW (≤ `maxViews`); 2 for SBS |
| `focalPx` | the input image's focal length in pixels of `extent`; ≤ 0 = unknown (the module assumes its default FOV). Photo → Gaussians modules take it as input; DEPTH/SBS/NVIEW modules ignore it |

**Viewpoints always reach the module explicitly.** With TRACKED, the lift thread samples the
panel display processor's predicted eyes just before each conversion and hands the pair to the
module; a module spreads N views around it. The lift display processor has no tracker session
of its own.

### 4.1 Viewpoint frame and policy (spec v2, ADR-048)

**Frame.** Every viewpoint the module receives is relative to the **centre of the lifted
region**, in display axes (+x right, +y up, +z toward the viewer), metres:

| Path | Region |
|---|---|
| lifted weave rect (§6) | that rect, on the weave client's window |
| `xrSubmitLiftFrameDXR` | the session's window (its window binding / presenter window) |
| `xrSubmitLiftFrameDXR` on a connection with no session window (e.g. `displayxr-cli lift`) | the panel |

So a lifted video in a corner window looks straight-on to a viewer facing that window, not to a
viewer facing the panel centre (the same window-relative rule ADR-012 sets for Kooima). This
**rebase is unconditional** in v2 — it fixes v1, which passed panel-centred eyes. EXPLICIT
viewpoints are given in display space as before and are rebased the same way; nothing else is
applied to them.

**Policy** (`XrLiftViewControlDXR`, TRACKED only; with an app rig see §4.2). Chain it on `XrLiftOptionsDXR` (submit or
weave rect) or directly on `XrWeaveRectLiftDXR` (the direct chain wins). It is **sticky per
stream**: the last one sent applies until another is sent. Omitted, the defaults apply:

| Field | Default | Meaning |
|---|---|---|
| `ipdFactor` | 1 | scales each eye's offset from the pair midpoint ([0, 1]; 0 = mono) — display-rig step 1a |
| `parallaxFactor` | 1 | lerps the midpoint toward the nominal viewer `(0, 0, nominal distance)`: 1 = follow the viewer, 0 = fixed straight-on — step 1b |
| `axisMode` | X | which midpoint components follow the viewer: X, XY or XYZ. Masked components are pinned (y = 0, z = the nominal distance) |
| `maxOffsetMeters` | 0 (none) | clamp of the midpoint's x / y offset from the rect centre |
| `recenterMode` | EASE_BACK | OFF or EASE_BACK: whether the camera returns to centre after a hold (below) |
| `recenterHoldSeconds` | 1 s | how long the offset must stay beyond ~5 mm before recentering starts (< 0 = default) |
| `recenterTimeConstantSeconds` | 2 s | exponential time constant of the recentering (<= 0 = default) |

Order, per conversion: rebase → ipd factor → parallax factor → axis mask → recenter → clamp.

**Recentering.** One filter per stream, frame-rate independent (`1 - exp(-dt / tau)` per step),
reset when the stream is created and whenever eye tracking is lost.

- `EASE_BACK` (default) — the camera eases back to the scene camera origin. If you move your
  head you see look-around temporarily; once your offset has stayed beyond the threshold for the
  hold time, the camera returns to centre — no look-around — with the time constant. Any new
  head motion gives immediate, temporary look-around again. (Mechanically: a reference viewer
  position starts at the straight-on viewer and, after the hold, slides toward the head; the
  rendered offset is the head's offset from that reference, so it decays to 0 wherever the
  viewer settles.)
- `OFF` — the rendered offset is always the viewer's offset from the rect centre.

**Defaults changed behaviour.** A v1 caller that chains nothing gets the rect-relative rebase
and the EASE_BACK default. `DXR_LIFT_RECENTER=off` (service environment, read once when
the lift module is created) forces recentering OFF for every stream, for A/B.

**Echo.** Chain `XrLiftResultViewpointsDXR` on `XrLiftResultDXR` to receive the viewpoints the
result was synthesized for (after the policy, same frame), the region's centre in display space
and its size — so an app can render matching 3D for the same eyes. `viewpointCountOutput` 0 =
no viewpoints were known (the module used its defaults) or a DEPTH stream.

**What the module is told.** The DP receives the processed viewpoints plus, in appended
`xrt_dp_lift_params` fields (`XRT_DP_LIFT_HAS_VIEWPOINT_POLICY`): `viewpoint_frame` (RECT),
`rect_width_m` / `rect_height_m`, `baseline_m` (first-to-last viewpoint distance, i.e. the eye
separation after the ipd factor), `axis_mode` and `max_offset_m`. The plug-in only translates
units — it normalises by `baseline_m` rather than a fixed eye distance, and honours every
component it is sent.

### 4.2 App rig drives the viewpoints (spec v3, ADR-048 Addendum A)

An app that renders its own 3D next to (or around) lifted content wants both to share **one
camera**. Chain the same `XR_DXR_view_rig` descriptor it chains on `xrLocateViews` — an
`XrDisplayRigDXR` or an `XrCameraRigDXR` — on `XrLiftOptionsDXR` (submit or weave rect) or
directly on `XrWeaveRectLiftDXR` (the direct chain wins; a camera rig wins over a display rig,
as in `xrLocateViews`). The runtime then derives the TRACKED viewpoints with **the same
functions `xrLocateViews` runs** (displayxr-common's shared rig core, `dxr_view_math.h`), with
the lifted rect as the screen:

| Rig | Lift viewpoints (rect-relative metres) |
|---|---|
| `XrDisplayRigDXR` | `dxr_display3d_compute_views(...).eye_display` with the rect as the screen and `virtualDisplayHeight` = the rect height (m2v = 1): the eyes after `ipdFactor` / `parallaxFactor` (steps 1a / 1b), scaled by `perspectiveFactor`. With m2v ≠ 1 these are exactly the located `eye_display / m2v`. |
| `XrCameraRigDXR` | the physical viewer whose Kooima frustum onto the rect **equals** the camera rig's frustum: with `l = m2v · (processed eye − (0, 0, nominal))` (the shared camera3d core's eye, identity pose), `invd = convergenceDiopters`, `ht = tan(verticalFov / 2)` and `Z0 = rect_h / (2 ht)`: **E = Z0 · (l · invd + (0, 0, 1))**. Proof: Kooima's `tan_right = (W/2 − Ex)/Ez` reduces to the camera's `(ht·aspect − lx·invd) / (1 + lz·invd)`; likewise for the other three edges. `convergenceDiopters` 0 (parallel cameras) collapses every view onto `(0, 0, Z0)` — no lift parallax, which is what parallel cameras see of a flat image. |

The rig `pose` and `virtualDisplayHeight` only place the app's virtual world; they do not move
the physical viewpoints. The rig **replaces** `XrLiftViewControlDXR`'s `ipdFactor` /
`parallaxFactor`; its axis mask, recentering and clamp still apply, to the viewer **before** the
rig maps it (order: rebase → axis mask → recenter → clamp → rig). For viewpoints identical to
`xrLocateViews`' (to float tolerance — `tests_aux_lift_rig_depth` checks both rigs against the
shared core), chain `XrLiftViewControlDXR` with `XR_LIFT_AXIS_MODE_XYZ_DXR` and recentering OFF.

The rig **rides with the options**, like EXPLICIT viewpoints: a submit (or lifted rect) that
carries `XrLiftOptionsDXR` without a rig clears it; one that carries no options keeps the
stream's last rig. EXPLICIT viewpoints, DEPTH and GAUSSIANS streams ignore it. The echo
(`XrLiftResultViewpointsDXR`) reports the rig's viewpoints, so the app renders from the very eyes
the lifted views were synthesized for. The DP is told where they came from in
`xrt_dp_lift_params::viewpoint_source` (`DISPLAY_RIG` / `CAMERA_RIG`; `XRT_DP_LIFT_HAS_APP_RIG`)
and must reproduce them as given — no extra view gain — plus `nominal_z_m`.

**EXPLICIT viewpoints on lifted weave rects** are accepted since v3, with the same validation
and the same rect rebase as on a submit.

### 4.3 Auxiliary depth (spec v3, ADR-048 Addendum A)

An SBS / NVIEW stream can return the **depth map of the same inference** that produced its
views, so app content can occlude, intersect and point at lifted video in one metric space
(ADR-046 cursor depth and ADR-040's rear budget reason about it the same way).

- **Ask** at creation: chain `XrLiftDepthRequestDXR` on `XrLiftStreamCreateInfoDXR`
  (`preferredFormat`: `DXGI_FORMAT_R32_FLOAT` / `_R16_FLOAT` / 0). On a DEPTH or GAUSSIANS stream
  it is `XR_ERROR_VALIDATION_FAILURE`. **Capability**: `XrLiftDepthPropertiesDXR` chained on
  `XrLiftPropertiesDXR` (`auxDepthSupported`, `auxDepthSemantics`). A request the module cannot
  serve still creates the stream; results then report `depthValid = XR_FALSE`.
- **Receive**: chain `XrLiftDepthResultDXR` on `XrLiftResultDXR`. `depthTexture` + `fence`
  follow §5.1's export rule (first acquire that carries depth, and every depth reallocation);
  the fence is the SAME object as the views' fence with the SAME `fenceValue` — one wait covers
  both. `frameId` equals the result's; `sameInference` says the module produced depth and views
  in one pass.
- **Decode** a sample `s`: `d = valueScale · s + valueOffset`; `depth = d` (LINEAR) or `1 / d`
  (INVERSE), in `units` (METRIC = metres along the source camera's axis; RELATIVE = unitless,
  larger = farther). `nearDepth` / `farDepth` bound it (0 = unknown).
- **Intrinsics** (`intrinsicsValid`, METRIC only): the source camera the module assumed, in
  depth-map pixels — `focalLengthPx`, `principalPointPx` — place a texel `(u, v)` in the
  camera's metric frame: `X = (u − cx)·depth/fx`, `Y = −(v − cy)·depth/fy`, `Z = −depth`.

**`depthToDisplay` — the display alignment.** The woven result presents the source image on the
lifted rect, seen from the viewpoint midpoint `C` (= `viewpoint`, rect-relative — the midpoint of
the echoed viewpoints, or the straight-on viewer `(0, 0, nominal)` when none were known), with the
module's convergence depth `dc` (= `convergenceDepth`, the decoded depth it put at zero
disparity) **on the screen plane**. So a texel at normalised coordinates `(un, vn)` — texel
centre, `un = (u + 0.5)/width`, `vn = (v + 0.5)/height`, v down — sits on the rect at

    P = (W·(un − ½), H·(½ − vn), 0)                (rect-relative; W × H = the rect in metres)

and a texel of depth `d` lies on the ray from the viewpoint through `P`, at the fraction `d / dc`
of the way:

    X = R + C + (P − C) · d / dc                   (R = rectCenter, display space)

`d = dc` lands on the rect (z = the screen plane), `d → 0` converges on the viewpoint, `d > dc` is
behind the screen. `X` is linear in `(un·d, vn·d, d, 1)`, so it is one column-major 4×4
(`m[col·4 + row]`, output `w` = 1):

| column | multiplies | x | y | z | w |
|---|---|---|---|---|---|
| 0 | `un·d` | `W/dc` | 0 | 0 | 0 |
| 1 | `vn·d` | 0 | `−H/dc` | 0 | 0 |
| 2 | `d` | `(−W/2 − Cx)/dc` | `(H/2 − Cy)/dc` | `−Cz/dc` | 0 |
| 3 | 1 | `Rx + Cx` | `Ry + Cy` | `Rz + Cz` | 1 |

Content an app renders from the echoed eyes (or from its own rig, §4.2) therefore lines up with
the lifted picture: both are perspective images of the same points from the same viewpoint. The
transform is the *display* geometry (strength 1); the intrinsics describe the *camera* geometry —
the two coincide only when the module's field of view equals the rect's angular size from `C`.
`transformValid` needs METRIC units, `dc > 0` and a known rect; **RELATIVE depth reports
`transformValid` / `intrinsicsValid` XR_FALSE rather than inventing a scale.** Reference
implementation and tests: `src/xrt/auxiliary/util/u_lift_depth.{h,c}`,
`tests/tests_aux_lift_rig_depth.cpp`.

**DP side.** The runtime passes the request in `xrt_dp_lift_stream_info::aux_outputs`
(`XRT_DP_LIFT_AUX_DEPTH`, + `aux_depth_format`), and right after each successful `lift_convert`
of such a stream calls the appended D3D11 slot **`lift_get_depth`** (`XRT_DP_D3D11_HAS_LIFT_DEPTH`),
which fills `struct xrt_dp_lift_depth` (`XRT_DP_LIFT_HAS_AUX_DEPTH`): the DP-owned R32F / R16F
texture (valid until the next convert), units, encoding + scale / offset, source size, focal /
principal point, near / far, `convergence_depth`, `same_inference`, the module's frame id. The
runtime copies it into the result's ring slot and then the per-stream depth export texture,
and derives `depthToDisplay` itself. Capability: `xrt_dp_lift_caps::aux_outputs` /
`aux_depth_semantics`. An older plug-in (no slot) simply returns no depth.

## 5. Acquiring results

### 5.1 Textures (DEPTH / SBS / NVIEW)

`xrAcquireLiftResultDXR` returns the newest finished result **newer than the last one it
returned**, or the success code `XR_LIFT_NOT_READY_DXR` (keep presenting the previous result).

Handles follow the XR_DXR_weave output contract. The service copies the result into a
per-stream **export texture** and signals the export **fence**:

- `outputTexture` / `fence` are shared HANDLEs, handed out on the **first** acquire and again
  whenever the export texture is **reallocated** (result size or format changed — watch
  `extent` / `format`); `NULL` on steady-state acquires. The caller imports them once per
  allocation and closes its handles when done.
- The caller GPU-waits `fence` to `fenceValue` before sampling, and **finishes sampling before
  its next acquire on the stream** — the next result is copied into the same texture.
- The runtime latches "exported" only when both handles were produced (the #1427 rule), so a
  transient export miss retries on the next acquire rather than leaving the caller handle-less.

`sourceTime` is the submitting call's value, verbatim; `frameId` identifies the frame;
`latency` is submit → conversion finished on the runtime clock.

### 5.2 Blobs (GAUSSIANS)

`xrAcquireLiftBlobDXR` uses the standard two-call idiom on `byteCapacityInput`:

1. Capacity 0 → fills `byteCountOutput`, `frameId`, `sourceTime`, `format` and **latches** that
   blob in the service.
2. Capacity ≥ `byteCountOutput` → the latched blob's bytes (the SAME frame, even if a newer one
   finished in between); the latch is consumed.

A non-zero capacity smaller than the blob is `XR_ERROR_SIZE_INSUFFICIENT` (latch kept).
`XR_LIFT_NOT_READY_DXR` when nothing newer than the last blob handed out exists. A texture
acquire on a GAUSSIANS stream (and vice versa) is `XR_ERROR_VALIDATION_FAILURE`. Blobs larger
than 256 MiB are refused by the transport (`XR_ERROR_RUNTIME_FAILURE`).

Why a separate entry point rather than a struct chained on the texture acquire: the two result
kinds never coexist on a stream, and a blob needs the two-call size negotiation a texture does
not. One call per result kind keeps each contract minimal.

### 5.3 D3D12 callers (engine apps, ADR-049)

The handles are D3D12-consumable as they are: the export texture is an NT-handle shared texture
**without** a keyed mutex, the fence an NT-handle shared fence.

- **Results:** `ID3D12Device::OpenSharedHandle(outputTexture)` → `ID3D12Resource`,
  `OpenSharedHandle(fence)` → `ID3D12Fence`; close both handles. Per result:
  `ID3D12CommandQueue::Wait(fence, fenceValue)` before the first read. The rest of §5.1 holds.
- **Adapter:** results live on the service's ingest adapter (the LUID an IPC D3D session is
  told to use). A device on another adapter cannot open them — treat an `OpenSharedHandle`
  failure as `UNAVAILABLE` and fall back.
- **Input:** create the input with `D3D12_HEAP_FLAG_SHARED` and `ID3D12Device::CreateSharedHandle`
  and pass that NT handle (`inputIsDxgi = XR_FALSE`). D3D12 has no keyed mutex, so the service
  snapshots the texture at submit **without a GPU wait**: complete the writes before
  `xrSubmitLiftFrameDXR` (signal + CPU-wait a fence) and do not rewrite the texture until a later
  frame — double-buffer the input. A submit-side fence is planned (ADR-049 follow-ups).

## 6. Lifting weave rects

```c
XrLiftOptionsDXR opt = {XR_TYPE_LIFT_OPTIONS_DXR, NULL, XR_LIFT_CONVERGENCE_AUTO_DXR, 1.0f, XR_TRUE,
                        XR_LIFT_VIEWPOINT_SOURCE_TRACKED_DXR, 2, NULL, 0.0f};
XrWeaveRectLiftDXR lift = {XR_TYPE_WEAVE_RECT_LIFT_DXR, &opt, /*rectIndex*/ 3, sbsStream};
XrWeaveSubmitLiftRectsDXR lifts = {XR_TYPE_WEAVE_SUBMIT_LIFT_RECTS_DXR, NULL, 1, &lift};
XrWeaveSubmitRectsDXR rects = {XR_TYPE_WEAVE_SUBMIT_RECTS_DXR, &lifts, n, rectArray};
XrWeaveSubmitInfoDXR in = {XR_TYPE_WEAVE_SUBMIT_INFO_DXR, &rects, input, ...};
xrWeaveSubmitDXR(session, &in, &out);
```

"The content of weave rect `rectIndex` is 2D — lift it before weaving." An `XrRect2Di` has no
`next`, so the association is by index into `XrWeaveSubmitRectsDXR::rects`; up to
`XR_WEAVE_SUBMIT_MAX_LIFT_RECTS_DXR` (8) rects per submit. The stream must be an SBS or NVIEW
stream of the same session. Since v3 options on the weave path may use EXPLICIT viewpoints
(§4.2), and a rig may be chained on the rect or its options.

What the caller draws:

| Weave layout | Where the 2D frame goes |
|---|---|
| Batch (v3/v4/v5) — window-sized input | the WHOLE rect holds the 2D frame (not squeezed SBS) |
| N-view atlas (v6) | the 2D frame at the rect's scaled position in EVERY tile (tile 0's copy is lifted) |

What the service does, inside the same `xrWeaveSubmitDXR`:

1. **Snapshot.** The rect's region (tile 0's on v6) is blitted into the stream's mailbox —
   exactly a `xrSubmitLiftFrameDXR`, stamped with the runtime clock — except that the service
   **downsamples** it so its long edge is at most a cap (default **1920**, aspect kept, both
   dims even; service env `DXR_LIFT_MAX_INPUT_EDGE`, `0` = off, smaller values clamp to 256).
   The rect is in device pixels (a fullscreen player on an 8K panel is 7680x4319), the module
   synthesizes its views at *input* resolution, and step 2 stretches the result back into the
   rect anyway — so an uncapped snapshot buys little sharpness at a large conversion-rate cost.
   The module sees the capped size as the frame's input size (a `focalPx` hint is scaled with
   it). An app's own `xrSubmitLiftFrameDXR` frame is never capped: its size is the app's choice.

   **Letterbox crop** (before the cap; service env `DXR_LIFT_LETTERBOX`, default on, `0` = off).
   The service measures each rect's rows and columns (the fraction of non-black pixels per
   bucket, a small GPU reduction read back asynchronously) and, once black bars have held for
   ~45 frames, snapshots only the **active** area. A bar GROWS only into truly black buckets
   (< 3 % non-black — a dark scene's edge is rarely that empty), and SHRINKS only when picture
   (≥ 25 %) intrudes for 6 consecutive frames and by more than the profile's jitter, so a
   caption burst does not un-crop. A shorter bar is extended to match the opposite one when the
   extra band is separated from the picture by a black gap (a subtitle line, however dense) or
   is only sparsely lit (< 60 %). Frames without picture (a cut to black) change nothing; a
   rect resize resets the crop. One WARN per stream per crop change. Like the cap, never
   applied to an app's own `xrSubmitLiftFrameDXR` frame.
2. **Weave the latest result at the CURRENT rect.** The stream's newest result (from an earlier
   frame) is stretched into the rect's current position — into its **active** part when the
   letterbox crop was in effect for that result; the bars (subtitles included) are then the 2D
   frame, written identically into both views AFTER the result and a few pixels into the active
   area (covering the profile's one-bucket edge slack and the module's edge seam) (v3), i.e.
   flat on the screen plane, or left as
   the caller drew them (v6): the middle stereo pair into the SBS scratch's left/right tiles
   (v3), or view *v* of the result into tile *v* (v6; equal view counts map 1:1, otherwise
   proportionally). Then the ordinary single weave of the whole window runs.
3. **Flat until the first result.** With no result yet, the rect is woven flat: the service
   writes the 2D frame into both views (v3), or leaves the caller's identical tiles as drawn (v6).

So position and size are always this frame's, and only the depth is one conversion behind.
A v6 frame carrying lift rects never takes the zero-copy path (the service writes into its crop
copy, never the caller's texture).

**Wire.** The lifted-rect set travels as its own IPC call (`lift_weave_rects`) immediately
before the `weave_submit` it belongs to, on the same connection; the service consumes it with
that one submit. The weave wire itself is byte-identical. A rect naming a stream the connection
does not own, or a DEPTH/GAUSSIANS stream, is refused (non-fatal) and woven as drawn.

**Compatibility.** A runtime without `XR_DXR_lift` ignores the unknown chained struct and weaves
the rect as whatever the caller drew — flat 2D, never a misread. No version gate is needed; the
extension being enabled is the gate.

## 7. Scheduling, priority and stats

The module converts one frame at a time (one GPU, often one serialised inference queue), so
concurrent streams share its throughput. The lift thread runs **rounds**; each round converts,
in order:

| Priority | Converted |
|---|---|
| `HIGH` | every HIGH stream with a new frame, every round |
| `NORMAL` (default) | ONE NORMAL stream with a new frame per round, round-robin |
| `LOW` | every LOW stream with a new frame, on every 4th round |
| `PAUSED` | never — it keeps serving its last result (the weave keeps weaving it; acquires report NOT READY) |

A round is counted only when some non-paused stream had a frame, so an idle service does not
spend LOW rounds. Example — a call with four tiles and the active speaker HIGH: the speaker
converts every round, the other three rotate one per round.

`xrSetLiftStreamPriorityDXR` is a per-stream setter (one IPC call, effective at the next round),
because priority changes rarely — when the active speaker changes — and a per-frame field would
cost nothing extra but invite churn. Scheduling is runtime policy; the vendor module never sees
it.

`xrGetLiftStreamStatsDXR` returns the stream's counters (`framesSubmitted`, `framesConverted`,
`framesDropped`, `framesFailed`), submit → result latency (last, moving average, min, max) and
the **effective conversion rate** (results per second, moving average over publish intervals) —
what the priority actually buys under the current load. One IPC round trip, no GPU work: poll at
UI rates.

## 8. Timestamps and latency

- `sourceTime` is the caller's (a video PTS, an `XrTime`); the runtime never interprets it and
  echoes it on the result produced from that frame, so a late result can be paired with its
  source.
- `latency` / stats latencies are the runtime clock from the moment the snapshot was committed to
  the moment the result was published into the ring (i.e. excluding the caller's acquire poll).
- A consumer that wants "the depth for the frame I am showing" keys on `sourceTime`; one that
  wants "the freshest depth" ignores it. The weave path always uses the freshest.

## 9. Service implementation (for reviewers)

- **One lift thread** per service (`d3d11_lift.cpp`), created on the first lift call. It owns a
  **dedicated D3D11 device** on the service adapter (`ID3D11Multithread` protection on) and the
  plug-in's **lift-only display processor** (`xrt_plugin_iface::create_dp_d3d11_lift`; fallback:
  the ordinary factory with a NULL window). It is the only thread that touches that DP.
- **Per stream:** a latest-wins mailbox (two input slots) and a two-slot output ring, both
  keyed-mutex shared textures crossing between the service device and the lift device; the
  module's output is copied into the ring before its next call. Consumers **pin** a ring slot for
  the length of one GPU-copy issue; the worker never overwrites the latest or a pinned slot. The
  state machine is platform-neutral and unit-tested (`u_lift_mailbox.h`,
  `tests/tests_lift_mailbox.cpp`), as is the round scheduler.
- **Locks:** the lift mutex is a leaf, never held across GPU or vendor work. IPC calls take the
  service's `immediate_ctx_mutex` alone, for one blit / copy; the weave path takes the lift mutex
  under it. The lift thread never takes the service's context or render locks — see
  [service-architecture §3](../../architecture/service-architecture.md#3-threads-and-locks).
- **Logging:** one WARN per module state change and per stream create/destroy; per-stream
  statistics at INFO, throttled to one line per 5 s.
- **Kill switch:** `DXR_LIFT=0` in the service's environment → UNAVAILABLE, the vendor DP is
  never created.
- **Vendor knobs live in the SERVICE's environment** (e.g. a module's backend selection), not
  the app's — the conversion runs in `displayxr-service.exe`.
- **Streams belong to the IPC connection**, not the session, and die with it; the calls need no
  compositor session, which is what lets `displayxr-cli lift` drive them headless — and what lets
  an in-process session use lift (ADR-049): it opens a sessionless `APP` connection named
  `<exe> (in-process lift)` (`ipc_client_lift_link.c`, background connect, never launches the
  service, retried at most every 5 s) and runs the same calls on it. WARN lines in the app's
  log: `[lift] in-process: lift-only service connection up (generation N)`, `... no service
  connection for 2D->3D conversion ...` (once), `... connection (generation N) is gone ...`.

## 10. Error codes and connection loss

Mirrors XR_DXR_weave §4b.

| Result | When | Session still usable? |
|---|---|---|
| `XR_LIFT_NOT_READY_DXR` (success) | no result newer than the last acquired | yes |
| `XR_ERROR_VALIDATION_FAILURE` | a struct out of contract (mode not one bit, bad viewCount, EXPLICIT without viewpoints, `XrLiftDepthRequestDXR` on a DEPTH / GAUSSIANS stream, wrong acquire for the mode, bad `rectIndex`) | yes — caller bug, nothing sent |
| `XR_ERROR_FEATURE_UNSUPPORTED` | in-process session other than D3D11/D3D12 on Windows; mode not supported by a READY module; no module; in-process: a stream of a lift connection that was lost (destroy it, create a new one) | yes — permanent for that request |
| `XR_ERROR_LIMIT_REACHED` | `maxStreams` reached | yes |
| `XR_ERROR_SIZE_INSUFFICIENT` | blob capacity too small (latch kept) | yes |
| `XR_ERROR_RUNTIME_FAILURE` | the service refused this call over a healthy pipe — most commonly the 4 ms input keyed-mutex miss, or a stream the service no longer knows | **yes — retry next frame** |
| `XR_ERROR_INSTANCE_LOST` | the IPC connection is gone (IPC sessions only — an in-process session's lift connection dying is `XR_ERROR_RUNTIME_FAILURE` on that call; the session renders on, and the next `xrGetLiftPropertiesDXR` reconnects) | no — recover with a new instance (weave §4b) |
| `XR_ERROR_SESSION_LOST` | any call after that | no |

## 11. Vendor contract, in one table

| Runtime owns | Plug-in owns |
|---|---|
| the lift thread, device, DP lifetime | one synchronous conversion per call |
| latest-wins mailbox, drops, frame ids | the model, its backend, licensing, warm-up (reported as ACTIVATING) |
| output ring copies, export textures, fences | an output valid until its next call |
| priority scheduling across streams | nothing about scheduling |
| tracked eyes → explicit viewpoints, rebased to the lifted rect, with the viewpoint policy applied (ADR-048) — or derived from the app's rig with the `xrLocateViews` math (v3) | honouring explicit viewpoints over any tracker of its own; unit translation only (normalise by `baseline_m`, honour every component sent; rig viewpoints reproduced as given) |
| copying the auxiliary depth out, exporting it, the display transform (`depthToDisplay`) | returning the depth of the same inference + its metadata (`lift_get_depth`) |
| weaving SBS / N-view results (ADR-007) | never weaving a lift result |
| timestamps, latency, stats | reporting `typical_latency_ns` |

Full slot reference: [`xrt_plugin_iface.md` § lift](../../reference/xrt_plugin_iface.md#turning-2d-into-3d-the-lift-slots-adr-042-xr_dxr_lift).
sim_display carries an env-gated fake (`SIM_DISPLAY_FAKE_LIFT=1`; `SIM_DISPLAY_FAKE_LIFT_LATENCY_MS`,
default 8): shifted SBS/N-view, a gradient depth, a two-layer 3DGS PLY — the whole path runs in CI
without hardware.

## 12. Probe and diagnostics

```
displayxr-cli lift caps [--json] [--wait S] [--timeout S]
displayxr-cli lift probe <image|frames_dir> [--mode depth|sbs|nview|gaussians] [--n N]
                         [--views N] [--strength F] [--convergence F] [--focal PX]
                         [--priority paused|low|normal|high] [--pipelined] [--fps F] [--out DIR]
```

Connects over IPC as a DIAG client (non-elevated prompt on Windows), waits through ACTIVATING,
creates a stream, submits the image(s) through a shared keyed-mutex texture exactly as a
browser does, reads each result back through the export texture + fence and writes
`lift_out_<i>.png` (depth normalised to 8-bit grey) or `lift_out_<i>.ply`. It prints per frame
the submit → acquire latency seen by the caller and the service's own latency, and at the end
the stream stats and a latency summary. `--pipelined` submits at `--fps` without waiting, to
measure throughput and drops. `lift caps` polls through ACTIVATING for `--wait` seconds
(default 8) and is hard-bounded by `--timeout` (default `max(10, wait + 2)` s): a service that
never answers — wedged, or not accepting connections — prints `TIMEOUT` and exits 4 instead of
blocking (#1812). The service answers caps from its last-known snapshot and never waits on the
module, so a module stuck loading reads ACTIVATING, not a hang; the service logs one WARN
(`[lift] module still ACTIVATING ...`) when that lasts past 30 s. `displayxr-cli selftest` includes a `lift_caps` check (headless
WARP + the lift-only factory): modes 0 passes; only malformed caps fail
(`CLI_SELFTEST_BAD_LIFT_CAPS`, exit 11).

## 13. Consumers

| Consumer | Path | Uses |
|---|---|---|
| DisplayXR Browser (Chromium fork) | GPU process → service | `xrGetLiftPropertiesDXR` to decide vendor vs open default (ADR-042); `XrWeaveSubmitLiftRectsDXR` for inline 2D video/images lifted in place; `xrSubmitLiftFrameDXR` + acquire where the page wants the depth/views itself |
| DisplayXR web SDK (`@displayxr/inline3d`) | via the browser | the same policy: prefer the runtime module when READY; GAUSSIANS via the blob path supersedes the SDK's open photo → splats lift |
| 3D calling (up to 4 mono tiles) | app → service | one SBS stream per tile; `xrSetLiftStreamPriorityDXR` HIGH for the active speaker |
| Engine apps (Unity display provider, D3D12 in-process) | in-process session → lift-only connection → service (ADR-049) | streams + texture acquire (§5.3); no weave rects (the app composes the views itself) |
| `displayxr-cli lift` | DIAG IPC | caps + the N0 probe (§12) |

When changing the header, byte-sync every consumer's vendored copy and rebuild it — coupled-PR
order: runtime → extensions auto-sync → consumers.

## 14. Version history

| Version | Change |
|---|---|
| 1 | Initial: properties + states, streams (DEPTH / SBS / NVIEW / GAUSSIANS), non-blocking latest-wins submit, texture acquire (weave-style handles + fence), blob acquire (two-call latch), weave-rect lift chain, per-stream priority scheduling + stats, `focalPx`. |
| 2 | Viewpoint policy (ADR-048, §4.1): viewpoints relative to the lifted rect / submitting window centre (was: the panel centre), `XrLiftViewControlDXR` (ipd / parallax factors, axis mode, offset clamp, recentering; defaults X + EASE_BACK 1 s / 2 s), `XrLiftResultViewpointsDXR` echo. |
| 3 | App camera + metric depth (ADR-048 Addendum A, §4.2–4.3): `XrDisplayRigDXR` / `XrCameraRigDXR` on `XrLiftOptionsDXR` / `XrWeaveRectLiftDXR` drive the TRACKED viewpoints with the `xrLocateViews` rig math; EXPLICIT viewpoints accepted on lifted weave rects; auxiliary depth for SBS / NVIEW streams — `XrLiftDepthRequestDXR`, `XrLiftDepthResultDXR` (texture + shared fence, units, encoding, intrinsics, convergence depth, viewpoint, `depthToDisplay`), `XrLiftDepthPropertiesDXR`. A separate version from 2 because v2 can ship on its own (#1864): an app tests `SPEC_VERSION >= 3` for these. |

Runtime behaviour, no spec-version change: in-process D3D11 / D3D12 sessions on Windows use lift
over a lift-only connection (ADR-049). A consumer detects it the same way as before — an older
runtime answers `xrGetLiftPropertiesDXR` on such a session with `XR_ERROR_FEATURE_UNSUPPORTED`.

## Probing on a Windows box — gotchas (first N0 run, 2026-09-25)

- **Selecting the display processor.** `XRT_PREFERRED_PLUGIN_ID` does *not* switch the active DP when an installed vendor plug-in also probes; `DXR_PLUGIN_EXCLUSIVE=<plugin-id>` in the **service's** environment does (relaunch the service non-elevated from a `.bat` that sets it). `SIM_DISPLAY_FAKE_LIFT=1` alongside `DXR_PLUGIN_EXCLUSIVE=sim-display` exercises the whole path hardware-free (caps modes = DEPTH|SBS|NVIEW|GAUSSIANS).
- **`displayxr-cli lift …` must run non-elevated** — an elevated prompt reports "not connected to the service" (same integrity-level rule as every other IPC client).
- **Vendor module version gate.** The Leia plug-in logs the NeurD version it loaded and reports `UNAVAILABLE` when it is outside the supported range (needs 0.4.3+): e.g. a box with Immersity Live's NeurD 0.3.7 shows `state -> ACTIVATING` then `UNAVAILABLE` with the reason in the service log. Upgrade the NeurD runtime; the plug-in never crashes on a mismatch.
- **Probe timing.** `lift probe` stamps submit→acquire before any readback; use `--no-write` (or `--write-every N`) for throughput runs — encoding a 4K SBS PNG takes seconds and would otherwise cap the pipelined rate.

# XR_DXR_lift — 2D→3D Conversion Service

| Field | Value |
|---|---|
| **Extension Name** | `XR_DXR_lift` |
| **Spec Version** | 4 (one extension; every earlier version's structs, values and behaviour are kept — §14) |
| **Extension Type** | Instance extension. Conversion runs in the service, on whichever **conversion provider** the runtime selects per task (ADR-053). Reached by IPC sessions, and by in-process D3D11 / D3D12 sessions on Windows over a lift-only connection (ADR-049); advertised on every desktop platform, where a service with no provider reports nothing to convert with |
| **Header** | `src/external/openxr_includes/openxr/XR_DXR_lift.h` (canonical; auto-syncs to `displayxr-extensions`) |
| **Status** | Provisional. Type values: `1004999270–285` (spec 1–3), `1004999340–369` (spec 4 and 5), pending Khronos registry |
| **Decision records** | [ADR-053](../../adr/ADR-053-conversion-providers.md) (conversion providers, tasks, readiness, the consumer contract; amends ADR-042 and ADR-049) · [ADR-042](../../adr/ADR-042-vendor-2d3d-conversion-supersedes-default.md) (pre-weave results, the asynchronous mailbox, scheduling) · [ADR-048](../../adr/ADR-048-lift-viewpoint-policy.md) + Addendum A (viewpoint policy, app rig, auxiliary depth) · [ADR-049](../../adr/ADR-049-in-process-lift-rides-the-service.md) (in-process sessions) |
| **Provider contract** | [`conversion-provider-discovery.md`](../runtime/conversion-provider-discovery.md) + `src/xrt/include/xrt/xrt_conversion_plugin.h` (§11). The display-processor lift slots (`xrt_dp_lift.h`, `XRT_DP_D3D11_HAS_LIFT`) are frozen and reached only through the built-in `dp-lift` adapter |

## 1. What it is

`XR_DXR_lift` turns 2D content into 3D for apps: monocular depth, depth for video, stereo and
N-view synthesis, and photo → Gaussian splats. The work is done by **conversion providers** —
plug-ins of their own type, from a display vendor, a model vendor or the open models — that the
service loads, selects and schedules (ADR-053). The extension exposes them **generically**,
exactly the way `XR_DXR_weave` exposes the vendor's weaver: the caller learns what a task can
do, how ready it is and what it produced, never which provider or model ran.

Four ideas carry the whole extension:

1. **Tasks, chosen by the runtime; a caller waits.** A caller asks for a task (depth, views,
   splats); the runtime selects one provider per task and never lets readiness reorder that
   choice. A task that is not READY yet is a reason to **wait**, never a reason to switch to an
   in-app converter (§2.4, the consumer contract; it replaces ADR-042's "open default").
2. **Lift never weaves.** Results are pre-weave SBS / N-view pixels, a depth map, or a splat
   blob. Weaving stays the display processor's job on the ordinary weave path (ADR-007).
3. **Asynchronous, one frame behind.** Conversion runs on runtime-owned threads, in the service
   or in a supervised conversion host. A submit is a snapshot into a latest-wins mailbox; an
   acquire returns the newest finished result. Nothing on the caller's frame path — and nothing
   on the service's weave, render or IPC threads — ever waits for a model.
4. **Geometry comes from the weave rect.** When lifted content is woven
   (`XrWeaveSubmitLiftRectsDXR`), the service weaves the latest result at the rect's *current*
   position every frame. Drag, resize and scroll are exact and real-time; only the depth lags.

```c
// Spec 3 shape — still valid, served by the task mapping of §3.1.
XrLiftPropertiesDXR props = {XR_TYPE_LIFT_PROPERTIES_DXR};
xrGetLiftPropertiesDXR(session, &props);                 // cheap; never waits
if (props.supportedModes & XR_LIFT_MODE_SBS_BIT_DXR) {   // READY or still ACTIVATING: create anyway
    XrLiftStreamCreateInfoDXR ci = {XR_TYPE_LIFT_STREAM_CREATE_INFO_DXR, NULL,
                                    XR_LIFT_MODE_SBS_DXR, XR_LIFT_CONTENT_HINT_VIDEO_DXR, 1.0f};
    xrCreateLiftStreamDXR(session, &ci, &stream);
    // per frame:
    xrSubmitLiftFrameDXR(stream, &submit, &frameId);       // never waits for the model
    XrLiftResultDXR res = {XR_TYPE_LIFT_RESULT_DXR};
    if (xrAcquireLiftResultDXR(stream, &res) == XR_SUCCESS) { /* newer result */ }
    // else XR_LIFT_NOT_READY_DXR: keep showing the 2D source / the previous result
}
```

```c
// Spec 4 shape — a task stream (§3.2); degrades to the line above on a spec-3 runtime.
XrLiftTaskStreamCreateInfoDXR task = {XR_TYPE_LIFT_TASK_STREAM_CREATE_INFO_DXR, NULL,
    XR_LIFT_TASK_VIEWS_MONO_DXR, XR_LIFT_INPUT_KIND_MONO_DXR, XR_LIFT_QUALITY_HINT_DEFAULT_DXR,
    /*viewCount: match the weave*/ 0, /*requiredOutputs*/ 0, /*exportKind: platform default*/ 0};
XrLiftStreamCreateInfoDXR ci = {XR_TYPE_LIFT_STREAM_CREATE_INFO_DXR, &task,
                                XR_LIFT_MODE_SBS_DXR, XR_LIFT_CONTENT_HINT_VIDEO_DXR, 1.0f};
xrCreateLiftStreamDXR(session, &ci, &stream);
```

## 2. Availability, readiness and the consumer contract

### 2.1 Who can call it

| Situation | Spec 3: `xrGetLiftPropertiesDXR` | Spec 4: `xrEnumerateLiftTasksDXR` |
|---|---|---|
| IPC session (any platform with a service) | the service's view (§2.3) | one element per task some provider can serve (§2.2) |
| In-process D3D11 / D3D12 session on Windows (engine apps, e.g. the Unity display provider) | the service's view, over a lift-only connection the session opens in the background on its first lift call (ADR-049): `ACTIVATING` while it connects; `UNAVAILABLE` when no service is running (it is never launched for this) or the service refuses the connection | the same, over the same connection; `imageKinds` = `D3D11_NT`, `D3D11_DXGI`, `D3D12_NT`, `CPU`; `exportKinds` = `D3D11_NT`, `CPU` |
| In-process GL / Vulkan session on Windows | `XR_ERROR_FEATURE_UNSUPPORTED` (spec 3 is D3D11 shared textures) | the service's tasks over the same kind of lift-only connection, with `imageKinds` = `exportKinds` = `CPU` |
| In-process Vulkan / GL / Metal session on desktop Linux or macOS | `XR_ERROR_FEATURE_UNSUPPORTED`, as in spec 3 | the service's tasks over a lift-only connection (a plain socket connect: the service is never launched for it; no service = no tasks), with `imageKinds` = `exportKinds` = `CPU` |
| IPC session on desktop Linux or macOS | the service's view: modes 0 / `UNAVAILABLE` (the POSIX service has no D3D11 textures to serve spec 3 with) | one element per task, `imageKinds` = `exportKinds` = `CPU` |
| Any other in-process session (Android) | `XR_ERROR_FEATURE_UNSUPPORTED`, as in spec 3 | nothing known yet (no lift-only connection on that platform; the transport lands without a spec bump, ADR-053 P5) |
| A service with no provider (no plug-in registered, or `DXR_LIFT=0`) | `XR_SUCCESS`, `supportedModes = 0`, `UNAVAILABLE` | `countOutput = 0` |

**Readiness starts early.** An instance that enables `XR_DXR_lift` tells the service it is
interested; on mains power the service then prewarms the selected provider of the default
tasks (views and video depth), on battery at the first create (`PrewarmOn`, discovery spec
§4.2). The service persists what every provider described last time, so its task table is
populated from the first query of a boot. No call of this extension ever waits for a model.

### 2.2 Tasks and readiness (spec 4)

| Task | Value | In → out | Spec-3 equivalent |
|---|---|---|---|
| `XR_LIFT_TASK_DEPTH_MONO_DXR` | 1 | one image → depth (no temporal state) | DEPTH + PHOTO |
| `XR_LIFT_TASK_DEPTH_VIDEO_DXR` | 2 | frames → temporally stable depth; reset on a discontinuity | DEPTH + VIDEO |
| `XR_LIFT_TASK_DISPARITY_STEREO_DXR` | 3 | rectified pair → disparity (spec 5) | — |
| `XR_LIFT_TASK_VIEWS_MONO_DXR` | 4 | image / frames → N ≥ 2 views, + same-inference depth | SBS, NVIEW |
| `XR_LIFT_TASK_VIEWS_FROM_DEPTH_DXR` | 5 | image + depth → N views (spec 5) | — |
| `XR_LIFT_TASK_SPLATS_MONO_DXR` | 6 | photo → 3D Gaussian splats (job lane) | GAUSSIANS |
| `XR_LIFT_TASK_SPLATS_STEREO_DXR` | 7 | pair → metric splats (spec 5) | — |
| `SPLATS_VIDEO`, `INPAINT` | 8, 9 | **reserved**: never accepted | — |

**`xrEnumerateLiftTasksDXR(session, capacityInput, &countOutput, tasks)`** (two-call idiom)
returns one `XrLiftTaskPropertiesDXR` per task that is not `ABSENT`, in ascending task order:
the **effective** capability of the task on this session — the provider the runtime selected,
its readiness, and its output contract. The element never grows (extend through `next`).

| Field | Meaning |
|---|---|
| `state`, `failureReason`, `progress` | readiness (below); `progress` in [0, 1] while `DOWNLOADING` / `WARMING`, < 0 = unknown |
| `lanes`, `inputKinds`, `outputs` | stream and / or job lane; accepted input shapes; output bits (`AUX_DEPTH`, `METRIC`, `INTRINSICS`, `TEMPORAL`, `EXPLICIT_VIEWPOINTS`, `CONVERGENCE_HINT`, `COMPOSED` = served by a depth provider plus the runtime's reference view synthesis) |
| `depthKinds`, `blobFormats` | what the task can return (§5.5) |
| `imageKinds`, `exportKinds` | what **this session** can hand in and take out (§4.5, §5.4). 0 = known, not reachable from this session yet |
| `maxStreams` | the selected provider's limit **for this task**, runtime-wide; 0 = no stream lane |
| `maxViews` | `VIEWS_*` tasks; enforced at create |
| `maxInputExtent` | larger inputs are downscaled by the runtime; 0 = none |
| `typicalLatency`, `warmEstimate` | per conversion (or per job), and cold → READY; 0 = unknown |
| `licenceClass` | `PERMISSIVE`, `COMMERCIAL`, `RESEARCH_ONLY` (present only when the **user** opted in) or `UNKNOWN` |

**States** (`XrLiftTaskStateDXR`; values equal the plug-in ABI's):

| State | Value | For a caller |
|---|---|---|
| `UNKNOWN` | 0 | discovery has not settled: **wait** |
| `READY` | 1 | conversions run now |
| `WARMING` | 2 | loading, compiling, first inference, or the provider's host restarting or being updated: **wait** |
| `ABSENT` | 3 | no provider can serve it (never enumerated) |
| `INCOMPATIBLE` | 4 | installed but unusable here (version / OS / GPU / driver) |
| `UNLICENSED` | 5 | its licence check refused it |
| `FAILED` | 6 | see `failureReason` (`LICENCE`, `LOAD`, `PLATFORM`, `NETWORK`, `VERSION`, `RESOURCE`, `MODEL`, `DEVICE_LOST`, `CRASHED`); may recover later, with an event |
| `DISABLED` | 7 | switched off by the user or policy |
| `INSTALLED` | 8 | present, model cold; a create or a warm request starts it: **wait** |
| `DOWNLOADING` | 9 | model bytes being fetched: **wait** |
| `EVICTED` | 10 | unloaded after long idle; a reload reports `WARMING`: **wait** |

The **waiting** states (UNKNOWN, INSTALLED, DOWNLOADING, WARMING, EVICTED) accept a create, and
the stream's frames wait latest-wins until READY. The **terminal** states make selection fall
through to the next provider at the next create, and the create is refused only when no
candidate is left (§3.2).

**`xrRequestLiftTaskWarmDXR(session, task)`** asks the runtime to warm the selected provider of
`task` now (for example when a page shows a "3D" control). It returns at once and is
idempotent; progress arrives through the state and the events.

**Events** (opt-in per session: sent only to a session that has made a spec-4 call, so a
spec-3 app never receives an event type it does not know):

- `XrEventDataLiftTaskStateChangedDXR` — a task's effective state changed, or its progress
  moved by ≥ 5 % (at most 2 Hz). Ordered per task, deduplicated.
- `XrEventDataLiftProvidersChangedDXR` — the provider set behind the tasks changed (an install,
  an uninstall, a user preference). Re-enumerate; live streams do not move.

**No provider enumeration.** There is no app-visible provider list, id, model name or vendor:
provider identity is diagnostics (`displayxr-cli conv`, the status snapshot, §12). A browser
must not expose provider identity to web content; it exposes at most a coarse
waiting / ready / unavailable state.

### 2.3 The spec-3 view (`xrGetLiftPropertiesDXR`)

A spec-3 caller is served by a fixed mapping of its four modes onto tasks. Backing tasks:
DEPTH → `DEPTH_VIDEO` (else `DEPTH_MONO`); SBS and NVIEW → `VIEWS_MONO`; GAUSSIANS →
`SPLATS_MONO`.

| Field | Value |
|---|---|
| `state` | **READY** when any advertised mode's backing task is READY ("any", not "all": a splat model downloading must not hold back live video). **ACTIVATING** otherwise while discovery has not settled (some registered provider is neither described nor terminal) **or** any backing task is in a waiting state — even before any mode bit is set. **UNAVAILABLE** otherwise |
| `supportedModes` | every mode whose backing task is not terminal, waiting ones included; NVIEW only when `VIEWS_MONO`'s `maxViews > 2` |
| `maxStreams`, `maxViews`, `backend`, `typicalLatency` | from `VIEWS_MONO`, else the depth task. `maxStreams` is that provider's per-task limit (§14.2 shift 1) |
| `depthSemantics` | METRIC iff the depth task's preferred kind is metric |
| `XrLiftDepthPropertiesDXR` | from `VIEWS_MONO`'s auxiliary depth (§4.3) |

"Properties queried on a cold boot before the first describe ⇒ ACTIVATING" is a golden test:
shipped browsers never re-poll UNAVAILABLE, so the runtime never answers it for a state that
will become READY.

### 2.4 The consumer contract (replaces ADR-042's "open default")

**A non-READY task is a reason to wait, never a reason to switch paths.**

- While waiting, a consumer **creates its stream or job anyway** (frames wait latest-wins; spec-5
  jobs wait in `WAITING_FOR_PROVIDER`), polls at ≤ 2 Hz or takes the state event, and **shows the
  2D source**.
- It **never caches a non-READY answer** beyond one poll interval. (The failure this closes was
  seen live: a consumer queried readiness once before the module was ready, cached the answer,
  and converted in the page at visibly lower quality for the whole session, with no stream in
  the service log.)
- It **never runs an in-app converter as a fallback**. The open models are a provider; a
  machine without a vendor provider is served by them through the same API.
- The only reasons to stay 2D are a runtime that does not expose the extension (decided with
  `xrEnumerateInstanceExtensionProperties`), a task with no candidate at all (enumerated
  `ABSENT`, i.e. not listed; spec 3: `UNAVAILABLE` with `supportedModes = 0`), and a create
  refused per §3.2.

A consumer that must also run on older runtimes may keep a time-bounded in-app path for a
runtime of spec 3 or older that reports no provider at all, and removes it once its minimum
runtime is spec 4 (`consumer_floors`).

## 3. Streams, modes and result layouts

The spec-3 modes and their layouts (a spec-4 task stream returns the same layouts; §5.5 adds
the metadata that says how to read them):

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

### 3.1 How a spec-3 mode is served

| Spec-3 request | Served by | Notes |
|---|---|---|
| DEPTH + VIDEO | `DEPTH_VIDEO`, mono input | without `XrLiftFrameControlDXR` the runtime's own cut detector drives the temporal resets |
| DEPTH + PHOTO | `DEPTH_MONO` | |
| SBS | `VIEWS_MONO`, 2 views | VIDEO keeps temporal state, PHOTO drops it |
| NVIEW + `XrLiftOptionsDXR::viewCount` | `VIEWS_MONO`, N views | N above the task's `maxViews` is `XR_ERROR_VALIDATION_FAILURE` (spec 3 left it unenforced) |
| GAUSSIANS (PHOTO) | `SPLATS_MONO` on the job worker, behind a latest-wins photo facade | spec-3 blob formats only (PLY or SOG); advertised only when a provider serves `SPLATS_MONO` |
| `XrLiftDepthRequestDXR` on SBS / NVIEW | `VIEWS_MONO` + auxiliary depth, advisory | as spec 3 |
| `focalPx > 0` | intrinsics source GIVEN | |

A stream created for a mode whose task was still warming stays valid if the task later fails
(its frames count in `framesFailed`; the next properties query drops the mode bit) and resumes
if the task recovers.

### 3.2 Task streams (spec 4)

Chain `XrLiftTaskStreamCreateInfoDXR` on `XrLiftStreamCreateInfoDXR`:

| Field | Meaning |
|---|---|
| `task` | a stream-lane task (§2.2) |
| `inputKind` | one of the task's `inputKinds` (`MONO` in spec 4; stereo pairs and image + depth arrive with spec 5) |
| `qualityHint` | `DEFAULT`, `LATENCY`, `BALANCED`, `QUALITY` — the only lever an app has on which variant runs |
| `viewCount` | `VIEWS_*`: 2..`maxViews`, or **0 = match the weave** (on a lifted weave rect, the active mode's view count, and the service resizes the stream on a mode switch without a new handle; on a plain stream, 2) |
| `requiredOutputs` | output bits to refuse rather than degrade (e.g. `METRIC`, `AUX_DEPTH`) |
| `exportKind` | how results leave the runtime (§5.4); 0 = the platform default, D3D11_NT on Windows, i.e. exactly spec 3 |

`XrLiftStreamCreateInfoDXR::mode` is then 0 or the legacy mode that matches the task (DEPTH for
`DEPTH_*`, SBS / NVIEW for `VIEWS_MONO`, GAUSSIANS for `SPLATS_MONO`); anything else is
`XR_ERROR_VALIDATION_FAILURE`. Setting the matching legacy mode makes the same create work on a
spec-3 runtime, which ignores the chained struct. `contentHint` and `inputScale` keep their
spec-3 meaning. `XrLiftDepthRequestDXR` may sit beside the task struct on a `VIEWS_MONO`
stream; it is equivalent to `requiredOutputs |= AUX_DEPTH` but advisory.

**Create rules**, after selection has fallen through every terminal candidate:

| Result | When |
|---|---|
| `XR_SUCCESS` | the selected provider is READY, or in any waiting state (frames wait latest-wins until READY) |
| `XR_ERROR_FEATURE_UNSUPPORTED` | the task has no non-terminal candidate; or a `requiredOutputs` bit is missing from a provider that is READY (before READY it is checked against the provider's **declared** outputs, which may be provisional, so a provider is never refused while it warms) |
| `XR_ERROR_VALIDATION_FAILURE` | `viewCount` > `maxViews`; an input or export kind no candidate offers; a reserved task; a mode / task mismatch |
| `XR_ERROR_LIMIT_REACHED` | the selected provider's `maxStreams` for the task |

**One rebinding rule.** The provider is chosen when the stream is created. A live stream never
moves to another provider, except on a terminal failure of its provider or a drain of the
provider's host binary (update, uninstall); the move is signalled by `discontinuity` on the next
result (§5.5) plus the task-state event. A preference or install change applies to the next
create only.

## 4. Submitting frames

In §4–§8, "the module" means the provider the runtime selected for the stream's task.

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
| `convergence` | RELATIVE depth placed at the display plane, in [0, 1] over the frame's depth range (0 = nearest content on the glass, 1 = farthest, 0.5 = mid). Negative = AUTO — on SBS / NVIEW streams the service then picks it (§4.4). >1 is clamped. The runtime hands the provider this plane in the provider's own decoded depth units (the last result's range maps [0, 1] onto them), so the two never disagree on normalisation. |
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
| `xrSubmitLiftFrameDXR` on a connection with no session window (e.g. `displayxr-cli lift`, an in-process link) | the panel; for a spec-4 submit, a rect of the submitted `extent`'s aspect fitted inside the panel, centred |

The windowless default is "aspect-fit, centred": the rect is geometry only, the module still synthesizes at the submitted `extent`, and an app-declared placement (a canvas or rect on the stream) is a spec-6 item.

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

**What the module is told.** A conversion provider receives the processed viewpoints and the
same geometry in `xrt_conv_params` (`viewpoints_xyz`, `viewpoint_frame`, `rect_width_m` /
`rect_height_m`, `baseline_m`, `axis_mode`, `max_offset_m`, `nominal_z_m`). Through the legacy
`dp-lift` adapter, the DP receives the processed viewpoints plus, in appended
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

**Provider side.** A conversion provider fills the requested depth target in the same
`stream_convert` that produces the views, and describes it in `xrt_conv_depth_info` (§5.5 is
its app-facing twin). **Legacy `dp-lift` side.** The runtime passes the request in `xrt_dp_lift_stream_info::aux_outputs`
(`XRT_DP_LIFT_AUX_DEPTH`, + `aux_depth_format`), and right after each successful `lift_convert`
of such a stream calls the appended D3D11 slot **`lift_get_depth`** (`XRT_DP_D3D11_HAS_LIFT_DEPTH`),
which fills `struct xrt_dp_lift_depth` (`XRT_DP_LIFT_HAS_AUX_DEPTH`): the DP-owned R32F / R16F
texture (valid until the next convert), units, encoding + scale / offset, source size, focal /
principal point, near / far, `convergence_depth`, `same_inference`, the module's frame id. The
runtime copies it into the result's ring slot and then the per-stream depth export texture,
and derives `depthToDisplay` itself. Capability: `xrt_dp_lift_caps::aux_outputs` /
`aux_depth_semantics`. An older plug-in (no slot) simply returns no depth.

### 4.4 AUTO convergence is runtime policy (epic #1858)

When an app leaves `convergence` AUTO on an SBS / NVIEW stream, the **service picks it** — no
API change; an explicit value from the app always wins. The conversion module only maps the
value into its units (it already honours a caller-set convergence), and the module's own
auto-convergence no longer runs for that stream.

- **Input:** the depth map of the conversion itself (the auxiliary depth of §4.3). The service
  requests it from the module for this purpose even when the app did not (the app-facing depth
  export still follows only what the app asked for). Weave-rect snapshots are letterbox-cropped
  before conversion (§6), so the depth holds no bars.
- **Sampling:** at most one depth map every 125 ms (~8 Hz) is copied to a staging texture on the
  lift device and mapped without waiting before a later conversion; never per frame.
- **Estimator** (`u_lift_autoconv.h`): inverse depth, box-downsampled to ≤ 128 cells wide and
  normalised over the frame's range (h = 0 farthest, 1 nearest); a soft centre prior (Gaussian,
  σ = 0.35 of the width / height) with a bottom damp (weight 0.5 at the bottom row); a weighted
  64-bin histogram smoothed with σ = 1.5 bins; the peak (range ends included) with the highest
  weighted mass × (1 + 1.0 · (h − weighted median)). A small near detail is little weighted
  mass and loses to the subject's coherent cluster; between comparable clusters the nearer one
  wins. A mode holding < 6 % of the weighted mass (±2 bins), or < 64 valid cells, is not
  confident: the value is held. Delivered convergence = 1 − h(mode).
- **Temporal policy (per stream):** median of the last three measurements, a 0.03 dead band,
  and a critically damped follower with τ = 0.6 s (no overshoot from rest; ~2.3 s to 90 % of a
  step). A measurement ≥ 0.25 from the target, or a scene-signature (16-bin depth histogram) L1
  change > 0.6, is a cut candidate; when the next measurement agrees (within 0.1) the follower
  switches to τ = 0.05 s for up to 0.4 s — ~0.2 s to 90 %, a few converted frames, never a
  one-frame snap. Until the first confident measurement (and when the module returns no depth)
  AUTO goes through unchanged.
- **Switch:** `DXR_LIFT_AUTO_CONVERGENCE=module` (service environment, read once at lift-module
  create; also `off` / `0`) restores the pre-#1858 behaviour — AUTO reaches the module — for
  A/B. The choice is logged once (WARN); each stream logs its first lock (WARN) and its
  convergence / target / subject / confidence / cut count every 5 s (INFO, in the per-stream
  stats).

### 4.5 Platform-neutral images and explicit sync (spec 4)

Chain `XrLiftFrameImagesDXR` (1..3 `XrLiftImageDXR`, at most one per role) on
`XrLiftFrameSubmitInfoDXR`. It **replaces** the base struct's `inputTexture` / `inputIsDxgi` /
`extent`, and `inputTexture` may then be NULL. `XrLiftOptionsDXR` stays on the same chain.

| `XrLiftImageDXR` field | Meaning |
|---|---|
| `role` | `COLOR` (the mono input, or a whole SBS pair); `COLOR_RIGHT` and `DEPTH` arrive with spec 5 |
| `kind` | `D3D11_NT` (= spec 3 `inputTexture`, keyed mutex key 0), `D3D11_DXGI` (= `inputIsDxgi`), `D3D12_NT`, `VK_OPAQUE_WIN32`, `OPAQUE_FD`, `DMABUF` (+ `XrLiftImageDmabufDescDXR` on `next`), `IOSURFACE`, `AHARDWAREBUFFER`, `CPU`. Values 1–6 equal `XrWeaveHandleKindDXR`'s. Must be one of the task's `imageKinds` for this session |
| `handle` / `cpuBytes` + `cpuRowPitch` | the image; handles are **borrowed** (the runtime duplicates what it keeps); CPU bytes are copied before the call returns |
| `format`, `colorSpace` | `XrLiftPixelFormatDXR` (RGBA8 / BGRA8 UNORM or SRGB, NV12, …), `XrLiftColorSpaceDXR` (sRGB, linear, BT.709 limited / full) |
| `region` | the sub-rect to convert (spec 3: `{0, 0, extent}`) |
| `syncKind`, `syncHandle`, `syncValue` | how the runtime knows the caller finished writing |

| Sync kind | Meaning |
|---|---|
| `NONE` | the kind's default: `KEYED_MUTEX` for `D3D11_*`, `CPU_COMPLETE` otherwise |
| `KEYED_MUTEX` | key 0 = caller done writing (spec 3) |
| `D3D_FENCE` | a shared `ID3D11Fence` / `ID3D12Fence` NT handle + value. **This is the D3D12 submit-side fence** ADR-049 left open: a D3D12 caller no longer CPU-waits before submitting |
| `SYNC_FILE` | a sync_file fd (Linux, Android) |
| `VK_TIMELINE` | an exported timeline semaphore (fd or NT handle) + value |
| `SHARED_EVENT` | a macOS shared event + value |
| `CPU_COMPLETE` | writes completed before the call: the spec-3 D3D12 rule of §5.3 (do not rewrite the image until a later frame; double-buffer) |

**What `imageKinds` lists today.** On Windows (IPC sessions and in-process D3D11 / D3D12
sessions): `D3D11_NT`, `D3D11_DXGI`, `D3D12_NT` and `CPU`, with any `region` inside the image.
Everywhere else a session can reach the service from — in-process GL / Vulkan on Windows, and
in-process Vulkan / GL / Metal plus IPC sessions on desktop Linux and macOS — `CPU` only, in
and out (§2.1); a `PLATFORM_DEFAULT` input or export on such a session means `CPU`. The CPU
bytes cross to the service in a shared section the runtime makes (a file mapping on Windows,
an unlinked shared-memory fd on Linux, an IOSurface used as a byte buffer on macOS); the app
only ever sees `cpuBytes` going in and a mapped `outputTexture` pointer coming out.
`D3D12_NT` takes `D3D_FENCE` (the service waits for `syncValue` on the GPU before its
snapshot) or `CPU_COMPLETE`; `D3D11_*` take their keyed mutex or `D3D_FENCE`; `CPU` takes
`NONE` / `CPU_COMPLETE`, RGBA8 or BGRA8, `cpuRowPitch ≥ (region x + width) · 4`, and the
region's bytes are copied before the call returns. `VK_OPAQUE_WIN32`, `OPAQUE_FD`, `DMABUF`,
`IOSURFACE` and `AHARDWAREBUFFER` appear in `imageKinds` only once their transports land
(ADR-053 P5) — until then they are never listed, and submitting one is
`XR_ERROR_VALIDATION_FAILURE`.

As in spec 3 a GPU image is **snapshotted** before the call returns (one copy on the runtime's
GPU, honouring the sync kind), so the caller may rewrite it afterwards — except under
`CPU_COMPLETE` on a kind without a keyed mutex.

### 4.6 Discontinuities (spec 4)

Chain `XrLiftFrameControlDXR` with `XR_LIFT_FRAME_CONTROL_DISCONTINUITY_BIT_DXR` on
`XrLiftFrameSubmitInfoDXR` (or on `XrWeaveRectLiftDXR`, §6.1) when the source jumped — a seek,
a cut, a source switch. The provider drops its temporal state before converting this frame,
and the result reports `discontinuity`. Per frame, not sticky. Without it, a `DEPTH_VIDEO` or
VIDEO-hinted stream is reset by the runtime's own cut detector.

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
  failure as "not reachable from this session" and stay 2D. Spec 4 reports this up front:
  `exportKinds` lists only what the session can open (§5.4).
- **Input:** create the input with `D3D12_HEAP_FLAG_SHARED` and `ID3D12Device::CreateSharedHandle`
  and pass that NT handle (`inputIsDxgi = XR_FALSE`). D3D12 has no keyed mutex, so the service
  snapshots the texture at submit **without a GPU wait**: complete the writes before
  `xrSubmitLiftFrameDXR` (signal + CPU-wait a fence) and do not rewrite the texture until a later
  frame — double-buffer the input. Spec 4 adds the submit-side fence: an `XrLiftImageDXR` of
  kind `D3D12_NT` with sync kind `D3D_FENCE` (§4.5).

### 5.4 Result export (spec 4)

`exportKinds` lists `D3D11_NT` and `CPU` on Windows D3D11 / D3D12 and IPC sessions, `CPU`
alone everywhere else (§2.1; Vulkan / dma-buf / IOSurface / AHB exports with P5). A **CPU** export: `outputTexture` is a pointer to the result's bytes, tightly packed
(`extent.width ·` bytes per pixel per row), runtime-owned and valid until the next acquire on the
stream; `fence` is NULL; `XrLiftResultExportDXR` says `kind = CPU`, the format, and
`syncKind = CPU_COMPLETE` (the bytes are complete when the acquire returns). An auxiliary depth
map keeps its D3D11 NT export (on a CPU-only session there is no auxiliary depth export yet).

A stream created with an `exportKind` other than D3D11_NT returns its results through the same
`outputTexture` / `fence` fields, described by `XrLiftResultExportDXR` chained on
`XrLiftResultDXR`: `kind` (also the auxiliary depth texture's), `format`, `colorSpace` and the
fence's `syncKind` (value = `fenceValue`). `XrLiftResultDXR::format` is then 0 and these fields
are authoritative. Handles still arrive under the §5.1 rule: on the first acquire and on every
reallocation. On a D3D11_NT stream the struct is left untouched and §5.1–5.3 hold unchanged.

**Adapters.** Results are exported on the service's ingest adapter (the one an IPC D3D session
is told to use). `exportKinds` lists only kinds this session can open, so a session on another
adapter, or on an API without a transport yet, sees 0 instead of discovering the mismatch from a
failed `OpenSharedHandle` (the adapter follow-up of ADR-049). A provider that runs on another
adapter is served through a CPU copy inside the runtime and never changes what the caller sees.

### 5.5 What a result means: depth and views (spec 4)

**`XrLiftDepthInfoDXR`** describes depth wherever it appears: chained on `XrLiftResultDXR` of a
`DEPTH_*` stream, on `XrLiftDepthResultDXR` (auxiliary depth, §4.3), and (spec 5) on a depth
input image and a job result. Decode a sample `s` as `v = rawScale · s + rawShift`, then read
`v` by `kind`:

| `XrLiftDepthKindDXR` | Value | Meaning | Spec-3 aux reading |
|---|---|---|---|
| `METRIC_DEPTH` | 1 | metres | METRIC + LINEAR |
| `METRIC_INVERSE` | 2 | 1 / metres | METRIC + INVERSE |
| `AFFINE_INVERSE_DEPTH` | 3 | 1 / depth up to an unknown scale and shift; larger = nearer | RELATIVE + INVERSE |
| `AFFINE_DISPARITY` | 4 | disparity up to an unknown scale and shift; larger = nearer | RELATIVE + INVERSE (§14.2 shift 3) |
| `AFFINE_DEPTH` | 5 | depth up to an unknown scale and shift; larger = farther | RELATIVE + LINEAR |
| `PIXEL_DISPARITY` | 6 | exact disparity in pixels of a rectified pair (`DISPARITY_STEREO`, spec 5) | never reaches a spec-3 result |

- `measure`: PLANAR (to the camera plane) or RAY (along the pixel ray).
- `nearZ` / `farZ`: **depth readings** `d` (see the decoding table below), so `nearZ < farZ` for
  every kind; `farZ < nearZ` = infinite far plane; both 0 = unknown.
- `rangeValid`, `rangeMin` / `rangeMax`, `rangeP05` / `rangeP95`: **this frame's** extent in
  decoded units, before any normalisation. This is what lets the runtime's depth-budget and
  convergence policy work on affine depth, whose scale is unknown by definition.
- `intrinsicsSource` (`UNKNOWN`, `GIVEN` from the caller, `ESTIMATED` by the model, `ASSUMED`
  provider default — not evidence), `focalLengthPx`, `principalPointPx`, `convention` (OpenCV
  right-down-forward or right-up-back).
- `temporallyConsistent`, `discontinuity` (temporal state was reset before this result, or the
  stream moved provider, §3.2), `baselineMeters` (pixel disparity only; given, never defaulted).

**Units are never invented.** With an affine kind, near / far and the intrinsics are as reported
or zero; a provider that does not know a field leaves it zero.

**Decoding, normatively.** The range fields and `convergenceValue` are in **decoded units**
`v = rawScale · s + rawShift`, never inverted. `nearZ` / `farZ` are **depth readings** `d` — the
middle column below, the same quantity as spec 3's `convergenceDepth` — so their order is the
same for every kind and `farZ < nearZ` keeps meaning an infinite far plane (for an inverse kind
the nearest content has the largest `v`, `rangeMax`, and the smallest `d`, `nearZ`). What a caller
computes from `v` depends on `kind`:

| `kind` | depth reading `d` | inverse-depth coordinate `q` | a distance? |
|---|---|---|---|
| `METRIC_DEPTH` | `v` (metres) | `1 / v` | yes, metres |
| `METRIC_INVERSE` | `1 / v` (metres) | `v` | yes, metres |
| `AFFINE_INVERSE_DEPTH` | `1 / v` | `v` | no: an order only (unknown scale and shift) |
| `AFFINE_DISPARITY` | `1 / v` | `v` | no |
| `AFFINE_DEPTH` | `v` | `1 / v` | no |

- `convergenceValid` / `convergenceValue`: the decoded `v` the conversion put at zero disparity
  (on the screen plane). Content whose `q` is larger than the plane's `q` is in front of it.
- The display relief (`XrLiftViewsInfoDXR::reliefValid`, `reliefScale`, `reliefOffset`) is
  affine in `q`: a texel is presented at `z = reliefScale · q + reliefOffset` metres in front of
  the screen plane (toward the viewer; behind = negative). For `METRIC_DEPTH` that is
  `reliefScale / d + reliefOffset`.
- Spec 3's `XrLiftDepthResultDXR::convergenceDepth` is the depth reading `d` of
  `convergenceValue` (the ray fraction `d / dc` of §4.3), and its `encoding` is INVERSE exactly for
  the kinds whose `d = 1 / v`.

The plug-in ABI uses the same convention (`xrt_conv_depth_info::convergence_value` decoded,
`relief_*` affine in `q`), so the runtime copies these fields and never rescales them.
`tests_ipc_conv_v4` checks every kind of the table through both the spec-3 and the spec-4 path.

**`XrLiftViewsInfoDXR`**, chained on `XrLiftResultDXR` of a `VIEWS_*` stream, says how the views
were made, in terms that round-trip to stereo-video metadata: `layout` (`ROW` = spec 3, view 0
leftmost; `ARRAY_LAYERS`; `ATLAS` reserved), `viewCount`, `columns` × `rows`, `viewExtent`,
per-view `eyeOffsetMeters` (8 entries), `baselineMeters`, `horizontalFovRadians`,
`disparityAdjust` and the rendered disparity range as fractions of a view's width (positive =
behind the screen), `cameraModel` (off-axis onto the lifted rect, or parallel + shift),
`convergenceSource` (app, runtime AUTO, or the provider's own — legacy adapter only),
`convergenceApplied`, the provider's `suggestedConvergence` / `suggestedStrength` (diagnostics:
policy ignores them unless the provider is validated against the runtime's stereo ground-truth
corpus), and the relief mapping (`z = reliefScale / d + reliefOffset`) the cursor uses. The view
ceiling is 8 at every level (`XR_LIFT_MAX_VIEWS_DXR`).

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

### 6.1 Weave rects with spec 4

Byte-identical: `XrWeaveRectLiftDXR`, `XrWeaveSubmitLiftRectsDXR`, the ≤ 8 rect limit, the IPC
ordering, the snapshot cap and letterbox crop, "flat until the first result", and the cursor
chain. What changes:

1. The stream must be a `VIEWS_MONO` stream of the same connection (a spec-3 SBS / NVIEW stream
   is one). Any other task is refused per rect, non-fatally, and the rect is woven as drawn.
2. A provider without explicit-viewpoint support is served through the composed path: its
   depth plus the runtime's reference view synthesis (`COMPOSED` output bit).
3. `viewCount = 0` means "match the weave": on a 2 → 4 view mode switch the service resizes the
   stream without a new handle, and view *v* lands in tile *v*.
4. `XrLiftFrameControlDXR` may be chained on the rect to signal a seek (§4.6).
5. `XrLiftOptionsDXR::convergence = AUTO` is the runtime's auto-convergence (§4.4).

## 7. Scheduling, priority and stats

Each provider converts one frame at a time unless it declares otherwise (one GPU, often one
serialised inference queue), so concurrent streams on one provider share its throughput;
streams on different providers are scheduled independently. The lift thread runs **rounds**; each round converts,
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
cost nothing extra but invite churn. Scheduling is runtime policy; a provider never sees it.

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

- **The conversion core** (`src/xrt/conversion/`, ADR-053) owns the provider registry and its
  persisted describe cache, per-task selection (`conv_select.c`), the readiness state machine
  (`conv_readiness.c`), one stream dispatcher and one job worker per provider, the pipeline
  stages (snapshot and crop, viewpoint policy, the provider, the reference view synthesis for a
  depth-only provider, depth decode and per-frame range, policy observers such as
  auto-convergence, publish), and the stall watchdog. It is reached by the spec-3 IPC handlers
  (unchanged wire) and the spec-4 `conv_*` call family.
- **Providers run behind a host interface** (`conv_host_iface.h`): in the service
  (`conv_host_inproc.c`; the built-in `dp-lift` adapter always, any provider registered with
  `Hosting=in-service`) or in a supervised **conversion host**, `displayxr-conversion-host`, one
  process per provider binary (`conv_host_proxy.c`, `conv_supervisor.c`). A host crash or hang
  costs WARMING(`restarting`) and a re-opened stream, never a session; an installer update costs
  WARMING(`updating`) (discovery spec §3.8). The service never queues a GPU wait on a fence the
  host signals: it observes completion from the CPU first.
- **The D3D11 glue** (`d3d11_lift.cpp`, `d3d11_conv_host_lane.cpp`, `d3d11_conv_viewsyn.cpp`)
  owns a dedicated **lift device** on the service adapter (`ID3D11Multithread` protection on).
  **Per stream:** a latest-wins mailbox (two input slots) and a two-slot output ring, both
  keyed-mutex shared textures crossing between the service device and the lift device; the
  provider's output is copied into the ring before its next call. Consumers **pin** a ring slot
  for the length of one GPU-copy issue; the worker never overwrites the latest or a pinned slot.
  The state machine is platform-neutral and unit-tested (`u_lift_mailbox.h`,
  `tests/tests_lift_mailbox.cpp`), as is the round scheduler.
- **Locks:** the lift mutex is a leaf, never held across GPU or provider work. IPC calls take the
  service's `immediate_ctx_mutex` alone, for one blit / copy; the weave path takes the lift mutex
  under it. No provider call is ever made from a render, compositor or IPC thread or with a
  conversion lock held — see
  [service-architecture §3](../../architecture/service-architecture.md#3-threads-and-locks).
- **Logging:** one WARN per readiness transition and per stream create / destroy (with the
  selected provider and why); per-stream statistics at INFO, throttled; a satellite host's WARN
  lines are forwarded into the service log prefixed `[conv:<id>]`.
- **Kill switches** (the **service's** environment): `DXR_LIFT=0` → UNAVAILABLE;
  `DXR_CONV_PROVIDERS=0` → no plug-in providers, only the built-in adapter;
  `DXR_CONV_EXCLUSIVE=<id>` → that provider only.
- **Provider settings** are read through the host's `get_setting`
  (`DXR_CONV_<ID>_<KEY>` in the hosting process's environment, else the provider's registry
  `Settings` or manifest `settings`), never from the app's environment — conversion runs in
  `displayxr-service.exe` or its host, not in the app.
- **Streams belong to the IPC connection**, not the session, and die with it; the calls need no
  compositor session, which is what lets `displayxr-cli lift` drive them headless — and what lets
  an in-process session use lift (ADR-049): it opens a sessionless `APP` connection named
  `<exe> (in-process lift)` (`ipc_client_lift_link.c`, background connect, never launches the
  service, retried at most every 5 s) and runs the same calls on it. Lines in the app's log:
  `[lift] in-process: no conversion service (...); in-process conversion unavailable, retrying
  quietly ...` (one WARN, then the retries log nothing until a connect succeeds),
  `[lift] in-process: lift-only service connection up (generation N)` (INFO), and
  `... connection (generation N) is gone ...` (WARN).

## 10. Error codes and connection loss

Mirrors XR_DXR_weave §4b.

| Result | When | Session still usable? |
|---|---|---|
| `XR_LIFT_NOT_READY_DXR` (success) | no result newer than the last acquired | yes |
| `XR_ERROR_VALIDATION_FAILURE` | a struct out of contract (mode not one bit, bad viewCount or `viewCount` > `maxViews`, EXPLICIT without viewpoints, `XrLiftDepthRequestDXR` on a DEPTH / GAUSSIANS stream, wrong acquire for the mode, bad `rectIndex`; spec 4: a mode / task mismatch, a reserved task, an image or export kind no candidate offers, more than one image per role) | yes — caller bug, nothing sent |
| `XR_ERROR_FEATURE_UNSUPPORTED` | in-process session other than D3D11/D3D12 on Windows (spec-3 calls); a task or mode with no non-terminal candidate; a `requiredOutputs` bit missing from a READY provider; in-process: a stream of a lift connection that was lost (destroy it, create a new one) | yes — permanent for that request |
| `XR_ERROR_LIMIT_REACHED` | the selected provider's `maxStreams` for the task | yes |
| `XR_ERROR_SIZE_INSUFFICIENT` | blob capacity too small (latch kept); `xrEnumerateLiftTasksDXR` capacity too small | yes |
| `XR_ERROR_RUNTIME_FAILURE` | the service refused this call over a healthy pipe — most commonly the 4 ms input keyed-mutex miss, or a stream the service no longer knows | **yes — retry next frame** |
| `XR_ERROR_INSTANCE_LOST` | the IPC connection is gone (IPC sessions only — an in-process session's lift connection dying is `XR_ERROR_RUNTIME_FAILURE` on that call; the session renders on, and the next `xrGetLiftPropertiesDXR` reconnects) | no — recover with a new instance (weave §4b) |
| `XR_ERROR_SESSION_LOST` | any call after that | no |

A provider crash, hang or update is **not** an error on any call: the task reports WARMING, the
stream keeps its handle and its last result, and conversions resume when the provider is back.

## 11. Provider contract, in one table

Providers are their own plug-in type (ADR-053): header `xrt/xrt_conversion_plugin.h`, entry
point `xrtConversionPluginNegotiate`, discovery under `ConversionProviders` — normative in the
[conversion-provider discovery spec](../runtime/conversion-provider-discovery.md); how to write
one: [`vendor-plugin-onboarding.md` §9](../../guides/vendor-plugin-onboarding.md#9-conversion-providers--the-third-plug-in-type).

| Runtime owns | Provider owns |
|---|---|
| discovery, selection per task, readiness, events, every thread and queue | one synchronous conversion per call, on runtime threads |
| latest-wins mailboxes, drops, frame ids, discontinuity flags | per-stream temporal state, reset when told |
| allocating every input and output image, export textures, fences | importing images (never exporting); an output valid until its next call |
| priority scheduling across streams, the job lane, progress, cancel | polling the cancel callback; reporting progress and honest state |
| tracked eyes → explicit viewpoints, rebased to the lifted rect, with the viewpoint policy applied (ADR-048) — or derived from the app's rig with the `xrLocateViews` math (spec 3) | rendering exactly the viewpoints it is given |
| convergence (app value, or runtime AUTO, handed over in the provider's own decoded units), the depth budget, the cursor's relief mapping | honouring a given convergence exactly; reporting units honestly (depth kind, per-frame range, intrinsics source); suggestions only |
| copying depth out, exporting it, the display transform (`depthToDisplay`) | returning the depth of the same inference + its metadata |
| weaving SBS / N-view results (ADR-007) | never weaving |
| hosting (service or satellite), crash containment, update without lost sessions | following the hosting-agnostic rules (no runtime device, no windows on non-pumping threads, settings only through the host) |

**The legacy path.** A display plug-in that implements the frozen D3D11 lift slots (ADR-042 §5,
`xrt_dp_lift.h`, `XRT_DP_D3D11_HAS_LIFT`) is served by the built-in `dp-lift` adapter at
ProbeOrder 150, always in the service. Nothing is ever appended to those slots; they are removed
at the next display-plug-in ABI major. A vendor's separate provider registered with
`Supersedes = dp-lift` hides the adapter. Slot reference:
[`xrt_plugin_iface.md` § lift](../../reference/xrt_plugin_iface.md#turning-2d-into-3d-the-lift-slots-adr-042-xr_dxr_lift).

**Hardware-free.** The in-tree `sim-conversion` provider (`src/xrt/drivers/sim_conversion/`,
ProbeOrder 250, FALLBACK, enabled by its `ENABLE` setting) serves every task on the CPU with
scriptable readiness, latency and failures, so the whole path — including the satellite host —
runs in CI without hardware. `SIM_DISPLAY_FAKE_LIFT=1` remains an alias for one release.

## 12. Probe and diagnostics

```
displayxr-cli conv list   [--json]                    # providers x tasks, selection order, why
displayxr-cli conv status [--json] [--watch]          # readiness, streams, jobs
displayxr-cli conv use <task> <id>[/<cap>] [--machine] | conv reset <task>|all
displayxr-cli conv enable|disable <id> [--machine]
displayxr-cli lift caps [--json] [--wait S] [--timeout S]
displayxr-cli lift probe <image|frames_dir> [--mode depth|sbs|nview|gaussians] [--n N]
                         [--views N] [--strength F] [--convergence F] [--focal PX]
                         [--priority paused|low|normal|high] [--pipelined] [--fps F] [--out DIR]
```

`conv list` / `conv status` read the running service's status snapshot over DIAG; with no
service they fall back to a headless pass that negotiates, probes and describes every registered
provider but **never warms one**, and say so (`source: headless`). Provider identity, backend,
model and licence class are visible here and in the dashboard — never through the app API.

`lift probe` connects over IPC as a DIAG client (non-elevated prompt on Windows), waits through
ACTIVATING, creates a stream (`--mode depth` asks for video content, served by `DEPTH_VIDEO`),
submits the image(s) through a shared keyed-mutex texture exactly as a browser does, reads each
result back through the export texture + fence and writes `lift_out_<i>.png` (depth normalised
to 8-bit grey) or `lift_out_<i>.ply`. It prints per frame the submit → acquire latency seen by
the caller and the service's own latency, and at the end the stream stats and a latency summary.
`--pipelined` submits at `--fps` without waiting, to measure throughput and drops. `lift caps`
polls through ACTIVATING for `--wait` seconds (default 8) and is hard-bounded by `--timeout`
(default `max(10, wait + 2)` s): a service that never answers prints `TIMEOUT` and exits 4
instead of blocking (#1812). The service answers from its last-known state and never waits on a
provider.

`displayxr-cli selftest` includes a `conversion_providers` check (ABI, well-formed capability
descriptors, in-range states; a malformed descriptor exits `CLI_SELFTEST_BAD_CONVERSION` = 13;
no provider passes) and keeps `lift_caps` (exit 11 on malformed caps), which now reports the
static `dp-lift` row and, when a service is listening, its live caps — it no longer instantiates
a model headless.

## 13. Consumers

| Consumer | Path | Uses |
|---|---|---|
| DisplayXR Browser (Chromium fork) | GPU process → service | `xrGetLiftPropertiesDXR` / task readiness under the §2.4 contract (wait for READY; no session-long fallback); `XrWeaveSubmitLiftRectsDXR` for inline 2D video and images lifted in place; `xrSubmitLiftFrameDXR` + acquire where the page wants the depth or views itself. Pages see only a coarse waiting / ready / unavailable state |
| DisplayXR web SDK (`@displayxr/inline3d`) | via the browser | the same contract; its in-page model store and converter are removed once its minimum runtime serves the open models as a provider |
| 3D calling (up to 4 mono tiles) | app → service | one views stream per tile; `xrSetLiftStreamPriorityDXR` HIGH for the active speaker |
| Engine apps (Unity display provider, D3D12 in-process) | in-process session → lift-only connection → service (ADR-049) | streams + texture acquire (§5.3); spec 4 adds the D3D12 fence sync kind (§4.5); no weave rects (the app composes the views itself) |
| `displayxr-cli conv` / `lift` | DIAG IPC | provider list, readiness, caps and the probe (§12) |

When changing the header, byte-sync every consumer's vendored copy and rebuild it — coupled-PR
order: runtime → extensions auto-sync → consumers. A consumer adopting spec-4 behaviour raises
its minimum runtime in the same change (`consumer_floors`).

## 14. Version history

| Version | Change |
|---|---|
| 1 | Initial: properties + states, streams (DEPTH / SBS / NVIEW / GAUSSIANS), non-blocking latest-wins submit, texture acquire (weave-style handles + fence), blob acquire (two-call latch), weave-rect lift chain, per-stream priority scheduling + stats, `focalPx`. |
| 2 | Viewpoint policy (ADR-048, §4.1): viewpoints relative to the lifted rect / submitting window centre (was: the panel centre), `XrLiftViewControlDXR` (ipd / parallax factors, axis mode, offset clamp, recentering; defaults X + EASE_BACK 1 s / 2 s), `XrLiftResultViewpointsDXR` echo. |
| 3 | App camera + metric depth (ADR-048 Addendum A, §4.2–4.3): `XrDisplayRigDXR` / `XrCameraRigDXR` on `XrLiftOptionsDXR` / `XrWeaveRectLiftDXR` drive the TRACKED viewpoints with the `xrLocateViews` rig math; EXPLICIT viewpoints accepted on lifted weave rects; auxiliary depth for SBS / NVIEW streams — `XrLiftDepthRequestDXR`, `XrLiftDepthResultDXR` (texture + shared fence, units, encoding, intrinsics, convergence depth, viewpoint, `depthToDisplay`), `XrLiftDepthPropertiesDXR`. A separate version from 2 because v2 can ship on its own (#1864): an app tests `SPEC_VERSION >= 3` for these. |
| 4 | Conversion providers (ADR-053, §2.2–2.4, §3.2, §4.5–4.6, §5.4–5.5, §6.1): the task vocabulary (`XrLiftTaskDXR`), input kinds and a quality hint; `xrEnumerateLiftTasksDXR` (the effective capability, readiness and licence class per task) and `xrRequestLiftTaskWarmDXR`; `XrLiftTaskStateDXR` + failure reason; `XrEventDataLiftTaskStateChangedDXR` / `XrEventDataLiftProvidersChangedDXR`, opt-in per session; `XrLiftTaskStreamCreateInfoDXR` (task, input kind, quality hint, `viewCount` 0 = match the weave, `requiredOutputs`, export kind); `XrLiftFrameImagesDXR` / `XrLiftImageDXR` with explicit sync, `XrLiftImageDmabufDescDXR`, `XrLiftFrameControlDXR` (discontinuity); `XrLiftResultExportDXR`, `XrLiftDepthInfoDXR`, `XrLiftViewsInfoDXR`. The consumer contract (§2.4) replaces ADR-042's "open default". Type values `1004999341–351`; enum values equal the plug-in ABI's. An app tests `SPEC_VERSION >= 4`. |

### 14.1 Spec 3 is frozen

Every spec-3 struct, enum value, entry point and wire message is unchanged in spec 4, and a
spec-3 caller is served by the fixed mapping of §2.3 / §3.1. `tests_lift_abi` pins every spec-3
`sizeof`, `offsetof` and enum value (x64 and arm64); `tests_oxr_lift_startup` pins the spec-3
startup sequence and is unchanged by spec 4. Spec 4 only appends.

### 14.2 What a spec-3 caller can observe

Moving lift onto conversion providers changes four things a spec-3 caller can see. Each is
pinned by a golden test (`tests/tests_lift_v3_shifts.cpp`, tag `[lift_v3_shift]`):

| # | Shift | Golden test |
|---|---|---|
| 1 | `maxStreams` is the **selected provider's per-task** limit, not one runtime-wide number; a depth stream and a views stream on different providers no longer share one budget | `v3 shift 1: maxStreams is the selected provider's per-task limit` |
| 2 | GAUSSIANS may return SOG with the splat camera block where the sim provider returns PLY; `format` says which, and both are spec-3 formats | `v3 shift 2: GAUSSIANS may return SOG with the camera block` |
| 3 | Auxiliary depth of kind `AFFINE_DISPARITY` is reported to spec 3 as RELATIVE + INVERSE, so `auxDepthSupported` stays true — and cursor depth keeps working — on boxes without a vendor provider | `v3 shift 3: AFFINE_DISPARITY aux depth reads RELATIVE + INVERSE` |
| 4 | The mode check at create follows **declared** capabilities: a mode whose task is still warming is accepted (its frames wait), where spec 3 only accepted modes of a READY module | `v3 shift 4: a mode backed by a warming task is accepted at create` |

### 14.3 Runtime behaviour without a spec-version change

- In-process D3D11 / D3D12 sessions on Windows use lift over a lift-only connection (ADR-049).
  An older runtime answers `xrGetLiftPropertiesDXR` on such a session with
  `XR_ERROR_FEATURE_UNSUPPORTED`.
- Lift runs on conversion providers through the conversion core (ADR-053 P1a, the same spec-3
  wire): readiness never latches UNAVAILABLE until a service restart, a cold boot reads
  ACTIVATING before the first describe, `maxViews` is enforced at submit, and a vendor provider
  runs in its own supervised host (P2), so a provider crash or update no longer touches any
  session.
- Non-D3D transports (Vulkan external memory, dma-buf, IOSurface, AHardwareBuffer) land without
  a version bump: availability is the per-task `imageKinds` / `exportKinds`.

## Probing on a Windows box — gotchas

- **Choosing the provider.** `displayxr-cli conv list` shows which provider serves each task and
  why; `displayxr-cli conv use <task> <id>` pins one per user, and `DXR_CONV_EXCLUSIVE=<id>` in
  the **service's** environment loads only that one (relaunch the service non-elevated from a
  `.bat` that sets it). `DXR_CONV_SIM_CONVERSION_ENABLE=1` with `scripts\register_dev_plugin.bat
  conv sim` exercises the whole path hardware-free.
- **Choosing the display processor** for the legacy `dp-lift` row: `XRT_PREFERRED_PLUGIN_ID` does
  *not* switch the active DP when an installed vendor plug-in also probes;
  `DXR_PLUGIN_EXCLUSIVE=<plugin-id>` in the service's environment does.
- **`displayxr-cli lift …` and `conv …` must run non-elevated** — an elevated prompt reports "not
  connected to the service" (same integrity-level rule as every other IPC client).
- **A provider's dependency too old.** A provider whose inference runtime is installed but older
  than its floor reports `FAILED` with reason `VERSION` (spec 3: UNAVAILABLE), with the reason in
  the service log and `conv status`; upgrade that runtime. A missing one reports `ABSENT`.
- **Probe timing.** `lift probe` stamps submit→acquire before any readback; use `--no-write` (or
  `--write-every N`) for throughput runs — encoding a 4K SBS PNG takes seconds and would
  otherwise cap the pipelined rate.

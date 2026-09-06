# XR_DXR_depth_budget

| Property | Value |
|----------|-------|
| Extension Name | `XR_DXR_depth_budget` |
| Spec Version | 2 |
| Type Values | `XR_TYPE_REAR_DEPTH_BUDGET_DXR` (1004999260) · `XR_TYPE_CONTENT_BOUNDS_DXR` (1004999261) · `XR_TYPE_EVENT_DATA_REAR_DEPTH_BUDGET_STATE_CHANGED_DXR` (1004999262) |
| Author | The DisplayXR Project |
| Platform | All. The budget only ever *opens* where a background source exists; elsewhere it reports the conservative state and the app behaves exactly as it does today. |

---

## 1. Overview

A transparent-mode app draws over the desktop. Content **behind** the display plane (the
zero-disparity plane, ZDP) carries positive disparity — "behind the screen" — while being drawn
**over** desktop pixels at zero disparity — "in front of them". The two cues contradict each
other, so transparent apps today clip their far plane at the ZDP and render nothing behind the
screen.

That contradiction is only readable when the background carries a **horizontal-disparity cue**:
horizontal luminance structure — vertical edges, text, icons, window borders. Over a flat
wallpaper, a vertical gradient or horizontal stripes there is no cue, and rear content is
perceptually fine.

`XR_DXR_depth_budget` is the channel that tells the app which of those two situations it is in.
The runtime measures the background, applies a hysteretic policy, and publishes a **rear depth
budget**: how far behind the ZDP this session may render right now.

> The runtime owns the **policy**, the display-processor plug-in owns **pixels**, the app owns
> **geometry**. Design record: [ADR-040](../../adr/ADR-040-rear-depth-budget.md).

**Units.** `farOffsetVH` is in **vH** — virtual display heights, the far-offset convention the
DisplayXR apps already use. `0` = clip at the ZDP; `>= 1000` = unrestricted. `farOffsetMeters`
carries the same number scaled by the rig's virtual display height, for apps that prefer metres.

The budget is **advisory**. An app that ignores it renders exactly as it does today. Nothing in
the compositor enforces it, and nothing in the runtime's frame path depends on the app honouring
it.

## 2. Enabling the Extension

Enabling is the application's opt-in **and** the runtime's gate: the runtime performs the
background fetch and the analysis only for sessions that enabled this extension **and** are
transparent **and** are standalone (not running under a workspace controller). An app that never
enables it costs nothing.

```c
const char *exts[] = {
    XR_DXR_DISPLAY_INFO_EXTENSION_NAME,
    XR_DXR_VIEW_RIG_EXTENSION_NAME,
    XR_DXR_DEPTH_BUDGET_EXTENSION_NAME,
};
XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
ci.enabledExtensionCount = 3;
ci.enabledExtensionNames = exts;
```

Query `xrEnumerateInstanceExtensionProperties` first and treat absence as normal — a runtime
predating this extension, or one whose display processor supplies no background source, is a
supported configuration. See §6.

## 3. API Reference

### 3.1 Extension name and constants

```c
#define XR_DXR_depth_budget 1
#define XR_DXR_depth_budget_SPEC_VERSION 2
#define XR_DXR_DEPTH_BUDGET_EXTENSION_NAME "XR_DXR_depth_budget"

#define XR_TYPE_REAR_DEPTH_BUDGET_DXR                          ((XrStructureType)1004999260)
#define XR_TYPE_CONTENT_BOUNDS_DXR                             ((XrStructureType)1004999261)
#define XR_TYPE_EVENT_DATA_REAR_DEPTH_BUDGET_STATE_CHANGED_DXR ((XrStructureType)1004999262)
```

### 3.2 XrRearDepthBudgetStateDXR

```c
typedef enum XrRearDepthBudgetStateDXR {
    XR_REAR_DEPTH_BUDGET_STATE_UNRESTRICTED_OPAQUE_DXR = 0,
    XR_REAR_DEPTH_BUDGET_STATE_UNRESTRICTED_WORKSPACE_DXR = 1,
    XR_REAR_DEPTH_BUDGET_STATE_OPEN_DXR = 2,
    XR_REAR_DEPTH_BUDGET_STATE_CLIPPED_BUSY_BACKGROUND_DXR = 3,
    XR_REAR_DEPTH_BUDGET_STATE_CLIPPED_NO_SOURCE_DXR = 4,
    XR_REAR_DEPTH_BUDGET_STATE_FORCED_DXR = 5,
    XR_REAR_DEPTH_BUDGET_STATE_MAX_ENUM_DXR = 0x7FFFFFFF
} XrRearDepthBudgetStateDXR;
```

### 3.3 XrRearDepthBudgetDXR — output, chained on `XrViewState`

```c
/* OUTPUT: the app chains this on XrViewState::next (beside XrViewDisplayRawDXR) in
   xrLocateViews. The runtime fills it on every locate. Zero-defaults if the runtime has
   nothing yet: farOffsetVH = 0 for transparent sessions, 1000 otherwise. */
typedef struct XrRearDepthBudgetDXR {
    XrStructureType             type;   /* XR_TYPE_REAR_DEPTH_BUDGET_DXR */
    void*                       next;
    float                       farOffsetVH;      /* >= 0; 0 = clip at ZDP; >= 1000 = unrestricted */
    float                       farOffsetMeters;  /* farOffsetVH * virtualDisplayHeight (0 if rig unknown) */
    XrRearDepthBudgetStateDXR   state;
    float                       backgroundCueEnergy; /* 0..1 diagnostic, 0 when no source */
} XrRearDepthBudgetDXR;
```

### 3.4 XrContentBoundsDXR — input, chained on `XrFrameEndInfo` (v2)

```c
/* INPUT: the app chains this on XrFrameEndInfo::next in xrEndFrame. Optional; when absent
   (or not chained for more than 1 s) the runtime measures the whole canvas, i.e. v1. */
typedef struct XrContentBoundsDXR {
    XrStructureType   type;      /* XR_TYPE_CONTENT_BOUNDS_DXR */
    const void*       next;
    XrRect2Df         bounds;    /* CANVAS-NORMALISED: offset/extent in [0,1], origin top-left
                                    (u right, v DOWN — the same convention as the display
                                    processor's background-preview canvas rect and
                                    XrViewDisplayRawDXR::canvasRectPx). The union over ALL views
                                    of the projected content AABB. An extent <= 0, or any
                                    non-finite component, means "unknown" = whole canvas. */
    float             marginNormalized; /* extra dilation the app wants, in canvas-normalised
                                    units, ON TOP of the runtime's own default (§4.5).
                                    0 = the runtime default alone. */
} XrContentBoundsDXR;
```

Input only — the runtime writes nothing back through it. It is a **hint**: no value of any field
can fail `xrEndFrame`. See §4.5.

### 3.5 XrEventDataRearDepthBudgetStateChangedDXR

```c
/* EVENT: emitted on every state change (not on ramp progress). */
typedef struct XrEventDataRearDepthBudgetStateChangedDXR {
    XrStructureType             type;   /* XR_TYPE_EVENT_DATA_REAR_DEPTH_BUDGET_STATE_CHANGED_DXR */
    const void*                 next;
    XrSession                   session;
    XrRearDepthBudgetStateDXR   previousState;
    XrRearDepthBudgetStateDXR   newState;
} XrEventDataRearDepthBudgetStateChangedDXR;
```

The event is a notification, not the value channel — it fires on transitions only, never on ramp
progress. The authoritative `farOffsetVH` for the frame you are about to render is always the one
that came back from that frame's `xrLocateViews`. An app can ignore the event entirely and lose
nothing but the chance to log or to reconfigure something expensive.

## 4. Semantics

### 4.1 States

| State | `farOffsetVH` | When |
|---|---|---|
| `UNRESTRICTED_OPAQUE` | 1000 | The session is not transparent. There is no desktop showing through, so there is no conflict to avoid. |
| `UNRESTRICTED_WORKSPACE` | 1000 | Transparent, but running under a workspace controller. The controller composites the scene; this is today's behaviour and v1 does not change it. |
| `OPEN` | ramps 0 → 1000 | Transparent, standalone, and the background has been measured neutral continuously for the open dwell. |
| `CLIPPED_BUSY_BACKGROUND` | ramps → 0 | The background carries a horizontal-disparity cue. |
| `CLIPPED_NO_SOURCE` | 0 | No background preview is available: the display processor does not implement the source slot, the source declined this frame, or flagged its preview invalid. Byte-for-byte today's behaviour. |
| `FORCED` | 0 or 1000 | An environment override is armed (§4.4). |

### 4.2 Dynamics

The runtime hands over an already-smoothed value. Defaults, with the env vars that override them
for tuning:

| Constant | Default | Meaning |
|---|---|---|
| `DXR_REAR_BUDGET_OPEN_DWELL_MS` | 400 | Neutral must hold **continuously** this long before the state opens. |
| `DXR_REAR_BUDGET_CLOSE_MS` | 100 | Any busy sample closes the state after this. |
| `DXR_REAR_BUDGET_RAMP_OPEN_MS` | 300 | Ease-out ramp toward 1000. |
| `DXR_REAR_BUDGET_RAMP_CLOSE_MS` | 150 | Ease-out ramp toward 0. |

Three properties follow, and applications depend on them:

- **The hysteresis is asymmetric on purpose.** Opening is slow, closing is fast: a visible
  occlusion conflict is worse than a missing rear volume.
- **`farOffsetVH` is ramped, not switched**, so the clip plane *slides* rather than pops. **Apply
  it as-is** — app-side smoothing fights the runtime's ramp and produces a slower, less
  predictable plane.
- **An unchanged preview is NOT stale.** Capture sources deliver a frame only when the desktop
  *changes*, so a generation that stops advancing means the last verdict still describes what is
  behind the app (a quiet desktop is the best case). Only the source withdrawing (`false` from the
  slot) or positively flagging its preview invalid (`XRT_DP_BG_PREVIEW_STALE`) closes the budget.

### 4.3 Applying the budget

The arithmetic is the existing clip-plane math with `farOffsetVH` substituted for the hard-coded
0/1000:

```
near_z  = ez - vH
far_z   = ez + farOffsetVH * vH
clipFar = transparent ? far_z : 0
```

`displayxr-common` provides `dxr::ClipPolicy::ResolveClipPlanes(...)`, which performs exactly this
and supplies the fallback of §6; prefer it to re-deriving the rule per app.

### 4.4 Environment override

`DXR_REAR_BUDGET` = `clip` | `open` | `auto` (default `auto`) forces the outcome for bring-up and
A/B work: `clip` pins `farOffsetVH` to 0, `open` pins it to 1000, and both report state `FORCED`.
An armed override logs once, with its value, so a forced run is never silently mistaken for a
measured one.

`DXR_REAR_BUDGET_DUMP=1` additionally writes the preview the analysis saw to
`%LOCALAPPDATA%\DisplayXR\rear_budget_preview.png` on each state change.

### 4.5 Region of interest

The conflict this extension polices is **local**: it exists only where rear content is drawn over
a horizontal cue. v1 nevertheless measured the whole canvas, so a busy patch anywhere under the
app closed the budget for the session — conservative in the wrong direction when the content sat
in one corner. (The panel run that motivated v2 read `cue = 0.93` off an *empty* Notepad window's
own menu and status bars, while the model was nowhere near them.)

**v2** lets the app say where its content actually projects, by chaining `XrContentBoundsDXR`
(§3.4) on `XrFrameEndInfo` in `xrEndFrame`. The runtime then measures only that region.

The app is the only party that can compute this — it owns the geometry and the matrices — so it
projects its content AABB, unions over all views, and reports the result in canvas-normalised
coordinates. `dxr::ProjectAabbToCanvasBounds` in `displayxr-common` does the projection; apps
should not roll their own.

How the runtime turns those bounds into a measured region:

1. **Map.** The bounds are canvas-normalised; the background preview covers `canvas_u0..v1` **of
   the canvas**, which is normally `0,0,1,1` but may include a margin. The bounds are mapped
   through that rect into preview pixels. A display processor that leaves the canvas rect zeroed
   is treated as `0,0,1,1` — the documented normal case.
2. **Dilate.** Every side is expanded by `max(4% of the preview width, 8 px)`, plus the app's
   `marginNormalized` (applied in canvas-normalised units before the mapping). The disparity
   conflict is read in the **band around the silhouette**, not strictly under it, so measuring the
   exact projected AABB would answer a question nobody asked.
3. **Clamp** to the preview.

Every failure path falls back to the **whole preview** — never to "neutral":

| Situation | Region measured |
|---|---|
| No `XrContentBoundsDXR` ever chained | Whole canvas (v1 behaviour) |
| Not chained for more than 1 s | Whole canvas — the app stopped, and a second-old rect describes geometry that has since moved |
| `extent <= 0`, or any non-finite component | Whole canvas ("unknown"); one-time `WARN` |
| Bounds that clamp away to fewer than 2 px | Whole canvas — an empty region would measure as neutral and open the budget over a desktop nobody looked at |
| `DXR_REAR_BUDGET_ROI=0` | Whole canvas (A/B kill switch; logs once when armed) |

The runtime re-measures when the capture generation advances **or when the derived region moves**.
The second is what keeps the ROI live: on a quiet desktop the generation never advances again, and
gating on it alone would pin the verdict to wherever the content used to be.

The region is reported on every state transition, in the runtime log:

```
REAR_BUDGET d3d11: roi=142,2,56,96 (app content bounds)
```

A rear-depth verdict without its region is unattributable — measured under the content, or over a
canvas the content was nowhere near?

## 5. Runtime Behavior

- **Source.** The background pixels come from an optional appended per-API display-processor slot,
  `get_background_preview`, which returns a small BGRA8 CPU preview (<= 512 px) of the desktop
  under the canvas plus a monotonic generation. See
  [`docs/reference/xrt_plugin_iface.md`](../../reference/xrt_plugin_iface.md). First integration:
  Leia SR. A NULL slot or a `false` return is `CLIPPED_NO_SOURCE`.
- **Cadence.** The compositor polls the slot after `process_atlas()` on the render thread, at most
  every 66 ms, and re-analyses only when the generation advanced. The vendor produces the preview
  at its own capture throttle (<= 15 Hz). There is no per-weave cost.
- **Analysis.** `u_bg_neutrality` — luma `Y = 0.299R + 0.587G + 0.114B`, **horizontal**
  differences only. Vertical differences are ignored by design: a vertical gradient is
  horizontally uniform, which is exactly what makes it depth-neutral.
- **Region.** The analysis ROI is the app's dilated content bounds when it chains them, and the
  whole preview otherwise (§4.5). The bounds arrive on the app thread in `xrEndFrame` and are read
  on the render thread by the analysis, so they move under the runner's own lock.
- **Policy.** `u_rear_budget`, one instance per native-compositor session or per service client.
- **IPC.** For service clients the budget is computed service-side (the service runs the display
  processor) and travels with the located views. Client-present / workspace-hosted sessions report
  `UNRESTRICTED_WORKSPACE` in v1.
- **Backends.** D3D11, Vulkan and D3D12 are wired: each drives the shared per-session runner
  (`comp_rear_budget`) once per **app** frame, immediately after handing its display processor the
  atlas — never from a repaint, which replays rendering only. GL and Metal report
  `CLIPPED_NO_SOURCE` until their slot call is wired, which is today's behaviour.
- **Which display processor is polled.** Whichever one is *weaving* the session, because that is
  the one running the desktop capture the preview is a downsample of. Under the hybrid
  output-device split (ADR-039, on by default at every tier, so on an iGPU/dGPU box every Vulkan
  session takes it) the weaver is the **D3D11** display processor on the scanout adapter, and the
  runtime polls that one; the compositor's Vulkan display processor is neither weaving nor
  capturing there. The policy lives on the compositor rather than on either display processor, so
  a session that retires the split mid-flight keeps its dwell, its ramp and its published value
  and simply changes which slot it asks.

## 6. Application Responsibilities

- **Treat absence as normal.** If the extension is unavailable, unenabled, or the chained struct
  comes back zero-filled, fall back to today's rule:
  `farOffsetVH := (transparent && standalone) ? 0 : 1000`.
- **Do not smooth.** Apply `farOffsetVH` as delivered (§4.2).
- **Feed every clip stage from the resolved value.** An app whose shader or compute pass performs
  its own far cull must drive that cull from the same resolved `far_z`, or the geometry and the
  cull will disagree while the budget ramps.
- **Do not capture the desktop yourself.** That path was considered and rejected in ADR-040:
  per-app capture cost and a different policy in every app.
- **Report your content bounds (v2, optional but strongly recommended).** Chain
  `XrContentBoundsDXR` on `XrFrameEndInfo` every frame with the projected AABB of the content that
  would occupy the rear volume, unioned over views and clamped to `[0,1]`. Without it the runtime
  judges the whole canvas and closes the budget for busy pixels the app is nowhere near. Use
  `dxr::ProjectAabbToCanvasBounds` — the projection has one correct answer and app-side variants
  of it will not agree. Do **not** apply any ROI logic of your own beyond that: the dilation, the
  staleness rule and the verdict are the runtime's.

## 7. Sample Usage

```c
/* --- at xrCreateInstance: enable alongside the rig extension --- */
/* (see §2) */

/* --- per frame, in the locate-views path --- */
XrRearDepthBudgetDXR budget = {XR_TYPE_REAR_DEPTH_BUDGET_DXR};
XrViewDisplayRawDXR  raw    = {XR_TYPE_VIEW_DISPLAY_RAW_DXR};
raw.next = &budget;                    /* chain both onto the view state */

XrViewState viewState = {XR_TYPE_VIEW_STATE};
viewState.next = &raw;

XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
li.viewConfigurationType = viewConfigType;
li.displayTime           = frameState.predictedDisplayTime;
li.space                 = appSpace;

uint32_t viewCount = 0;
xrLocateViews(session, &li, &viewState, viewCapacity, &viewCount, views);

/* --- resolve the clip planes --- */
float farOffsetVH;
if (haveDepthBudgetExt && budget.state != XR_REAR_DEPTH_BUDGET_STATE_MAX_ENUM_DXR) {
    farOffsetVH = budget.farOffsetVH;          /* apply as-is; the runtime already ramped it */
} else {
    farOffsetVH = (transparent && standalone) ? 0.0f : 1000.0f;   /* §6 fallback */
}

const float near_z  = ez - vH;
const float far_z   = ez + farOffsetVH * vH;
const float clipFar = transparent ? far_z : 0.0f;   /* 0 = "no shader-side clip" */

/* far_z drives the projection AND any shader/compute far cull, so they agree while it ramps. */

/* --- per frame, at xrEndFrame: say where the content is (v2) --- */
XrRect2Df bounds;                       /* canvas-normalised, origin top-left */
if (!dxr_project_aabb_to_canvas_bounds(aabbMin, aabbMax, viewProj, viewCount, &bounds)) {
    bounds = (XrRect2Df){{0, 0}, {0, 0}};   /* extent 0 = "unknown" = whole canvas */
}

XrContentBoundsDXR cb = {XR_TYPE_CONTENT_BOUNDS_DXR};
cb.bounds           = bounds;
cb.marginNormalized = 0.0f;             /* 0 = the runtime's own dilation alone */

XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
cb.next             = fei.next;         /* preserve whatever else is chained */
fei.next            = &cb;
fei.displayTime     = frameState.predictedDisplayTime;
fei.environmentBlendMode = blendMode;
fei.layerCount      = layerCount;
fei.layers          = layers;
xrEndFrame(session, &fei);

/* --- optional: react to transitions --- */
XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
    if (ev.type == XR_TYPE_EVENT_DATA_REAR_DEPTH_BUDGET_STATE_CHANGED_DXR) {
        const XrEventDataRearDepthBudgetStateChangedDXR *e =
            (const XrEventDataRearDepthBudgetStateChangedDXR *)&ev;
        log_state_change(e->previousState, e->newState);   /* notification only */
    }
    ev = (XrEventDataBuffer){XR_TYPE_EVENT_DATA_BUFFER};
}
```

## 8. Versioning

The extension is versioned by `XR_DXR_depth_budget_SPEC_VERSION`; every struct is fixed-layout and
identified by its own `XrStructureType`. New capability arrives as **new chained structs**, never
as new fields on an existing one — growing `XrRearDepthBudgetDXR` would let a newer runtime write
past the end of a struct declared by an app compiled against an older header. `XrViewDisplayRawDXR`
is likewise untouched by this extension for exactly that reason (ADR-040, *Alternatives
considered*).

An app compiled against v1 headers runs unchanged on a v2 runtime; the v2 runtime simply never
sees the structs the app does not chain. v2 added exactly that: `XrContentBoundsDXR`, a new input
struct on a type value v1 had already claimed. `XrRearDepthBudgetDXR` is byte-identical between
the two versions, and a v2 app talking to a v1 runtime has its content bounds ignored — which is
v1's canvas-wide ROI, i.e. today.

Applications that vendor these headers should note that a vendored copy does **not** track spec
bumps automatically — the `consumer_floors` drift audit is what catches the gap.

## 9. Reference Implementation

- Extension header: `src/external/openxr_includes/openxr/XR_DXR_depth_budget.h` (auto-synced to
  `displayxr-extensions`)
- Analysis: `src/xrt/auxiliary/util/u_bg_neutrality.{c,h}` (+ unit tests)
- Policy: `src/xrt/auxiliary/util/u_rear_budget.{c,h}` (+ unit tests)
- Display-processor slot: `get_background_preview` / `struct xrt_dp_background_preview` in
  `xrt_display_processor.h` — see [`xrt_plugin_iface.md`](../../reference/xrt_plugin_iface.md)
- Per-session runner (cadence, ROI derivation, publish): `src/xrt/compositor/util/comp_rear_budget.{c,h}`
  (+ unit tests), driven by the D3D11, Vulkan and D3D12 native compositors
- oxr consumption: `oxr_session.c` (locate-views path, beside `XrViewDisplayRawDXR`),
  `oxr_session_frame_end.c` (`XrContentBoundsDXR` parse + per-backend dispatch),
  `oxr_event.c` (state-changed event)
- App-side helper: `dxr::ClipPolicy::ResolveClipPlanes` in `displayxr-common`

## 10. Out of Scope / Future

Tracked on [#1365](https://github.com/DisplayXR/displayxr-runtime/issues/1365):

- **Vendor-neutral background source** — a runtime-owned Windows capture probe (so `sim_display`
  and other vendors are not gated on implementing the slot) and a `bg2d` socket source on
  Linux / Android.
- **Graded budget** — mapping partial `backgroundCueEnergy` to a partial offset rather than the
  v1 binary open/clip. Needs a perceptual calibration pass before any curve is chosen.
- **Workspace sessions** — `UNRESTRICTED_WORKSPACE` is a placeholder for today's behaviour, not a
  statement that a budget could never apply under a workspace controller.

## 11. Revision History

| Version | Changes |
|---------|---------|
| 1 | Initial version — `XrRearDepthBudgetDXR` on `XrViewState`, the state-changed event, canvas-wide ROI; `XR_TYPE_CONTENT_BOUNDS_DXR` reserved for v2 (epic #1363, ADR-040) |
| 2 | `XrContentBoundsDXR` on `XrFrameEndInfo` — the app reports where its content projects and the analysis measures only there, dilated (§4.5). Additive: `XrRearDepthBudgetDXR` unchanged, no new entry points, no new event ([#1365](https://github.com/DisplayXR/displayxr-runtime/issues/1365)) |

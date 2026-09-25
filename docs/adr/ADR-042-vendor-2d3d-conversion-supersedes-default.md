# ADR-042: A vendor 2D→3D conversion module supersedes the open default — the runtime exposes it, weaving stays the DP's

**Status:** Accepted (2026-09-25) · introduces
[`XR_DXR_lift`](../specs/extensions/XR_DXR_lift.md) · appends five optional D3D11
display-processor slots and one optional plug-in factory under the
[ADR-020](ADR-020-plugin-abi-compatibility-policy.md) append-at-end rule · related:
[ADR-007](ADR-007-compositor-never-weaves.md),
[ADR-019](ADR-019-vendor-plugin-aux-boundary.md),
[ADR-040](ADR-040-rear-depth-budget.md)

## Context

Turning ordinary 2D content — a video in a page, a photo, a call participant — into something
the panel can show in 3D is done today by **open defaults that live in the consumer**: the
DisplayXR Browser and the web SDK run an open monocular depth estimator plus a view generator
(and, for photos, an open depth + splat generator) in the page, then hand the result to the
weave like any other stereo content.

Display vendors have their own conversion modules — trained and tuned for their optics, often
running on dedicated inference paths the page cannot reach (Leia's NeurD over DirectML/CUDA is
the first; a Leia-owned photo → Gaussian-splat model is next). The vendor plug-in is already the
one component that knows the panel, and it already ships per machine. What was missing is a
generic way for the runtime to expose such a module, and a rule for who wins when both exist.

Three questions have to be settled together:

1. **Who wins?** An app — or the SDK inside it — that has its own converter, on a machine whose
   plug-in also has one.
2. **What crosses the boundary?** Woven pixels would make the conversion a second weaver and
   break ADR-007; depth alone would push view synthesis back into every consumer.
3. **How does it meet the frame loop?** Models take tens of milliseconds (seconds for photo →
   splats). The weave is a ~1 ms synchronous service on the present thread of a browser.

## Decision

### 1. A READY vendor module supersedes the open default

When `xrGetLiftPropertiesDXR` reports `READY` with the needed mode bit, a consumer that also
ships an open converter uses the runtime's. It falls back to its open default only when the
runtime reports `UNAVAILABLE` (no module, a failed one, a non-Windows service, an in-process
session) — and treats `ACTIVATING` as "not yet", polling, never as a permanent fallback.

This is the same shape as weaving: the vendor's calibrated implementation, behind the plug-in,
beats a generic one in the app. The runtime exposes the capability **generically** — modes,
limits, state, an informational backend name — never which model runs.

**Vendor Gaussian modules supersede the SDK's open MoGe + generator lift the same way NeurD
supersedes the open depth (VDA-class) default.** A plug-in advertising `GAUSSIANS` is preferred
for photo → splats; the SDK passes the photo's focal length (`focalPx`, e.g. the MoGe-estimated
`fx`) so the vendor model gets the intrinsics it takes as input.

### 2. The module returns pre-weave views; weaving stays the DP's

SBS and N-view results are ordinary pre-weave views (two / N views side by side, not woven),
DEPTH is a depth map, GAUSSIANS a blob. The runtime weaves SBS/N-view results on the existing
weave path, through the same display processor as everything else (ADR-007). So:

- one weaver per panel, never two; the conversion module is never on the present path;
- a consumer may also take the views or depth itself (effects, look-around re-render) — the
  result is useful beyond weaving;
- a vendor module needs no knowledge of windows, phase, or the interlace.

### 3. Asynchronous, one frame behind; geometry from the weave rect

Conversion runs on a runtime-owned **lift thread** with its **own device**, off the weave, render
and IPC threads. A submit is a snapshot into a **latest-wins mailbox** (a slow module lags a
frame; it never builds a backlog); an acquire returns the newest finished result. Nothing on any
latency-sensitive thread waits for the model.

When lifted content is woven, the caller flags the weave rect (`XrWeaveSubmitLiftRectsDXR`); the
service snapshots the rect's 2D content into the stream **and weaves the stream's latest result
at the rect's current position** in the same submit. Drag, resize and scroll therefore stay exact
and real-time — they come from this frame's rect — while only the depth is one conversion
behind. Until the first result the rect is woven flat.

### 4. Scheduling is runtime policy

Multiple streams share one module. The runtime schedules them by a per-stream priority (HIGH
every round, NORMAL round-robin, LOW every 4th round, PAUSED never) and reports each stream's
effective rate. The plug-in converts one frame per call and knows nothing about streams'
relative importance.

### 5. The plug-in contract is minimal and synchronous

Five appended D3D11 DP slots (`lift_get_caps`, `lift_stream_create`, `lift_stream_destroy`,
`lift_convert`, `lift_convert_blob`) and one appended plug-in factory
(`create_dp_d3d11_lift` — a DP that serves only lift: no weaver, no tracker session), all
optional (ADR-020, no ABI bump). The runtime always passes explicit viewpoints (the panel's
predicted tracked eyes, or the app's), so the lift DP needs no tracker. A module's warm-up
(licence, model load) is reported as ACTIVATING and polled.

## Consumer contract (browser + web SDK)

| Situation | Browser / SDK does |
|---|---|
| `READY` + mode bit | use the runtime: `XrWeaveSubmitLiftRectsDXR` for inline 2D video/images lifted in place; `xrSubmitLiftFrameDXR` + `xrAcquireLiftResultDXR` where the page wants views/depth itself; `xrAcquireLiftBlobDXR` for photo → splats |
| `ACTIVATING` | keep the current rendering (flat, or the open default if already running); poll ≤ 2 Hz; switch to the runtime on READY |
| `UNAVAILABLE` / `XR_ERROR_FEATURE_UNSUPPORTED` | open default |
| a lift call returns `XR_ERROR_RUNTIME_FAILURE` | transient: retry next frame, do not fall back |
| `XR_ERROR_INSTANCE_LOST` | the weave §4b recovery (new instance), then re-query properties |

- Draw lifted content into the weave input as 2D (the whole rect on the batch layout; every tile
  on the N-view layout) — never pre-convert it when the runtime will.
- Key results on `sourceTime` when the pairing matters; otherwise take the freshest.
- Set stream priority from what the user is looking at (active speaker, focused video);
  PAUSED for off-screen content keeps its last result without spending the module.
- Gate the chained structs on the extension being enabled, not on a spec version (v1).

## Consequences

- **+** The best available converter on each machine is used without the app knowing which it is.
- **+** No second weaver; ADR-007 holds. The conversion never touches the present path.
- **+** Geometry of lifted content is exact at weave time; depth latency is bounded by the
  module, visible per stream, and shaped by priority.
- **+** Hardware-free end to end: sim_display's env-gated fake module exercises every path in CI;
  `displayxr-cli lift probe` measures a real module on a panel box.
- **−** One more thread and one more D3D11 device in the service (lazily created — zero cost on a
  machine that never asks), and a second DP instance of the vendor plug-in (lift-only).
- **−** Frames cross devices as keyed-mutex shared textures: one extra copy each way. On a hybrid
  box the lift device is on the service's render adapter; a module that runs on another adapter
  (its own device) pays its own bridge.
- **−** Windows/D3D11 only in v1. Other platforms advertise the extension and report
  `supportedModes = 0`, which the consumer contract already handles.

## Alternatives rejected

- **The module weaves its own output.** Two weavers per panel, calibration duplicated, and a
  conversion on the present path — ADR-007 exists to prevent exactly this.
- **Return depth only; consumers synthesize views.** Pushes the vendor-specific part (view
  synthesis tuned to the optics) back into every consumer.
- **Synchronous conversion inside `xrWeaveSubmitDXR`.** Turns a ~1 ms service into a tens-of-ms
  one on the browser's present thread.
- **Reuse the weaving DP instance for lift.** It is driven on the service's single shared
  immediate context under the render lock and is recreated on presenter changes; a conversion
  would stall every weave for its whole duration.
- **Priority as a per-frame submit field.** Priority changes when attention changes, not per
  frame; a per-stream setter says so and keeps the submit minimal.

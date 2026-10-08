# ADR-049: In-process lift rides the service — one conversion module per machine, reached over a lift-only connection

**Status:** Proposed (2026-10-08) · Phase 1 implemented (runtime routing) · extends
[ADR-042](ADR-042-vendor-2d3d-conversion-supersedes-default.md) ·
spec: [XR_DXR_lift.md](../specs/extensions/XR_DXR_lift.md) §2, §5.3, §10 · issue #1862, epic #1858

## In one paragraph

`XR_DXR_lift` (ADR-042) converts 2D content to 3D with the vendor's conversion module, which
runs inside `displayxr-service.exe` on a lift thread with its own D3D11 device. An in-process
session — every engine app, and above all the Unity display provider, which renders in-process
on Unity's own D3D12 device — got `XR_ERROR_FEATURE_UNSUPPORTED` from every lift call, so no
engine app could use the module. This ADR keeps the module where it is and gives an in-process
D3D11 / D3D12 session a **second, sessionless, lift-only connection to the service**: the same
IPC calls the browser and `displayxr-cli lift` already make, results back as the same NT handles
(a texture and a shared fence) that a D3D12 device opens with `OpenSharedHandle`. Rendering
stays in-process; conversion stays in the one service. Nothing in the plug-in contract changes.

## Context

- **Streams belong to a connection, not a session.** That is already how the service implements
  lift (ADR-042, spec §9): `displayxr-cli lift` drives it headless over a sessionless DIAG
  connection, and the service admits `APP` connections to every lift call.
- **The export is already D3D12-consumable.** A result's export texture is
  `SHARED | SHARED_NTHANDLE` with **no keyed mutex**, and its fence is an `ID3D11Fence` created
  `D3D11_FENCE_FLAG_SHARED`. `ID3D12Device::OpenSharedHandle` opens both, as an
  `ID3D12Resource` and an `ID3D12Fence`; the app's queue `Wait`s the fence value. No new export
  kind is needed.
- **The vendor conversion runtime is process-global and single-worker.** The first module behind
  the lift slots keeps its properties (model, output type, tuning) in process-wide state and
  marshals every call to one worker thread. A second process that loads it gets a second copy
  of the model in VRAM (hundreds of MB to GB), a second warm-up (licence check, engine build:
  seconds), and a second worker competing for the same GPU with no shared scheduler. Two
  converters per machine is the expensive shape this decision is mostly about.
- **Scheduling is runtime policy** (ADR-042 §4): priorities and round-robin across every
  stream on the machine only work if every stream is in the scheduler.
- **Hybrid boxes.** On an iGPU + dGPU machine the service's ingest device — where lift
  textures live — is on the adapter it publishes as `client_d3d_deviceLUID` (ADR-037 §7). A
  shared NT handle opens only on a device of the **same adapter**.

## Options

### (A) Shared lift module, instantiated in the app process

Refactor `d3d11_lift.cpp` into a module the in-process runtime can also host: its own D3D11
device in the app process, a lift-only DP from the in-process instance's plug-in
(`create_dp_d3d11_lift`, which the in-process instance already discovers), results shared to
the app's D3D12 device through NT handles and a shared fence.

- **Vendor neutrality:** fine — the same factory and slots as the service.
- **Latency:** best case one IPC round trip less per call (~0.1 ms) — immaterial next to a
  conversion of tens of ms.
- **Adapter:** the lift device can be created on the app's own adapter, so no cross-adapter
  hazard. This is A's one real advantage.
- **ABI:** none.
- **Vendor-runtime state:** **the deciding cost.** Every engine app that lifts loads its own
  copy of the vendor runtime: double VRAM and double warm-up whenever a browser (service-side
  module) and an engine app run together, and two workers fighting over one GPU with no common
  scheduler. Process-global properties also mean two streams in one app process can still
  stomp each other's settings — the service already serialises that; A would have to again.
- **Cost:** large. The module is wired to service-only pieces (the service immediate-context
  mutex, the weave-rect pin path, the service device). Splitting it is a refactor of a
  2,200-line file that the shipping browser path depends on.

### (B) A native D3D12 lift slot in the vendor contract

Append `lift_*` slots to `xrt_display_processor_d3d12` (or a `create_dp_d3d12_lift` factory)
so a plug-in converts directly on a D3D12 device.

- **Vendor neutrality:** fine in form, but it doubles what every vendor must implement for one
  feature (a D3D11 and a D3D12 path), and the first module's runtime is D3D11 / DirectML
  internally — the plug-in would bridge back to D3D11 anyway.
- **Latency:** no better than A.
- **ABI:** append-only slots under ADR-020 — allowed, no ABI bump, but permanent surface.
- **Vendor-runtime state:** the same per-process duplication as A, since the slot still runs
  in the app process.
- **Cost:** vendor work in every plug-in plus everything A needs in the runtime.

### (C) In-process sessions reach the service's lift over a lift-only IPC connection (hybrid)

The app renders in-process exactly as today. Its lift calls open — lazily, in the background —
a sessionless `APP` connection to the service and run the existing lift IPC calls on it.
Results come back as the existing NT handles.

- **Vendor neutrality:** nothing vendor-specific anywhere; no plug-in change.
- **Latency:** each lift call is one IPC round trip (submit: round trip + one blit in the
  service, the same as the browser pays). Conversion itself is unchanged and dominates.
  Never on the app's frame path for longer than the round trip: the connect is asynchronous,
  and only `xrCreateLiftStreamDXR` (a setup call) waits for it.
- **Adapter:** results live on the service's ingest adapter. An app on another adapter cannot
  open them — the same constraint every IPC D3D app already has, and the one ADR-037 §7
  already steers apps around with `xrGetD3D12GraphicsRequirementsKHR`. An engine that picks
  its own adapter (Unity) must check `OpenSharedHandle` and treat a failure as
  `UNAVAILABLE`; a follow-up exposes the lift adapter LUID in properties (§Follow-ups).
- **ABI:** none (no plug-in change, no wire change, no header change).
- **Vendor-runtime state:** **one module per machine.** The vendor runtime is loaded once, by
  the service, warmed once, and every stream on the box — browser tabs, calls, engine apps —
  shares its VRAM and one scheduler with priorities that mean something across apps.
- **Cost:** small and additive: a connection holder in `ipc_client` and a router in
  `oxr_lift.c`. The browser / CLI / IPC-session path is unchanged.
- **Costs specific to C:** the app depends on a running service for lift (it already does for
  the weave on the browser path; an engine app without a service falls back exactly as it
  does on a machine without a module), and the connection is subject to the client↔service
  version-tag gate (a from-source runtime against an installed service reads `UNAVAILABLE`).

### Scorecard

| | (A) in-process module | (B) D3D12 slot | (C) lift over IPC |
|---|---|---|---|
| Vendor runtime instances per machine | one per lifting process | one per lifting process | **one** |
| VRAM / warm-up | doubled with a browser running | doubled | **shared** |
| Cross-app scheduling (ADR-042 §4) | per process | per process | **machine-wide** |
| Plug-in ABI | none | appended slots | **none** |
| Vendor work | none | a D3D12 path each | **none** |
| Latency per call | lowest | lowest | + one IPC round trip (~0.1 ms) |
| Hybrid adapters | app's adapter | app's adapter | service ingest adapter (app must match) |
| Runtime change | large refactor | large + vendor | **small, additive** |

## Decision

**(C).** An in-process D3D11 or D3D12 session on Windows reaches the service's conversion
module over a lift-only connection it owns. One vendor conversion runtime per machine is worth
far more than the sub-millisecond IPC hop A and B save, and C needs nothing from vendors.

1. **The connection.** `ipc_client_lift_link` (in `ipc_client`) owns one sessionless
   connection per in-process session, declared `APP` and named `<exe> (in-process lift)`.
   It is created on the session's first lift call and connects on a background thread; it
   never launches the service (it dials only when the service's single-instance mutex
   exists). A failed connect is retried at most every 5 s. It is closed when the session is
   destroyed, after the session's lift streams; closing it drops the streams service-side,
   as for any connection.
2. **Routing.** `oxr_lift.c` picks the connection per call: an IPC session's own, or the
   in-process session's link. Every call then runs the same `ipc_client_lift_*` code. Other
   in-process sessions (GL, Vulkan, Metal) keep `XR_ERROR_FEATURE_UNSUPPORTED`.
3. **States while not connected.** `xrGetLiftPropertiesDXR` reports `ACTIVATING` while the
   connect is in flight and `UNAVAILABLE` (`supportedModes = 0`) when there is no service —
   the consumer contract of ADR-042 then does the right thing unmodified.
   `xrCreateLiftStreamDXR` waits up to 6 s for an in-flight connect (setup call), then
   returns `XR_ERROR_RUNTIME_FAILURE` (still connecting, retry) or
   `XR_ERROR_FEATURE_UNSUPPORTED` (no service).
4. **Connection loss never loses the session.** The session renders in-process, so a dead
   lift connection (service restart) is a transient `XR_ERROR_RUNTIME_FAILURE` on the call
   that sees it; the stream handles of that connection then return
   `XR_ERROR_FEATURE_UNSUPPORTED` ("recreate the stream"), and the next properties query
   reconnects. (An IPC session keeps the weave §4b contract: `XR_ERROR_INSTANCE_LOST`.)
5. **Result consumption on D3D12.** The app opens `outputTexture` and `fence` with
   `ID3D12Device::OpenSharedHandle`, closes the handles, `ID3D12CommandQueue::Wait`s
   `fenceValue` before sampling, and finishes sampling before its next acquire on the stream
   (spec §5.1, unchanged).
6. **Input on D3D12.** The input handle is a D3D12 shared resource
   (`D3D12_HEAP_FLAG_SHARED`, `CreateSharedHandle`) — no keyed mutex, which D3D12 cannot
   take. The service snapshots it at submit with no GPU wait, so the app makes its writes
   complete before `xrSubmitLiftFrameDXR` (fence + CPU wait, or the frame's existing
   completion) and does not rewrite the texture until a later frame — double-buffering the
   input is the simple way. A submit-side fence is a follow-up (§Follow-ups).

## Consequences

- **+** Engine apps get the vendor module with no plug-in change, no ABI change, no wire or
  header change; the existing ADR-042 consumer contract applies verbatim.
- **+** One vendor conversion runtime on the machine: shared VRAM, one warm-up, one scheduler
  across the browser, calls and engine apps.
- **+** The IPC-session path is untouched (its calls now go through the same
  `ipc_client_lift_*` functions directly instead of one-line bridges).
- **−** Lift for an in-process app needs the service running and version-matched.
- **−** An extra sessionless `APP` connection per lifting in-process session; it counts
  against the service's APP quota and shows in `displayxr-cli clients`.
- **−** Results live on the service's ingest adapter; an engine on a different adapter cannot
  open them (it sees `OpenSharedHandle` fail and must fall back).
- **−** No GPU-side input sync for D3D12 callers yet (decision 6).

## Follow-ups

- **Lift adapter in properties.** Chain an `XrLiftAdapterPropertiesDXR { LUID adapterLuid }`
  on `XrLiftPropertiesDXR` so an engine that picked its own adapter knows *before* opening a
  handle whether lift results are reachable. Spec bump; coordinate with the lift viewpoint
  policy bump (ADR-048).
- **Submit-side fence.** Chain an `XrLiftSubmitSyncDXR { fence handle, waitValue }` on
  `XrLiftFrameSubmitInfoDXR`; the service `Wait`s it on its context before the snapshot blit.
  Removes decision 6's CPU wait / double-buffer requirement for D3D12 callers.
- **Weave rects stay IPC-only.** `XrWeaveSubmitLiftRectsDXR` belongs to the weave path, which
  in-process sessions do not use; an in-process engine app composes the SBS / N-view result
  into its own views.
- **Unity provider** (`displayxr-unity`, separate PR): consume lift from the D3D12 provider —
  properties gate, stream lifecycle, D3D12 input double-buffer, `OpenSharedHandle` of the
  export + `Wait` on the fence, a C# surface for scripts (e.g. a camera-feed component).

## Alternatives rejected

- **(A) and (B)** — above: per-process vendor runtime instances are the cost that matters, and
  B adds vendor surface for no gain over A.
- **Force engine apps onto the IPC path** (`XRT_FORCE_MODE=ipc`) when they enable
  `XR_DXR_lift`. Unity under the service takes the client → IPC → D3D11-service route, which
  costs the provider its zero-copy D3D12 swapchain and the in-process compositor it is built
  around; lift should not decide how an app renders.
- **Reuse the session's compositor connection.** An in-process session has none.

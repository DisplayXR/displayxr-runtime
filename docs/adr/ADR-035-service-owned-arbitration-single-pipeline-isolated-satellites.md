# ADR-035: The Service Owns Arbitration, Runs One Compositor Pipeline, and Isolates Its Satellites

**Status:** Accepted (design; implementation phased in epic #974, issues #950–#973 — nothing in this ADR is shipped at the date below)
**Date:** 2026-08-16

## Context

DisplayXR's service must host many independent IPC clients at once — a workspace
controller with several apps, a legacy WebXR session in Chrome, a `displayxr-browser`
inline-3D session, the WebXR bridge, and (per #943) sensor/input providers — indefinitely,
without one degrading or killing the others. On Android every app class is forced onto
IPC and there is no shell, so N-concurrent-IPC is the *only* topology there.

The 2026-08-16 review mapped the service as built
([service-architecture.md](../architecture/service-architecture.md), §9 lists the
twenty defects). Compressed to what the design must answer:

1. **Identity and authorization do not exist.** Caller identity is a client-asserted PID;
   111 of 131 handlers are unauthenticated; the gates that exist fail *open* without an
   orchestrator (Android, macOS, dev). Any client can deactivate the workspace, flip the
   panel, move another client's window, take an input grab, or recenter everyone.
2. **The compositor has two modes selected by one process-global bool.** Workspace mode
   (service window, render thread, one shared DP — the #925-hardened, healthy path) and
   standalone mode (per-client window + swap chain + **DP**, presenting on the client's
   IPC thread, written single-tenant). Slot membership is decided once at session create.
   Clients that straddle a transition are stranded (dark with an orphan DP, or without a
   swap chain). N standalone clients ⇒ N vendor weavers on one panel. A present-owner
   under a workspace drives the *workspace's* DP from an IPC thread.
3. **Shared display state has ~10 writers and no owner.** Hardware 2D/3D, the content
   mode, the atlas grid, focus (two authorities), the DP. Vetoed requests are latched and
   replay later. Mode events fire before the vendor confirms and are never reverted (#761).
4. **In-process DLLs share the service's fate** (#943, #930). Five hand-placed catch-alls
   are the only containment; there is no `set_terminate`, no job object, no backoff, no
   client reconnect. The WebXR bridge never exits when the service dies and squats on :9014.
5. **Capacity is 8 connections** and the bridge burns 2 — the target scenario does not fit.
   The cap sits on the wire (`ipc_client_list.ids[8]`); each client maps ~21 MB of shm.
6. **The IPC core is stock Monado**: blocking pipes with no timeouts, one global mutex held
   across pipe I/O in two handlers and nested over `render_mutex` at eleven, unbounded
   client-supplied allocations, an unbounded per-session event queue.
7. **Input arbitration is per-provider, not per-consumer**: qwerty is an unlocked
   destructive integrator shared by N pollers; `get_presence` runs under a global mutex on
   every client's `xrSyncActions`; there is no input focus.
8. **Nothing is observable per client.** `[RENDER] wait_avg` samples only the render
   thread; the #939 datum (`12/10 s @ 865 ms, wait_avg ≈ 1 µs`) could not be attributed.
9. **Android has none of the Windows arbitration** (it lives inside
   `comp_d3d11_service.cpp`), one global surface, one DP *per client*, and `comp_multi`
   reproduces the #920/#922/#924 coupling verbatim. ADR-025's "out-of-process" isolates
   the app from the vendor SDK, not the DP from the service.

### Options considered

1. **Status quo + tactical fixes** (auth on the worst handlers, bounded waits, bridge
   lifecycle). Cheap, and several of these ship in Phase 1 regardless — but it leaves N
   DPs, the global mode bool, the 8-slot cap, fate-sharing, and Android untouched.
2. **Controller-owned arbitration** — the shell decides who owns the panel and focus,
   the service obeys. Fails the moment there is no shell (Android today, every
   standalone-only Windows session, dev boxes) and makes a private repo the arbiter of
   public-runtime behaviour. Rejected as the *primary* mechanism; the controller remains
   a privileged *policy* client of a service-owned mechanism.
3. **Service-owned arbitration + one compositor pipeline + isolated satellites** — this
   ADR. The service holds explicit leases; there is one multi-compositor pipeline that is
   always on with a built-in default policy when no controller is present; input providers
   move to a supervised host process; the vendor DP stays in-process but fault-contained.
4. **Everything out of process, including the vendor DP.** Rejected for now: the DP is on
   the per-frame render hot path (`process_atlas` on the atlas the render thread just
   composed), needs the compositor's D3D device and window, has no in-tree precedent on
   any platform, and every vendor SDK we know of (SR, CNSDK) is designed to be linked into
   the presenting process. The cost is a cross-process texture handoff + fence per frame on
   the latency-critical leg for a fault class (`exit()`/throw in a DP) that guarded vtable
   calls, a duration watchdog and a DP-recreate path contain nearly as well.
5. **One service per client class** (workspace service, browser service…). Rejected: the
   panel is one physical resource; N services would need an arbiter above them — this
   ADR's arbiter — and would multiply the DP-instance problem instead of solving it.

## Decision

The service is the **panel owner**. It arbitrates every shared display resource itself,
composites every IPC client through one pipeline, and pushes everything that can fault
independently into supervised satellite processes. Concretely, eight decisions:

### D1 — Server-derived identity and explicit client classes

- At accept, the server derives the peer's process identity itself
  (`GetNamedPipeClientProcessId` + process start time on Windows; `SO_PEERCRED` /
  `LOCAL_PEERPID` on POSIX; `Binder.getCallingPid/Uid` on Android). The client-supplied
  PID becomes informational only.
- Every connection is assigned a **class** at handshake, declared by the client and
  **verified** by the server: `CONTROLLER` (peer exe matches a registered workspace
  controller, or the orchestrator-spawned PID), `APP`, `PRESENT_OWNER` (weave extension
  enabled), `RELAY` (headless bridge), `PROVIDER_HOST` (spawned by the service), and
  `DIAG` (`displayxr-cli`). The class is stored on the client state, exposed to the
  controller, and drives quotas, priority and authorization.
- Authorization **fails closed**. First-claim of the controller role without an
  orchestrator is allowed only for a verified controller binary or under an explicit,
  loudly-logged dev override.

### D2 — Ownership is expressed as leases held by the service

| Resource | Holder | What non-holders may do |
|---|---|---|
| **Panel lease** (hardware 2D/3D, content mode / atlas grid, DP configuration) | the `CONTROLLER` while one is active; otherwise the service's built-in default policy, which grants it to the **focused presenting client** | *request* a mode; the request is delivered to the holder (controller: as an event to decide on; default policy: applied if the requester is focused) or **denied with a reason event**. Never latched for replay. `hw_override`-style "hold me flat" remains a per-client fact the holder honours |
| **Focus** | one authority: the compositor slot table (`focused_slot`); the IPC layer's `active_client_index` becomes a derived read, and `predict_frame` no longer promotes anyone | nothing; a client learns focus by event |
| **Input focus** | derived from focus; unfocused clients get inert devices server-side (`io_active` = focused), qwerty integrates on the window thread only and hands out snapshots | nothing |
| **Windows / slots** | each slot's owner (the client) for its own state; the controller for placement | own-slot state only; never another slot |
| **Input grab, pointer capture, cursor, overlays, capture clients, reserved keys, chrome** | controller only | nothing |
| **Space origin / recenter** | controller when present, else the focused client | request |
| **Atlas capture** | controller or `DIAG`; apps only their own slot | own slot |

Mode-change events are emitted **after** the DP confirms (or are reverted when it does
not) — closing #761 by construction. The lease table is a service-side data structure in
`ipc_server`, above the compositor, so it is the same code on Windows, macOS, Linux and
Android; the compositor exposes a narrow interface (register surface, set presenter, apply
mode with a lease token) instead of owning the policy.

### D3 — One compositor pipeline: the multi-compositor is always on

- Every IPC client renders into a **service-owned per-client surface** (today's
  `c->render.atlas_texture`) — always, controller or not. The render thread composes and
  presents. There is **one DP per panel**, owned by the pipeline.
- Without a controller the pipeline runs a **default presenter policy**: the focused
  presenting client is shown full-screen at native resolution in its requested mode (what
  the hosted standalone path does today), other clients are composed hidden or as the
  policy dictates. A controller replaces the policy, not the mechanism. This is exactly the
  shape Android needs (one surface, N clients, no shell) — the same code path.
- **Presenter strategies** replace "modes": *service window* (hosted, WebXR, no controller,
  workspace), *app HWND* (`_handle` forced-IPC — the pipeline presents into the app's window
  through the one DP), *client-presents* (ADR-029 shared texture; #625 present-owner). The
  present-owner path stays synchronous (displayxr-browser's zero-lag design depends on it)
  but it is **serialised by the panel owner, bounded, and may not mutate mode or geometry**
  — the DP is invoked only through the pipeline's serialisation and only on behalf of a
  lease holder.
- The standalone per-client-DP path is retired behind `DXR_LEGACY_STANDALONE=1` for one
  release (A/B on motion-to-photon and the Intel-iGPU present path), then deleted.
- Commit-thread writes to global mode/geometry move behind the S3 queue; no vendor DP
  call is made on a client IPC thread outside the present-owner path.

### D4 — Isolation: input providers leave the process; the DP stays and is contained

- **Provider host** (`displayxr-provider-host.exe` on Windows, an AIDL-bound
  `:providers` service in the runtime APK on Android; POSIX daemon). Providers load
  there. The host **pushes** poses, joints, presence and the roles generation into a
  shared-memory ring; the service's proxy `xrt_device`s read lock-free and fan out to N
  clients from one snapshot. Host death ⇒ roles drop to the qwerty floor via the existing
  presence machinery, supervisor restarts the host with backoff, the compositor never
  blinks. The `xrt_input_plugin_iface` gains append-only lifecycle slots (start/stop/health).
  `net_input`'s wire protocol is the seed (prediction-grade timestamps into
  `m_relation_history`); it needs a joints message class and multi-peer support.
  Cost, honestly: today the service already pays one synchronous RPC per hand per client
  per frame; the host adds one producer hop *in total* (not per client) at ≤ one producer
  period of jitter, and removes provider code from every client thread and from
  `g_arb_mutex`. In-process apps keep their in-process providers (their fate is their own).
- **Vendor DP stays in-process** with containment: every vtable call from service threads
  goes through a guard (SEH/`try` on all threads, not just the two guarded today), a
  duration watchdog on `process_atlas`/`request_display_mode`/factory, `DEVICE_REMOVED`
  and `Present` HRESULT handling, and a DP-recreate path that does not require a service
  restart. Revisit option 4 only if a vendor SDK proves uncontainable.
- MCP is already out of the service (per-app + shell). The bridge stays out but is fixed:
  exits on `INSTANCE_LOST`, one connection, restarted by the supervisor.

### D5 — Supervision and restart

- Service-spawned children (controller, bridge, provider host) run in a **job object**
  (`KILL_ON_JOB_CLOSE`), are restarted with exponential backoff and a crash-loop cutoff,
  and their exit codes are logged. An orderly-shutdown IPC verb replaces
  `TerminateProcess` as the first resort (ADR-016's open item).
- Service graceful restart: broadcast `XrEventDataInstanceLossPending` with a loss time,
  join client threads in `teardown_all`, then exit. Satellites survive by
  restart-under-supervision; apps follow the OpenXR contract (recreate the instance).
  **Non-goal:** transparent session-level reconnect for apps (rebuilding swapchains,
  shared handles and spaces behind the app's back) — the isolation work makes service
  death rare instead.
- Every service thread entry installs a per-thread terminate handler and an `atexit`
  stack tripwire logs the exiting thread's stack (the #943/#930 forensics gap).

### D6 — Client classes scale predictably

- `IPC_MAX_CLIENTS` 8 → 16 as a coordinated protocol bump (the git-tag gate keeps client
  and service in lockstep; `ipc_client_list.ids[]` and the per-client shm layout change
  together; shm is slimmed from `slots[128] × layers[128]` to what the zones layer model
  needs).
- Per-class quotas: 1 reserved `CONTROLLER` slot; `RELAY` ≤ 1; `PRESENT_OWNER` ≤ 2;
  `PROVIDER_HOST` outside the app budget; the rest for `APP`. Refusal at the cap returns a
  distinguishable error (`XR_ERROR_LIMIT_REACHED` on `xrCreateInstance`) and a log line
  naming the class that hit its quota. Under pressure the *lowest-priority newest* client
  is refused — never the controller, never an already-presenting client.

### D7 — Observability per client

- A per-client health record (class, pid, slot, lease held, last commit, commit period,
  fence timeouts, acquire/present skips, mode requests/denials, RPC p99, max
  blocked-in-handler, event-queue depth) emitted as a `[HEALTH]` line every 10 s and served
  by a new RPC to `displayxr-cli clients`.
- `[RENDER]` gains handler-side lock-wait and per-thread DP-call duration histograms so
  the next collapse is attributable in one log window.

### D8 — Android alignment

Same shape, same code: D1/D2/D6/D7 live in `ipc_server` and apply unchanged (identity via
binder credentials, quotas returned to `MonadoImpl.connect`, telemetry via
sysprops/logcat). D3 requires `comp_multi` to gain the shared-surface compositing that
macOS already has (one surface, one DP, N clients) plus the S1/S4-class bounded waits;
D4's provider host is an AIDL-bound service; the single merged runtime APK (#1031) is built in CI and
#510 folds into this plan.

## Decisions confirmed (2026-08-16)

- **Vendor DP stays in-process, fault-contained** (D4 option, not option 4). Confirmed by David: the DP is on the per-frame render hot path, needs the compositor's device+window, and every vendor SDK (SR, CNSDK) is built to link into the presenting process; guarded vtable calls + a duration watchdog + a DP-recreate path (#971) contain the fault class without a per-frame cross-process handoff.
- **#943's `exit()` source is left to self-report** rather than hunted: the filed mechanism was dead code, and the #950 `[EXIT]` tripwire now names the calling thread+stack on recurrence while #952 refuses the worktree-DLL footgun that staged it.

## Consequences

**Positive**

- One arbitration implementation for four platforms; the healthy workspace path becomes
  *the* path instead of one of two; N clients ⇒ one DP, one present, one owner.
- A provider or bridge fault is that process's problem; the compositor keeps its cadence.
- Capacity is a policy, not an array size; the target scenario fits with headroom.
- The next wedge is attributable per client from one log window.

**Costs and risks (design around these, in order)**

- **Motion-to-photon for today's standalone clients** (Chrome legacy WebXR, forced-IPC).
  Client-paced `Present(1)` on the IPC thread becomes render-thread compose under
  late-weave pacing. Must be measured (`motion-to-photon-levers.md` levers, the LL probes),
  gated for A/B, and validated on the Intel-iGPU present path where `present_wait` is
  absent before the legacy path is deleted.
- **Vendor DP semantics with one instance and several presenters** (per-window phase,
  the browser's per-rect synchronous weave). ADR-033 says the weaver owns phase via
  placement; validate on `sim_display`, then SR, before retiring per-client DPs. The
  present-owner fast path is kept precisely so inline-3D does not regress to async.
- **Protocol bumps** (class field, `IPC_MAX_CLIENTS`, shm layout) need lockstep updates in
  the shell, bridge, `displayxr-cli`, and `displayxr-browser`'s vendored patches. Bump once,
  together, behind one release.
- **Fail-closed auth** breaks today's manual controller launches on macOS/dev unless the
  verified-controller-binary rule and the dev override ship first.
- **Provider host latency** must be measured against today's per-client pull before the
  in-process arbiter path is removed; the in-process-app question (two LeapC clients on one
  box) is decided by that measurement.
- **Android** cannot host N on-screen clients until the shared-surface compositing port
  lands; until then D3's default policy on Android is "focused client only", which is at
  least explicit.
- **Sequencing:** ship the tripwires and per-client telemetry first so every later phase
  is measurable and #943's real exit source is caught if it recurs; do not touch the
  workspace path's behaviour without the close gauntlet as the definition of done.

**What this ADR does not decide:** the exact wire encoding of classes/leases (spec follows
in `docs/specs/runtime/`), the provider-host ring format (Phase 4 spec), or whether
`compositor/multi/` is ported or replaced on Android (Phase 5 spike).

## References

- `docs/architecture/service-architecture.md` — the as-built map this decision is measured against.
- #939 (arbitration), #943 (provider isolation), #925 (stability epic; S1–S5 are the workspace-path groundwork this builds on), #929, #930, #924, #944, #761, #762, #510.
- `docs/adr/ADR-016` (controllers own tray/lifecycle — the orderly-shutdown item lands here), `ADR-019` (vendor plug-in boundary; process isolation is its next step), `ADR-025` (Android app↔service split; clarified: the DP is in the service), `ADR-028` (mode recipe vs hardware state — the lease is its owner), `ADR-029` (client-presents strategy), `ADR-033` (weaver owns phase), `ADR-034` (input provider plug-ins; the host is where they live next).
- `docs/reference/workspace-stability.md`, `docs/reference/motion-to-photon-levers.md`.

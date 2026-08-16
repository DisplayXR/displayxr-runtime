# Workspace stability — the wedge family, its principles, and the diagnostic toolkit

The August 2026 stabilization effort (runtime v2.6.0 → v2.6.1+) found and fixed a
family of "one bad actor freezes everything" failures in the D3D11 service
compositor and its satellites. This page records the failure *classes*, the
principles that prevent them, and the diagnostics that found them — so the next
wedge hunt starts from here instead of from scratch. Architectural epic: #925.

## The principle

**No unbounded work on the critical path.** The service render thread, the IPC
handler threads, and any thread holding `render_mutex` may never wait on
something another process controls without a bound: not a fence a dying client
will never signal, not a `Present` into a jammed flip chain, not a DWM
composition pass, not a keyed-mutex a frozen producer holds, not vendor-SDK
creation work with disk I/O, not a pipe write a stalled peer isn't reading.
A misbehaving client's failure domain is itself: it may lose frames, its
connection, or its slot — never the compositor.

## The classes found (all fixed unless noted)

| Class | Mechanism | Fix |
|---|---|---|
| Lock starvation (#920) | `render_mutex` held ~a full display period under SR-v2 late latching; `std::recursive_mutex` has no waiter fairness → IPC handlers starved 20–30 s | `render_mutex_fair_lock` waiter-announce + capture-loop yield (PR #921) |
| Dying-client fence jam (#922, ×2 sites) | `context->Wait()` queued on a workspace fence value a dead client never GPU-signals → immediate context jammed | `GetCompletedValue()` poll; unfinished frame = stale, reuse + retry (PR #921) |
| Blocking Present (#924) | `Present(1,0)` into a jammed flip chain (hybrid NV render → Intel scanout) blocks minutes | present token from the frame-latency waitable; no free back buffer → skip (PR #921); standalone path: `DO_NOT_WAIT` retry ≤50 ms |
| Unbounded DwmFlush | successful Present + hung DWM still wedged the client's IPC thread | bounded `DCompositionWaitForCompositorClock` (4–100 ms clamp) (PR #927) |
| Oversized acquires / INFINITE waits | weave-submit `AcquireSync(1000 ms)` under `render_mutex`; `swapchain_wait_image` literal `INFINITE` + an `XR_INFINITE_DURATION` → `DWORD` truncation bug | 4 ms compose-path budget; 1000 ms clamps (PR #927) |
| Vendor weaver create/destroy on the critical path (leia-plugin#144) | SR weaver creation = SR-service retry loops + correction-texture disk I/O + senses start, called synchronously by the DP factory under `render_mutex` (sometimes on the render thread); teardown similar | async create with flat-blit fallback + detached-reaper destroy in the plug-in (leia-plugin PR #146); `DXR_LEIA_ASYNC_WEAVER=0` reverts |
| MCP notification write wedge (#928) | Phase-A `broadcast_notification` did a blocking pipe write on the app's MAIN thread inside an OpenXR call; any non-reading MCP client (e.g. `displayxr-voice`) froze every subsequent app launch at `xrSetMCPAppInfoDXR` | bounded writes + abort-on-timeout + trylock in displayxr-mcp (PR displayxr-mcp#22); server pipes needed `FILE_FLAG_OVERLAPPED` for any of it to be real |
| Session-ended client never evicted (#929) | a client that ends its session without exiting keeps its slot and workspace **focus** indefinitely | S4 tier 1: focus surrendered immediately, slot evicted after `DXR_EVICT_ENDED_MS` (default 2000 ms) (`a669e3809`); S4 tier 2: opt-in idle eviction via `DXR_EVICT_IDLE_MS` (`64e46b8df`) with commit-time re-register for a client that recovers (`8edc2f41d`) |
| Silent service exit (#930) | service vanished post-teardown under late-weave with no crash record | One root-caused class landed: an exception in the D3D11 window proc must not kill the service — try/catch **inside** `wnd_proc` (`cc7df1676`); a `std::set_terminate` net was prototyped and removed because MSVC's terminate handler is per-thread. #943's `exit()`-from-an-in-process-plug-in class produces the same "no reason" signature. WER LocalDumps armed (`%TEMP%\dxr_dumps`, full dumps) |
| Cross-thread window calls under `render_mutex` (audit rank 7) | window operations issued from the render thread against the UI thread's window serialised behind the lock | moved off `render_mutex` (`370c695ba`) |
| Joins + per-frame filesystem stats on the render path (audit ranks 5, 6, 10) | thread joins and per-frame file stats executed on the critical path | removed (`5e67ad231`, #937) |
| Unbounded in-process target `Present` (audit rank 1) | `comp_d3d11_target.cpp` `Present` with no bound, same jammed-flip-chain mechanism as #924 but on the in-process path | bounded (`64bc40b14`, #931, #925 S1) |
| Standalone strobe (#944) | an atlas slot was cleared per-frame while a skipped blit was set to reuse it → black flashes originating in the **atlas**, not the present | never clear an atlas slot a skipped blit will reuse (`9dee0a3da`) |
| In-process input provider took the service down (#943) | an input provider plug-in loaded in-process in `displayxr-service.exe` called `exit()`; the whole service and every client went with it | filed; process-level isolation of satellites is ADR-035 decision D4 |

The full site-by-site audit (every wait, acquire, present, vendor call, and
filesystem touch on the critical path, with thread + lock attribution) lives in
the [#925 S2 audit comment](https://github.com/DisplayXR/displayxr-runtime/issues/925).
Slice status: **S3** (workspace-control command queue — handlers stop taking
`render_mutex`) shipped (`3dca7e09f`), default on, `DXR_CMD_QUEUE=0` reverts.
**S4** (eviction) shipped, both tiers — see the table row above. **S5**
(compose-from-copy) shipped *partially* (`a6d36daf6`, #940): the per-client
*content* atlas was already service-owned, so S5 landed only as
`compose_copy_cache` for the controller producers (chrome, overlay, cursor),
gated behind `DXR_COMPOSE_FROM_COPY=1` and **default off**. So "S5 shipped" does
not mean compose never touches a client-owned resource.

## Diagnostics that earn their keep

- **`[EXIT]` / `[TERMINATE]` tripwires (#950).** The service registers an
  `atexit` hook that logs `[EXIT] unexpected process exit()` with the exiting
  thread's stack whenever the process ends off its orderly shutdown path (an
  in-process DLL calling `exit()` — the #943 signature: clean banner, no cause),
  and every service thread entry runs under a structured-exception guard that
  logs `[TERMINATE] thread '<name>' died: <kind> code=0x… at <module>+<offset>`
  plus the faulting stack *before* the exception propagates unchanged (WER /
  `std::terminate` happen exactly as before). Frames are `module+offset`; resolve
  with `cdb -z <exe-or-dump> -y <pdb dir> -c "ln displayxr_service+0x<off>; q"`.
  Dev-only fault injection on the client-teardown site: `DXR_TEST_EXIT_ON_DISCONNECT=1`
  (`exit(3)`) / `=2` (write to NULL) — never on a production box.
- **`[RENDER]` diag** (10 s window, service log): healthy ≈ 590/10 s.
  `capture_avg_us` is the per-iteration cost; `wait_avg_us` is `render_mutex`
  acquire latency — *that* climbing means lock trouble; low `wait_avg_us` with
  low `capture_renders` means GPU contention (uncapped clients), not locks.
  Collapse (≤300) or absence = jam. `client_renders/client_skips=0` does NOT
  prove no frames — eyeballs or `layer_commit` lines do.
- **Release builds carry PDBs** (PR #926): `displayxr-service.pdb` +
  `DisplayXRClient.pdb` stage into `_package/bin` and deploy alongside dev
  binaries — cdb stacks resolve to real frames including inlines. Never debug a
  wedge on nearest-export symbols again.
- **Stall proof requires re-sampling**: one snapshot of a thread inside
  `Present`/`weave`/`WriteFile` proves nothing — in-flight I/O looks identical.
  Same stack 2–3 s apart = stuck.
- **A kill that unwedges everything** is itself a diagnosis: whatever the stuck
  thread waited on was owned by the killed process — the wait was unbounded.
- **Wedge captures** go to `%TEMP%\wedge*`; service crash dumps (full) to
  `%TEMP%\dxr_dumps` via WER LocalDumps.

## Box/test conventions (SR dev box)

- Never debug DisplayXR clients from an elevated shell (`ipc_client_setup_shm`
  fails, then ~60 s connect-retry hang). Launch via a `schtasks /RL LIMITED`
  wrapper — it also delivers fresh user env (a long-lived `explorer.exe` serves
  stale env to its children).
- A shell-spawned app inherits the launch wrapper's stdout handle: a surviving
  app keeps the log locked and later wrapper runs die silently on the `>`
  redirect — use per-run log names.
- The close gauntlet (the definition of done for wedge work): two VK demos,
  {X, DELETE, ESC, hard-kill, close-during-maximize, close-last-app} × 3, under
  `DXR_LATE_WEAVE=0` **and** `=1`, every close < 2 s, `[RENDER]` steady, and the
  operator's own hands on the panel.

## See also

- [Service architecture](../architecture/service-architecture.md) — the as-built
  map of the service process: threads, locks, failure domains, the two compositor
  modes, client classes, and limits. It also qualifies the `[RENDER]` numbers
  above: `wait_avg_us` samples only the **render thread's own** `render_mutex`
  wait, so starvation of the IPC handler threads is invisible to it, and
  `client_renders` is structurally 0 in workspace mode.
- [ADR-035](../adr/ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md)
  — the target architecture these fixes converge on.

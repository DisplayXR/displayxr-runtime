---
status: Design
owner: David Fattal
updated: 2026-10-12
adr: ADR-053
issues: []
code-paths: [src/xrt/include/xrt/xrt_conversion_plugin.h, src/xrt/conversion/, src/xrt/targets/conversion_host/, src/xrt/compositor/d3d11_service/d3d11_lift.cpp, src/xrt/state_trackers/oxr/oxr_lift.c, src/xrt/ipc/, src/xrt/targets/cli/, src/xrt/drivers/sim_display/]
---

# Conversion providers — plan (ADR-053)

**Status:** Design complete 2026-10-12, red-team amendments applied; nothing implemented. Phases
per ADR-053 (§9).

- Decision record: [ADR-053](../adr/ADR-053-conversion-providers.md).
- Provider-facing contract and registration: [`conversion-provider-discovery.md`](../specs/runtime/conversion-provider-discovery.md).
- App-facing C: [`conversion-providers-headers-draft.md`](conversion-providers-headers-draft.md).
- App-facing spec: [`XR_DXR_lift.md`](../specs/extensions/XR_DXR_lift.md) (spec 3 today; specs 4 and 5 are planned here).

This page is the living plan: architecture, hosting, transport, policy, diagnostics, the app API
summary, consumer migration, phases and open items. The ADR and both specs stay vendor-neutral;
this page names products where an operational fact needs them. Where an earlier design draft
disagrees with ADR-053, ADR-053 wins. Anchors refer to `main` @ `ec6aee48b`.

**Vocabulary.** Internally (headers, loader, CLI, ADR) the word is **conversion**. A *conversion
provider* is a third plug-in type, beside display processors and input providers. App-facing, the
word stays **lift** (`XR_DXR_lift`, `dxr-lift`, "Convert to 3D"). The lift spec states that split
once (ADR-053).

---

## 1. Why

- **Lift is welded to the display plug-in's D3D11 vtable.** The only call surface is six DP slots
  (BASE+25..29, +33) plus the `create_dp_d3d11_lift` factory (`xrt_plugin.h:1261`). A vendor that
  makes no display, or an open-model provider, has nowhere to plug in.
- **One module per machine, chosen by the display plug-in.** `d3d11_lift` captures one DP factory
  at create (`d3d11_lift.cpp:1546`, `:1564-1565`) and never refreshes it. The lift-only factory and
  its fallback can even come from different plug-ins (`comp_d3d11_service.cpp:35798-35799`).
- **The browser ships its own converter and a triplicated model store.** 1.65 GB of ONNX models
  (2.99 GB for the full set) are fetched and sha256-verified by three implementations: the installer
  (`fetch-models.ps1`), the browser (`model_store.cc`, patch 0324) and the web SDK (`models.js`).
  The in-page converter also becomes a *silent, session-long fallback*, for three code reasons:
  1. the SDK caches the first caps answer per document (`native.js liftCapabilities()` WeakMap);
  2. the GPU process enables `XR_DXR_lift` in one pre-sandbox instance and, after 6 × 250 ms of
     retries, gives up on lift for the life of the process (0340);
  3. the Viz gate `LiftAvailable()` never re-polls UNAVAILABLE.

  A module that was merely warming at first contact therefore loses the whole session.
- **No N-view and no splat module exists.** The one shipping module serves DEPTH, SBS and aux
  depth; it refuses NVIEW, and GAUSSIANS exists only as the sim fake's PLY. The four v3 modes cannot
  name stereo→disparity, image+depth→views, stereo→metric splats, video jobs, or metric vs relative.
- **Windows-only.** Conversion is hosted only in the D3D11 service. macOS and desktop Linux
  advertise the extension (`oxr_extension_support.h:777-778`) with the module always absent;
  in-process GL, Vulkan and Metal sessions get `FEATURE_UNSUPPORTED` (`oxr_lift.c:196-200`).
- **The Restart-Manager file-lock saga.** Once lift has run, the vendor conversion runtime's DLLs
  stay mapped in `displayxr-service.exe`. Idle-unload and shadow-deps (leia-plugin #285) were
  reverted (#286) because a reload cost 8–10 s of unconverted frames. Today's contract is "the
  vendor installer closes the service", so every vendor update kills every session.
- **Policy leaks into the model.** Convergence and auto-convergence live inside the vendor runtime
  (one shared feed id since NeurD 0.4.9); the plug-in maps viewpoints into model units and derives
  `ref_z_m` from vendor geometry; the browser flips relative depth into pseudo-disparity and assumes
  a 60° HFOV for splats. Units are invented where they should be declared absent.
- **Activation latches.** No module at first activation ⇒ UNAVAILABLE until the service restarts
  (`d3d11_lift.cpp:847-858`).

**What changes for whom.**

- **Apps** keep `XR_DXR_lift`. v3 callers are served byte-for-byte. Spec 4 adds tasks, readiness
  events and neutral images; spec 5 adds jobs. Apps never pick or see a model, and a non-READY task
  means wait, never fall back.
- **Display vendors** stop carrying lift in their DP. The DP lift slots freeze in P1. A built-in
  legacy adapter, always in the service, serves them until the vendor ships its conversion module as
  a separate provider DLL (P3), hosted in a supervised satellite that the vendor's installer can
  close without touching the service.
- **Model providers**, open or commercial, get a registration root, a hosting-agnostic ABI, a
  model-pack manifest and a runtime-owned store.
- **The browser** and the web SDK delete their converter, model store and ORT bundle (~8 k lines).
- **The dashboard** and `displayxr-cli` gain per-provider and per-task readiness rows, `conv` verbs
  and host health.

---

## 2. Architecture

### 2.1 Shape

The conversion service has three parts:

- a platform-neutral **core**: registry, readiness, selection, stream scheduler, job queue,
  policy stages and diagnostics;
- **per-graphics-API GPU glue**: import, snapshot, export and the reference view synthesis;
- a **provider host interface**, with two implementations: in-service (P1) and the
  `displayxr-conversion-host` satellite (P2).

The core is not inside `comp_d3d11_service.cpp`, so the same core runs in the Windows, macOS and
Linux service and in Android's `:conversion` process (§4.3).

```
 IPC threads / lift link / weave_submit          render thread / DP eye listeners
            |  (producers)                                ^  (eyes snapshot per DP, weave pin)
            v                                             |
 +---------------------- conversion core (conv_*) ---------------------------------+
 | registry + capability table (+ persisted describe cache) -- selection -- FSM    |
 | stream lane: u_lift_mailbox per stream, u_lift_sched per host                   |
 | job lane: queue per host, progress/cancel, result staging                       |
 | stages: letterbox/cap -> viewpoints -> [provider] -> view synthesis -> observers |
 +-------------------------------|------------------------------------------------+
                                 | conv_host_iface (vtable, async completions)
                 +---------------+----------------+
                 | in-service shim (P1)           | satellite proxy (P2) -- pipe + shm rings
                 | legacy adapter, always; others | displayxr-conversion-host.exe --provider <id>
                 +--------------------------------+
```

### 2.2 Component map

| Component | New file(s) | Generalises today's | Thread |
|---|---|---|---|
| Registry, capability table, describe cache | `src/xrt/conversion/conv_registry.{c,h}`; loader `target_conversion_plugin_loader.{c,h}` | one DP factory captured at `d3d11_lift_create` (`d3d11_lift.cpp:1546`) | registry |
| Selection | `conv_select.{c,h}` (pure, host-tested) | none | caller (IPC) |
| Readiness FSM | `conv_readiness.{c,h}` (pure) | `activated` latch + caps poll (`d3d11_lift.cpp:777-896`) | registry |
| Stream lane | `conv_stream.{c,h}`; `u_lift_mailbox` + `u_lift_sched` reused unchanged | `lift_thread_main` (`:1430-1528`) | one dispatcher per host |
| Pipeline composer | `conv_pipeline.{c,h}` | hard-wired paths in `lift_convert_one` (`:1212-1405`) | dispatcher |
| Job lane + result staging | `conv_job.{c,h}`, `conv_stage.{c,h}` | GAUSSIANS on the stream thread; the 256 MiB blob latch (`:2725-2784`, `ipc_protocol.h:1118`) | one job thread per host |
| GPU glue, D3D11 | `d3d11_conv_gpu.cpp`, split out of `d3d11_lift.cpp` in P1b | snapshot, letterbox, import cache, export + fence (`:1861-2700`) | IPC, dispatcher |
| GPU glue, Vulkan | `vk_conv_gpu.c` (P5) | none on `main` | same |
| Reference view synthesis | `d3d11_conv_viewsyn.cpp`, `vk_conv_viewsyn.c` (P5), shared `u_conv_viewsyn.h` | the browser's `live-dibr.js` (ported) | dispatcher |
| Policy stages | `conv_policy.{c,h}` wrapping `resolve_viewpoints` (`:988-1112`), `u_lift_depth`, `u_lift_recenter`, rt#1978's `u_lift_autoconv` | same code, behind a stage interface | dispatcher |
| Host interface + in-service shim | `conv_host_iface.h`, `conv_host_inproc.c` | the DP lift slots, `lift_activate` | exec thread per provider |
| Legacy DP adapter (`dp-lift`) | `conv_provider_legacy_dp.cpp` | the six slots + `create_dp_d3d11_lift` | exec thread |
| Satellite proxy + supervisor | `conv_host_proxy.c`, `conv_supervisor.c`; `src/xrt/targets/conversion_host/` | ADR-035 D4/D5 (unshipped) | supervisor |
| Diagnostics | `u_status_snapshot.c`, `cli_cmd_conv.cpp` | `lift_caps` JSON (`cli_query.c:2646-2672`) | main loop |

### 2.3 Capability table

One row per provider × task × variant, filled at **describe** time: after load and negotiate,
before any model load. The provider ABI's `describe_cap(inst, index, out)` returns one capability
per call (count via `out == NULL`), so a growing capability struct never mis-strides an array.
Stream creates are validated against these declared capabilities, never against whatever happens
to be READY (that fixes G3, §10).

**Persisted describe cache.** The service stores the last describe result per (binary path, size,
mtime) and reloads it at start. After the first run the table is never empty, no host is spawned
just to say what a provider can do, and the spec-3 mapping (§7.3) has rows from the first query.

| Column | Source | Used by |
|---|---|---|
| `provider_id`, `binary`, `abi`, `version`, `probe_order` | registry + negotiate | selection, diagnostics |
| `task` (DEPTH_MONO … SPLATS_STEREO; SPLATS_VIDEO, INPAINT reserved) | describe | selection |
| `input_kinds`, `import_kinds` (CPU, NT handle, VK fd/win32, dma-buf, IOSurface, AHB) | describe | composer, transport (§4) |
| outputs: depth kind/measure, views layout, `max_views` (≤ `XRT_CONV_MAX_VIEWS` = 8), aux depth, relief, splat format; may be **provisional** until READY | describe | create checks, policy |
| flags `TEMPORAL`, `EXPLICIT_VIEWPOINTS` (required for VIEWS_*), `OFFAXIS_CAMERA`, `SUGGESTS_CONVERGENCE`, `LANES_CONCURRENT`, `WARM_CONCURRENT`, `NEEDS_NETWORK_TO_WARM`; `max_concurrent`; `max_streams` | describe | scheduler, threading (§3.1), limits |
| `backend`, `device_luid` (default: the service's ingest adapter) | describe + host | adapter |
| `licence_class` + SPDX | describe / model-pack manifest | gating |
| `est_vram_mb`, `est_warm_ms`, `typical_latency_ns` per variant | describe / manifest | FAILED(RESOURCES), progress, watchdog |
| `model_id` (diagnostics only, never in the app API) | describe | snapshot, logs |

A process-wide model choice is one capability whose variant follows `qualityHint`; there is no
`exclusive_group`.

### 2.4 Selection

Selection runs per task, at stream or job create, and is deterministic:

1. **Candidates.** Every row for the task with a matching input kind, plus composed pipelines (for
   example VIEWS_MONO served by DEPTH_VIDEO followed by the reference view synthesis).
2. **Drop terminal states.** Terminal = ABSENT (including ABSENT(`policy`) for a download that
   policy forbids), INCOMPATIBLE, UNLICENSED, DISABLED, and **any FAILED**, retryable or not.
   Licence-gated rows are dropped too: `research-only` and `unknown` need `AllowResearchOnly`.
3. **Order.**
   - `PreferredProvider\<task>` first (HKCU over HKLM, unless `Policy\Lock` is set; §6.4).
   - Then ProbeOrder: vendor 50, default 100, `dp-lift` 150, open provider 200, sim 250. FALLBACK
     providers always sort last.
   - Then a direct row before a composed pipeline; then a provider-id tie-break, with one WARN.
4. **Pick the first, whatever its readiness.** INSTALLED, DOWNLOADING, WARMING and EVICTED are the
   only waited-for states. **Readiness never reorders.** A fall-through is logged once.
5. **The preferred provider is re-selected at the next create**, never by moving a live stream.

**The one rebinding rule.** A live stream never moves except on a terminal failure of its provider
or a drain of its host binary (§5.3). The move is signalled by `discontinuity` plus the task-state
event; there is no per-stream provider-changed event. A preference change, an install or `conv use`
applies to the next create only.

`requiredOutputs` is checked against the describe-time declared outputs, provisional ones included.
A create is refused only when the provider is READY and still lacks the output; it never triggers
re-selection.

`displayxr-cli conv list` prints exactly this ordering, with a reason for every kept or dropped row.

### 2.5 Readiness per (provider, task)

Plug-in state values 0..7 are numerically equal to today's `XRT_DP_LIFT_MODULE_*`
(`xrt_dp_lift.h:130-137`); INSTALLED, DOWNLOADING and EVICTED are appended as 8..10. The app enum
uses the same values (§7.2).

| From | Event | To | Notes |
|---|---|---|---|
| — | registered, negotiated, task declared (or loaded from the describe cache) | INSTALLED | cold |
| — | binary missing / ABI rejected | ABSENT (reason) | re-probed on registry change |
| INSTALLED / EVICTED | warm, blobs missing, download allowed | DOWNLOADING (bytes/total) | the host downloads |
| INSTALLED / EVICTED | warm, blobs missing, download forbidden by policy | ABSENT(`policy`) | terminal |
| INSTALLED / EVICTED / download done | model present | WARMING (progress) | load + engine build + licence |
| any warm | `est_vram_mb` exceeds the adapter with nothing evictable | FAILED(`RESOURCES`) | terminal until the budget changes |
| WARMING | provider ready | READY | |
| READY | idle beyond keep-alive | EVICTED | never while a stream or job is bound |
| READY / WARMING | host crash or watchdog kill | WARMING (`restarting`, *n*) | streams survive (§3.5) |
| any | crash-loop cutoff | FAILED(`crashed`) | |
| WARMING | licence / platform / version / network failure | FAILED (reason) | network: backoff 15 s → 5 min |
| any | `conv disable`, `Enabled=0` | DISABLED | |

There is no `activated` boolean. A FAILED provider keeps its own retry triggers (a registry change,
`conv warm`, 30 min after a crash-loop, the network backoff timer); while it is FAILED, new creates
fall through to the next candidate.

**Prewarm.** The trigger is a **`conv_hint_interest(tasks)`** message the client sends when an
instance enables `XR_DXR_lift`: IPC sessions at `xrCreateInstance`, in-process apps when the lift
link opens. The service acts on it per the `PrewarmOn` policy:

| `PrewarmOn` | Meaning | Default on |
|---|---|---|
| `interest` | warm the prewarm tasks on the first `conv_hint_interest` | mains power / desktop |
| `first-create` | warm only at the first stream or job create | battery |
| `start` | warm at service start | kiosk opt-in |

The default task list is `views-mono`, `depth-video` (`Prewarm\<task>` DWORDs). Readiness is always
honest ACTIVATING meanwhile.

**Keep-alive** (two rules only): never evict a model while a stream or job is bound to it, and evict
job-only models after 600 s idle. `KeepAliveSec=0` (the default for prewarm tasks) means never evict
on idle. A reload is always WARMING with progress, never UNAVAILABLE.

**Events.** Every transition bumps `conv_gen` and `status_gen`, logs one WARN, and is pushed as
`XrEventDataLiftTaskStateChangedDXR` to the connections that opted in (§7).

### 2.6 Stream lane

`u_lift_mailbox` (two input slots, two output slots, pins, drop accounting) and `u_lift_sched`
(PAUSED/LOW/NORMAL/HIGH) are reused unchanged. What changes: one core per service, one
**dispatcher thread per provider host** (streams on different providers no longer serialise), and
asynchronous commands through `conv_host_iface`, bounded by the provider's threading declaration
(§3.1).

| Stage | Owner | Present when |
|---|---|---|
| S0 snapshot / import (`d3d11_lift.cpp:2223-2362`) | runtime, producer thread | always |
| S1 letterbox crop + input cap (`DXR_LIFT_LETTERBOX`, `DXR_LIFT_MAX_INPUT_EDGE`) | runtime | weave-rect sources; opt-in for submits |
| S2 colour / format normalise (NV12 → RGBA, sRGB tag) | runtime | provider lacks the source format |
| S3 viewpoint resolve (ADR-048) + params (convergence, budget) | runtime policy | view-producing tasks |
| **S4 provider task** | provider | always |
| S5 **reference view synthesis** | runtime | composed pipelines; explicit VIEWS_FROM_DEPTH |
| S6 depth decode + per-frame range (p5/p95) + relief | runtime | aux depth present |
| S7 observers: auto-convergence (rt#1978), cursor relief (ADR-046 A1) | runtime policy | a consumer enabled them |
| S8 publish to ring → export / weave pin | runtime | always |

**The reference view synthesis (S5)** is a runtime pipeline stage — never a provider row, never
selectable; the `COMPOSED` bit reports it. On every box without a view-producing provider it *is*
the shipping lifted-video quality, replacing the browser's in-page `live-dibr.js` in the release that
deletes it. **Acceptance bar:** it must match or beat the browser's current in-page view synthesis
on the lift panel corpus (real media), including temporal stability. Its hole-fill is ported from
`live-dibr.js`, not written anew. INPAINT (reserved) can later slot in after S5.

### 2.7 Job lane

| Aspect | Design |
|---|---|
| Scope | per connection; jobs die with it |
| Queue | one per host; interactive before background, FIFO within a priority; ≤ 16 queued per connection, then `LIMIT_REACHED` |
| Worker | one job thread per host, separate from the dispatcher |
| Progress / watchdog | a provider callback at least every 2 s |
| Cancel | `is_cancelled(ctx)` polled at least every 250 ms; hard cancel = host restart (satellite only) after a 10 s grace |
| Inputs | **decoded pixels only**: a GPU image, or a CPU image ≤ 256 MiB via shm. Clients downscale to `maxInputExtent` before submitting |
| Results | ≤ 1 MiB inline; else a shm section; else a **caller-opened file handle**, which the service checks is `FILE_TYPE_DISK` with write access before use |

There is no result cache in v1. Without `LANES_CONCURRENT` the host serialises job and stream calls,
selection prefers another provider for the job, and otherwise the stream's stats show
`blocked_by_job` with an event.

A job **never takes a file path**: the service would open files on behalf of callers that could not
(confused deputy), and decoding media would put every image-codec CVE into an always-on process. The
CLI and MCP decode and encode files themselves.

### 2.8 Export path

| Result | Windows today (kept) | Generalised |
|---|---|---|
| Views / depth texture | per-stream export texture, `SHARED_NTHANDLE`, no keyed mutex, plus a shared `ID3D11Fence` (`d3d11_lift.cpp:2473-2700`; D3D12-openable per ADR-049) | exportable VK image + timeline semaphore, IOSurface + `MTLSharedEvent`, AHardwareBuffer + sync_fd |
| Depth for CPU consumers | none | optional per-stream shm readback ring (≤ 256×256 R16F) |
| Blob (job) | two-call latch, 256 MiB | the §2.7 rules |

Keyed mutexes exist only on the client → service input import, as today; never between service and
host. `SYNC_NONE` means the producer finished before hand-over. A host output reaches a client
export only after the dispatcher has observed its completion (§3.5).

---

## 3. Hosting

### 3.1 The host interface and provider contract

```
struct conv_host_iface {          // "a place where one provider runs"
  describe_cap(index, out)                      // no model load; answered from memory
  warm(task, variant, is_cancelled) / evict(task)
  stream_open / stream_reset / stream_close     // provider-side temporal state
  submit(cmd)                                   // async; on_complete(seq, status, out)
  job_submit(job) / job_cancel(job_id)          // async; on_progress / on_complete
  register_image(xrt_conv_image) -> handle_idx  // import once per slot
  heartbeat() -> {state, vram_used, current_call, since_ns}
  shutdown(orderly)
}
```

Provider ABI points the plan relies on (full text in the discovery spec):

- **The provider instance is the module.** `warm` and `job_execute` block on runtime-owned threads
  and take a `bool (*is_cancelled)(void *ctx)` callback (also in `xrt_conv_job_control`). Providers
  own no activation threads.
- **Images are runtime-allocated** for inputs and outputs; the provider imports, never exports. In
  satellite mode the host, which is runtime code, allocates outputs on the provider's LUID.
- **Aux depth is an output of `stream_convert`.**
- **Append-only is real.** Every embedded struct (`xrt_conv_sync`, `xrt_conv_image`,
  `xrt_conv_depth_info`, `xrt_conv_views_info`, `xrt_conv_splat_info`,
  `xrt_conv_stereo_calibration`) is frozen — no `struct_size`, pinned by `sizeof` asserts on x64 and
  arm64 (and armv7 if the APK ships it). Growth happens only at the tail of top-level structs or via
  new iface slots. Images are not memcpy-able across processes; the proxy re-materialises them per
  hosting.
- Hosting-agnosticism rules: no runtime device, no windows on non-pumping threads, no process-state
  changes, network only inside `warm`, configuration only via `get_setting`.

**Threading contract.** By default **one call is in flight per provider**. `get_provider_status`,
`describe_cap`, `get_state` and `stream_get_output_desc` are the exceptions: they stay concurrent and
answer from memory. Opt-ins:

| Flag | Allows |
|---|---|
| `XRT_CONV_CAP_LANES_CONCURRENT` | a job to overlap streams |
| `max_concurrent` > 1 | converts to overlap each other |
| `XRT_CONV_CAP_WARM_CONCURRENT` | `warm` to overlap converts; without it, warm serialises and the stall is reported in stats |

### 3.2 In-service shim

The shim loads a provider into the service with one exec thread per provider (plus the opt-in lanes
above), so it behaves exactly like a satellite. It still passes shared handles, opened once per slot
lifetime, so a provider tested in-service runs unchanged in a host. In-service hosting is permanent
for the legacy adapter; the sim provider runs either way (CI runs it in the satellite); for any other
provider it is the `Hosting=in-service` escape hatch.

### 3.3 Legacy DP-lift adapter (`dp-lift`)

`dp-lift` is a built-in provider at ProbeOrder 150 that wraps the active display plug-in's
`create_dp_d3d11_lift` (re-resolved on every retry, from the plug-in that registered it), owns a
D3D11 device on the service's adapter, and calls `lift_convert` / `lift_get_depth`.

**It stays in the service, always.** The factory is reachable only through the display negotiate,
and the vendor's lift DP reads plug-in state that only probe populates, so a host-side copy would
silently change viewpoint normalisation. ADR-035 D4 (the display plug-in never leaves the service) is
not contradicted.

**Static describe.** If the active display plug-in exposes the lift-only factory (or
`create_dp_d3d11` with `XRT_DP_D3D11_HAS_LIFT`), the adapter declares its capabilities as
**INSTALLED** with provisional outputs before any factory call. It refines them after the first
`lift_get_caps`, and reports ABSENT only after the factory refuses or the module reports ABSENT.
"Not yet asked" is never terminal.

| Capability | From DP mode bit | Note |
|---|---|---|
| DEPTH_MONO, DEPTH_VIDEO | DEPTH | TEMPORAL on DEPTH_VIDEO |
| VIEWS_MONO, min 2, max = `caps.max_views` (≤ 8) | SBS / NVIEW | one capability; aux depth iff `aux_outputs & AUX_DEPTH` (provisional until READY) |

No SPLATS capability (no shipping plug-in implements the blob slot) and no exclusive group, so DEPTH
and VIEWS streams coexist as they do today. State is a plain copy of `module_status`; licence class
**COMMERCIAL** (a user installed it); `range_*` stays 0. A provider registered with
`Supersedes = dp-lift` hides it. The slots are frozen from P1 and removed at the next DP ABI major.

### 3.4 The satellite: spawn and supervision

| Aspect | Design |
|---|---|
| Binary | `displayxr-conversion-host(.exe)`, one binary for every provider. Android: the `:conversion` service (§4.3) |
| Instances | one per provider DLL; for a combined display + conversion DLL the host loads only the conversion symbol and never probes the display half |
| Spawn | **lazily**, by the supervisor, at the first warm or create that needs it (the describe cache answers before that): `--provider <id> --binary <path> --channel <name> --parent <pid> --adapter-luid <hex>` |
| Adapter | defaults to the service's ingest adapter. Cross-adapter hosting is cut from v1; when it returns, the host copies (D3D12 cross-adapter heap or staging) and the cost is stated |
| Containment | Windows: a job object with `KILL_ON_JOB_CLOSE` and an **in-host minidump writer** (not `DIE_ON_UNHANDLED_EXCEPTION`, which bypasses dumps). POSIX: `PR_SET_PDEATHSIG` / kqueue `NOTE_EXIT` |
| Integrity | inherits the service token: medium IL, never elevated (the host checks itself) |
| Idle exit | a host with no bound task and no prewarm exits after 10 min |
| Restart backoff | 0.5 s doubling to a 30 s cap; reset after 10 min healthy |
| Crash-loop cutoff | 5 abnormal exits in 10 min → FAILED for all its tasks |
| Orderly shutdown | the host exits 0 within 3 s, else `TerminateProcess` |
| Logs | `DisplayXR_conversion-host_<provider>.<pid>_<ts>.log`; WARN+ forwarded as `[conv:<provider>]` |

Exit codes (own namespace, unrelated to selftest exit 13): 0 orderly, 10 ABI reject, 11
negotiate/describe failed, 12 licence, 13 no usable device, 14 elevated, 20 closed for update (no
backoff, no crash-loop accounting), 30 watchdog kill, other = crash (minidump written).

### 3.5 Service ↔ host protocol

A **dedicated channel**, not a Monado IPC client: the service drives the host, and the hot path needs
no request/reply. The protocol is ~10 fixed messages, written as hand-written packed structs under
the §6.6 wire rules (no second generator spec). It reuses peer-credential checks
(`ipc_server_peer_creds.c`) and a git-tag gate at hello.

| Plane | Mechanism | Traffic |
|---|---|---|
| Control | named pipe / AF_UNIX `displayxr-conv-<provider>-<svcpid>`, ACL'd to the service user | hello, describe, warm/evict, stream open/close/reset, register_image, jobs, shutdown, logs, 1 Hz heartbeat |
| Data | per host and lane: a 64-record shm command ring + completion ring, one wake signal each way | `submit {seq, stream, in_image_idx, in_fence_value, params, viewpoints}` / `complete {seq, status, out_image_idx, out_fence_value, meta}` |
| GPU sync | shared fences carried as **values** per record (D3D NT fence, VK timeline, `MTLSharedEvent`, sync_fd) | see the fence rule |

**Handles.** The service holds the only cross-process handle right. It allocates input slots and
**pulls** host-allocated output handles with `DuplicateHandle(host → self)`, using its own process
handle on the host. The host never receives a handle to the service process. POSIX: `SCM_RIGHTS`
both ways. Rings carry indices, never handles.

**Fence rule (normative).** The service never enqueues a GPU wait on a host-signalled fence on any
compositor context. The dispatcher observes completion from the CPU (`SetEventOnCompletion` or a
fence poll, with a timeout) before a host output is consumed, and the client-facing export path
follows the same rule. A completion record says only that the host *issued* a signal. CI case: kill
the host between submit and signal; the compositor frame rate is unaffected.

**Crash → WARMING; streams survive.**
1. The supervisor sees the exit or a broken pipe.
2. Every in-flight command of that host completes as `failed`. Output rings keep their last result,
   so the weave keeps the last good views (or the flat rect).
3. All the provider's tasks go to WARMING(`restarting`, *n*); stream handles stay valid, producers
   keep submitting latest-wins.
4. After the backoff the dispatcher re-opens provider streams (implicit reset), re-registers images
   and resumes.
5. A crash-loop cutoff → FAILED(`crashed`) → the stream moves per the §2.4 rebinding rule, or stays
   alive in FAILED if there is no other candidate.

### 3.6 Restart Manager / update flow (P3)

| Step | What happens |
|---|---|
| Vendor installer takes `Global\DisplayXR-ConvMaintenance-<id>`, then `RmRegisterResources` → `RmGetList` | lists `displayxr-conversion-host.exe`, not the service (to be verified, §11.2) |
| `RmShutdown` | the host answers `WM_QUERYENDSESSION` on a hidden top-level window and exits 20 |
| Service | WARMING(`updating`); sessions and streams untouched |
| Installer replaces files, writes `Version` last, releases the mutex | — |

**Forced shutdown is the normal case for one shipping module.** The vendor module's own worker
owns a hidden window that never pumps messages, so `RmShutdown`'s graceful close fails and
`RmForceShutdown` kills the holder. Today the holder is the service; in a satellite it is only the
host. The supervisor treats that kill like a crash (§3.4: WARMING(`restarting`), streams survive,
images re-registered on respawn) and the respawn itself waits for the maintenance mutex as below.
The idle unload and the shadow-copy load are **not** coming back: the owner rejected the 8–10 s
reload stalls, and the satellite makes both unnecessary.

**Respawn after exit 20** never happens on a timer. The supervisor respawns only on a registry or
manifest change, or when the maintenance mutex is free and demand exists. The host never calls
`RegisterApplicationRestart`.

**The installer contract (P3; onboarding guide §9 and discovery spec):** *close the host, never the
service*, holding the maintenance mutex across the RM session. Old installers that close the service
keep working at the old cost. A combined display + conversion DLL keeps the vendor files locked in
the service; the guide recommends separate DLLs.

### 3.7 Cost model (per stream conversion, Windows, single adapter)

| Hop | Today (in-service DP) | Satellite |
|---|---|---|
| Snapshot blit into an input slot | 1 GPU draw | 1 GPU draw |
| Slot → module input | keyed-mutex acquire + copy into `exact_tex` (`d3d11_lift.cpp:1262-1288`) | none: exact-size slot, read after the CPU observes the fence |
| Output → ring; ring → client export | 2 copies | 2 copies |
| CPU signalling | none | 2 ring writes + 2 wakes + a CPU fence observe ≈ 20–60 µs |
| **Net** | — | **≈ +0.05 ms CPU, one fewer GPU copy**, against a 20–175 ms conversion |

### 3.8 Failure domains

| Fault in… | In-service (legacy adapter, escape hatch) | Satellite |
|---|---|---|
| hang in a stream call | the dispatcher is stuck; watchdog WARN; tasks FAILED(`hung`); thread abandoned; compositor unaffected | watchdog kills the host (exit 30) → §3.5 |
| crash, `exit()`, uncaught throw, heap corruption | **the whole service**: every app gets `XR_ERROR_INSTANCE_LOST` | the host only; streams survive |
| DLL conflict (two inference runtimes, OpenSSL pins) | process-wide, unfixable | per host |
| host dies before signalling a fence | n/a | invisible to the compositor (fence rule) |
| installer replacing provider files | service closed, all sessions lost | host closed, nothing lost |
| service crash | everything | hosts die with the job object |

---

## 4. Transport

### 4.1 Matrix

| Platform · app API | Route | Input / result | Sync | Today |
|---|---|---|---|---|
| Win · D3D11 IPC session (browser) | session IPC | NT-handle snapshot slot / NT export texture | keyed mutex on the input import; `ID3D11Fence` NT | **yes** (v3) |
| Win · D3D12 / D3D11 in-process (Unity, Unreal) | ADR-049 lift link | app texture as NT handle / NT texture + fence | D3D fence | **yes** |
| Win · OpenGL, Vulkan | generalised link | runtime-allocated NT input ring, imported via `GL_EXT_memory_object_win32` / `VK_KHR_external_memory_win32` | external semaphore on a D3D fence | P5 |
| Linux · Vulkan | session or link | opaque fd or dma-buf / exportable VK image fd | timeline semaphore fd; sync_fd | P5 |
| macOS · Metal / MoltenVK | session or link | IOSurface (zero-copy for a CoreML EP) | `MTLSharedEvent` | P5; AF_UNIX transfer unverified |
| Android · Vulkan | binder bootstrap to `:conversion`, then a socketpair | AHardwareBuffer | sync_fd | P5 |
| any · CPU (CLI, MCP, jobs) | DIAG / APP connection | shm image / inline, shm or caller handle | none | partial |

Per stream the core picks the cheapest pairing of the source memory kind and the provider's
`import_kinds`: a direct pairing, else a GPU copy, else CPU staging (one WARN, counted, raised as
`CONV_CPU_STAGING`).

**No GL, Vulkan or Metal link code lands before P5** — no input rings, no `conv_input_ring_get`, no
`oxr_conv_gpu_{gl,vk,metal}.c` — until mediaplayer is a real consumer.

### 4.2 The generalised in-process link (P5)

ADR-049's rule extends to every in-process API: one model per machine, reached through a
sessionless `APP` connection opened on a background thread, which sends `conv_hint_interest` when it
opens. Windows: the link never launches the service (the dashboard-job trap). Linux: socket
activation launches it. macOS: dial only if running. With no service, in-process sessions report
ABSENT(`no_conversion_service`) and never host a provider themselves; the headless CLI may, for
`conv probe` and descriptor checks only.

**Linux idle exit.** The socket-activated service exits 30 s after its last client, which bounds
keep-alive whatever `KeepAliveSec` says: the first lift after an idle gap cold-warms. The discovery
spec documents it.

### 4.3 Android `:conversion`

Each Android client's compositor runs in its own `:dxrN` satellite, so hosting per `:dxrN` would load
one model per app. Instead:

- The core and all providers live in **one `:conversion` bound service of the runtime APK**;
  providers ship in the APK as `libdxrc<NNN>_<id>.so` (ADR-038 shape; the prefix differs from
  `libdxrp`). `:conversion` *is* the satellite.
- It is an **exported service protected by a signature permission**, so in-process apps can bind it
  and nothing else can.
- `:dxrN` binds with `BIND_ABOVE_CLIENT` (importance inheritance unverified).
- **GPU-timeout behaviour:** a client never blocks its render on a conversion fence. If `:conversion`
  is frozen or a conversion exceeds the §5.3 watchdog, the client keeps the last good result (or the
  flat source), the task reports WARMING(`restarting`) or FAILED(`hung`), and the stream resumes
  latest-wins.
- Providers in a separate vendor APK are deferred.

---

## 5. Policy ownership, threading, watchdog

### 5.1 Policy stays in the runtime

| Policy | Stage | Code | Needs from providers |
|---|---|---|---|
| Viewpoints / app rig (ADR-048) | S3 | `resolve_viewpoints` (`d3d11_lift.cpp:988-1112`) + `u_lift_recenter`, moved unchanged | `EXPLICIT_VIEWPOINTS`, `max_views` (enforced at create and submit), the camera model |
| Convergence | S7 observer feeding S3 | rt#1978's `u_lift_autoconv` (**lands first**; P1 moves its call site, not its code) reading S6 aux depth. The observer consumes only aux depth with `same_inference = 1` (otherwise the plane is computed on a different frame than the one shown — the "depth late vs image" symptom); its spring filter, cut detection and refractory reset on `FRAME_RESET` and on a result `discontinuity` (provider rebind), not only the provider's state. The plane is handed to the provider in **decoded units** (`convergence_mode = DECODED`, the mirror of `convergence_value`), mapped from the app's [0,1] with the last result's range, so estimator and provider cannot disagree on normalisation | a given plane means "provider auto-convergence off"; `suggested_convergence` / `suggested_strength` are **ignored by default** — a vendor module's own auto-convergence recomputes every 20 frames, resets its EMA on histogram cuts and shares state across streams, and measured markedly worse than the #1978 blend rule against stereo ground truth (internal optimisation work); a hint is used only once that provider is validated on that set |
| Depth budget | S3 target, S6 measure | new `conv_policy_budget()`. **METRIC kinds:** target disparity from the content's decoded range + display geometry + viewer distance + the ADR-040 rear budget. **AFFINE kinds:** the range cannot give an absolute budget (scale unknown by definition; on a stereo ground-truth set — internal optimisation work — the true budget varies by more than an order of magnitude per shot while the module's output is nearly flat, and cheap image cues do not predict it), so the budget is the strength default × the ADR-040 rear cap, plus an optional validated `suggested_strength` hint | per-frame range; honest units; no silent per-frame min-max normalisation; `suggested_strength` reserved now while `xrt_conv_depth_info` is unfrozen |
| Cursor on lifted content (ADR-046 A1) | S7 | the relief mapping (`xrt_dp_lift.h:261-272`) becomes a generic `relief` field | relief or the camera model; else the cursor stays flat |
| Letterbox / input cap | S1 | unchanged (`u_lift_mailbox.h:283-404`) | `max_input_edge` per variant |
| Licence gating | selection | `conv_select.c` | licence class + SPDX; a pack's `commercial` = AND over model and dependencies |

Mediaplayer's stereo auto-convergence stays in-app; it is not a provider task.

### 5.2 Threads, locks, eyes

| Thread | Count | Calls the provider? | Takes |
|---|---|---|---|
| IPC / producer | per client | never | `immediate_ctx_mutex` (one blit) → `streams_mtx` |
| render | 1 | never | `render_mutex`; pins results under `streams_mtx` |
| registry | 1 | describe / warm / evict via the host iface | `registry_mtx` (leaf) |
| dispatcher, job | 1 each per host | in-service via the exec thread; satellite: ring writes only | `streams_mtx` / `jobs_mtx`, between commands only |
| exec (in-service) | 1 per provider + opt-in lanes (§3.1) | yes | none |
| supervisor | 1 | no | `hosts_mtx` (leaf) |

```
c->mutex → render_mutex → {existing leaves}                 (unchanged)
immediate_ctx_mutex → conv.streams_mtx → conv.host[i].mtx   (new chain; leaves w.r.t. render_mutex)
conv.registry_mtx, conv.jobs_mtx, conv.hosts_mtx            (independent leaves)
```

**No conversion lock is ever held across** a provider call, a ring send, a GPU / keyed-mutex /
fence wait, or a host spawn.

**One eyes snapshot per live display processor.** Today the lift thread takes `render_mutex` once
per conversion via `eyes_fn` (`comp_d3d11_service.cpp:30896`, undocumented in `d3d11_lift.h:29-34`).
Instead, each live DP (per screen or segment, multi-screen M6) publishes `{xrt_eye_positions,
timestamp, generation}` into its own seqlock, from the DP's eye listener or a timer — not only after
`process_atlas`, so eyes stay fresh while the render thread is quiesced. A stream binds to the DP
that owns its rect; the generation counter carries the #363 lifetime guarantee. With no panel DP
(lift-link clients only), the snapshot is flagged stale and S3 uses the nominal viewer, as today.

### 5.3 Refresh and watchdog

**Refresh.** `RegNotifyChangeKeyValue` on the conversion root (inotify / FSEvents on POSIX) plus a
30 s poll re-runs discovery. New rows are described (from the cache when the binary is unchanged);
removed rows → ABSENT. A changed binary on a running satellite provider is **drained**: new creates
go to a new host, and live streams move once — the host-drain case of the §2.4 rebinding rule. A
display plug-in swap (`target_instance.c:565`) re-describes `dp-lift`.

**Watchdog.** Every host-interface call is bracketed with `{call, stream/job, since_ns}` (today only
the factory and `lift_get_caps` are, `d3d11_lift.cpp:836-845, 884`). Limits: stream call max(10 s,
20 × typical latency); warm `est_warm_ms` × 3 + 60 s; job no progress for 60 s; describe 10 s. A
breach logs one WARN; satellite: kill the host (exit 30) → §3.5; in-service: FAILED(`hung`), stop
routing, abandon the exec thread.

---

## 6. Diagnostics and IPC wire

### 6.1 Status snapshot

`xrt_status_snapshot` gains **`conv_providers[]`** (≤ 8) and **per-task rows** (≤ 32), fetched by
index through a new DIAG call so each reply stays under the 1 KB DIAG limit and the
`system_get_status_snapshot` reply does not grow. The schema number **stays 1** (keys only added,
ADR-051).

| Row | Fields |
|---|---|
| provider | id, name, vendor, version, abi, binary, probe_order, hosting (`in_service` / `satellite` / `android_service`), host pid, restarts, last exit code, host state, adapter LUID, backend, VRAM used (exact in satellite mode), licence class + SPDX, research opt-in |
| task | provider index, task, state, reason, progress, `model_id`, typical latency, streams, jobs queued, selected-for-task |

New `warnings[]` codes: `CONV_CRASH_LOOP`, `CONV_RESEARCH_MODEL_ENABLED`, `CONV_TASK_UNSERVED`,
`CONV_CPU_STAGING`. `status_gen` bumps on every readiness transition. The dashboard keeps its
`lift_caps` reader for one release.

### 6.2 CLI

`displayxr-cli conv list|status|use|reset|warm|evict|enable|disable|probe|run|jobs|cancel|models`;
`lift caps|probe` stay as aliases mapping the four v3 modes.

| Verb | Path | What |
|---|---|---|
| `conv list [--json]` / `conv status [--watch]` | DIAG; headless describe as fallback | providers × tasks and selection order with reasons; readiness, streams, jobs |
| `conv use <task> <provider> [--machine]` / `conv reset` | registry | per-task preference |
| `conv warm` / `conv evict <task>` | DIAG → service | `evict` is accepted only on the CLI's own DIAG connection |
| `conv enable` / `conv disable <provider>` | registry | |
| `conv probe <file\|dir> --task … [--views N] [--job] [--pipelined --fps F]` | APP | today's `lift probe` plus stereo/depth inputs and jobs |
| `conv run <task> <in> <out> [--json]` | APP, job lane | the CLI decodes/encodes files itself; the MCP backend |
| `conv jobs` / `conv cancel <id>`; `conv models list\|verify\|prune` | DIAG; local store | |

The CLI never loads a second model instance (with no service it reports `source: headless` and checks
descriptors only), and never auto-launches the service into the dashboard's job.

### 6.3 Selftest

New check **`conversion_providers`**: enumerate, negotiate, describe, no model load. Absence never
fails. A malformed descriptor (unknown task, lanes = 0, min > max views, VIEWS_* without
`EXPLICIT_VIEWPOINTS`, unknown licence class, duplicate `cap_id`, out-of-range state) fails with new
exit **13** (`CLI_SELFTEST_BAD_CONVERSION`). `probe_lift_caps_d3d11` (`cli_query.c:258-350`, which
instantiates the vendor module on WARP) is retired; `lift_caps` / exit 11 stays and reads the
`dp-lift` row. CI registers `sim-conversion` and requires `conversion_providers.count ≥ 1`.

### 6.4 Settings, policy, environment

- Registry: `HKLM|HKCU\Software\DisplayXR\ConversionProviders\…` (`PreferredProvider\<task>`,
  `Prewarm\<task>`, `PrewarmOn`, `KeepAliveSec`, `AllowResearchOnly`, per-provider `Enabled` and
  `Hosting`). HKCU wins over HKLM unless **`HKLM\…\ConversionProviders\Policy\Lock`** is set, in
  which case HKLM wins (kiosk / IT pinning).
- Provider settings reach a provider only via `get_setting`, overridable as `DXR_CONV_<ID>_<KEY>`.
- Environment variables are `DXR_CONV_*`; `DXR_LIFT=0` is kept as an alias.

### 6.5 MCP and logging

MCP `convert_file` and `get_conversion_status` shell out to `conv run … --json` and `conv list
--json` (ADR-051 D4: the CLI is the brain); `get_status_snapshot` carries the rows for free. Provider
identity is fine here: an agent is not web content.

Logging: discovery and rejection, readiness transitions, host spawn/exit/backoff/cutoff, the
selection decision at create, watchdog breaches and CPU-staging fallbacks each log one WARN per
event; per-stream stats are INFO throttled to 10 s; nothing is logged per frame.

### 6.6 IPC wire

**v3 stays byte-for-byte.** Every `lift_*` message (`proto.json:838-960`) keeps its structs, because
the browser vendors the client side; the v3 handlers become thin adapters onto the core.

**The G1 fix.** `ipc_client_lift_get_properties` (`ipc_client_lift.c:44-70`) copies `modes` but not
`aux_outputs` / `aux_depth_semantics` (sent at `ipc_server_handler.c:8991-8992`), so
`auxDepthSupported` is always false over IPC. Two assignments, plus a round-trip test of every field.

| Message | Request → reply | Handles |
|---|---|---|
| `conv_hint_interest` | task list → – (the prewarm trigger, §2.5) | – |
| `conv_get_generation` | – → `conv_gen`, `status_gen` | – |
| `conv_enum_provider` / `conv_enum_task` | index → descriptor + readiness (DIAG, for CLI and snapshot) | – |
| `conv_stream_create` / `destroy` / `reset` | desc → id, resolved pipeline | – |
| `conv_submit` | submit (params in scratch shm when > 1 KB) → frame id | in: 0–2 images + fence |
| `conv_acquire_result` | id → views / depth meta | export handles |
| `conv_job_submit` / `status` / `fetch` / `cancel` | job → id; state + progress; result | in: image / shm; out: shm |
| `conv_warm` / `conv_evict` | task (evict: the CLI's DIAG only) | – |
| `conv_input_ring_get` | **P5 only** | GL / VK / Metal rings |
| events | per-session queue + sessionless `conv_poll_event`: `TaskStateChanged`, `ProvidersChanged`, `JobStateChanged` | – |

Auth reuses `require_lift_client` (`ipc_server_handler.c:8904-8914`). Stream limits are the selected
provider's `max_streams` per task; v3 `maxStreams` reports VIEWS_MONO's.

**Ending the field-by-field copy trap** (how G1 happened):
1. One packed wire struct per message family, in `xrt_conv_wire.h`: `uint32_t struct_size;
   uint32_t family_version;` first, fixed-width types, no embedded runtime structs.
2. One copy: the state tracker fills the wire struct; the server reads `memcpy(min(struct_size,
   sizeof))` plus zero-extension.
3. `static_assert(sizeof(msg) <= IPC_BUF_SIZE)` per message; variable data in a per-connection
   scratch shm.
4. A CI lint: every wire field is read by the server unpack or listed as reserved.

The hand-written service ↔ host structs follow the same rules.

---

## 7. App API summary

The full C is in [`conversion-providers-headers-draft.md`](conversion-providers-headers-draft.md).

### 7.1 Decision

**`XR_DXR_lift` spec 4, then spec 5; no `XR_DXR_conversion` extension** (ADR-053). The browser's GPU
process enables lift once in a pre-sandbox instance it can never re-create, and the weave-rect and
cursor_depth chains take `XrLiftStreamDXR`. A second extension would need a second weave-rect struct
and a cursor_depth v8 colliding with mac/cursor's in-flight work.

### 7.2 What each version adds

| | Spec 4 (P3) | Spec 5 (P6) |
|---|---|---|
| Model | task vocabulary (`XrLiftTaskDXR`), input kinds, a quality hint (the app's only lever) | — |
| Discovery | `xrEnumerateLiftTasksDXR` (effective per-task capability, readiness, licence class), `xrRequestLiftTaskWarmDXR` | — |
| Readiness | `XrLiftTaskStateDXR` + failure reason; `XrEventDataLiftTaskStateChangedDXR` / `…ProvidersChangedDXR`, **opt-in per session** (sent only after a v4 call) | `XrEventDataLiftJobStateChangedDXR` |
| Streams | `XrLiftTaskStreamCreateInfoDXR` (task, input kind, quality hint, `viewCount` with 0 = match the weave, `requiredOutputs`, export kind) | stereo input (`XrLiftStereoInputDXR` / `XrStereoCameraCalibrationDXR`); image+depth input; DISPARITY_STEREO, SPLATS_STEREO, VIEWS_FROM_DEPTH |
| Input | `XrLiftFrameImagesDXR` / `XrLiftImageDXR` (D3D11/D3D12 NT, VK fd/win32, dma-buf, IOSurface, AHB, CPU) with explicit sync; `XrLiftFrameControlDXR` (discontinuity) | — |
| Results | `XrLiftResultExportDXR`; `XrLiftDepthInfoDXR`; `XrLiftViewsInfoDXR` (8 entries) | `XrLiftSplatsInfoDXR` (camera block, baseline never defaulted); SPZ and RAW_IMAGE blobs |
| Jobs | — | `xrCreateLiftJobDXR` / `GetState` / `Cancel` / `ReadResult` (chunked) / `Destroy`; `XrLiftJobFileOutputDXR` |

- **No provider enumeration in the app API.** There is no `xrEnumerateLiftProvidersDXR` and no
  `providerIndex`; provider identity is visible only through the CLI and the status snapshot.
- **Reserved, no code in v1:** `XrLiftStreamStereoSourceDXR` (stereo-camera stream binding),
  SPLATS_VIDEO, INPAINT, `INPUT_ENCODED`, SEQUENCE_MANIFEST.
- **Enum values equal the ABI.** State, depth kind, measure, pixel format, views layout, licence
  class and blob format use the plug-in ABI's values (spec 4 is unpublished, so renumbering is
  free); `oxr_plugin_stub.c` static-asserts every pair. The app state enum therefore includes
  INCOMPATIBLE and UNLICENSED.
- **Eight views.** Both ceilings are 8 in v1 (`XRT_CONV_MAX_VIEWS 8`); `XRT_CONV_VIEWS_ATLAS` is
  reserved in the ABI; the runtime picks ATLAS or ARRAY when `N × w` exceeds the texture limit.
  Raising the ceiling is a later slot.
- No version bump when non-D3D transports land (P5): availability is `imageKinds` / `exportKinds`.
  Type values `1004999340–369` are reserved by a registry-only PR at the start of P3.

**Invariants.** v3 is frozen (static-assert test). Enumerated array elements never grow. Apps never
pick a provider. No vendor, model or SDK names in the header or spec. A browser must not expose
provider identity to web content.

### 7.3 The legacy mapping

| v3 request | Served by |
|---|---|
| DEPTH + VIDEO / PHOTO | DEPTH_VIDEO / DEPTH_MONO (the runtime's cut detector drives resets) |
| SBS | VIEWS_MONO, 2 views |
| NVIEW + `viewCount` | VIEWS_MONO, N views; N > maxViews is now `XR_ERROR_VALIDATION_FAILURE` |
| GAUSSIANS (PHOTO) | SPLATS_MONO on the job worker, behind a latest-wins photo facade; v3 formats only. Advertised only when a provider serves SPLATS_MONO (`dp-lift` does not) |
| `XrLiftDepthRequestDXR` | VIEWS_MONO with aux depth, advisory |

**`XrLiftPropertiesDXR` for a v3 caller:**
- **`state`**: READY when any advertised mode's backing task is READY. **ACTIVATING** while
  discovery has not settled (every registered provider described or terminal), and while any backing
  task is non-terminal — even before any mode bit is set. UNAVAILABLE otherwise. With the persisted
  describe cache the table is populated from the first query. Golden test: properties queried before
  the first describe on a cold boot ⇒ ACTIVATING.
- **`supportedModes`** = every mode whose backing task is not terminal; NVIEW only when
  `maxViews > 2`.
- `maxViews`, `maxStreams`, `backend`, `typicalLatency` come from VIEWS_MONO (else DEPTH);
  `XrLiftDepthPropertiesDXR` comes from VIEWS_MONO's aux depth, now correct over IPC.

**Four v3-observable shifts**, each listed in the spec-4 history with a golden test:
1. `maxStreams` is the selected provider's per-task limit, not runtime-wide.
2. GAUSSIANS may return SOG with the camera block where the sim returns PLY today.
3. Aux depth of kind AFFINE_DISPARITY is reported as RELATIVE + INVERSE, so `auxDepthSupported`
   stays true (and cursor_depth v6 keeps working) on non-vendor boxes.
4. The mode check at create follows declared capabilities: a mode backed by a non-terminal task is
   accepted while it warms.

### 7.4 Consumer contract (replaces ADR-042's "open default")

*A non-READY task is a reason to wait, never a reason to switch paths.* While waiting, a consumer
creates its stream or job anyway (frames wait latest-wins; jobs sit in `WAITING_FOR_PROVIDER`),
polls at ≤ 2 Hz, or takes the state event, and shows the 2D source. It must not cache a non-READY
answer beyond one poll interval, and must not run an in-app converter as a fallback.

Create results: `FEATURE_UNSUPPORTED` only for a task with no non-terminal candidate, or an unmet
`requiredOutputs` from a READY provider; `VALIDATION_FAILURE` for `viewCount` > maxViews, an input or
export kind offered by no candidate, or a reserved task; `LIMIT_REACHED` per the provider's
`max_streams`. ADR-042 and ADR-049 get amendments; ADR-053 records them.

### 7.5 Weave-rect path

**Byte-identical:** `XrWeaveRectLiftDXR`, `XrWeaveSubmitLiftRectsDXR`, the ≤ 8 rect limit, the IPC
ordering, the snapshot cap and letterbox crop, "flat until first result", and the cursor chain.

**Changes:**
1. The stream must be a VIEWS_MONO stream of the same connection (a legacy SBS/NVIEW stream is one);
   any other task is refused per rect, non-fatally.
2. A provider without explicit-viewpoint support is served through the composed path: its depth plus
   the reference view synthesis.
3. `viewCount = 0` = match the weave; the service resizes the stream on a 2 → 4 mode switch without a
   new handle (view *v* → tile *v*).
4. `XrLiftFrameControlDXR` may chain on a rect to signal a seek.
5. `XrLiftOptionsDXR::convergence = AUTO` now means runtime auto-convergence (rt#1978).

---

## 8. Consumer migration

### 8.1 DisplayXR Browser (`displayxr-browser-pvt`) + web SDK (`displayxr-web`)

The browser becomes a thin client and `native.js` the SDK's only provider. **Pages never see provider
identity**: the browser exposes only a coarsened `waiting | ready | unavailable` state
(`dxrliftstatechange`), with no provider, model, latency or licence strings.

**The three fallback fixes stand, in this order (P3):**

| # | Cause | Fix | ≈ lines |
|---|---|---|---|
| 1 | the SDK's per-document caps cache; `entry.js convert()` ignores `{native}` | delete the cache; 0330's mojom gains `OnLiftTaskStateChanged`, forwarded to the lift world; not READY = 2D + a spinner | ~120 |
| 2 | 0340 retries *without* `XR_DXR_lift` after 6 × 250 ms | decide "runtime lacks lift" with `xrEnumerateInstanceExtensionProperties`; keep lift in every attempt; backoff to ~10 s | ~40 |
| 3 | `LiftAvailable()` never re-polls UNAVAILABLE (0329) | re-poll at 2 Hz while not READY; from P4 also take `TaskStateChanged` | ~60 |

**Release cadence makes the fixes land late and unevenly.** Browser releases are near-daily on
Chromium rebases and the update feed is tagged by hand, so old browsers stay in the field for weeks.
Therefore: the P3 "wait for READY" **times out to the in-page path** when the runtime is spec 3 or
older or reports no provider at all; the P4 deletion gates on the runtime floor in `consumer_floors`,
not on the calendar; and every browser patch PR compiles on every lane (Windows + Android) before
merge, since a patch-gate pass only proves the patch applies.

**Header adoption.** The browser keeps the spec-3 header and runtime detection until its patches need
spec-4 behaviour (P4). It then vendors the spec-4 header and raises its minimum runtime in the same
change (`consumer_floors`). Merge order: runtime → extensions auto-sync → browser.

**Delete at P4** (≈ −1.4 k browser C++, −0.4 k installer/scripts, −6.1 k SDK JS): the 0324 model store
and ORT loader parts; `installer/models.json`, `fetch-models.ps1`, `verify-model-store.ps1` and the
NSIS models component; the SDK's ORT providers, `models.json`, `live-dibr.js` + shaders (after its
hole-fill is ported, §2.6), `gen/`, `stubs/` and most of `lift.js`; 0327 once explore uses the page's
single session. 0326 (taint exemption) stays until stills are snapshotted GPU-side.

**Keep:** the native client patches (0328/0329/0333/0336/0338–0341/0400), the simplified menu, and
explore / orbit / SOG export.

**Add:**
- **Stills and splats via jobs.** 0330's `ConvertDepth` / `ConvertGaussians` call
  `xrCreateLiftJobDXR` with the decoded RGBA8, downscaled to `maxInputExtent`. `/lift/depth` returns
  `XrLiftDepthInfoDXR` (ending the pseudo-disparity flip); `/lift/gaussians` returns
  `XrLiftSplatsInfoDXR` (no 60° HFOV assumption).
- Calls: `js/call/lift.js` (323 → ~80 lines) sets `dxr-lift="auto"` per mono tile; the ORT session
  rules become plain stream limits.
- Canvas lift via the canvas clause in `ShouldOfferConvert3D` (needs a panel damage-cost check).
- Android: 0329 snapshots into an AHardwareBuffer gated on `imageKinds`; hidden until P5.

**Exit:** a service-log lift stream per lifted element; no `onnxruntime` / `models/` request in a
`--log-file`; after a cold boot with the provider WARMING, a lifted element turns 3D by itself.
**Named failure to test against:** the browser queried caps before the module was READY and latched
in-page conversion for the whole session (seen live on the panel 2026-10-12; quality visibly worse, no
stream in the service log). The consumer rule "never cache a non-READY answer beyond one poll
interval" (§7.4) is the fix, and the cold-boot smoke test is its regression test.

### 8.2 Other consumers

| Consumer | Plan | ≈ lines |
|---|---|---|
| demo-mediaplayer (Vulkan, four OSes) | stereo auto-convergence stays in-app. Mono→3D via `LiftClient`: VIEWS_MONO, `viewCount = 0`, the decoded VkImage as the platform's kind with timeline / sync-file sync, discontinuity on seek; hidden until P5. LIF via VIEWS_FROM_DEPTH at P6 | +450, +120 |
| demo-gaussiansplat | SPLATS_MONO job (SPLATS_STEREO with a typed baseline); `stb_image` pixels downscaled to `maxInputExtent`; `preferredFormat = SOG` so `gs_sog_loader` consumes it unchanged; relative-scene heuristic when `metricSource == NONE`; progress, Cancel, "Save .sog" via a caller handle | +350 |
| Unity | VIEWS_MONO, 2 views, aux depth required (provisional outputs make this safe during warm-up); double-buffered `D3D12_NT` input with D3D fence sync (closes ADR-049's submit-fence follow-up); C# `DisplayXRLiftSource` | +600, +350 |
| Unreal | follows Unity; SBS feeds `SideBySideCoords`; adaptive N-view uses `viewCount = 0` | +500 |
| MCP | `convert_file`, `get_conversion_status`, CLI-backed | +150 |
| CLI | §6.2; the depth PNG keeps "near = dark", now derived from the depth kind | — |
| XRInteractiveVideoPlayer, LeiaPlayerWin | vendor-repo decisions: SPLATS_MONO jobs; VIEWS_MONO with `qualityHint` once ported | — |

Shell window lift ("lift this window" as a workspace-controller action over the WGC capture) is
parked until after P6; the lift API does not change.

---

## 9. Phases

### P0 — Docs

ADR-053; `conversion-provider-discovery.md`; this roadmap; the headers draft; ADR-042 / ADR-049
amendment stubs. **Exit:** owner sign-off on §11.1 (a)–(e).

### P1a — Core behind the existing lift thread

| Deliverable | Files | ≈ lines |
|---|---|---|
| Provider header (negotiate, `describe_cap`, vtable, frozen embedded structs, `sizeof` asserts) | `xrt_conversion_plugin.h` | +700 |
| Packed wire structs | `xrt_conv_wire.h` | +350 |
| Loader (registry / POSIX `NNN-<id>-conversion-provider.json`, ABI gate); the DP loader skips the suffix (`target_plugin_loader.c:2511-2517`) | `target_conversion_plugin_loader.c` | +900 |
| Core: registry + describe cache, select, readiness, stream, pipeline, job, stage, policy, host iface, in-service shim | `src/xrt/conversion/` | +3,200 |
| `dp-lift` with static describe, wrapping the **same** `d3d11_lift` DP calls; no file split; the pre-core path is a branch inside the one lift thread | `conv_provider_legacy_dp.cpp`, `d3d11_lift.cpp` | +450; ±300 |
| Per-DP eyes snapshot; `svc_lift` → core lookup; weave consumption untouched | `comp_d3d11_service.cpp` | ±150 |
| Reference view synthesis (ported hole-fill) | `d3d11_conv_viewsyn.cpp` | +500 |
| Sim provider `DisplayXR-SimConversion` (every task, scriptable states, honours viewpoints); `SIM_DISPLAY_FAKE_LIFT` alias for one release | `src/xrt/drivers/sim_conversion/` | ~700 moved |
| v3 handlers re-pointed at the core (same wire); G1; `conv_hint_interest` | `ipc_server_handler.c`, `ipc_client_lift.c`, `oxr_lift.c` | ±250 |
| Selftest (exit 13); CLI `conv`; snapshot rows + DIAG; dashboard Components page | `cli_query.c`, `cli_cmd_conv.cpp`, `u_status_snapshot.c`, `Components.cs` | +1,100 |
| Gates: state and enum static_asserts, `downstream-pins.json` surface, `check_plugin_abi.py`, CI sim registration, `register_dev_plugin.bat conv` | various | small |

**Tests:** `tests_conv_select` (preference, licence, ties, any-FAILED fall-through, never reorders on
readiness, ABSENT(policy), FAILED(RESOURCES)); `tests_conv_readiness`; `tests_conv_wire`;
`tests_conversion_abi` (frozen-struct `sizeof`, `describe_cap`); `tests_lift_abi` (every v3 `sizeof` /
`offsetof` / enum, x64 + arm64); the v3 startup sequence pinned **before** refactoring; the legacy
golden replay incl. cold-boot ACTIVATING, "properties before the first describe ⇒ ACTIVATING" and
"splats downloading + SBS READY → READY"; lane isolation (a 3 s sim job costs a VIEWS_MONO stream
≤ 10 %); view synthesis vs `live-dibr.js` on the panel corpus; selftest with zero / sim / malformed
providers. **Golden set for the convergence / budget stages and the composed-views acceptance bar:** a stereo
ground-truth disparity set (internal optimisation work; positive = in front), alongside the real-media
lift corpus.

**Exit:** browser path unchanged on the panel (bperf + the lift panel recipe, A/B against the
previous release); all six §10 bugs closed; `conv list` shows `dp-lift` + sim in order.

**Risks:** destabilising the shipping path (medium / high) → v3 wire and weave consumption untouched,
golden replay, panel A/B; collision with rt#1978 → land it first, move its call site verbatim.

### P1b — File split and deletions (one release later)

Split `d3d11_lift.cpp` (2830 L): GPU glue → `d3d11_conv_gpu.cpp`; thread, scheduler and activation
code (`:442-1528`) into the core; delete `lift_activate`, `lift_poll_caps`, `lift_convert_one`,
`lift_copy_depth` and the pre-core branch; `d3d11_lift.h` loses `lift_factory` / `fallback_factory` /
`eyes_fn`. ≈ −1,700 / +600. There is no `DXR_CONV_CORE=0` dual path. **Exit:** P1a's tests and panel
A/B unchanged.

### P2 — Minimal conversion-host satellite (Windows)

| Deliverable | Files | ≈ lines |
|---|---|---|
| Host binary (loop, hidden RM window, D3D11 NT-handle registration + fences, in-host minidump, idle exit). The host opens its D3D device on the **adapter LUID the service passes on the command line** (the service's render adapter), never the default adapter: on a hybrid box the service renders on the dGPU and weaves on the iGPU, and a cross-adapter shared texture can fail silently (a cross-family copy is dropped with no error) | `src/xrt/targets/conversion_host/` | +1,600 |
| Host proxy over pipe + rings; CPU-observed fence completion; handle pulling | `conv_host_proxy.c` | +900 |
| Supervisor: lazy spawn, job object, backoff, exit codes, watchdog kill, maintenance-mutex check, respawn rules | `conv_supervisor.c`, `os_job_object_win.c` | +700 |
| Hand-written host protocol structs; shm rings | `conv_host_wire.h`, `u_shm_ring.{c,h}` | +550 |
| Orchestrator shares spawn helpers; hosts excluded from the controller watchdog; installer ships the host | `service_orchestrator.c`, `installer/*.nsi` | ±190 |
| Docs: `service-architecture.md` rows | docs | — |

**The test vehicle is the sim provider's D3D11 NT-handle + fence path in the satellite.** The legacy
adapter is not in it: it stays in the service, unchanged.

**Tests:** supervisor FSM (pure); rings; CI kill tests with the sim provider — kill mid-stream (the
stream keeps its handle and recovers) and kill between submit and signal (compositor frame rate
unaffected); a fence-value test under the D3D11 debug layer; WARP shared-resource coverage per §11.2.

**Exit:** the sim provider in a host produces the same output as in-service; kill tests green;
`dp-lift` output unchanged from P1; **the per-frame cost of the satellite hop is measured and
budgeted**: today the vendor module shares the service device in-process, so the satellite adds an
IPC round trip, an NT-handle open and possibly one copy per video frame. Measure it on the hybrid
box with the weave-on-scanout split engaged against the in-process number (typical lift latency
today is about 22 ms end to end) and write the budget into §3.7 before P3 starts.

**Risks:** cross-process GPU sync bugs → a fence value per record, debug layer in CI; WARP lacking
shared fences → those cases run on hardware only.

### P3 — Lift spec 4 + the vendor's conversion provider

| Deliverable | Where |
|---|---|
| Registry-only PR claiming `1004999340–369` | `openxr_includes/openxr/README.md` |
| `XR_DXR_lift.h` spec 4; `oxr_lift.c` → `oxr_conv.c` + `oxr_conv_gpu_d3d.c`; the IPC `conv_*` family | runtime |
| The spec reorganised as one extension; `index.json` note; the v3-observable shifts in its history | `XR_DXR_lift.md`, `index.json` |
| The vendor conversion provider DLL: separate from the display DLL, `Supersedes = dp-lift`, ProbeOrder 50, **satellite-hosted from day one** | `displayxr-leia-plugin` |
| **Installer contract:** "close the host, never the service", holding `Global\DisplayXR-ConvMaintenance-<id>` across the RM session; onboarding guide §9 + discovery spec | runtime docs, vendor installer |
| The vendor provider's **inference-runtime version gate** (today the display plug-in refuses a module older than its floor) reports `FAILED(VERSION)`, never `FAILED(MODEL)`, so the dashboard says "too old", not "missing"; extended entry points (off-axis convert) stay version-gated inside the provider | `displayxr-leia-plugin` |
| Per-request geometry stays runtime→provider: display dims in metres, resolved viewpoints and the off-axis parameters go in on every call (`xrt_conv_params`), the relief mapping comes back out (`xrt_conv_depth_info.relief_*`); the display plug-in supplies only display geometry to the runtime, as today | ABI (already in the draft); confirm in the provider's tests |
| Onboarding guide §9 "write a conversion provider" + a sibling `displayxr-conversion-template` repo (model vendors are not display vendors and should not read the weaver half) | runtime docs, new public repo |
| Browser fallback fixes 1–3, on the spec-3 header | `displayxr-browser-pvt`, `displayxr-web` |

**Tests:** create rules; task events ordered and deduplicated, the opt-in latch for v3-only sessions,
`ProvidersChanged` on a simulated install, UNAVAILABLE → READY without a restart; transports on WARP
(D3D11_NT, D3D12_NT + fence, CPU); the depth-kind contract; browser cold-boot smoke (sim WARMING 5 s
→ a lifted `<video>` turns 3D with no user action); an RM pass over the vendor files with the service
running.

**Exit:** the vendor provider serves the browser through the satellite with `dp-lift` hidden; a vendor
update on a live box loses no session (WARMING(`updating`) → READY); ABI gate and repin tracks green.
This is where the file-lock saga ends. **The common external case is "vendor module absent":** the
vendor module ships from a separate, internal installer and is in neither the bundle nor the display
plug-in installer, so for every external user the vendor provider's tasks are `FAILED(MODEL)` and the
fall-through to the open provider (P4) is the main path, not an edge. `Supersedes = dp-lift` hides
only the legacy row; it never affects selection among the other providers.

**Risks:** the type-value block taken mid-flight → renumber (spec 4 is unreleased); coupled-PR merge
order → runtime → extensions → browser; the weaving DP mapping the conversion runtime into the
service (§11.2) → RM would still list the service.

### P4 — Open-model provider → browser + SDK delete

| Deliverable | Where |
|---|---|
| The open provider (ONNX Runtime; DirectML on Windows), model packs (manifest schema 2 with `defaults`, no schema-1 compatibility block), ProbeOrder 200 | sibling public repo; `versions.json`; `downstream-pins.json` track |
| Runtime-owned model store: root resolved **in this order**: `HKLM\Software\DisplayXR\Models\Path`, then the browser's `HKLM\Software\DisplayXR\Browser\ModelsPath`, then `%ProgramData%\DisplayXR\Models`; the browser installer writes **both** keys until it stops shipping models, or a `/MODELSPATH=` override silently splits the store. The browser's layout and write protocol; the host downloads; GC manual (`conv models prune`). The host **verifies sha256 on first load per process** (inside warm-up, cached by file id + mtime + size, about 1–2 s for a 700 MB blob) because the browser's read check is stamp + size with no re-hash and the store inherits `%ProgramData%`'s default ACL, under which any local user can plant a same-size blob plus its `.ok` stamp; tightening the ACL waits until the browser no longer writes the store in user context. Installer pre-fetch runs as a SYSTEM writer and **skips, never retries**, a stamped blob that is mapped by a running process | runtime installer, host |
| **Blocker before adoption: the browser uninstaller wipes the store.** `DisplayXRBrowserInstaller.nsi` runs `RMDir /r "<root>\blobs"`; once the runtime adopts the folder, uninstalling the browser deletes every runtime model. The browser fix (never delete when `HKLM\Software\DisplayXR\Models` exists, or an owner marker) ships **before or with** adoption, and the runtime still treats a wiped store as `DOWNLOADING`, never as a latched failure, because older browser installers in the field can still wipe it | browser-pvt, runtime |
| Website page "conversion providers": what the runtime does for apps, the open defaults and their licences, how a commercial provider is installed, the readiness rules; the open-provider repo joins the ecosystem list through the site's org sync | `displayxr-website` (`/sync-website`) |
| Bundle-installed provider, models on demand (§11.1 c) | `displayxr-installer` |
| Browser vendors the spec-4 header, raises its minimum runtime, deletes per §8.1 | browser-pvt, displayxr-web |

**Tests:** the write protocol with the sim fake pack via `file://`; a tampered same-size blob is
rejected; research-only gating; no `onnxruntime` / `models/` requests in a browser `--log-file`; SDK
`native.js` unit tests (no cache, event-driven).

**Exit:** §8.1's exit criteria; the installer carries no models component.

**Risks:** prewarm VRAM on laptops → `PrewarmOn = first-create` on battery, VRAM in the dashboard.

### P5 — Non-Windows transport + in-process links + Android

Vulkan GPU glue + view synthesis; the in-process link and input rings for GL / VK / Metal; the POSIX
host, Linux socket activation (idle exit documented), macOS IOSurface + `MTLSharedEvent`; Android
`:conversion` as an exported, signature-protected service with the `libdxrc` loader (a
prefix-parametrised parse, `target_plugin_loader.c:1850-1880`). **Tests:** lavapipe OPAQUE_FD and
DMABUF + SYNC_FILE; CPU everywhere; mediaplayer headless against the sim. **Exit:** mediaplayer
mono→3D on Linux and macOS; the Android browser menu shows with `:conversion` READY. **Risks:**
IOSurface over AF_UNIX; Android importance and freezing (device-only validation).

### P6 — Lift spec 5 + job consumers

Spec 5 (jobs, `XrLiftSplatsInfoDXR` with SPZ + camera block, stereo + depth input; DISPARITY_STEREO,
SPLATS_STEREO, VIEWS_FROM_DEPTH); mediaplayer, gaussiansplat, Unity then Unreal, MCP. **Tests:** job
progress monotonic, cancel within 1 s, destroy = cancel, a 300 MB PLY read in chunks, teardown reaps
jobs; the splat camera block equals the SOG `meta.json`; per-consumer smoke tests. **Exit:** each
consumer's smoke test green; no consumer runs an in-app converter.

### Cut from v1

Dropped without harming the architecture (values reserved where noted, no code):
1. the legacy splats path (`lift_convert_blob`);
2. VRAM budget admission and LRU pressure eviction — v1 keeps only "never evict while bound" and
   "idle-evict job-only models after 600 s";
3. the per-user result cache;
4. cross-adapter host mode;
5. `xrEnumerateLiftProvidersDXR` and `providerIndex` in the app API;
6. stereo-camera stream binding, SPLATS_VIDEO, INPAINT, `INPUT_ENCODED`, SEQUENCE_MANIFEST (reserved);
7. the model-pack schema-1 compatibility block (`defaults` kept);
8. a second proto-generator spec (hand-written host protocol structs instead);
9. GL / VK / Metal in-process links before P5;
10. `exclusive_group`;
11. the `DXR_CONV_CORE=0` dual path (replaced by P1a/P1b).

---

## 10. Known bugs fixed in passing (P1)

| # | Bug | Today (file:line) | Fix |
|---|---|---|---|
| G1 | Aux-depth caps dropped over IPC: `auxDepthSupported` is always false through IPC and the lift link | `ipc_client_lift.c:44-70` (the server sends them, `ipc_server_handler.c:8991-8992`; read at `oxr_lift.c:458-466`) | copy both fields; a round-trip test of every field; the `xrt_conv_wire.h` one-copy rule |
| G2 | `maxViews` never enforced: oxr checks only the v3 constant 8 | `oxr_lift.c:296`, `oxr_weave.c:925` | validate against the task's `max_views`, at create and at submit |
| G3 | Mode not checked while ACTIVATING: the stream is accepted, then fails forever while its handle stays valid | `d3d11_lift.cpp:1251-1259`, `:1467-1475`, `:1758-1765` | validate against the declared (describe-time) capabilities |
| G5 | Activation latched: no module at first activation ⇒ UNAVAILABLE until service restart; the factory is captured once | `d3d11_lift.cpp:847-858`, `:1564-1565` | the readiness state machine with retry triggers; `dp-lift` re-resolves the factory on each retry |
| G8 | Splats (GAUSSIANS) run on the stream thread, stalling every live stream | `d3d11_lift.cpp:2725-2784` (inside `lift_thread_main :1430-1528`) | the job lane, with its own thread per host |
| G7 | `render_mutex` taken per conversion, via `eyes_fn` | `comp_d3d11_service.cpp:30896`; undocumented in `d3d11_lift.h:29-34` | the eyes seqlock; conversion never takes `render_mutex` (ADR-053) |

Also closed in passing:
- the lift-only factory and its fallback coming from different plug-ins
  (`comp_d3d11_service.cpp:35798-35799`);
- the CLI's second module instance on WARP (`cli_query.c:288-322`);
- the stall watchdog's blindness to convert calls (`d3d11_lift.cpp:836-845, 884`);
- the sim fake's rejection of shorter caller structs, and its ignored viewpoints
  (`sim_display_lift_d3d11.cpp:232, 350`).

---

## 11. Open questions and must-verify items

### 11.1 Decided by the owner 2026-10-12 (recommendations accepted as written)

| | Question | Decision |
|---|---|---|
| (a) | Ship the satellite host before any out-of-tree provider? | **Yes, in P2**, proven on the sim provider with CI kill tests. The legacy adapter stays in the service; the file-lock saga ends in P3, when the vendor provider DLL is satellite-hosted from day one. This also keeps two inference runtimes out of one process before P4. |
| (b) | Lift v4/v5, or a new `XR_DXR_conversion`? | **Lift spec 4, then 5** (ADR-053), pending veto. |
| (c) | Open provider packaging | **A sibling public repo; the provider DLL bundle-installed unconditionally; models a separate installer component, checked by default, that pre-fetches only the default pack.** Default pack = the small streaming video-depth model (Apache-2.0, ~58 MB), the small still-depth model (Apache-2.0, ~50 MB) and the light inpainter (MIT, ~5 MB): about 113 MB, covering video (the main lift use; its temporal cache is what keeps video from flickering), stills at low quality, and hole fill. The large metric still model (MIT, ~0.7 GB per size) is on demand with a `DOWNLOADING` state, or an unchecked installer option. Only the *small* variants of the depth families are Apache; base/large are CC-BY-NC. The large mono-depth model's "Apache-2.0" in the browser manifest was never independently checked: re-verify upstream before any pack ships it. Signed installers never pre-fetch a non-commercial blob, and any pre-fetch runs as a SYSTEM writer. |
| (d) | Android providers | **In the runtime APK**, as `libdxrc…`, in one exported, signature-protected `:conversion` service; vendor-separate APKs deferred. |
| (e) | Licence wording | A provider declares the licence class of its **inference runtime and models together**: the AND covers the runtime, not only the model files (a vendor module whose detail string reads "internal use only" cannot be advertised as COMMERCIAL by the provider that loads it; the vendor's own installer may set the opt-in for an internal build, since it is not a signed DisplayXR installer). `AllowResearchOnly` gates RESEARCH_ONLY and UNKNOWN; signed DisplayXR installers never write it. The legacy `dp-lift` adapter is **grandfathered**: it reports UNKNOWN with the module's own licence string shown in the dashboard, and is exempt from the gate because the user installed that module before the gate existed. A pack's `commercial` = AND over the model and its dependencies. |

Decided as stated unless the owner objects: `requiredOutputs` is checked against declared (possibly
provisional) outputs and is otherwise strict — revisit with data; progress events are rate-limited
(2 Hz / 5 %); a stereo-camera convergence hint for holocall, if wanted, belongs on
`XR_DXR_stereo_camera`, not on lift (parked); vendors are advised to split video and photo models
into separate DLLs where one worker cannot run both lanes.

### 11.2 Must verify before coding

- [ ] WARP shared NT-handle textures + shared fences in CI
- [ ] a shared NT-handle texture opened on the wrong adapter on the hybrid box fails loudly (not a silent drop)
- [ ] the browser uninstaller's `RMDir /r blobs` and which browser versions in the field still run it
- [ ] the store's effective ACL on an installed box (inherited `%ProgramData%` default vs anything the browser installer set)
- [ ] the vendor lift factory without probe (stays in-service, so only for the adapter's static describe)
- [ ] whether any weaving-DP path maps the conversion runtime into the service (decides whether RmGetList ever lists the service after P3)
- [ ] the `conv_hint_interest` plumbing
- [ ] the current v3 startup sequence pinned by a test
- [ ] IPC handle capacity per message and the snapshot reply size
- [ ] the browser GPU process's integrity level / AppContainer status
- [ ] measured behaviour of a queue wait on a fence whose signaller died
- [ ] whether the open provider's inference runtime can write into an externally allocated resource
- [ ] the model-store write protocol run by the host under the ACL
- [ ] exit code 13 and type values 340–369 still free at merge
- [ ] measured VRAM and warm-up of one vendor model and one open depth model
- [ ] whether the Android APK ships armeabi-v7a

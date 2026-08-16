---
status: Active
owner: David Fattal
updated: 2026-08-16
anchors: main @ c961d4d85 (comp_d3d11_service.cpp = 18,145 L; ipc_server_handler.c = 6,755 L)
code-paths: [src/xrt/targets/service/, src/xrt/ipc/, src/xrt/compositor/d3d11_service/, src/xrt/compositor/multi/, src/xrt/targets/common/]
---
# The DisplayXR service — architecture map (as built)

This is the evidence-based map of `displayxr-service` **as it exists**, produced by the
2026-08-16 architecture review (Phase 1 of the concurrent-IPC pass, #939/#943). Every
claim carries a `file:line` anchor at the commit above; where a claim is reasoned
rather than read it is marked **INFERRED**. It supersedes the service half of
[in-process-vs-service.md](in-process-vs-service.md) and the topology half of
[multi-compositor.md](multi-compositor.md), both of which predate the plug-in
extraction and the #925 stabilisation. The *target* architecture is
[ADR-035](../adr/ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md);
the phased implementation plan is epic #974 (#950–#973).

Companion references: [workspace-stability.md](../reference/workspace-stability.md)
(the wedge family + `[RENDER]` diagnostics), the
[#925 S2 site audit](https://github.com/DisplayXR/displayxr-runtime/issues/925),
[production-components.md](production-components.md) (what ships).

> **Phase-1 update (2026-08-16).** This map is a snapshot at `main @ c961d4d85`; the
> `file:line` anchors are pinned to that commit. Since then, Phase 1 of the [ADR-035]
> plan (epic #974) has **landed on `main`** and closed a number of the §9 defects — read
> those §9 items as "the problem that was fixed":
> - **#954** server-derived peer identity (`GetNamedPipeClientProcessId` / `SO_PEERCRED` /
>   `LOCAL_PEERPID`) — the gates no longer trust the client-sent pid (§9-1).
> - **#955** the 13 controller-only `workspace_*` mutators are now authorized (§9-2, part of §4.2).
> - **#956** no blocking pipe I/O under `global_state.lock`; bounded client allocations;
>   swapchain/semaphore id bounds; capped event queue (§9-4, §9-6).
> - **#957** the window handlers resolve id→xc under the lock and call the compositor
>   out-of-lock (part of §9-4; the nesting is latency, not deadlock — the compositor never
>   takes `global_state.lock`; the `ics->xc` read is pointer-identity, currently safe).
> - **#958** provider `get_presence` runs on a background poll thread, never a client
>   thread; qwerty integrator is locked (§9-15).
> - **#959** `IPC_MAX_CLIENTS` is now **32** (was 8), with a runtime-admitted cap read from
>   box specs / `DXR_MAX_CLIENTS` and a reserved controller slot; refusal is a clean `[CAP]`
>   log (§2.3, §9-17). The ~21 MB/client shm slim stays an Android follow-up.
> - Phase-0 forensics/telemetry also landed: the `[EXIT]`/`[TERMINATE]` tripwires (#950),
>   the `[HEALTH]` per-client line (#951), the plugin dev-path guard (#952), the bridge
>   exit-on-loss (#953), and the #975 trampoline crash fix.
>
> Still open / next: the arbitration contract (client classes #960, panel lease #961 — the
> core of #939), the one-pipeline work (#964), and the noted per-slice follow-ups. The DP
> stays in-process (fault-contained); #943's exit source waits for the armed tripwire.

**One-paragraph summary.** The service is a single-process, no-isolation host. `WinMain`
starts a tray/message-pump thread, an orchestrator that spawns two children (workspace
controller, WebXR bridge) with **no job object**, and then blocks forever in
`ipc_server_main` — a 20 Hz poll loop on the main thread. Everything else runs *in* that
process: the vendor DP plug-in DLL, every input-provider DLL, the D3D11 service compositor
with its window and render threads, and one IPC thread per client (≤ 8). The IPC core is
essentially stock Monado: one thread per client, one global mutex, a fixed 8-slot array,
blocking synchronous pipes with no timeouts, no liveness model, no client classes, and
caller identity that the client asserts itself. The compositor has **two structurally
different modes selected by one process-global bool** (`workspace_mode`): the
workspace/multi-compositor path (service window, render thread, one shared DP — the
healthy, #925-hardened path) and the standalone per-client path (per-client window +
swap chain + **DP**, presenting on the client's IPC thread — written single-tenant). Every
arbitration rule that exists lives inside `comp_d3d11_service.cpp` and therefore reaches
Windows only.

---

## 1. Process topology

```
                    HKLM\...\Run  ──► displayxr-service.exe --autostart   (main.c:112-128)
                    client connect-ladder ──► displayxr-service.exe (no args)   (ipc_client_connection.c:162-262)
                                     │
   ┌─────────────────────────────────┴───────────────────────────────────────────────────┐
   │ displayxr-service.exe  (ONE process, ONE heap)                                       │
   │  main thread ─ ipc_server_main → main_loop (20 Hz)            ipc_server_process.c:537│
   │  tray thread ─ message pump + WH_KEYBOARD_LL Ctrl+Space hook   service_tray_win.c:514 │
   │  trampoline thread ─ 127.0.0.1:9014 select() loop              service_orchestrator.c:634│
   │  watchdog threads ─ WaitForSingleObject(child, INFINITE) ×2   :418, :510            │
   │  per-client IPC threads ×N (≤ IPC_MAX_CLIENTS 8)               ipc_server_process.c:1029│
   │  D3D11 window thread (service-owned HWND)                      comp_d3d11_window.cpp:1650│
   │  capture/render thread (workspace mode only)                   comp_d3d11_service.cpp:6186│
   │  window-op worker (detached, never joined)                     comp_d3d11_service.cpp:1398│
   │  WinRT capture pool threads (2D window capture)               d3d11_capture.cpp:285   │
   │  IN-PROCESS DLLs: vendor DP (leaked HMODULE), input providers (leaked), LeapC.dll     │
   └──┬──────────────────────┬────────────────────────────────────────────────────────────┘
      │ CreateProcessA        │ CreateProcessA (on first :9014 accept)
      ▼                       ▼
   displayxr-shell.exe     displayxr-webxr-bridge.exe
   --service-managed       (2 IPC connections, headless)
   (privileged IPC client)     ▲ WebSocket :9014
      │ CreateProcessA          │
      ▼                     Chrome extension
   apps (DISPLAYXR_WORKSPACE_SESSION=1)     Chrome WebXR (AppContainer → IPC)     displayxr-browser GPU proc (Low IL → IPC)
```

### 1.1 Who starts the service, who starts whom

| Edge | Mechanism | Anchor |
|---|---|---|
| Logon → service | `HKLM\...\Run` value, `--autostart`; exits immediately if `start_on_login=false` | `installer/DisplayXRInstaller.nsi:873`; `main.c:125-128` (`ExitProcess(0)`) |
| Any IPC client → service | pipe missing ⇒ `CreateProcessA(displayxr-service.exe)` → `service\` sibling → `ShellExecuteExA` (works from AppContainer). **No arguments** — workspace mode is entered later by `workspace_activate`, never by launch | `ipc_client_connection.c:195-262` |
| Singleton | the named pipe with `FILE_FLAG_FIRST_PIPE_INSTANCE`; the pidfile path is a libbsd-only no-op on Windows | `ipc_server_mainloop_windows.cpp:153-155,184-198`; `u_process.c:86-91` |
| Service → shell | `WH_KEYBOARD_LL` hook on the **tray** thread → `PostMessage(WM_ORCHESTRATOR_SPAWN_WORKSPACE)` → `spawn_workspace()` → `CreateProcessA("<binary> --service-managed", CREATE_NO_WINDOW, inherited env)`. Hook is **uninstalled while the shell runs** (#344) and re-armed by the watchdog on exit | `service_orchestrator.c:700-779, 388, 267, 409-412, 358-362` |
| Service → bridge | trampoline binds `127.0.0.1:9014`, on first `accept()` closes the listener and `spawn_bridge()`; bind failure = "bridge already running externally", WARN only | `service_orchestrator.c:527-638` |
| Shell → apps | `CreateProcessA` with `DISPLAYXR_WORKSPACE_SESSION=1`, `CREATE_NEW_CONSOLE`, **no job object**; the shell reaps them only on its own orderly exit (`request_client_exit` → 1 s → `TerminateProcess`) | `displayxr-shell-pvt/src/main.c:3519-3599, 7106-7145` |
| Shell → voice | `CreateProcessA(displayxr-voice.exe)`, MCP client only, never touches the service | `displayxr-shell-pvt/src/shell_voice.c:652-767` |
| Tray → control panel | `ShellExecuteW`, untracked | `service_tray_win.c:227` |
| MCP adapter | **not spawned by anyone here and not in the service** — the MCP server runs inside every *app* at `xrCreateInstance`; the shell hosts the workspace endpoint; the service hosts none | `oxr_instance.c:651-654`; `main.c:159-162, 228-231` |

### 1.2 Supervision — what happens when each process dies

| Process dies | Who notices | What happens | Backoff / crash-loop detection |
|---|---|---|---|
| **service** | nobody supervises it. Children keep running (no job object anywhere: `grep JobObject src/` = 0) | shell: `xrPollEvent` → `XR_ERROR_INSTANCE_LOST` → exits cleanly (`shell-pvt src/shell_openxr.cpp:519-528`, `main.c:6541`). **bridge: never exits** — spins on `XR_ERROR_INSTANCE_LOST` with `Sleep(100)` forever, still bound to :9014 (`webxr_bridge/main.cpp:2084-2086`) → a restarted service cannot re-arm its trampoline. Apps: `XR_ERROR_INSTANCE_LOST` on the next call, no `XrEventDataInstanceLossPending` (`oxr_xret.h:22-32`); no reconnect exists anywhere in `ipc_client_*` | n/a |
| shell (ENABLE) | `workspace_watch_thread_func` | immediate relaunch, recursive watchdog | **none** — a crash-on-launch shell spins (`service_orchestrator.c:365-372`) |
| shell (AUTO, default) | same | re-install the Ctrl+Space hook; service clears `workspace_mode`/`workspace_controller_pid` and calls `deactivate_workspace` on the dying client's IPC thread | `service_orchestrator.c:358-362`; `ipc_server_per_client_thread.c:186-200` |
| bridge (ENABLE / AUTO) | `bridge_watch_thread_func` | relaunch / re-arm the trampoline | **none**; exit code never read (`:432-475`) |
| a client app | its IPC thread's `ReadFile` breaks → `common_shutdown` | see §2.4 | n/a |
| macOS shell | `posix_spawn` + waitpid respawn | 1 s settle | `service_orchestrator.c:1385-1395` |

Windows service defaults: `workspace=auto`, `bridge=auto`, `start_on_login=true`
(`service_config.c:37-39`); `workspace_binary` containing a path separator is a dev
override that bypasses the registry (`service_orchestrator.c:146-186`).

### 1.3 Exit paths and crash handling

| # | Way the service ends | Logs first? | Anchor |
|---|---|---|---|
| 1 | `--autostart` with `start_on_login=false` → `ExitProcess(0)` (the only `ExitProcess`) | yes | `main.c:125-128` |
| 2 | Tray **Exit** → `g_service_shutdown_requested` → mainloop returns → `WinMain` returns | yes | `service_tray_win.c:406-411`; `ipc_server_mainloop_windows.cpp:273-277` |
| 3 | `IPC_EXIT_ON_DISCONNECT` / `IPC_EXIT_WHEN_IDLE` (both default off) | yes | `ipc_server_per_client_thread.c:166-176` |
| 4 | pipe-layer failure → `ipc_server_handle_failure` → `running=false` | yes | `ipc_server_process.c:933-937` |
| 5 | `init_all` failure (e.g. no DP found) | yes | `ipc_server_process.c:1075-1084` |
| 6 | **`exit()` from any in-process DLL** — runs `atexit` → the log gets its `=== Log Ended ===` banner with zero teardown chatter. That banner is the discriminator (#943 signature) | no | `u_file_logging.c:240` |
| 7 | **uncaught C++ exception on any unguarded thread → `std::terminate`** — no banner, no WER record | no | see guard table |
| 8 | AV / heap corruption → WER (if LocalDumps armed on the box; the installer arms nothing) | no | — |

Crash containment that exists — exactly **five** hand-placed catch-alls and nothing else
(no `set_terminate`, no `SetUnhandledExceptionFilter`, no vectored handler; `set_terminate`
was prototyped and **removed** because MSVC's terminate handler is per-thread and did not
fire for the window thread — `git show cc7df1676`):

| Guarded entry | Anchor | After the catch |
|---|---|---|
| `wnd_proc` (trampoline `try{wnd_proc_inner}` **inside** the callback — the only placement that works across the x64 kernel-callback boundary) | `comp_d3d11_window.cpp:1377-1391` | message dropped, window lives |
| `window_thread_func` | `comp_d3d11_window.cpp:1575-1591` | thread stops, `window_release` vote |
| `capture_render_thread_func` | `comp_d3d11_service.cpp:6041, 6139-6164` | thread stops, `capture_render_running=false` — restartable but **nothing polls it**; recovery needs a new client or a Ctrl+Space cycle |
| `window_op_worker_func` (detached) | `comp_d3d11_service.cpp:1333-1375` | worker gone for the process lifetime; queued 2D restores silently stop |
| capture `frame_pool.Recreate` only | `d3d11_capture.cpp:166` | — |

**Unguarded:** every per-client IPC thread, the tray thread, all four orchestrator threads,
the mainloop, the WinRT capture callbacks. A vendor DP throw from `process_atlas` on an
IPC thread (`comp_d3d11_service.cpp:12615, 13757, 13792, 13912`) or a provider throw
crossing the C ABI is #7 above.

Two shutdown races: `teardown_all` destroys the system compositor, devices and the global
mutex **without joining the per-client threads** (`ipc_server_process.c:198-220`; on
Windows those threads sit in a blocking `ReadFile` that `running=false` cannot interrupt,
`ipc_server_per_client_thread.c:482-500`); `delayed_exit_thread` is started on a stack
`os_thread` and never joined (`:173-175`).

### 1.4 In-process plug-in loading

Two loaders, same shape (`target_plugin_loader.c` for DPs, `target_input_plugin_loader.c`
for input providers):

| Aspect | Vendor DP | Input provider |
|---|---|---|
| Discovery (Win) | `HKLM\Software\DisplayXR\DisplayProcessors\*` (`Binary`, `ProbeOrder` default 100, cap 16, `PreferredPlugin` override) | `HKLM\Software\DisplayXR\InputProviders\*` (+ `Enabled`, cap 16, `Input\ForceQwerty` kill-switch) |
| Load | `LoadLibraryExW(path, LOAD_WITH_ALTERED_SEARCH_PATH)`, ABI-major gate before any dispatch, **handle leaked for the process lifetime by design** | same (`target_input_plugin_loader.c:291, 323, 746`) |
| When / thread | first `target_plugin_get_active()` — inside `xrt_instance_create_system` on the **main thread** at `init_all` (`ipc_server_process.c:506`); **plus** `target_plugin_refresh_active()` at **every per-client compositor create on that client's IPC thread**, under the process-wide `g_refresh_mutex` (`target_plugin_loader.c:1960-2008`; fired from `comp_d3d11_service.cpp:14196-14199`) | once per process at system build; no refresh, no unload, `iface->destroy` **never called** |
| Where else | the same builder is linked into the runtime DLL and `displayxr-cli`, so an **in-process app hosts its own DP and its own providers** — two LeapC clients on one box (`targets/openxr/CMakeLists.txt:64`) | same |
| Dev-path guard (#943 hardening 2a) | **none** — any absolute path in the registry loads verbatim; only a registry↔DLL version-skew WARN exists (`:315-352`) | **none** |
| Vtable lifecycle | create / process_atlas / request_display_mode / destroy … all synchronous, unguarded | `probe`, `create_devices`, `destroy` (unused), `get_presence` — **no start/stop/health**; providers own their threads (`xrt_input_plugin.h:241-355`) |

---

## 2. Client classes and how they connect

### 2.1 The IPC decision (client side)

`u_sandbox_should_use_ipc()` (`u_sandbox.c:97-147`), preceded by one check in
`targets/openxr/target.c:49-52`:

0. `ext_spatial_workspace_enabled` (the shell) → IPC
1. `XRT_FORCE_MODE=native` → in-process · 2. `XRT_FORCE_MODE=ipc` → IPC
3. `DISPLAYXR_WORKSPACE_SESSION=1` → IPC · 4. `TokenIsAppContainer` / `sandbox_check` → IPC
5. otherwise in-process

Both env reads have a `GetEnvironmentVariableA` fallback because the runtime DLL has its
own static CRT (`u_sandbox.c:105-111`).

### 2.2 The client-class matrix

There is **no client-class field** anywhere in the IPC layer. Class is re-derived ad hoc
per call site from three client-controlled signals (§4.1). This table is what the code
*implies*:

| Class | Route | Registers as | Frame path | Slots | Asserts on shared state | When the service dies |
|---|---|---|---|---|---|---|
| **Workspace controller** (shell) | ext flag → IPC | `workspace_activate`, PID-gated against the orchestrator-spawned PID; provider 0 ⇒ first-claim (`ipc_server_handler.c:3471-3489`) | none (WS layers: chrome/overlay/cursor swapchains) | 1 | mode, focus, poses, layout, capture clients, `force_display_3d` on activate (`:3507`) | exits cleanly; orchestrator re-arms/restarts |
| **Shell-launched app** | env → IPC | ordinary; gets a multi-comp slot at session create (`comp_d3d11_service.cpp:14335-14444`) | tile → service render thread | 1 each | its mode requests are dropped under workspace mode (`:10316`) | `INSTANCE_LOST`, app-specific |
| **Forced-IPC standalone** (`hosted`/`handle`/`texture`) | `XRT_FORCE_MODE=ipc` | ordinary; **captured into a slot if workspace mode is on, regardless of who launched it** (`:14338`) | standalone: own swap chain + own DP, present on its IPC thread (`:12713-12745`) | 1 | writes process-global mode/geometry (§4.2) | `INSTANCE_LOST` |
| **Chrome legacy WebXR** | AppContainer auto | ordinary + `ext_win32_appcontainer_compatible_enabled` (the sole "is Chrome" discriminator, `ipc_server_handler.c:591-594`) | NULL binding ⇒ **service creates the HWND** (`:3648-3668`, becomes `sys->compositor_hwnd`); standalone present, or a slot under a workspace | 1 | same as standalone | Chrome-side not read |
| **WebXR bridge** | forces `XRT_FORCE_MODE=ipc` on itself | **two** connections: its headless `XrInstance` (`XR_MND_headless`+`XR_DXR_display_info`) and a raw `ipc_client_connection_init` for client enumeration (`webxr_bridge/main.cpp:1085-1105, 1610-1631`); flagged `is_bridge_relay`, excluded from slots (`:14222-14236`) | **never presents**; metadata via `SetPropW` on the service window (`DXR_RequestMode` etc., `main.cpp:1258-1266`) — not IPC | **2** | mode requests via window props (`comp_d3d11_service.cpp:10736-10775`); no input/HT features | **zombie** (§1.2) |
| **displayxr-browser** (CEF) | GPU process, `XRT_FORCE_MODE=ipc` pre-sandbox, **Low IL + restricted token** (`displayxr-browser/patches/0023…`) | present-owner: `xrWeaveBindWindowDXR(browser frame HWND)`; gets a slot under a workspace (never submits layers → empty tile) | `xrWeaveSubmitDXR` on Viz's present thread; server holds `render_mutex` end-to-end (`:13496`); under a workspace it drives the **workspace's own DP** (`:13448-13487`) | ≥1 | holds the panel in 3D by existing (`weave_force_3d_if_needed`, `:13242-13307`); explicit `SetDisplayMode` from tab policy | not read |
| **MCP adapter / voice** | not OpenXR clients | per-app / shell pipes | — | 0 | — | unaffected |
| **Input providers, vendor DP** | in-process DLLs | — | — | 0 | roles via the arbiter (`target_input_arbiter.c`) | **they are the service** |
| **Unity/Unreal under the shell** | env → IPC | ordinary slot | tile | 1 | — | `INSTANCE_LOST` |

### 2.3 Slot arithmetic against the target scenario

`IPC_MAX_CLIENTS 8` (`ipc_protocol.h:40`) sizes `threads[8]`, `isms[8]` and the Windows
pipe `nMaxInstances` (`ipc_server.h:388,419`; `ipc_server_mainloop_windows.cpp:161`). It is
a **connection** cap — sessions and compositors are not the limit (`D3D11_MULTI_MAX_CLIENTS
24`, `comp_d3d11_service.cpp:1435`; `MULTI_MAX_CLIENTS 64`, `comp_multi_private.h:67`).

shell (1) + Chrome WebXR (1) + bridge (**2**) + browser (1) = **5 before a single app
launches**; three shell apps exhaust 8. At the cap on Windows the listener instance is
dropped and re-armed later (`ipc_server_mainloop_windows.cpp:220-234`) — the `ics`-full
branch (`ipc_server_process.c:970-978`) is effectively dead; the client sees
`ERROR_PIPE_BUSY`, retries 5 s (`ipc_client_connection.c:96-147`) then fails
`xrCreateInstance` with no distinguishable error. On POSIX the socket is accepted then
closed with **no message to the client**. An evicted-but-alive client (#925 S4) frees a
*compositor* slot but keeps its IPC slot, thread, pipe and shm until its pipe breaks
(§2.4). `IPC_MAX_CLIENTS` also sizes `ipc_client_list.ids[8]` on the wire
(`ipc_protocol.h:303`) — raising it is a protocol change.

### 2.4 Connection lifecycle

| Step | What | Anchor |
|---|---|---|
| Accept | 20 Hz mainloop, one accept per tick; pipe `PIPE_TYPE_MESSAGE|PIPE_NOWAIT|REJECT_REMOTE`, buffers **1024 B** each way, **no `FILE_FLAG_OVERLAPPED` anywhere in the runtime**; DACL: deny BG/AN, allow AC/AU/RC/BA + Low-integrity label (for the browser GPU process) | `ipc_server_mainloop_windows.cpp:120-165, 243-253, 286-295`; `ipc_server_process.c:548` |
| Handshake | `get_shm_fd` (per-client section, `DuplicateHandle` into the client — needs `OpenProcess(PROCESS_DUP_HANDLE)`, which fails against an **elevated** client) → git-tag string equality (the *entire* version gate, `IPC_IGNORE_VERSION` bypass) → `describe_client` (client-supplied pid + app info, **stored verbatim, repeatable**) | `ipc_client_connection.c:346-424, 433-500`; `ipc_server_handler.c:1655-1695`; `ipc_message_channel_windows.cpp:81, 176-183` |
| Slot | linear scan under `global_state.lock`; `id = ++id_generator` (never validated); `init_shm` maps ~21 MB (**INFERRED**: `slots[128] × layers[128] × ~1.3 KB`) **under the lock, on the mainloop thread**; one OS thread per client for life | `ipc_server_process.c:951-1032, 286-452` |
| Teardown (`common_shutdown`) | close channel → destroy shm → free slot (`server_thread_index=-1`, `memset(client_state)`) → unlock → `xrt_comp_destroy` (**unlocked**) → drop swapchain/semaphore refs → destroy session → spaces → `feature_dec` sweep → exit knobs → `deactivate_session` → if controller: clear workspace mode + `deactivate_workspace` | `ipc_server_per_client_thread.c:77-207` |
| Zombie slots | **nothing in the IPC layer reclaims a slot**; `session_end`/`session_destroy` leave thread, pipe, shm and `server_thread_index` intact — only pipe break frees them (#929's second class) | `ipc_server_handler.c:1814-1844` |
| Client-side loss | any RPC failure → `XRT_ERROR_IPC_FAILURE` → `has_lost` → `XR_ERROR_INSTANCE_LOST` at 43 sites; **no reconnect, no `InstanceLossPending`** | `oxr_xret.h:22-32` |
| Per-client RPC serialisation | every `ipc_call_*` takes `ipc_c->mutex` for the whole round trip — a client's render and main threads share one pipe and one lock (why #924's app main thread sat in `NtWriteFile`) | `shared/proto.py:68-131` |

---

## 3. Threads and locks

### 3.1 Threads (complete)

| Thread | Created | Runs | Guard |
|---|---|---|---|
| main | OS | `WinMain` → `ipc_server_main` → `main_loop`: 50 ms sleep, mode-index push into every client's shm, `ipc_server_mainloop_poll` (accept + tray flag). **No message pump** | none |
| tray | `service_tray_win.c:514` | `GetMessageW` loop; owns the message-only HWND, tray menu, the LL keyboard hook and (via subclass) `spawn_workspace()` | none |
| trampoline, watchdogs ×2 | `service_orchestrator.c:634, 418, 510` | §1 | none |
| per-client IPC ×N | `ipc_server_process.c:1029` | blocking `ReadFile` → `ipc_dispatch` (bare `switch`, 131 handlers, no auth layer, `proto.py:266-330`) — **all handlers, including `compositor_layer_commit`, `weave_submit`, `request_display_mode`, session create/destroy, DP factory calls** | none |
| capture/render | `comp_d3d11_service.cpp:6186` (workspace only) | pace on `frame_latency_waitable`/`render_wakeup_event` (100/14 ms) → `render_mutex` (plain `lock_guard`) → `multi_compositor_render` (the whole hold window) → `Sleep(2)` if waiters | try/catch |
| D3D11 window | `comp_d3d11_window.cpp:1650` (one per service window **and one per hosted standalone client**) | WndProc, input ring; `WM_CLOSE` calls the vendor DP **with no lock** (`comp_d3d11_window.cpp:643-650`) | try/catch |
| window-op worker | `comp_d3d11_service.cpp:1398` | `SetWindowPlacement`-family restores off the render path (#925 rank 7) | try/catch, no restart |
| WinRT capture pool | `d3d11_capture.cpp:285-300` | `on_frame_arrived` → `CopyResource` on the **shared immediate context, outside `render_mutex`** (relies on `SetMultithreadProtected(TRUE)`, `comp_d3d11_service.cpp:14697-14705`) | partial |
| provider threads | provider-owned (`ultraleap_provider.cpp:568` poll thread; net_input hub) | LeapC polling, #941 idle watchdog (a branch of the poll thread) | none |

### 3.2 Locks

| Lock | Type / scope | Guards | Notable holders |
|---|---|---|---|
| `render_mutex` | `std::recursive_mutex` on `d3d11_service_system` (`comp_d3d11_service.cpp:916`) | nominally "D3D11 context access"; de facto all compositor state. **47 sites**: 46 via `render_mutex_fair_lock` (waiter-announce, `:1030-1045`), 1 plain (the render thread, `:6101`) — that asymmetry *is* the #920 fairness fix (`Sleep(2)` hand-off hint, not a queue, `:6133-6135`) | render thread (whole frame incl. vendor `process_atlas` `:9363`); `weave_submit` end-to-end (`:13496`); `compositor_destroy` across vendor DP destroy (`:12986-13011`); ~26 controller RPCs still inline (getters, chrome ×4, cursor, overlay, input drain, `ensure_workspace_window` `:17819`, `deactivate_workspace` `:17938`) |
| S3 command queue | `ws_cmd_ring[256]` + ns `ws_cmd_mutex` (`:695-719, 937-941`), default ON (`DXR_CMD_QUEUE=0` reverts) | 9 write-shaped controller commands (pose ×2, cap, vis ×2, exit, focus, mode flip, input-grab flag) enqueue; drained at `:7405` before `apply_pending_mode_flip`; overflow drops oldest POSE else applies inline | — |
| `c->mutex` | per-client `std::mutex` (`:485`) | held from the top of `compositor_layer_commit` (`:10396`) to return | order `c->mutex → render_mutex` (`:10554-10559`, the only written ordering rule in the file) |
| `global_state.lock` | one `os_mutex` per server (`ipc_server.h:434`) | `active_client_index`, `connected_client_count`, `threads[]` occupancy — plus 54 references in the handler file | connect (with the 21 MB `init_shm` inside), disconnect, activate/deactivate session, ~20 `workspace_*` id→slot resolutions |
| `g_arb_mutex` | process-global (`target_input_arbiter.c:114`) | arbiter table, presence cache, roles | `arbiter_get_roles` on **every client's `xrSyncActions`** — and it calls the provider's `get_presence()` while held (`:167, 377-381`) |
| `g_refresh_mutex` | process-global (`target_plugin_loader.c:1980-1982`) | DP re-discovery | every per-client compositor create |
| `hub->mutex` | Ultraleap provider | joint sets | poll thread + every consumer's `get_hand_tracking` |
| `ipc_c->mutex` | per client process | the whole pipe round trip | every RPC |
| `usys->sessions.mutex` | per system | session list; event push (unbounded malloc'd per-session list, `u_session.c:34-42`) | broadcasts |

**Nesting observed:** `global_state.lock → render_mutex` at ≥ 11 handler sites
(`ipc_server_handler.c:3735, 4079, 4110, 4204, 4370, 4429, 4475, 4696, 4792, 4968, 5106`
via `comp_d3d11_service_workspace_find_slot_by_xc` → fair lock at `:16543`), and
`global_state.lock → compositor` at `flush_state_to_all_clients_locked`
(`ipc_server_process.c:645-661`, reached from every client's first `predict_frame`). The
compositor never takes `global_state.lock`, so no ABBA — but **any render-thread stall
parks the server-global lock**, which then blocks every connect, disconnect, session
activate and `layer_sync` slot bump for every client. This channel is **invisible to
`[RENDER]`/`[MUTEX]`** because those counters sample only the render thread's own waits
(`comp_d3d11_service.cpp:6103-6105`; `client_renders` is never incremented, `:6012`).

**Blocking pipe I/O under `global_state.lock`:** `space_locate_spaces` (`:2101-2180`) and
`device_set_haptic_output` (`:6472-6521`) do `ipc_send`/`ipc_receive` while holding it, on
1 KB pipe buffers with no timeout. A client that stalls mid-message parks the mainloop
(which needs the lock to accept) and every other client — with **zero** compositor
involvement (`wait_avg` would read ~1 µs). Same shape as #928, on the runtime's own pipes.

**No lock-ordering statement exists in code or docs** beyond the one local note.

---

## 4. Ownership and arbitration — as built

### 4.1 Caller identity is client-asserted

Every PID gate compares against `ics->client_state.pid`, which the client sends in
`describe_client` (`ipc_client_connection.c:410-415` → `ipc_server_handler.c:1655-1659`) and
may re-send at any time. There is **no `GetNamedPipeClientProcessId` / `SO_PEERCRED` /
`LOCAL_PEERPID` anywhere in `src/xrt/ipc/`**. The server-assigned `client_state.id`
(`ipc_server_process.c:1002`) is the one field the client cannot forge and is not what the
gates use. `ext_win32_appcontainer_compatible_enabled` (the "is Chrome" discriminator) and
`is_bridge_relay`/`is_workspace_controller` (session-info flags) are equally client-asserted.

Authorization census over the 131 handlers (`ipc_server_handler.c`): **111 have none**;
13 check the orchestrator-spawned PID (`get_orchestrator_workspace_pid()`, which is **0** —
gate open to everyone — whenever no orchestrator installed a provider: Android, macOS,
dev-launched service, `:95-107`); 3 check `s->workspace_controller_pid` (fail closed);
1 uses the "effective" fallback. `session_set_modal_state`/`session_request_file_picker`
document their no-auth choice; the 14 ungated `workspace_*` handlers do not.

### 4.2 Shared display state — who writes it

| State | Writers (class → anchor) | Rule today |
|---|---|---|
| **Workspace mode on/off** (`sys->workspace_mode`, `xsysc->info.workspace_mode`, `s->workspace_mode`) | activate: controller (or first-claim) `ipc_server_handler.c:3516-3519`; **deactivate: any client, no auth** `:3552-3577`; controller pipe break `per_client_thread.c:186-199`; late-arriving client latch `comp_d3d11_service.cpp:14257-14261` | one process-global bool; unlocked writes from IPC threads read per frame by the render thread; `xsysc->info` is copied by value to every client at connect and never re-broadcast (`ipc_server_handler.c:1703`) |
| **Hardware 2D/3D (lens)** | any client `compositor_request_display_mode` `:2581`; controller `workspace_request_display_mode` `:3581` (PID-gated) + `force_display_3d` on activate `:3507`; standalone `[force_3d]` re-assert `comp_d3d11_service.cpp:10971-10992`; deferred-3D kick `:11179-11221`; zones tier-1 `:10796-10826`; present-owner `weave_force_3d_if_needed` **every submit** while SR reads 2D `:13242-13307`; vendor-drift follow `:10994-11041`; window `WM_CLOSE` `comp_d3d11_window.cpp:643-650`; #814 teardown failsafe `:12883-12941`; workspace deactivate `:18013` | last-writer-wins on `sys->hardware_display_3d` with **one downstream veto**: `service_apply_pending_mode` returns early under `workspace_mode || bridge_live` (`:10316-10318`) — but the pending values are **not cleared** on the veto, so a request made during workspace mode replays on deactivate |
| **Content rendering mode / atlas grid** (`head->hmd->active_rendering_mode_index`, `sys->tile_*`, `sys->view_*`) | any client `compositor_request_rendering_mode` `:2601` → `:10346-10360` + `broadcast_rendering_mode_change` to **every** session; every standalone writer above; `sync_tile_layout` on the commit thread **outside `render_mutex`** (`:10693-10700`, the scope ends at `:10691`) | process-global device field, no owner; #761: the event fires before the DP call and is never reverted (`:2182-2210`; `dp_request_display_mode_checked` knows the vendor rejected, no caller acts, `:2314-2332`) |
| **Display processor** | workspace: ONE shared `mc->display_processor` (`:6873-6877`); standalone: **one per client** (`:3958-4079`, N clients ⇒ N weavers on one panel — the config `:3953-3957` says causes SR recalibration); present-owner under a workspace: the workspace's DP from an IPC thread (`:13448-13487`); dismiss/hot-switch creates per-client DPs on the render thread (`:7181-7191`) or the IPC thread (`:12493-12507`) | no lease; DP calls happen on render, IPC and window threads with different lock rules |
| **Focus** | controller `workspace_set_focused_client` `:4029` (writes both authorities); `system_set_focused_client`/`system_set_primary_client` **any client** `:3433/:3423` (IPC authority only); **any client's first `predict_frame`** promotes itself via `ipc_server_activate_session` `:2482`; `session_end` clears compositor focus only `comp_d3d11_service.cpp:4756-4774` | **two authorities** (`s->global_state.active_client_index` vs `mc->focused_slot`) partially reconciled; under workspace mode the IPC one is forced visible+focused for everyone (`ipc_server_process.c:620-626`) |
| **Panel / windows** | `set_window_pose`/`visibility` **any client, any slot** `:3758/:3836`; `add/remove_capture_client` any client, any HWND `:3966/:4000`; `weave_bind_window` any HWND `:5579`; cursor/overlay/chrome ×6 any client `:4663-4952` | none |
| **Input grab / pointer capture** | `set_input_grab` **any client** `:3666` (the stronger primitive); `pointer_capture_set` controller `:4308` | inconsistent |
| **Input events** | `enumerate_input_events` destructive drain, controller-gated except macOS | first-drainer-wins |
| **Input roles / hand-tracking source** | provider hierarchy (presence-ranked, qwerty floor, masquerade — ADR-034 Amdts 1–3) is **process-global**; the feature refcount (`u_system_helpers.c:131-181`) is service-global — and **nothing in `oxr` increments the hand-tracking feature**; `static_roles.eyes` is never assigned; no device implements `begin/end_feature`; `xrt_device_begin/end_feature` have no NULL guard (`xrt_device.h:1148-1164`) | no per-consumer arbitration: `ics->io_active` is always true (`ipc_server_process.c:1012`), `device_get_hand_tracking` has no focus check; the state tracker zeroes *action values* for unfocused sessions after the device call. **qwerty is a destructive, unlocked integrator** (`qwerty_device.c:336-432`: advances position per call, consumes mouse deltas, ignores `at_timestamp_ns`) — N pollers ⇒ N× speed |
| **Space origin** | `recenter_local_spaces`, `set_tracking_origin_offset`, `set_reference_space_offset` **any client** `:2312-2357` | none |
| **Atlas capture** | `workspace_capture_frame` **any client**, caller-supplied path, `Map(READ)`+PNG under `render_mutex` `:4599` | none |
| **Workspace view rig** | controller only, enforced once at `:664-701` | **the model the rest should follow** |

What a plain non-controller IPC client can therefore do today, verbatim from the code:
tear down the shell's workspace (`workspace_deactivate`), flip the panel outside
workspace mode (and queue a flip for replay inside it), change the service-global content
mode and broadcast the event to everyone, move/hide another client's window, take a global
input grab, register any HWND as a capture client, replace the cursor/overlay, trigger a
full-res atlas capture under the render lock, steal `active_client_index` by calling
`xrWaitFrame`, mute another client's inputs, recenter everyone's origin, and read every
other client's name/pid/geometry.

### 4.3 The two compositor modes and the seam between them

`workspace_mode` is set **only** through `service_set_workspace_mode()`
(`comp_d3d11_service.cpp:2057-2063`; callers `:7220, 14260, 16096, 17811, 17828, 17955`).
`sys->multi_comp` exists **only** under workspace mode. Slot membership is decided **once,
at session create** (`:14335-14444`) by that global bool: connect-before-shell ⇒ never joins;
connect-after ⇒ always joins (bridge relay and controller excluded, browser and Chrome
**included**).

| | Workspace / multi-comp | Standalone per-client | Weave present-owner (#625) | Client-transparent (ADR-029) |
|---|---|---|---|---|
| Window | service-owned (`comp_d3d11_window_create`, `:6583-6593`) | app HWND (`CreateSwapChainForHwnd`, `:3808-3813`) or **service-created hosted window** for NULL binding (`:3648-3667`, one window thread each) | app-bound HWND, no swap chain | app HWND; service exports a shared texture + fence, no swap chain (`:3725-3773`) |
| DP | one shared | one per client | per-client, or **the workspace's** | per-client |
| Present | render thread, token/probe/skip (#924) `:9434-9469`, HRESULT discarded | client's IPC thread, `Present(1, DO_NOT_WAIT)` ≤ 50 ms `:12709-12746`, then bounded compositor-clock wait `:12768` | none (`Signal`+`Flush`, `:14025`) | client presents |
| `render_mutex` | held for the whole frame | **not held** for blit/`process_atlas`/present (`:11331-12746`; only `:10481, 10561, 12067`) — safety = D3D11 MT protection | held end-to-end `:13496` | as standalone |
| Mode authority | acked-flip state machine (`:2234-2475`) + controller | every writer in §4.2 | `weave_force_3d_if_needed` | as standalone |
| Global state written per frame | — | `sys->active_compositor` (last committer wins, `:10459`), `sys->view_*/display_*/output_*` on resize/DP create (`:10618-10662, 4003-4030`), `sys->compositor_hwnd` pinned to the **first** client (`:3669-3672`), `SetMaximumFrameLatency` once per process (`:3847-3861`), shared 100 ms vendor-poll cache | — | — |

**Transitions** (evidence in the standalone review): a client that was standalone when the
shell activates is **never** flagged for reverse hot-switch (only slot members are, `:17836-17844`) —
its commits take the workspace early-return (`:12431`) before `process_atlas`/`Present`, so it
**goes dark with a live orphan DP** (INFERRED reachable: forced-IPC cube then Ctrl+Space). On
deactivate, slot members lazily rebuild per-client resources on their own IPC threads
(`:12440-12513`, DP factory on the IPC thread) — but a client with **no `app_hwnd`** (Chrome/hosted
that joined under the shell) fails the `IsWindow` test and logs `swap_chain=NULL` forever
(`:12437-12440`). Two standalone clients on a pure-standalone service ⇒ two fullscreen
`HWND_TOP` windows, two DPs, shared qwerty camera, shared `set_fullscreen` statics
(`comp_d3d11_window.cpp:545-548`), and the #814 survivor scan (`:12929-12953`) sees `survivors==0`
(standalone clients are never in `mc->clients[]`) so the first to leave flattens the panel
for the other (INFERRED).

### 4.4 #925 status (correcting the reference doc)

S1 bounded waits ✔, S2 audit ✔, **S3 command queue ✔ default on**, **S4 eviction ✔** (tier 1
session-ended `DXR_EVICT_ENDED_MS=2000` on; tier 2 idle `DXR_EVICT_IDLE_MS` default **off**;
commit-time re-register), **S5 ✔ partially, default off** (`DXR_COMPOSE_FROM_COPY`): the
per-client *content* atlas was already service-owned (`:3572-3603`), so S5 shipped as
`compose_copy_cache` for the three controller producers (chrome/overlay/cursor, `:8699, 8903,
9061`); with the default off the render thread still does up to (1 chrome/slot + ≤16
overlays + 1 cursor) × 4 ms blocking acquires per frame against the shell's process.
Remaining unbounded work on the render thread under `render_mutex`: `process_atlas` `:9363`,
`DP_REQUEST_DISPLAY_MODE` `:2405/:7228`, DP factory `:6557/6876/7184/17858`, DP destroy
`:7207/17888/18015/6489`, MCP capture on hit `:9409→18103`, `comp_d3d11_window_destroy` 30 s
`:17898`. Not handled anywhere: `DXGI_ERROR_DEVICE_REMOVED` (0 hits), workspace `Present`
HRESULT.

---

## 5. Failure domains

| Component (in the service process) | Hang | `exit()` | throw / `std::terminate` | heap corruption |
|---|---|---|---|---|
| **Vendor DP** | in `probe`/`DllMain` during per-client `refresh_display_processors` → **every** client's `xrCreateSession` blocks behind `g_refresh_mutex`; in `process_atlas` on the render thread → workspace stops, controller RPCs park; on an IPC thread (standalone) → that client + shared-context contention | whole service (banner, no chatter) | caught only in `wnd_proc`/render thread; IPC threads → silent death | whole process |
| **Input providers** | `get_presence` under `g_arb_mutex` → **every** client's `xrSyncActions` blocks (and each client's frame loop behind `ipc_c->mutex`); provider `hub->mutex` stall → all consumers of that provider | whole service (#943 signature — but see below) | crosses the C ABI ⇒ terminate, no banner, no WER | whole process |
| tray thread | tray + Ctrl+Space dead; IPC survives | — | terminate | — |
| window thread | frozen window; cross-thread window calls behind it | — | contained | — |
| render thread | workspace stops compositing; clients keep sessions | — | contained, not self-healing | — |
| per-client IPC thread | that client — unless inside `global_state.lock`/`g_arb_mutex`/`g_refresh_mutex`/`render_mutex` | whole service | terminate | — |
| main thread | no new clients; existing keep running; mode-index push stops | whole service | terminate | — |
| shell / bridge (separate processes) | nothing blocks in the service; a hung shell keeps the Ctrl+Space hook uninstalled | watchdog respawn/re-arm, no backoff | same (exit code never read) | **isolated — the only real boundary in the design** |

**On #943's mechanism.** The filed chain (last-client teardown → `xrt_system_devices_feature_dec`
→ refcount 0 → provider `end_feature` → `exit()`) is **not live at this commit**: `feature_inc`
is called only from `oxr_space.c:78/144` for `XRT_DEVICE_FEATURE_EYE_TRACKING`, gated on
`static_roles.eyes` which is never assigned; `XR_EXT_hand_tracking` never touches the feature
API; the in-tree Ultraleap provider implements no `begin/end_feature` and contains no `exit`,
`abort`, `throw` or `catch`; so `ics->device_feature_used[]` is all-false at teardown and the
sweep at `ipc_server_per_client_thread.c:150` never runs. The ~30 ms correlation with the
last disconnect stands; the surviving candidates are (1) `LeapC.dll`'s own CRT/error path
(third-party, loaded with `LOAD_WITH_ALTERED_SEARCH_PATH`), (2) the torn worktree DLL image
that was rebuilt under the live service, (3) an uncaught C++ exception — which would give
the #930 signature (no banner) rather than #943's (banner present). See the #943 update.

---

## 6. Resource limits

| Resource | Limit | At the cap |
|---|---|---|
| IPC connections | **8** | §2.3 |
| per-client shm | ≈ 21 MB (**INFERRED**; `slots[128] × layers[128]`) mapped under `global_state.lock` | ≈ 170 MB at 8 |
| pipe buffers | 1024 B in/out | any reply > 1 KB blocks on a non-reading peer forever |
| per-client swapchains / semaphores / spaces | 256 / 8 / 128 (`ipc_server.h:58-60`) | swapchain ids indexed **unchecked** at `ipc_server_handler.c:5348, 5363, 5386, 5404` |
| `space_count`, `num_samples` | **unbounded**, client-supplied allocation sizes (`:2085-2103, 6474`) | — |
| per-session event queue | unbounded malloc'd list | a client that never polls leaks service heap |
| multi-comp slots | 24 (16 reachable only by capture clients) | session create fails `XRT_ERROR_D3D11` |
| overlays / file pickers / S3 ring / window-op queue | 16 / 8 / 256 / 64 | drop-oldest or inline |
| per-client atlas | display-native RGBA (~33 MB at 4K) per IPC client | ~265 MB at 8 |
| DPs | 1 (workspace) or N (standalone) | vendor recalibration |

---

## 7. Platform shape

| | Windows | macOS | Linux desktop | Android |
|---|---|---|---|---|
| service | `displayxr-service.exe` | `displayxr-service` + orchestrator (posix_spawn, respawn) | `displayxr-service`, **no orchestrator**; systemd socket activation optional | `libservice_target.so` in a bound **foreground** `Service` (`START_STICKY`), `ipc_server_main_android` on a `std::thread` |
| transport | named pipe | AF_UNIX | AF_UNIX | binder for bootstrap only (`IMonado.aidl` 4 methods); per-frame traffic on a socketpair; **connect blocks a binder thread on a condvar until the 50 ms mainloop accepts** (`ipc_server_mainloop_android.c:170-222`) |
| system compositor | `comp_d3d11_service` | **null + `comp_multi`** (shared-surface workspace path exists) | null + `comp_multi`, **headless only** (no present arm) | null + `comp_multi`, per-session `comp_window_android` |
| DP | in-process; 1 or N | in-process; **one shared** | in-process | in-process (`dlopen`); **one per client**, `process_atlas` per client per frame |
| clients on screen | N | N | 0 | **1** — the app surface is a process-global singleton (`android_globals.cpp:21-44`); two clients ⇒ two `vkCreateAndroidSurfaceKHR` on one window |
| arbitration | inside `comp_d3d11_service.cpp` | Monado active-client fallthrough only | — | Monado active-client fallthrough only; mode = per-client DP, last writer wins |
| #925 S1–S5 | ✔ | ✘ (`list_and_timing_lock` holds the whole per-client render + present; `vkWaitForFences(UINT64_MAX)`; unbounded `process_atlas`) | ✘ | ✘ |
| supervision | orchestrator for children; nothing for the service | orchestrator | none | the OS (`START_STICKY`, but restart arrives with a null Intent → not re-foregrounded, `MonadoService.kt:63-71`); no client reconnect |

**ADR-025 clarification.** "Out-of-process" there means the *app* is isolated from the vendor
SDK by the app↔service split; the DP still lives in the service. There is no DP proxy/host in
the tree, #510 is open with every box unchecked, and CI builds only the `inProcess` Android
flavor (`build-android.yml:137`). Android is not a precedent for process-isolating the DP; it
*is* the platform where every app is IPC, so every gap in §4 applies with no shell to hide it.

---

## 8. Observability today

| Signal | What it measures | Blind spot |
|---|---|---|
| `[RENDER]` (10 s) | render-thread iterations, `capture_avg_us` (body cost), `wait_avg_us` (**render thread's own** lock wait), `client_skips` | IPC/controller waits never sampled; `client_renders` structurally 0; no per-client attribution; the #939 datum (`12/10 s @ 865 ms, wait_avg 1 µs`) means "slow inside the body" (vendor/present), not lock contention |
| `[MUTEX]`, `[PRESENT_NS]`, `[COMMIT_PROFILE_SVC]`, `#625 weave timing` | fair-lock waiters, present duration, commit phases, weave acquire/weave/epilogue | opt-in env, global |
| `[FENCE]` incomplete tally, `[app_mode]`/`[weave_3d]`/`[force_3d]` | per-client fence timeouts, mode writers | scattered, no per-client roll-up |
| `displayxr-cli selftest|info` | plug-in discovery, ABI, display info | nothing about live clients, slots, health |
| WER LocalDumps (box policy) | crashes | never fires for `exit()`; PDBs deployed since #926 |
| Android: ADPF + `OOP_PRESENT_TS` | per-frame phase timings | no per-client counters, no `[RENDER]` analogue |

Not observable at all: which client holds the mode, per-client RPC latency, blocked-in-handler
time, `global_state.lock` wait, event-queue depth, slot exhaustion (client sees `PIPE_BUSY`),
DP call durations per thread, `Present` HRESULT, device-removed.

---

## 9. Defects and hazards found by this review (not previously filed)

Numbered for reference from the ADR and the issue plan.

1. Caller identity is client-asserted; 111/131 handlers unauthenticated; gates fail open without an orchestrator (§4.1).
2. `workspace_deactivate` has no auth; any client can tear the shell's workspace down (`ipc_server_handler.c:3552-3577`).
3. Mode requests vetoed during workspace mode are latched and replay on deactivate (`comp_d3d11_service.cpp:10316-10319`).
4. `global_state.lock` held across blocking pipe I/O (`:2101-2180`, `:6472-6521`) and nested over `render_mutex` at ≥ 11 sites.
5. `ics->xc` written/destroyed unlocked, read under `global_state.lock` by other clients' threads → cross-client UAF window (`ipc_server_handler.c:1751`; `per_client_thread.c:581`).
6. Unbounded client-supplied allocations (`space_count`, `num_samples`); unchecked swapchain ids; unbounded per-session event queue.
7. Commit-thread writes to `sys->tile_*`, `hardware_display_3d` and the shared workspace DP outside `render_mutex` (`:10693-10700, 10950`).
8. Standalone client stranded dark with an orphan DP when the shell activates; hosted/Chrome client stranded without a swap chain when it deactivates (§4.3).
9. Present-owner drives the workspace's DP from an IPC thread under `render_mutex` (`:13448-13487`); `weave_force_3d_if_needed` fires every submit while SR reads 2D.
10. #814 failsafe survivor scan ignores standalone clients (INFERRED flattening) (`:12929-12953`).
11. N standalone clients ⇒ N DPs, shared qwerty camera, shared `set_fullscreen` statics, `compositor_hwnd` pinned to the first client, `active_compositor` last-writer.
12. WebXR bridge never exits on `INSTANCE_LOST` and squats on :9014; costs 2 of 8 slots.
13. No job objects; no restart backoff; child exit codes never read; `teardown_all` doesn't join client threads.
14. No `set_terminate`/UEF; five catch-alls total; render thread not self-healing; window-op worker permanently dead after one throw; no `DEVICE_REMOVED` handling; `Present` HRESULT dropped.
15. Provider `get_presence` called under `g_arb_mutex` on every client's `xrSyncActions`; qwerty integrator unlocked and destructive across N consumers; `iface->destroy` never called; `xrt_device_begin/end_feature` NULL-unsafe; providers also loaded in every in-process app.
16. No dev-path guard in either loader (#943 hardening 2a not implemented).
17. `IPC_MAX_CLIENTS 8` on the wire; ~21 MB shm per client; 24 compositor slots unreachable.
18. #943's filed mechanism is dead code (§5); the true exit source is unidentified.
19. `[RENDER] wait_avg` cannot see handler starvation; nothing per-client; `sys->mcp_capture` polled per frame with (INFERRED) no producer.
20. Android: single global surface, per-client DPs, no arbitration, `Watchdog` client counting likely wrong for N>1 (INFERRED), `START_STICKY` restart not re-foregrounded, `outOfProcess` flavor unbuilt in CI, `MULTI_MAX_CLIENTS 64` silent truncation.

Doc corrections applied alongside this map: `workspace-stability.md` (S3/S4/S5 shipped, #929
tier-1 fixed, missing rows), `production-components.md` (auto-start, names, paths),
`multi-compositor.md` (Windows uses `d3d11_service`; the standalone mode; the bridge is
headless), `in-process-vs-service.md` (superseded banner + the env-var/vendor-symbol
corrections), `separation-of-concerns.md` (vendor rule inverted by ADR-019, dead shell paths),
`workspace-runtime-contract.md`/ADR-016/ADR-019/ADR-034 status lines, `plugin-discovery.md`
(ABI v5, Linux ships), `input-provider-discovery.md` (header vs §5), ADR-025 clarification.

---
status: Active (design note for #964; Phase 3 of ADR-035, epic #974)
owner: David Fattal
updated: 2026-08-21
anchors: main @ 32cd9612d (comp_d3d11_service.cpp = 18,545 L)
code-paths: [src/xrt/compositor/d3d11_service/comp_d3d11_service.cpp, src/xrt/ipc/server/ipc_server_process.c, src/xrt/compositor/d3d11/comp_d3d11_window.cpp]
---
# One compositor pipeline — design note (#964)

[ADR-035 D3](../adr/ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md)
says: the multi-compositor is *always on*, every IPC client renders into a service-owned
per-client surface, the render thread composes and presents, **one display processor (DP)
per panel**, and a *default presenter policy* runs when no controller is attached. This note
records the spike that decided the shape, against the code as mapped in
[service-architecture.md](service-architecture.md) §4.3.

## 1. What the spike established (facts, with the anchors that matter)

- **The multi-comp never composites without a controller today.** A slot is drawn only if
  `placed` (`multi_compositor_register_client` leaves it `false`; only `WS_CMD_SET_POSE_*`
  from the controller sets it) and `focused_slot` is written only by controller commands.
  There is no fullscreen/maximize fallback layout (#307 made `request_fullscreen` a no-op).
- **`sys->workspace_mode` conflates two things**: "the pipeline exists" (multi_comp, service
  window, render thread, shared DP) and "a controller drives policy". ~30 sites gate on it.
  The mode writers (`client_holds_panel_lease` → `return workspace_mode ? false :
  c->state_focused`, `service_apply_pending_mode`'s `WORKSPACE_OWNS_MODE`, `[force_3d]`,
  deferred-3D, zones tier-1, `weave_force_3d_if_needed`) all mean the *policy* sense.
- **`deactivate_workspace` tears the pipeline down** (DP destroyed, render thread joined,
  `mc->suspended`) and every client lazily rebuilds a per-client window/swap-chain/DP on its
  own IPC thread; #963's `enrol_standalone_clients_locked` / `pending_workspace_reentry` /
  runtime-window-on-deactivate exist only to bridge that seam.
- **`set_window_screen_rect` is the platform-neutral placement channel** (ADR-036 D6,
  #1033): the VK DP variant's successor to `set_present_origin`, carrying origin **+ size +
  display id**. It is how a per-window compositor instance tells the weaver where its window
  sits when there is no HWND to subclass — Android's case, where a pure window *move* raises
  no resize at all. ADR-033 is unchanged: the runtime reports geometry, the weaver owns phase
  (including snapping), so this is the successor to *HWND-derived phase*, not to phase policy.
- **The D3D11 DP has no placement channel.** `set_present_origin` exists only on the VK
  vtable; on D3D11 the SR weaver takes its interlace phase from the HWND it was *created*
  with (SDK polls `Window2::getScreenRect()` per frame) and the target size must equal that
  window's client area. The SR SDK supports `setWindowHandle()` after create, but the Leia
  plug-in never calls it — today the only re-bind is destroy + async recreate (flat blit
  meanwhile). `sim_display` ignores the HWND entirely.
- **N DPs ⇒ N `SRContext`s ⇒ N lens hints, no cross-DP arbitration** — the observed
  "V→2D by the lease holder is followed back to 3D by the vendor poll" is exactly this.
  One DP fixes it by construction; the runtime then owns the union (lease + merged zone mask).
- Two per-client atlas formats exist (workspace branch `R8G8B8A8_TYPELESS` + UNORM/SRGB
  SRVs; standalone branch plain `UNORM`, no SRGB SRV) — the render thread expects the former.
- The pipeline present site discards the `Present` HRESULT (`DXGI_ERROR_DEVICE_REMOVED` is
  never seen — #1002).
- Present-owner (`XR_DXR_weave`) already uses the shared DP under a workspace, from the IPC
  thread under `render_mutex` (#965's territory); ADR-029 client-presents weaves into the
  client's shared texture, never presents.

## 2. Decisions

**D-1 `workspace_mode` keeps its name and means "controller policy is active".** The
pipeline's existence is decoupled from it: `sys->multi_comp`, the service window, the panel
DP and the render thread are created at the first non-relay/non-controller client (or at
service start under `--workspace`) and destroyed only in `system_destroy`. Activate/deactivate
become **policy switches**; nothing is rebuilt. This keeps every `!workspace_mode` gate on
the mode writers correct as written.

**D-2 Presenter strategies are per client, one active presenter under the default policy.**

| Presenter | Who | Swap chain / target | Created on | Presented by |
|---|---|---|---|---|
| `SERVICE_WINDOW` | hosted (NULL HWND), WebXR/Chrome, workspace | `mc->swap_chain` on `mc->hwnd` (fullscreen, native res) | pipeline start | render thread |
| `APP_HWND` | `_handle` forced-IPC (real HWND) | per-client `CreateSwapChainForHwnd(app_hwnd)` sized to the client area | the client's IPC thread (DXGI/WM-deadlock rule) | render thread |
| `CLIENT_TEXTURE` | ADR-029 client-presents (`transparent + HWND`) | the client's shared NT texture + fence, no swap chain | the client's IPC thread | render thread weaves + signals; app presents |
| `SELF` | present-owner (`XR_DXR_weave`, displayxr-browser) | none — the client weaves synchronously through the panel DP and presents itself | `weave_bind_window` | nobody; counts as *presenting* from its first weave so it can hold focus/lease |

`PRESENT_OWNER` (#625) keeps its own synchronous submit path (#965); the pipeline only binds the
panel DP to its window while it is focused.

**Never block the render thread on a foreign window.** `APP_HWND` swap chains are created with
`FRAME_LATENCY_WAITABLE_OBJECT` + per-chain `SetMaximumFrameLatency(1)` (`DXR_APP_HWND_LATENCY`;
never the device-wide call) and are presented only after a zero-timeout probe of the waitable (the
#924 token model; the loop paces on the active presenter's waitable). Measured: `Present(1)` on an
occluded cross-process flip chain throttles to DWM's drain rate for that window — 138–626 ms per
call, growing — because DWM does not consume flips of an occluded foreign window; sync-interval-1
`DO_NOT_WAIT` does not help (it only guards a full queue). Unfocused `APP_HWND` windows get a
probe-gated flat 2D repaint with a 1 s backoff on occlusion/failure.

**D-3 Default presenter policy (no controller).** The render thread runs a *direct path*
each frame: focused presenting slot → its presenter; the panel DP is bound to that
presenter's HWND; the client's atlas is cropped (ADR-030) and woven straight into the
presenter's back buffer (`SERVICE_WINDOW`: fullscreen at native res; `APP_HWND`: the app's
window at its client size — i.e. what standalone does today, executed by the render thread
with the shared DP). No compose pass, no chrome, no cursor. Non-focused `APP_HWND` presenters
get a **flat 2D** present (view 0, no DP) so their windows are neither frozen nor woven;
non-focused hosted clients are simply not shown (the service window shows the focused one).
The service window is visible only while it is the active presenter (or under a controller).
Under a controller the existing compose path runs unchanged (service window presenter,
DP bound to `mc->hwnd`).

**D-4 One DP per panel, re-bound on presenter change — never freed synchronously.** `mc->display_processor` is bound to
`mc->panel_dp_hwnd`. When the active presenter's HWND differs (focus moves between an
`APP_HWND` client and a hosted one, or a controller attaches/detaches) the render thread
creates a new DP bound to the new HWND and retires the old one (async create flat-blits
meanwhile). Follow-up: an append-only D3D11 vtable
slot 20 `set_window(HWND)` → plug-in calls `setWindowHandle`, making the re-bind free
(#964 follow-up issue; ABI append is legal, runtime degrades to recreate when absent).
Re-bind is *create → configure → publish → retire*: the old DP is pushed to a small graveyard
(its lens vote dropped first) and destroyed by the render thread after `DXR_DP_GRAVEYARD_MS`
(2 s). Every DP call is bounded, so a stale pointer loaded once by an IPC thread stays a live object
without taking `render_mutex` on the eye-pose hot path; the first live round crashed exactly here
(`weave_submit` resolved the DP before the lock; the render thread had freed it — call through a
freed vtable). Sites that hold `render_mutex` anyway resolve the DP inside it.

**D-5 Focus authority is the compositor slot table always.** The "newest presenting client"
default rule moves into the compositor (first committed frame under the default policy →
`focused_slot`; on unregister → newest remaining presenting slot). The IPC mainloop mirror
(`sync_focus_from_compositor`) runs whenever `multi_comp` exists, not only under
`controller_policy_locked`; the IPC-side default rule is disabled whenever the compositor is authoritative
(`compositor_owns_focus_locked` = controller policy, or the D3D11 pipeline is on) — two writers
made focus flap; it stays as the fallback for platforms without this compositor and under
`DXR_LEGACY_STANDALONE`. A focus change re-arms the new holder's recorded mode wish
(`wish_content_mode` / `wish_hw_3d`, recorded at request time even when denied) into its pending
request, so the panel follows the focused client's mode as it did with per-client DP hints.

**D-6 Mode writers keep their thread but target the panel DP under `render_mutex`.**
`service_apply_pending_mode`, `[force_3d]`, deferred-3D, zones tier-1 and
`weave_force_3d_if_needed` call `apply_mode_transition()` (the #961 seam) with
`panel_dp(sys)` = `mc->display_processor`, wrapped in `render_mutex_fair_lock` (legal:
`c->mutex → render_mutex`). #966 then moves them behind the S3 queue for latency; this note
only removes the data race on the shared DP. `client_holds_panel_lease` is unchanged
(`!workspace_mode && c->state_focused` — the mirror keeps `state_focused` = compositor focus).
The #814 failsafe becomes slot-based: when the last presenting slot leaves under the default
policy, `apply_mode_transition(as_service, 2D)`; the `sys->all_clients` survivor scan is legacy-only.

**D-7 Legacy path.** `DXR_LEGACY_STANDALONE=1` (read once at system create,
`sys->legacy_standalone`) keeps today's per-client window/swap-chain/DP path byte-for-byte:
every new branch is gated on `!legacy`. It ships for one release for the motion-to-photon
A/B (`docs/reference/motion-to-photon-levers.md`; the Intel-iGPU present path has no
`present_wait`) and is then deleted together with #963's seam machinery.

**D-6 amendment (#966) and the present-owner contract (#965).** All mode
transitions are now *applied* on the render thread: IPC-thread writers enqueue a
`WS_CMD_MODE_TRANSITION` on the S3 ring and the drain at the top of
`multi_compositor_render` calls `apply_mode_transition` under `render_mutex`.
The lease/veto decision and its denial event stay on the caller's thread, so a
client still learns synchronously whether it was allowed to ask; only the write
to `sys->` mode/geometry and the DP hint move. Identical queued transitions
coalesce. Under `DXR_LEGACY_STANDALONE`, and during teardown when no render
thread is running, the apply stays inline. `WM_CLOSE` no longer calls the vendor
DP from the window thread — it leaves a flag the render thread consumes (#966).
`weave_submit` keeps its synchronous path (zero-lag inline-3D) but is now
contract-bound: serialized by the panel owner, bounded, no mode/geometry
mutation, DP resolved through `panel_dp()` *after* the lock (#965).

**D-8 Not in this slice.** #965 (present-owner through the panel owner), #966 (queue), #967
(comp_multi), the DP re-bind slot, #1002 detection (separate small PR on the same present
site: check `Present`/create HRESULTs for `0x887A0005/6`, log `[DEVICE_REMOVED] reason=`,
stop driving the DP, orderly exit so the connect ladder relaunches).

**D-9 Focus follows the OS foreground window (2026-08-17, David).** No focus hotkeys. Every
client is a first-class window citizen: `_handle` apps their own HWND, hosted clients a
runtime-created taskbar window each (title = app name; minimized when not the presenter;
`WS_SYSMENU|WS_MINIMIZEBOX` survive the borderless-fullscreen style strip or the shell refuses
to Alt-Tab-restore them), present-owners their own window. The render thread samples the
debounced foreground root each frame: a client window in the foreground takes the panel
(`(foreground)`); a freshly launched app that never got OS activation (foreground-lock) takes
it once (`(new-app)`, per-slot `ever_focused`); an unrelated foreground window changes nothing;
the newest presenting slot is the fallback. Losing the panel parks (minimizes) an APP_HWND
window; restoring it takes the panel back. On a chain handover the incoming swap chain is
re-primed (`SetMaximumFrameLatency`) and the pacer token dropped — the frame-latency waitable
is a semaphore and a slot consumed for a chain that then never presents is lost forever
(observed: 14 presents then a dead presenter); a desync watchdog (500 ms probe-miss ⇒ re-prime
+ one unpaced recovery present) self-heals the rest.

**D-9a Holder liveness — the panel goes only to a presenter that can present (2026-09-03,
#939 leg 4).** Two rules close the gap D-9 left for a second immersive client beside a
present-owner (the DisplayXR Browser beside plain-Chrome WebXR). (1) *Raise on grant:* when a
slot takes the panel, its runtime-owned hosted window is raised to the top **without
activation** (`SetWindowPos HWND_TOP|SWP_NOACTIVATE`). A window that wins via `(new-app)`
came up behind whatever the user was in — foreground-lock refused it activation — and DWM
does not drain an occluded cross-process chain: 20 ms presents, park, dead waitable, black.
Restoring an iconic window already did this; raising a visible-but-covered one completes it.
An app's real HWND is never touched. (2) *Yield on stall:* an active `APP_HWND` holder that
earns no present grant for `DXR_HOLDER_STALL_MS` (default 2 s — behind the 500 ms desync
watchdog and the 1 s park, and far above a merely slow ~10-16 fps hosted presenter) yields
the panel to a live present-owner (`(holder-stalled)`), which outranks the foreground rule
because the stalled holder's window is usually the one the user is staring at. The yielded
slot is barred from (c)/(d) for `DXR_HOLDER_STALL_DWELL_MS` (default 3 s), doubling per
consecutive yield (cap 30 s) and reset by a real grant — a dead presenter converges to
"checked rarely", never a slow blink. A present-owner that loses the panel is unchanged: it
keeps weaving through its own ingest DP (#1172) and its `CLIENT_TEXTURE` path drops to flat 2D
(#1208) — it never blacks out, which is what makes it the safe place to hand the panel back to.

**D-10 The workspace is scoped and never exclusive (2026-08-17, David).** The shell composes
ONLY clients launched under it (`DISPLAYXR_WORKSPACE_SESSION=1`, reported by the client in
`xrt_application_info.workspace_session`; `workspace_enumerate/get_client_info` filter).
While the shell runs, foregrounding an outside client's window runs the direct path for that
client (foreground override: panel + lease follow it; workspace state untouched, no
deactivate); foregrounding the workspace window resumes the compose. The workspace empty
state keys on *placed* slots. Window metrics/Kooima: a PLACED slot uses its slot rect; every
unplaced client uses its presenter's rect (the browser under a shell previously got a phantom
centered sentinel box — wrong point of view).

## 3. What changes, by site (grep by name — lines drift)

- `system_create_native_compositor` / `init_client_render_resources`: always TYPELESS atlas +
  dual SRV; never a per-client DP; presenter kind decided from `external_window_handle` ×
  `transparent_background_enabled`; `APP_HWND` swap chain created here (no DP);
  `multi_compositor_ensure_output` + `multi_compositor_register_client` unconditionally
  (relay/controller excluded).
- `multi_compositor_ensure_output`: service window created **hidden**; DP bound to `mc->hwnd`;
  render thread started; never gated on `workspace_mode`.
- `compositor_layer_commit`: after the atlas blit, always return (no IPC-thread
  present/DP); the resize block only resizes the `APP_HWND` swap chain (on the IPC thread)
  and records the client's content dims; hot-switch / `pending_workspace_reentry` /
  standalone lazy init become legacy-only.
- `multi_compositor_render`: `if (!workspace_mode) default_policy_render()` else the
  existing compose path; `Present` HRESULT read (#1002 hook).
- `comp_d3d11_service_ensure_workspace_window` / `deactivate_workspace` / dismiss / ESC:
  policy switches only — hide/show `APP_HWND` windows, re-bind DP, reset `focused_slot`,
  revert pending flip; the DP-destroy / render-thread-join / `mc->suspended` /
  `enrol_standalone_clients_locked` calls become legacy-only. ESC on the service window under
  the default policy = `EXIT_REQUEST` to the focused hosted client (#999 semantics), not deactivate.
- `service_failsafe_hardware_2d_on_teardown`: slot-based (D-6).
- `comp_d3d11_service_get_client_window_metrics` / `_owns_window`: valid for every slot
  (default policy: presenter rect; controller: slot rect); `owns_window` = presenter is the
  service window and no controller — closes #994 for hosted clients under either policy (verify).
- `resolve_eye_display_processor`, vendor-poll cache, `[HEALTH]`: panel DP always.
- `ipc_server_process.c` `sync_focus_from_compositor`: mirror whenever the compositor reports a slot table.

## 4. Live rounds (2026-08-17, SR box, `main @ 32cd9612d` + this branch)

Verified: hosted ×2 + forced-IPC `_handle` ×1 + shell attach/detach + displayxr-browser
(present-owner) + ESC on the service window + legacy gate — one `Multi-comp: display processor
created` per panel, focus follows the newest presenting slot, DP re-binds on presenter change
(`[pipeline] panel DP re-bound to hwnd=…`, ~200 ms flat blit), activate/deactivate rebuild nothing,
`[TERMINATE]`/`[EXIT]` silent, render thread ≈1 ms/frame. Not yet eyeballed with a person in front
of the panel. Lesson: a `[force_3d]` re-assert every 2 s meant the **presenter was starved** (the
render loop ran at 10 Hz on the 100 ms fallback and the SR service saw no weaving, so it dropped
the lens) — not tracking loss; once the loop presents at display rate the lens holds. Known: the
service window is destroyed+recreated (with the DP) after ESC — a hide would be cheaper; the
workspace `Late-weave saturation backoff` governor misreads the hidden service window's waitable.

## 5. Validation (done-when for the slice)

- sim_display first, then SR: hosted cube ×2, forced-IPC handle cube ×2, hosted + handle,
  then shell attach/detach with all of them alive: exactly one `DP created` per panel in the
  Leia log, `[HEALTH]` shows every client, focus follows the compositor, V→2D by the focused
  client is not reverted by the vendor poll, `[force_3d]` never fights, close gauntlet holds,
  `[TERMINATE]`/`[EXIT]` silent.
- `DXR_LEGACY_STANDALONE=1` reproduces today's behaviour exactly (N DPs, per-client windows).
- Motion-to-photon A/B (LL probes) hosted cube legacy vs pipeline, on the NV and the Intel
  present path, before the legacy path is deleted.

**A/B, first pass (2026-08-17, SR box, NV renders / panel on the Intel iGPU, `DXR_WEAVE_LATENCY_CSV`,
R = weave→scanout, 50–70 s per arm, no face):**

| shape | pipeline p50 / p95 | legacy (`DXR_LEGACY_STANDALONE=1`) p50 / p95 |
|---|---|---|
| forced-IPC `_handle` cube (APP_HWND) | **16.6 / 16.6 ms** @ 60 Hz | 30.3 / 31.6 ms |
| hosted cube (SERVICE_WINDOW), run 1 | 33.2 / 33.3 ms | 30.7 / 63.4 ms |
| hosted, alternating repeats ×3 | 33.3, 39.4, 39.2 ms p50 @ ~57 Hz | 62.4, 62.5, 53.4 ms p50 @ ~33 Hz |

The `_handle` shape gains a full refresh (the render thread paces on the app chain's waitable);
the hosted shape is parity-or-better with a much tighter tail (legacy p95 = 2× p50). Caveats:
the box is not a clean lab — an elevated IDE window over the panel throttles whichever window
DWM considers occluded and moved the legacy hosted arm from 31 to 62 ms between sessions; both
hosted arms sit at ≈2 refresh periods where the handle window sits at 1 (fullscreen borderless →
independent-flip on the cross-adapter iGPU path is the suspect, not the compositor: legacy shows
it too); no `D` (prediction-error) rows exist on the service paths. Repeat on a clean desktop and
on an Intel-only present path before deleting the legacy code (#964 follow-up).

## 6. The output-device split (#918 Phase 2b)

On a non-MUX hybrid laptop the panel is scanned out by the iGPU while apps render on the dGPU, so
every woven frame crosses the adapter boundary inside `Present`. The split moves the
**output half** of this pipeline — the presenter swap chains, their RTVs, the panel DP's weave and
the present — onto the scanout adapter, and sends only the DP-INPUT ATLAS across, once per rendered
frame, through a D3D12 cross-adapter heap (`comp_xbridge`). **Default ON since #918 Phase 3**
(ADR-037 §1); `DXR_WEAVE_ON_SCANOUT=0` is the kill switch. PR 3 covered the
DIRECT path; PR 4 added the COMPOSE path, so **both** paths weave and present on the scanout
adapter; PR 5 moved the zones wish mask onto the panel DP's device; PR 6 made the ingress adaptive,
gave each presenter chain its own weave-latency ledger, and closed the DEVICE_REMOVED / watchdog /
teardown gaps.

Why the service is a simpler split than the in-process compositor was: Local2D, zones and the
authored mask all composite into the CLIENT ATLAS pre-DP, and the service never calls
`set_background_2d` — so there is exactly one image to carry, and no plane transport at all.

### The two sources, one bridge

| Path | When | What crosses |
|---|---|---|
| DIRECT | no controller attached | the focused client's cropped atlas |
| COMPOSE | a workspace controller is attached | the crop of `mc->combined_atlas` |

The compose path builds its frame exactly where it always did — every per-client blit, the chrome,
title bars, font, logo, cursor and the ADR-030 crop paint into `combined_atlas` on the APP device.
Only the destination moved: the cropped composite is submitted to the same bridge instance, and
`process_atlas` plus the present run on the output device.

One bridge, two sources, is what made **Option-II (staged) ingress** the right call through PR 3-5:
the submitted texture changes IDENTITY on a focus change *and* on a controller attach, where Option
I would re-open an NT handle each time. PR 6 replaces that with an **adaptive** rule — see below.

`split_active` is therefore Stage A's verdict for the life of the process. PR 3's workspace
**suspend** — which moved the panel DP and the service-window chain back to the app device while a
controller was attached — is deleted, along with its `split_suspend` counter. The panel DP stays on
the output device across attach and detach: one fewer rebind class.

### Transitions

Attach and detach switch the path per frame under the machinery PR 3 already built. The layout
SIGNATURE carries the source (`SPLIT_COMPOSE_SLOT` where the direct path puts the focused slot), so
a transition bumps the generation and slots of the other path's recipe are refused — the panel holds
its last good frame for a frame or two rather than weaving a compose atlas at a client's tile
stride. Both the weave *and* the present are skipped on such a frame: under `FLIP_DISCARD`,
presenting the cleared back buffer instead is the black flash.

The content box changes with the transition (compose = the canvas, direct = the focused client's
crop). The R2 hysteresis sees that as the single step it is — a resize DRAG is what it treats as
churn — so it reallocates the ring once and settles.

### Adaptive ingress (PR 6)

Staging every frame costs one extra full-content copy on the APP device, per frame, forever — to
cover an event that happens when a user changes focus or attaches the shell. PR 3's
rate-normalised A/B put the price on the board: 9.7 ms of iGPU and 5.9 ms of dGPU per weave with
the app device's copy engine still at 254 ms/s.

Adaptive ingress keeps the staging ring allocated as the **per-frame fallback** and lets a source
that has held still be read IN PLACE. Each frame nominates its source
(`comp_xbridge_set_source(nt_handle, key)`); a match with the bound source reads in place, a
mismatch stages. Three rules make it safe, and each of them is load-bearing:

- **Only a texture the RENDER THREAD alone writes may be nominated.** The producer's copy of frame
  N is still running when frame N+1 starts, and the only thing that orders an app-device write
  against it is the F6 back-fence, issued once at the top of `multi_compositor_render`. The
  immediate context is one ordered stream, so that single wait covers every writer that follows it
  *on that thread* — the ADR-030 crop on the direct path, the whole compose pass on the other — and
  covers nothing a CLIENT'S IPC THREAD does. So the eligible sources are the focused client's
  `crop_texture`, `mc->crop_texture` and `mc->combined_atlas`; a client's own `atlas_texture`
  (written by `compositor_layer_commit` on that client's thread) is never nominated and stages.
  `service_crop_atlas_for_dp` hands the atlas back only when the content exactly fills it, and
  every 3D mode crops by construction — so the case that keeps staging is full-screen 2D.
- **The key is per-ALLOCATION, never a pointer.** The allocator recycles addresses; a recycled
  address compared equal would leave the producer reading the previous allocation, which the
  bridge's own D3D12 open is still keeping alive, and bridge stale pixels with nothing able to tell.
- **A superseded open is RETIRED BEHIND THE PRODUCER FENCE, never drained.** The obvious
  alternative is a bounded CPU wait, and the caller is the render thread holding `render_mutex`:
  that is the #925 wedge class. An exhausted retire ring **leaks** the open (counted, `[XBRIDGE]`
  error) rather than waiting — "unreachable" is not a licence to keep such a wait.

And one hysteresis, for the same reason the egress ring has one. A crop texture is reallocated
whenever the content box moves, which during a resize drag is every mouse event, so "key changed ⟹
re-open" would mean one `OpenSharedHandle` per frame through a drag. A key change therefore retires
the old open **once** and only *arms* the new key; the re-open waits for it to hold still for
250 ms. Through a drag there is no bound source at all and every frame stages — exactly the PR 3-5
behaviour, and the right degrade.

`DXR_SPLIT_INGRESS=staged` pins the old behaviour, which is the A/B control the perf claim is
measured against.

### Per-presenter weave latency (PR 6)

`weave_latency_log` correlates a weave timestamp with the flip that carried it, through
`GetLastPresentCount` — and **PresentCount is per chain**. The pipeline fed ONE file-scope log from
three: the service window (compose always, direct whenever a hosted client presents) and every
app-HWND client's own chain. A ring entry recorded against one chain then resolves against
another's statistics, and the residual R that comes out is not a measurement of anything. R is not
just a report — it is what `set_frame_timing` hands the vendor eye predictor.

The ledgers now live on the chain owners: `mc->weave_lat` (site `workspace`) and
`c->render.weave_lat` (site `apphwnd.sN`), resolved from the presenter kind *before* the weave so
mark and `after_present` are the same ledger by construction. `mc->panel_r_ns` carries the panel's
best-known residual — written by whichever chain actually wove AND presented — and is what the DP
is fed, including on the ADR-029 `CLIENT_TEXTURE` path, which weaves through the panel DP and
presents its own texture and so has no chain and no PresentCount of its own. The saturation
governor stays shared: its inputs are the display period and the render-tick interval, properties
of the panel and the loop rather than of a chain.

### The output-device crop

Whenever the slot is larger than the content it holds, that content sits top-left inside the larger
texture — and the DP derives its tile stride from the atlas width, so weaving it directly would
slice every tile at the wrong offset. PR 3 refused those frames. PR 4 crops them on the output
device instead: one same-device `CopySubresourceRegion` of the slot's own content box into an
output-device staging texture, no shader (the blit shaders all live on the app device) and no second
crossing. Counted as `out_crop`.

Two things put a session in that state, and PR 4's own note recorded only the first. The R2
hysteresis parks a **worst-case ring** through an interactive resize, deliberately — that is the
trade the hysteresis exists to make. But a **zones-class client is there on every ordinary frame**:
its slot is worst-case-sized while its content box is the active mode's, so the output-device crop
is that session's steady state rather than a resize artefact (PR 5 measured `out_crop=600` per 10 s
window on the eligible zones arm, with `no_slot=0`). Neither is a fault, and neither costs a second
crossing.

### Presenter-kind eligibility

| Presenter | Eligible | Why |
|---|---|---|
| `SERVICE_WINDOW` | yes | the runtime owns the window and presents it |
| `APP_HWND` | yes | the runtime owns the chain on the app's window and presents it |
| `CLIENT_TEXTURE` | **no** | ADR-029: the destination is a shared NT texture the client opened on the app adapter; shared textures do not cross adapters, and the client presents |
| `SELF` | **no** | present-owner (#625): the client weaves synchronously and presents into its own window |

Ineligibility is structural and permanent for Phase 2b (supervisor ruling D-b); changing ADR-029 is
out of scope.

### The bind key

`mc->display_processor` is keyed on `(panel_dp_hwnd, panel_dp_device)`.

- same device, different window → the free #1008 `set_window` re-point, unchanged;
- same device, same window → no-op;
- **different device** → retire first, then create on the new device (the shape a STALE backend
  already used). The weaver's D3D11 objects belong to the device it was created on, and
  `set_window` re-points a window, not an adapter.

A device crossing happens on exactly one event: focus moving across the eligibility boundary — an
Alt-Tab between a browser (present-owner) and a handle cube, say. A user can do that as fast as they
can press keys, so crossings are limited by a **>= 1 s dwell** and counted as `pipe_dev_rebind`. A
frame that finds the DP still on the wrong device does not weave through it: D3D11 drives a foreign
resource silently rather than erroring, so the frame degrades to the raw copy instead.

### Wrong-device tripwires

A swap chain's device cannot be changed, and D3D11 does not error on a foreign-device resource — it
is silent. So both paths compare before they act: a presenter chain whose `chain_device` is not the
output device is not ready (its owning client's IPC thread rebuilds it — only that thread may, per
the DXGI/WM deadlock rule), and a panel DP whose `panel_dp_device` is not the output device does not
get handed the SRV; the frame degrades to the egress copy. The DP case is a REAL state, not a
should-never-happen: the ≥1 s dwell can legitimately leave the DP one rebind behind. The chain case
lost its cause when the suspend went away, and stays armed as a tripwire.

### Reading it in the log

```
weave placement: render='…' LUID=…, panel scanout='…' LUID=… — weave/present on the SCANOUT adapter (split=1) (#918)
#918 output-device split ACTIVE: …
[RENDER] split=1 xb_kb=… xb_degraded=0 pipe_dev_rebind=0 flat_skip=0 maskpub_skip=0 no_slot=0
         out_crop=0 ingress=adaptive ing_direct=598 ing_staged=0 ing_rebind=2 ing_churn=0
         ing_leak=0 window_s=10
```

`no_slot` counts frames that skipped both the weave and the present because nothing of the current
layout generation had landed — warmup, a mode switch, a focus change, or a controller attaching or
detaching.

`out_crop` counts frames the output-device crop rescued from a slot whose box is smaller than the
ring holding it. Two populations, and PR 4's note named only the first:

- **an interactive resize**, where the R2 hysteresis parks the ring at worst-case on purpose;
- **every frame of a zones-class session**, structurally — a zones client's slot is worst-case
  (`1920x2160` on the reference panel) while its content box is the mode's (`1280x720`), so the
  crop is the steady state there rather than an artefact. Measured `out_crop=600` per 10 s window
  on PR 5's eligible zones arm. It costs one same-device sub-rect copy and is not a fault.

`flat_skip` counts unfocused app-HWND courtesy repaints skipped under the split (supervisor ruling;
those windows are parked anyway). `maskpub_skip` is a **tripwire and should read 0** — see the
zones sideband below.

The `ing_*` terms are adaptive ingress. `ingress=adaptive` with `ing_staged=0` is the steady state;
a standing `ing_staged` in a session nobody is Alt-Tabbing means a source that never binds (a
full-screen 2D atlas submission, or a share the producer refused). `ing_rebind` counts SETTLED
source changes and `ing_churn` counts key changes that never became one — a resize drag reads as
churn climbing with rebind flat, then one rebind when it settles. **`ing_leak` is a tripwire and
must read 0**: it counts superseded opens dropped without release because the retire ring was full,
which the settle hysteresis makes unreachable.

### The zones sideband follows the DP, not the output half (PR 5)

`XR_DXR_display_zones` and the `XR_DXR_weave` v8 wish publish the same thing: a ~64 KB `R8_UNORM`
binary mask (ADR-027 D5) handed to the panel DP as a *hardware control signal*. It never crosses
the bridge and it never touches a woven pixel, so nothing about it wants the scanout adapter — it
only ever wanted **whichever device the panel DP is on**, which is the DP's bind key, not the
output half.

Those two diverge exactly where the eligibility table above does. A `CLIENT_TEXTURE` or `SELF`
presenter keeps its DP on the APP device while the split is engaged, so PR 2's `svc_out_*` raster
and PR 3's device comparison meant every publish from such a client was refused. The cost was
worse than the PR 3 note recorded: both tier-1 blocks demote the whole-panel hardware 3D request
on the strength of the DP merely *accepting* the mask slot (`mask_capable`), and neither can see a
device skip — so a refused publish left the panel driven by **neither** the wish nor the
whole-panel request, not "driven whole".

PR 5 rasters and publishes on `svc_zone_mask_device()` = `mc->panel_dp_device`: the out device for
an eligible presenter, the app device for an ineligible one, and nothing is refused on either side
of the boundary. Two things make that safe:

- **The device is a dirty-check term.** `wish_mask_device` sits beside `wish_mask_w/h` on the
  client. Without it an unchanged rect list short-circuits the raster after a device-crossing
  rebind and hands the fresh DP an SRV from the device it no longer runs on — which D3D11 does not
  diagnose.
- **The DP and its device are resolved under one lock.** The zones publisher now re-resolves
  `panel_dp()` inside `render_mutex` (it used to resolve pre-lock, leaving a window for the render
  thread's rebind); the weave publisher was already inside it.

`maskpub_skip` therefore keeps its counter and changes meaning: from a reachable degrade to an
unreachable tripwire for a silent cross-device hand-off. **Any non-zero value is a bug** — a call
site that resolved the DP and the device under different locks — and the one-shot WARN says so.

`DXR_SPLIT_COVER_DIAG=1` (observe) or `=2` (sentinel-clear the back buffer first) reports, on both
paths, whether the DP actually covered the output-device back buffer and whether a black frame was
already black upstream.

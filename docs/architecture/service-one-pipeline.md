---
status: Active (design note for #964; Phase 3 of ADR-035, epic #974)
owner: David Fattal
updated: 2026-08-17
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
of the panel (the SR lens auto-drops to 2D on tracking loss, so the `[force_3d]` 2 s re-assert
cadence in an empty room is expected). Known: the service window is destroyed+recreated (with the DP)
after ESC — a hide would be cheaper; the workspace `Late-weave saturation backoff` governor misreads
the hidden service window's waitable.

## 5. Validation (done-when for the slice)

- sim_display first, then SR: hosted cube ×2, forced-IPC handle cube ×2, hosted + handle,
  then shell attach/detach with all of them alive: exactly one `DP created` per panel in the
  Leia log, `[HEALTH]` shows every client, focus follows the compositor, V→2D by the focused
  client is not reverted by the vendor poll, `[force_3d]` never fights, close gauntlet holds,
  `[TERMINATE]`/`[EXIT]` silent.
- `DXR_LEGACY_STANDALONE=1` reproduces today's behaviour exactly (N DPs, per-client windows).
- Motion-to-photon A/B (LL probes) hosted cube legacy vs pipeline, on the NV and the Intel
  present path, before the legacy path is deleted.

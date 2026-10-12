---
status: Active (design note for #967; Phase 3/5 of ADR-035, epic #974). D-4/D-5 re-scoped 2026-08-18 as an opt-in Android **workspace overlay mode** by ADR-036 / epic #1031 — the Android default is the per-window path. D-2/D-3/D-4/D-5 implemented 2026-10-12 (§5).
owner: David Fattal
updated: 2026-10-12
anchors: main @ 32cd9612d
code-paths: [src/xrt/compositor/multi/, src/xrt/compositor/null/null_compositor.c, src/xrt/targets/common/target_instance.c]
---
# comp_multi (macOS / Linux / Android) — one pipeline, bounded waits (#967)

Companion to [service-one-pipeline.md](service-one-pipeline.md) (the Windows D3D11 side of
[ADR-035 D3/D8](../adr/ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md)).
This note records the survey that scoped #967 and the resulting issue split; it is the
spec for that work.

## 1. Facts (grep by name; anchors drift)

- **`comp_multi` is the production system compositor off Windows.** Its single entry point
  is `null_compositor.c` (`comp_multi_create_system_compositor`), selected by
  `target_instance.c` whenever `XRT_D3D11_SERVICE_ONLY` is not set. The header banners in
  `comp_multi_interface.h`, `comp_multi_private.h`, `comp_multi_system.c`,
  `comp_multi_compositor.c` ("reachable only via null_compositor.c (headless testing) …
  does NOT affect workspace-mode behavior") are **false** on macOS/Linux/Android.
- **Per-platform target service** (`null_compositor.c`, `create_from_window`): Windows
  (fallback only), Android, macOS. **Linux desktop has no branch** → `target_service` is
  NULL → `render_per_session_clients_locked` bails before any per-client render; Linux
  runs the plain multiplex path (`transfer_layers_locked` → `xrt_comp_layer_commit` into
  the native VK compositor). Linux is touched by the bounded-wait work only.
- **One main loop** (`multi_main_loop`): predict → broadcast timings → wait → begin →
  `[lock] transfer_layers_locked [unlock]` → `layer_commit` → `[lock] render_shared_surface_locked
  (macOS) | render_per_session_clients_locked (else) [unlock]`. **All per-client record,
  submit, fence wait, DP `process_atlas` and present run under `list_and_timing_lock`**,
  serially for N clients; `multi_compositor_create/destroy` take the same lock (lock order
  `list_and_timing_lock → slot_lock`).
- **macOS shared surface** (`render_shared_surface_locked`, ~680 lines): one service-owned
  fullscreen NSWindow target (`shared_surface_init`, window handle NULL), one
  `msc->shared_dp`, one atlas; N clients composited by workspace pose (controller-driven;
  an unplaced lone app falls back to the full display rect); painter's sort far→near; focus
  is a glow tint only; `tile_columns=2, tile_rows=1` hardcoded ("M1 → always stereo");
  **`request_display_mode` never reaches `shared_dp`** (per-session render is never
  initialised on macOS, so `multi_compositor_request_display_mode` returns early) — no
  hardware 2D/3D channel exists on macOS. Present: `vkQueueSubmit` → `vkWaitForFences(UINT64_MAX)`
  → `comp_target_present`, on the main loop, under the lock. `delivered` is deliberately not
  retired = stale-frame reuse (#922 semantics) already.
- **Android per-client** (`render_per_session_clients_locked` → `render_session_to_own_target`):
  every client has its own `comp_target`/swapchain/atlas/**DP** (`multi_compositor_init_session_render`,
  DP factory marshalled to the main Looper), `process_atlas` + present per client — all
  over the **one process-global `ANativeWindow`** (`android_globals`). Two clients today =
  two swapchains on one window (last-writer flicker) + N DPs ⇒ N vendor contexts ⇒ N lens
  hints (the same fight the D3D11 side had). `end_session` on Android **pauses** the DP
  instead of destroying it (CNSDK core release joins a thread that never exits —
  leia-plugin#39); non-Android destroys inline.
- **Unbounded waits** (all `UINT64_MAX`, main loop, lock held unless noted):
  `render_session_to_own_target` previous-frame fence; post-submit guard before present;
  `recreate_session_swapchain` (all buffers + **`vkDeviceWaitIdle`**); macOS shared previous
  frame + pre-present; `shared_surface_fini` `vkDeviceWaitIdle`; `end_session` and client
  destroy (IPC thread); `comp_multi_weave_macos.c` present-owner weave + `vkQueueWaitIdle` on
  import/teardown (IPC thread). Untimed condvars in `layer_commit`/`wait_for_scheduled_free`.
  **The bounded pattern already exists**: `wait_fence`/`wait_semaphore` in
  `comp_multi_compositor.c` (100 ms timeout + WARN + retry-while-running). No `MULTI_*_TIMEOUT`
  constants exist.
- **No focus authority in comp_multi**: `mc->state.{visible,focused,z_order}` are event-only;
  `sync_focus_from_compositor` (`ipc_server_process.c`) reads
  `comp_multi_workspace_get_focused_client` on macOS and **NULL** elsewhere, and returns early
  without a controller — nothing sets focus on any comp_multi platform without a shell.
- **`MULTI_MAX_CLIENTS 64` overflow is silent by comment** ("just ignore it"): the client
  compositor is constructed and returned but never enters `msc->clients[]` — it renders
  nothing, forever, with no log.

## 2. Decisions

- **D-1 Truth first.** Fix the four "headless testing" banners and make slot exhaustion
  *fail the create* (`XRT_ERROR_*` + `U_LOG_E`) — an app must not get a session that never
  renders. No behaviour change otherwise. All platforms.
- **D-2 Bound every wait, all platforms** (S1/S4): the eight `vkWaitForFences(UINT64_MAX)`
  + `vkDeviceWaitIdle`/`vkQueueWaitIdle` sites adopt the in-tree 100 ms-retry pattern; a
  per-client deadline marks a client stalled and *skips* it (reusing macOS's non-retiring
  stale-frame semantics) instead of blocking the loop; DP destroy never runs under
  `list_and_timing_lock` on the render thread (Android precedent: `on_pause`, defer destroy
  to client teardown — CNSDK #39).
- **D-3 GPU work leaves `list_and_timing_lock`** (#924): snapshot the render set under the
  lock, drop it, then record/submit/DP/present. macOS first (one submit+present per frame,
  local change); Android after D-4. A per-client render lock, if needed, sits *below*
  `list_and_timing_lock → slot_lock`.
- **D-4 Android WORKSPACE OVERLAY MODE** (opt-in via a workspace controller; shared surface
  + one DP + mode channel). **Re-scoped 2026-08-18 by [ADR-036](../adr/ADR-036-android-per-window-compositor-instances.md)
  / #1031: this is no longer the Android default.** The Android **default is the per-window
  path** — every app keeps its own Surface, its own compositor instance and its own DP, and
  SurfaceFlinger composites them (ADR-036 D1/D2/D3). The shared-surface shape is entered
  **only when a registered workspace controller activates it**
  ([workspace-controller-registration.md](../specs/runtime/workspace-controller-registration.md)),
  exactly as on Windows — a spatial-desktop overlay, not a replacement for the platform's
  window manager.

  Mechanically unchanged when that mode *is* active: de-`#ifdef` the macOS shared block in
  `comp_multi_private.h`, generalise target acquisition (NSWindow ←
  `comp_target_service_create(…, NULL, …)` vs `ANativeWindow` ← `android_globals`), one atlas,
  **one DP**, N clients composited at controller-supplied poses. What changes vs the original
  plan: the per-client target/DP path on Android is **kept**, not deleted — it is the default
  — so the shared path is added *alongside* it and the two are selected by controller
  presence (keeping the Android pause-not-destroy rule in both). The **default presenter
  policy + focus rule** of `service-one-pipeline.md` D-3/D-5 (focused = newest committed
  client, shown full-screen at native res in its requested mode; compositor slot table is the
  focus authority; IPC mirrors) applies **within** the overlay mode, deciding what fills the
  shared atlas; with no controller there is no shared atlas to fill.
- **D-5 Shared-DP mode channel** (2D/3D + tile grid on `shared_dp`, currently absent on
  macOS and inherited by the Android overlay mode) is a distinct gap: route
  `multi_compositor_request_display_mode` to `shared_dp` under the panel-lease rule when
  the shared surface is active. Ships with D-4 (the overlay mode needs it for hardware 3D on
  Android); macOS benefits for free. In the per-window default the mode channel stays where
  it is today — per-client DP — with the display-global lens state refcounted across
  instances (ADR-036 D1).

## 3. Issue split

| Sub-issue | Scope | Platforms | Risk |
|---|---|---|---|
| #967a | D-1: header truth + loud `MULTI_MAX_CLIENTS` overflow | all | low |
| #967b | D-2: bounded waits + per-client stall deadline | all | medium |
| #967c | D-3: GPU work out of `list_and_timing_lock` | macOS, then Android | medium |
| #967d (#1006) | D-4 + D-5: Android **workspace overlay mode** (opt-in via a registered workspace controller) — shared surface + one DP + presenter policy + shared-DP mode channel. **Not the Android default**; the per-window path (ADR-036, epic #1031) is | Android (macOS gains the mode channel) | high; needs device validation |

Order: a → b → c(macOS) → d → c(Android). #967a/b can ship before #964 settles; c/d take
the presenter-policy shape from `service-one-pipeline.md` once it is live-validated on Windows.
D-1/D-2/D-3 are unaffected by the ADR-036 re-scope — they help every path, per-window and
overlay alike — and are no longer blocked on deciding the Android shape.

## 4. Done-when

- **D-1/D-2/D-3 (all platforms):** banners tell the truth and slot exhaustion fails the create
  loudly; no unbounded wait remains on the main loop, and a stalled client is skipped rather
  than blocking the loop; GPU work and present run outside `list_and_timing_lock`. macOS
  unchanged behaviourally; Linux unaffected beyond bounded waits.
- **D-4/D-5 (Android overlay mode, #1006):** with a registered workspace controller active,
  two IPC clients are both visible (composited into one atlas through one DP) and killing one
  does not stall the other's frames; the shared DP accepts mode requests under the panel
  lease. **The per-window default is unaffected** — with no controller active, each client
  still renders to its own target with its own DP, and that path must show no regression.

## 5. Implementation (2026-10-12)

What landed, and how each done-when was shown on an NP02J (Leia CNSDK release build). §1
describes the code as it was *before*; read this section for the current shape.

**D-2 bounded waits (#1007).** `multi_wait_fences_bounded()` (100 ms slices,
`MULTI_FENCE_TIMEOUT_NS`, up to `MULTI_FENCE_DEADLINE_NS` = 500 ms) replaces every
`vkWaitForFences(UINT64_MAX)` on the main loop and the teardown paths. A client whose fence
misses the deadline is marked `gpu_stalled`, its frame is skipped with the fence kept
pending (stale-frame reuse, #922), and from then on the fence is only polled, so it costs
the other clients nothing per frame; the shared surface has the same rule. `vkDeviceWaitIdle`
in `recreate_session_swapchain` and `shared_surface_fini` became a bounded fence on an empty
submit to the main queue. `comp_target::acquire_timeout_ns` (0 = infinite, the default for
every other user) bounds `vkAcquireNextImageKHR` at 500 ms. Teardown that times out leaks
its Vulkan objects rather than freeing memory a wedged GPU may read.
`android_run_on_main_thread_timeout()` abandons a main-Looper task that has not started
within 2 s (heap-refcounted, never run late against a dead stack frame); both DP factory
calls use it. The session-DP destroy unpublishes the pointer under the locks before
destroying it with the locks dropped (the old in-place destroy was a use-after-free against
the Android convergence pass). Not touched: each client's own wait thread, and the macOS
present-owner weave.

**D-3 GPU work out of `list_and_timing_lock` (#1005).** New `msc->render_lock`; the order is
`render_lock -> list_and_timing_lock -> slot_lock`, written up as `@page comp_multi_locking`
in `comp_multi_interface.h`. The main loop holds `render_lock` for the whole frame and takes
the list lock only to transfer layers and snapshot `msc->clients[]` into `msc->render_set`,
then again to apply the retires the render recorded (they touch the app pacer). Client
destroy unlists first under both locks, then tears down with none held.

**D-4 Android workspace overlay mode (#1006).** The gate is split:
`COMP_MULTI_SHARED_SURFACE` = shared surface compiled (macOS, desktop Linux, Android),
`COMP_MULTI_PER_SESSION_TARGET` = per-session path compiled (!macOS && !desktop Linux).
`multi_shared_surface_active(msc)` is a compile-time `true` on macOS / Linux (unchanged
builds) and `msc->shared_mode_active` on Android, which the render thread flips at the top
of a frame when a verified controller activates or deactivates (`android_overlay_transition`).
Entry and target are described in [service-architecture.md §7b](service-architecture.md#7b-android-workspace-overlay-mode-adr-036-d4-1006)
and the controller contract in
[workspace-controller-registration.md §Android](../specs/runtime/workspace-controller-registration.md#android-workspace-controllers-1006).
Measured: one main-service process, one `created shared display processor`, two IPC cubes
composited side by side in the controller's surface in landscape and portrait; one cube
force-stopped and the other kept rendering at 60 fps through the surface; deactivate
released the shared target, paused the shared DP and resumed per-session rendering, and a
new client got a satellite again. The per-window default (two forced-IPC cubes on `:dxr0`
/ `:dxr1`, a DP each) shows no change.

**D-5 shared-DP mode channel.** While a controller is active it is the display-mode
authority (apps see their modes as not requestable — the existing contract), so the channel
that matters is the controller's: `workspace_request_display_mode` gained a shared-surface
arm that resolves the mode's hardware flag and hands it to the render thread
(`comp_multi_system_request_workspace_display_mode` -> `shared_converge_display_mode_locked`),
which drives the one DP and tells every composited client. Measured: controller mode 0 ->
the vendor lens off, mode 1 -> on. With no controller wish the focused / lone client's own
wish still governs, as before.

**Measured risks.** R1: a content Activity paused behind the fullscreen controller has its
session stopped by the client runtime (ON_PAUSE -> STOPPING) and many apps also stop their
frame loop without a window. Overlay members are exempt from the former (as #558 overlay
apps are); the latter is an app rule — keep rendering while the runtime reports the session
VISIBLE / FOCUSED. R2: a per-session DP was created while the paused shared DP was alive in
the same process and both vendor cores initialised — creation only, not a long-run test;
the shared DP itself is held back while any per-session DP is alive in its process.
R3: macOS was not built (CI covers it). R4: no deadlock seen; the lock audit is in the
#1005 commit.

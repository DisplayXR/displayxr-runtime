---
status: Accepted
date: 2026-10-14
deciders: David Fattal
issues: ["#2095"]
related: [ADR-007, ADR-020, ADR-038]
---

# ADR-055: Weave synchronisation is GPU-side — semaphores at every layer, no host drains

**Status:** Accepted (2026-10-14, project-owner decision on #2095 after the CNSDK 0.10.78
measurements) · builds on [ADR-007](ADR-007-compositor-never-weaves.md) (the compositor
never weaves; the display processor does) and
[ADR-020](ADR-020-plugin-abi-compatibility-policy.md) (the slot it rides on is an
append-only addition within ABI major 5) · Android hosting:
[ADR-038](ADR-038-android-vendor-plugin-ships-in-the-runtime-apk.md).

## In one paragraph

The weave is a GPU stage between the app's view rendering and the present. Today the
in-process Vulkan path orders it with host waits — queue drains and a synchronous frame
fence — that were written when the vendor weaver submitted on its own queue and were never
re-examined when it moved onto the runtime's. On an in-order queue those waits now drain the
app's *next* frame along with the weave, costing 10–40 % on GPU-bound apps, and on a second
queue they order nothing at all. This record makes the ordering explicit: every GPU
dependency between app, weave and present is a Vulkan semaphore, at every layer (runtime,
vendor SDK, vendor plug-in), and no thread that commits a frame waits on the host for GPU
completion. The browser path already works this way; the in-process path joins it.

## Context

### Three kinds of display-processor submission

ADR-007 fixed who weaves: the display processor (DP), on a Vulkan device and queue the
runtime hands it at factory time. It did not fix *how the DP's work joins the runtime's*.
Three shapes exist on `main`:

1. **Recording DPs** — `sim_display`, the desktop SR weaver — record into the command
   buffer the runtime passes to `process_atlas`. Ordering is free: one batch, one submit.
2. **Self-submitting on its own queue** — the legacy CNSDK initialisation. The DP creates
   its own queue and submits there. Ordering against the runtime's batches was never
   expressed; it held by accident, because on Adreno one hardware queue and a single GPU
   context execute everything in submission order.
3. **Self-submitting on the runtime-owned queue** — CNSDK 0.10.78 `initialize_with_queue`
   (leia-plugin#361, runtime#2084). The DP submits on the queue the runtime gave it; #2012
   settled which one (it captures `repaint_queue`).

### What the in-process path did about it

The in-process `vk_native` commit path (every Android app that is not the browser; the
production path there, ADR-038) synchronised the weave with **host waits**:

- an acquire-side dummy submit followed by `vkQueueWaitIdle`;
- a pre-DP `vkQueueWaitIdle(queue)` so the DP's first submit could be trusted to see the
  runtime's pre-weave work;
- the #837 post-weave drain, `vkQueueWaitIdle(repaint_queue)` (`dxr_postwait_queue_only`,
  default ON since 2026-08-31), whose stated premise was that the DP's queue was otherwise
  idle, so draining it cost nothing;
- a synchronous wait on the frame fence before the commit thread moved on.

Each was correct for shape 2 and cheap under it. Shape 3 broke the premise: the queue being
drained is now the app's queue. With the non-blocking hand-off (#1905, #1962) the app has
already submitted frame N+1 when the commit thread submits weave N, so draining weave N
drains frame N+1 too, and the app's frame interval stretches to the weave's.

### Measured (#2095)

2026-10-14 bundle (CNSDK 0.10.78, queue path) against the 2026-10-13 bundle (0.10.75,
legacy init), floors asserted, warm re-runs:

| app | tablet A | tablet B |
|---|---|---|
| model_viewer | 46.1 → 27.3 fps | 53.8 → 42.5 |
| gaussiansplat | 46.3 → 44.9 | 54.4 → 46.9 |
| earthview | 59.8 → 49.8 | 59.9 → 44.7 |
| mediaplayer / avatar / browser | unchanged | unchanged |

The runtime's own diagnostic named the mechanism: `non-blocking hand-off -> BLOCKING: over
32 frames the commit drain exceeded P = 16.67 ms on 32 (mean drain 41.08 ms)`. No weave
errors, no thermal throttling, tracker CPU 3–18 % of one core; a split with runtime v2.41.1
on the same 0.10.78 services reproduced the old numbers, so the services are not the cause.
No shipped knob recovers it — `weave_postwait_queue=0` buys 4 fps on one device,
`async_handoff=0` and `midframe_fill=0` make it worse. The GPU-bound apps lose; the apps
that were already waiting on something else (video pacing, a 48 fps avatar) do not.

### Why this is a correctness gap, not only a regression

The in-process path hands the DP **no semaphores**. On a second queue with NULL/NULL there is
no Vulkan ordering between the app's view rendering and the weave that samples it, nor
between the weave and the present. Shape 2 got that ordering from same-queue submission and
Adreno's single context hides the hole today; a vendor with two hardware queues, or a
desktop Vulkan DP that self-submits, would expose it as tearing between views.

### The precedent already in the tree

The browser path — `XR_DXR_weave` v20, `comp_multi_weave_android.c` — was built the other
way round. It imports the caller's acquire sync_file into a semaphore, chains
`pre_sem → DP → dp_sem` through `xrt_display_processor_vk::set_weave_sync`, signals an
exportable release semaphore from its final batch, and returns without a CPU wait; every
case it cannot chain (no SYNC_FD, a satellite presenter, a DP that declines the slot) falls
back per frame to the synchronous behaviour, logged once per mode change. It measures
59.9 weaves/s on both tablets under 0.10.78 — unaffected. The slot's contract (what a DP
commits to on `true`, what the runtime does on a dropped frame, binary now with timeline
values reserved) is already written and already shipped as an ADR-020 append.

## Decision

**The weave is a pure GPU stage.** Its inputs are made ready by a semaphore, its output is
consumed through a semaphore, and nothing on the commit thread waits for the GPU. The
contract is stated per layer so that no layer can reintroduce a host wait on another's
behalf.

### 1. Runtime (the in-process `vk_native` commit path)

- The pre-DP batch (view copies, crop, Local2D under-layer) **signals a wait semaphore**:
  "views ready". No queue idle before the DP.
- `set_weave_sync(dp, wait_sem, 0, signal_sem, 0)` arms the DP immediately before
  `process_atlas`, per the slot's contract (only `set_target_color_view` may intervene).
- The post-DP batch (Local2D over-layer, HUD, the COLOR→PRESENT barrier) **waits the DP's
  signal** and **signals a present semaphore**; `vkQueuePresentKHR` waits that.
- The frame fence becomes a **ring** that bounds frames in flight; the commit thread
  recycles the slot whose fence has retired and never blocks on the current one.
- **App swapchain images are protected by backpressure, not serialisation.** At
  `xrWaitSwapchainImage` the app waits the fence of the commit that *read* that image —
  which only ever blocks when the app laps the compositor. The app's frame N+1 is never
  held behind weave N.
- **Dropped frames** fall back exactly as the browser path does: when the DP reports
  `get_last_frame_dropped`, the runtime treats the signal as not guaranteed, waits on the
  CPU for that frame, drains, and destroys and recreates both semaphores before the next
  `process_atlas`.
- **A DP that lacks or declines the slot** keeps the host-wait path for its frames, logged
  once per mode change — the same shape as the browser's `FALLBACK_DP_DECLINED`. Recording
  DPs (shape 1) need no slot and no fallback: one batch orders itself.

### 2. Vendor SDK

**No host wait inside the weave call.** A weave that blocks on its own previous frame is a
host drain under another name and defeats the chain from inside. For CNSDK that means
ringing the per-`RenderState` fence and the per-in-flight uniform buffers so a weave can be
submitted while the previous one is still executing (tracked on the CNSDK side; a
prerequisite for full pipelining, not for correctness).

### 3. Vendor plug-in

**One initialisation path: queue plus semaphores.** The DP takes the runtime's queue
(`initialize_with_queue`) and implements `set_weave_sync` — wait before the first atlas
read with a `dstStageMask` that covers the reading stage, signal after the last target
write, both in the submits it issues, and **both even on a dropped frame** (an empty submit
that only waits and signals; a failed vendor submit never signals on its own). The legacy
own-queue path is retired once §1 and §2 are in; a plug-in does not keep two init paths so
that one of them can keep ordering by accident.

**Every DP submit goes on the queue the runtime handed it — not only the weave passes.** A
self-submitting DP issues more than the vendor weave: the semaphore relays that pay
`set_weave_sync`, a compose-under pass, an alpha gate, a mono blit, one-time uploads. In the
out-of-process service the bundle's main queue *is* the weave queue, so reading it live is
harmless there; in-process it is the **app's** queue, which the app has already loaded with
its next frame by the time the DP's out-relay is submitted. A relay on that queue signals
"weave done" only after the app's next frame renders, and the DP's own reclaim of that
relay's fence then blocks the commit thread for exactly the frame §1 stopped draining. So
the DP captures the queue once at factory time and routes **all** of its submits and drains
through it (leia-plugin#374).

### Queue choice becomes a knob

Once ordering is explicit, *which* queue the DP submits on is a tuning decision, not a
correctness one. Same queue costs nothing on hardware with one queue (Adreno) and is the
default there; a second queue is worth trying where the hardware has one and the weave
would otherwise sit behind a long app batch. Neither choice may change the semaphore
contract.

### Semaphore form

Binary semaphores, each waited and signalled exactly once per `process_atlas`, are the only
form used. Timeline values are reserved by the slot's signature (`wait_value` /
`signal_value` non-zero) and a DP that does not support them returns `false`; moving to
timelines later is an additive change under this record, not a new one.

### Invariants

1. **No host wait on the commit thread for GPU completion.** Fences bound frames in flight;
   they are never waited to order work.
2. **Every GPU dependency between app, weave and present is a Vulkan semaphore** — never a
   queue drain, never a device wait, never an assumption about submission order.
3. **A DP that takes `set_weave_sync` must wait and signal even on a dropped frame.** The
   runtime's recreate-on-drop is the backstop for a vendor weaver whose consumption of the
   wait cannot be known, not permission to skip the signal.

## Consequences

- `dxr_postwait_queue_only`, `DXR_WEAVE_POSTWAIT_QUEUE` and `debug.dxr.weave_postwait_queue`
  are **deleted** with the #837 drain they gated. The drain's premise (an otherwise-idle DP
  queue) is false under shape 3 and irrelevant under this record.
- The non-blocking hand-off (#1962) and mid-frame fill (#1905) stop being compensations for
  host waits and become what they were meant to be: the app renders ahead, the repaint
  thread fills when the app is idle. The "hand-off -> BLOCKING" trip that fired on every
  GPU-bound app is expected to stay silent; if it fires, something is draining again.
- A kill switch exists for A/B only: `DXR_WEAVE_SYNC_INPROC=0` /
  `debug.dxr.weave_sync_inproc=0` restores the host-wait path for the session, logged once.
  It is a diagnostic, not a supported configuration, and it is removed once the vendor
  fence ring has shipped and the numbers are on the record.
- The vendor-side fence ring (§2) is the prerequisite for full pipelining. Until it ships,
  §1 is already correct (ordering is explicit) and already faster (the runtime no longer
  drains the app's frame); the remaining serialisation is inside the weave call and is the
  vendor's to remove.
- Plug-in cleanup follows: the own-queue init path, its queue creation and the
  `is_self_submitting`-era branches that only existed to tell the runtime "I ordered this
  myself" go. The DP's declaration to the runtime is now the slot's return value.
- The ADR-007 boundary is unchanged: the compositor still hands the DP a target and a
  canvas rect and still knows nothing about the weave. What crosses the boundary gains two
  handles, and that was already true since the browser path shipped.
- Trade-off accepted: the ring adds frames-in-flight state to the commit path and a
  recreate-on-drop branch that is exercised only when a vendor submit fails. Both are
  copied from a path that has run in production on the browser; neither is new mechanism.

## Alternatives rejected

- **A plug-in guard that keeps legacy init off the browser path.** Restores the numbers on
  the 2026-10-14 bundle by sending in-process apps back to shape 2. Keeps two DP init paths
  alive, leaves the NULL/NULL ordering hazard exactly where it is, and makes the browser and
  in-process paths diverge further on the one contract they should share.
- **Keep the drains, wait smarter.** Drain only the DP's own submissions (a fence the DP
  hands back), or drain only when the app is idle. Still a host wait on the commit thread,
  still orders nothing on a second queue, and still has to guess which queue the DP used —
  the guess #2012 showed is wrong across SDK versions.
- **Stubbed-library A/B.** Swapping the vendor library for a no-op weaver to prove the cost
  is in the drains. Evidence, not a fix; the knob A/B in #2095 already isolated it.
- **Device-wide waits.** `vkDeviceWaitIdle` or a fence on every queue is the one option that
  is correct on any queue topology — and the one that serialises the most. Correctness by
  stopping the GPU is what this record exists to replace.

## References

- Issues: #2095 (tracking, measurements, decision), #837 (the post-weave drain and
  `dxr_postwait_queue_only`), #868 (weave rate decoupled from app rate), #1905 (mid-frame
  fill, in-process weave at app rate), #1962 (non-blocking hand-off), #2012 (which queue the
  DP submits on), runtime#2084 and leia-plugin#361 (the CNSDK 0.10.78 pin that moved the
  weave onto the runtime's queue).
- Records: [ADR-007](ADR-007-compositor-never-weaves.md),
  [ADR-020](ADR-020-plugin-abi-compatibility-policy.md),
  [ADR-038](ADR-038-android-vendor-plugin-ships-in-the-runtime-apk.md).
- Code: the slot and its contract — `xrt_display_processor_vk::set_weave_sync` in
  `src/xrt/include/xrt/xrt_display_processor_vk.h` (`XRT_DP_VK_HAS_WEAVE_SYNC`); the
  precedent — `src/xrt/compositor/multi/comp_multi_weave_android.c` (`weave_sync_mode`, the
  `pre_sem → DP → dp_sem` chain and its per-frame fallbacks).

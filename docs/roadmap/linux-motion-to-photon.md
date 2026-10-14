# Motion-to-photon levers on desktop Linux — gap analysis and plan

**Status:** analysis + plan, 2026-10-13. Epic **#2059**. Nothing here is implemented yet.
**Scope:** the in-process Vulkan compositor (`src/xrt/compositor/vk_native/`) on desktop Linux,
X11 (`comp_vk_native_window_xcb.c`, `VK_KHR_xcb_surface`) and native Wayland
(`VK_KHR_wayland_surface` on the app's `wl_surface`), on Intel iGPUs (Mesa ANV) and on
NVIDIA (proprietary, including PRIME hybrid laptops).
**Read first:** [`docs/reference/motion-to-photon-levers.md`](../reference/motion-to-photon-levers.md)
covers the levers, their defaults and the present / repaint / weave vocabulary used below.
[`weave-cadence-vs-eye-prediction.md`](../reference/weave-cadence-vs-eye-prediction.md) has the
per-platform table this document extends with a Linux column.

## TL;DR

The levers-doc topology table says late weave and repaint are "on" everywhere. On Linux that is
true of the *switches* and false of the *mechanisms*:

| lever | Linux today | why | smallest fix |
|---|---|---|---|
| **Late weave** | **dormant**, on X11 and Wayland, Intel and NVIDIA | Two problems. (1) The `VK_KHR_present_wait` tier is all `#ifdef XRT_OS_WINDOWS`, even though the runtime already enables `presentId`/`presentWait` on every enable2 device. (2) The vblank-grid tier's only feeder is `VK_GOOGLE_display_timing`, which no Linux desktop driver exposes on a windowed swapchain. | Un-gate the present_wait tier (**S**, #2060), then add grid feeders from `VK_EXT_present_timing` (**M**, #2061) and from `wp_presentation` / X11 Present (**M**, #2062) |
| **Repaint** | **enable2 apps only** on single-queue GPUs | Tier 2 (`VK_LAYER_DXR_queue_lock`) is injected only by `xrCreateVulkanInstanceKHR`. It has shipped on Linux only since v2.42.0 (#2036). enable1 apps (all shipped native demos, #886) have to enable the layer themselves. | A runtime-armed implicit layer (**S/M**, #2063), or the same-adapter fill engine (#2065) |
| **Split / weave-on-scanout** | **absent** | `comp_vk_native_split.cpp` is `XRT_OS_WINDOWS && XRT_HAVE_D3D11`: the transport is D3D11/D3D12 end to end | Same-adapter fill engine with its own `VkDevice` (**L**, #2065), then PRIME dma-buf weave-on-scanout (**L**, #2066) |

The slot partition (#1257) and the #206 forward horizon consume the same vblank grid and are
dormant for the same reason. #2064 adds their Linux arm and one log line that names the live
tiers.

## Measured state on the reference Linux laptop (2026-10-13)

Box: Intel Core Ultra 7 355 / Intel Graphics (PTL), Mesa 26.0.8 (`mesa-vulkan-drivers
26.0.8-1ubuntu0.3`), Vulkan loader 1.4.341, Ubuntu 26.04, GNOME Shell 50.1 on Wayland. A
3840×2160 3D panel on HDMI-1 at 200 %. Runtime 2.40.0 / 2.42.0 (`.deb`), with the plug-in from its
own `.deb`.

| quantity | value | source |
|---|---|---|
| graphics queue family | **1 queue** (plus a compute-only family with 1 queue and a transfer family with 1 queue) | `vulkaninfo` |
| `ANV_QUEUE_OVERRIDE=gc=2` | graphics family reports **2 queues** | `vulkaninfo` with the env set |
| `VK_KHR_present_wait` / `present_id` (+ `…2`) | **present** | `vulkaninfo` |
| `VK_EXT_present_timing` | **absent** (ships in Mesa 26.1) | `vulkaninfo` |
| `VK_GOOGLE_display_timing` | **absent** | `vulkaninfo` |
| `VK_EXT_external_memory_dma_buf`, `VK_KHR_external_memory_fd`, `VK_KHR_external_semaphore_fd`, `VK_EXT_image_drm_format_modifier`, `VK_EXT_physical_device_drm`, `VK_KHR_timeline_semaphore` | all **present** | `vulkaninfo` |
| grid feeder | `#902: display-timing feeder — vk=1 has_ext=0 swapchain=1 get_past=0 get_refresh=0` | runtime log |
| grid late weave | `#902: late weave on the measured grid` **never logged** | runtime log |
| present_wait late weave | not reachable on Linux (no log line exists: the probe that would print `vkWaitForPresentKHR unavailable` is Windows-only) | code |
| repaint, Unity enable2 (2.42.0 layer) | `#902: repaint will submit on the APP's queue …, serialized per-call by VK_LAYER_DXR_queue_lock`. Witness present/s = weave/s = **59.4–60.0**, repaint/s 0 (no free slots), no CPU/GPU change versus no layer | #2039 |
| repaint, native enable1 app, layer off | `#868: repaint loop NOT started`. Witness present/s **49 → 23–28** over 60 s: an app regression plus a tracked viewer and a low GPU clock, not a layer cost | #2039 correction |
| repaint, native enable1 app, layer on | **unmeasured on a fixed app build**: the earlier "layer halves the frame rate" table is withdrawn | #2039 |
| split | absent: no `weave placement:` line on Linux | code |

## Lever 1 — late weave

### What exists, per path

| path | mechanism | file / function | runtime proof |
|---|---|---|---|
| Windows D3D11 / D3D12 | DXGI frame-latency waitable + `GetFrameStatistics` vblank grid; #850 governor | `comp_weave_latency_win.h`, the D3D targets | `[WITNESS]` + `DXR_DP_FORWARD_HORIZON_TRACE` rows |
| Windows VK, multi-queue (NVIDIA) | `VK_KHR_present_wait`: `comp_vk_native_target_acquire` blocks on present id `last − (MAX_LATENCY−1)`; the residual is measured there and fed to `set_frame_timing` | `target_present_wait_fn`, acquire block under `#ifdef XRT_OS_WINDOWS` | no `vkWaitForPresentKHR unavailable` line |
| Windows VK, Intel (DComp bridge) | DXGI waitable on the bridge swapchain, governor-managed | `dcomp_setup` (`DXR_VK_BRIDGE_PACING`) | `#912: pacing the VK bridge on the DXGI waitable, depth N` |
| Windows VK under the split | the D3D11 fill arm's own waitable | `comp_vk_native_split.cpp` | `weave placement: … split=1` |
| Android VK | vblank grid from `VK_GOOGLE_display_timing`, then sleep to `next_vblank − (weave_cost + 1.5 ms)` | `target_feed_vblank_grid`; the grid block in `comp_vk_native_target_acquire` | `#902: display-timing grid — refresh …`, `#902: late weave on the measured grid …` |
| **Linux VK, X11** | grid block compiled, **grid never trusted**; present_wait tier compiled out | same | `#902: display-timing feeder — … has_ext=0` |
| **Linux VK, Wayland** | same | same | same |
| Linux VK, `VK_KHR_display` direct (`DXR_LINUX_DIRECT_SCANOUT=1`) | same today; Mesa 26.2 added `VK_GOOGLE_display_timing` on `KHR_display`, so the existing feeder should light up unchanged there | `comp_vk_native_window_direct.c` | to verify |

Everything that hangs off the grid is dormant on Linux too:
- `comp_vk_native_target_vblank_period_ns()` returns 0, so the repaint loop paces on the assumed `display_refresh_rate`.
- `comp_vk_native_target_weave_to_scanout_ns()` returns 0, so `set_predicted_scanout(0)` and the DP falls back to its own heuristic.
- `get_measured_weave_ns()` and `get_predicted_lookahead_ns()` are stubs that return 0, so `set_frame_timing` gets 0.
- `part_tier_ok` (the #1257 partition gate) has no Linux arm.

### What Linux is missing, and what substrate exists

The runtime needs a measured refresh period, a phase anchor (when presents actually reached the
panel), and ideally a blocking "previous present is on glass" sync point. Five sources exist:

| substrate | what it gives | Intel ANV (Mesa) | NVIDIA proprietary | works for enable1 apps? |
|---|---|---|---|---|
| `VK_KHR_present_wait` (+ `present_id`) | blocking wait until present id N is displayed; no timestamp (time the return) | yes (26.0.8 on this box; Mesa implements it over Present events on X11 and `wp_presentation` on Wayland) | yes (X11; Wayland since ~555, with hang fixes in 575) | **no**: the app's device must enable the features. The runtime already does so for enable2 (`oxr_vulkan.c`) |
| `VK_EXT_present_timing` (Vulkan 1.4.335) | refresh duration, past presentation timings per stage (including first-pixel-out), time-domain query, optional target times | **Mesa ≥ 26.1** (ANV, RADV, NVK; X11 + Wayland). This box has 26.0.8 | reported in **595.58.03+** (secondary source; confirm with `vulkaninfo`) | no (device extension) |
| `VK_GOOGLE_display_timing` | the same retrospective data, older API, which the feeder already reads | not on windowed WSI; **`KHR_display` only, Mesa ≥ 26.2** | not expected on Linux desktop; verify | no |
| Wayland `wp_presentation` (presentation-time) | per-commit `presented(tv, refresh_ns, seq/MSC, flags: vsync, hw_clock, hw_completion, zero_copy)` or `discarded`; clock from `wp_presentation.clock_id` (`CLOCK_MONOTONIC` on Mutter); `wl_surface.frame` gives only a "good time to draw" hint, no timestamp | compositor-side: any driver | same | **yes**: runs on the app's `wl_display` via a private event queue, independent of its VkDevice |
| X11 Present `CompleteNotify` | `ust` (µs, `CLOCK_MONOTONIC`) and `msc` per completed present; can be selected from **another** client connection | X-server side: any driver | same | **yes**: the runtime's own xcb connection on the app's window |
| DRM vblank (`drmWaitVBlank` / `drmCrtcGetSequence`) | raw CRTC vblank timestamps | needs the DRM fd + CRTC; not available to a Wayland client, and not the right clock under a compositor | — | rejected: the compositor's presentation time is the truth, the CRTC is not |

### Proposed design

The existing design is already the right one: present_wait where it exists, and a measured
vblank grid where it does not. Linux needs **feeders**, not new consumers.

1. **#2060 — un-gate the present_wait tier (S).** Move `target_present_wait_fn`, the acquire-time
   wait, the `VkPresentIdKHR` tagging, `comp_vk_native_target_repaint_pace` and the residual
   measurement out of `#ifdef XRT_OS_WINDOWS`, using `os_monotonic_get_ns()` instead of QPC. Make
   `have_present_wait` true on Linux so the grid tier stands down, as on Windows. That one change
   covers every enable2 app (Unity, the cube test apps) on Intel and NVIDIA, X11 and Wayland.
2. **#2061 — `VK_EXT_present_timing` grid feeder (M).** Add it (and `VK_KHR_present_id2`) to the
   enable2 optional device extensions, create the swapchain with present timing enabled, and give
   `target_feed_vblank_grid` a second arm: `comp_vblank_grid_set_period` from the swapchain's
   timing properties, and `comp_vblank_grid_observe` from past timings joined by present id into
   the existing `residual_ring`. Keep the TIMEBASE GUARD. This restores the #206 forward horizon
   with a real first-pixel-out timestamp.
3. **#2062 — window-system grid feeder (M).** This is the one that reaches **enable1** apps.
   - **X11**: open a runtime-owned xcb connection, `xcb_present_select_input(COMPLETE_NOTIFY)` on
     the app's window, and poll non-blocking after each present.
   - **Wayland**: use `wl_proxy_create_wrapper`'d display and surface on a private
     `wl_event_queue`, bind `wp_presentation`, request `feedback(surface)` immediately before
     `vkQueuePresentKHR` so it attaches to the WSI's commit, and dispatch only our queue.
   - Precedence: present_timing > GOOGLE_display_timing > window system.

**Effort:** S + M + M. **Risks.**
- Wayland feedback attaches to the *next commit*. An app that commits the same surface itself can
  shift attribution by a frame. That is harmless to the grid (timestamps and period only) but not
  to the #206 join, so key it by commit order and drop it on mismatch.
- `discarded` feedback must be treated like the #1394 uncounted presents.
- The levers doc's caveat applies in full: on Android the grid-based late-weave tier measured
  **null**, because the weave budget already exceeded a refresh period. Measure on Intel before
  building on it.

**Verify (log + witness).**
- `#902: display-timing feeder — source=…` (new), `#902: display-timing grid — refresh 16.667 ms (60.00 Hz) [initial]`, `#902: display-timing timebase check … SAME CLOCK`, and either a present_wait one-shot (new) or `#902: late weave on the measured grid — period …, weave budget … ms`.
- `DXR_WEAVE_CADENCE_TRACE=1` prints `#206: last measured weave->scanout residual … ms (… refresh periods)`. Expect about 1–2 periods once late weave engages.
- `DXR_FRAME_WITNESS=5`: present/s must not drop against `DXR_LATE_WEAVE=0` (a pacing change that costs app frames is a defect), and weave/s should stay steady.

## Lever 2 — repaint (#868)

### What exists, per path

| path | queue tier | proof |
|---|---|---|
| Windows D3D11 / D3D12 | driver lock (`ID3D10Multithread`) / own queue: no tier problem | `#868: repaint loop start` |
| Windows VK, NVIDIA, enable2 | tier 1: runtime-owned queue from `xrCreateVulkanDeviceKHR` | `#868: repaint will submit on the runtime-owned queue (family F index I)` |
| Windows VK, Intel, enable2 | tier 2: `VK_LAYER_DXR_queue_lock`, injected at `xrCreateVulkanInstanceKHR`, packaged since #917 | `#902: repaint will submit on the APP's queue …` |
| Windows VK under the split | fill on the D3D11 arm; the session-create tier gate still runs first (`comp_vk_native_compositor.c`, tier selection) | `weave placement: … split=1` |
| Android VK | tier 1 (Adreno has queue index 1); the layer, if live, only serialises (#1905) | `#1905: VK_LAYER_DXR_queue_lock is live on a device with a runtime-owned queue …` |
| **Linux VK, NVIDIA, enable2** | tier 1 (16 graphics queues) | `#868: repaint will submit on the runtime-owned queue …` |
| **Linux VK, Intel, enable2** | tier 2 since **v2.42.0** (#2036/#2037: the layer ships in `/usr/share/vulkan/explicit_layer.d/` or `$XDG_DATA_HOME/vulkan/explicit_layer.d/`) | `#902: repaint will submit on the APP's queue …`; `displayxr-cli info` shows the layer reachable |
| **Linux VK, any GPU, enable1** | **tier 3: off**, unless the app enables the explicit layer itself (Intel), or always on NVIDIA | `#868: no runtime-owned VkQueue (vulkan_enable1, …) and no queue-lock layer (#902 marker absent) — repaint disabled` |

Repaint *pacing* on Linux has a second, smaller gap: with no trusted grid, the loop paces on the
assumed `display_refresh_rate` and `comp_vk_native_target_repaint_pace()` is a no-op. #2060 to
#2062 fix that as a side effect.

### What Linux is missing

A way to put the repaint on a queue the app's own submits cannot race, for apps that created
their own device. The options:

| route | how | Intel single queue | cost / risk |
|---|---|---|---|
| **A. runtime-armed implicit layer** (#2063) | Ship a second manifest in `implicit_layer.d` with `enable_environment` (and the mandatory `disable_environment`). `xrCreateInstance` `setenv`s the arm variable, so the app's later `vkCreateInstance` (enable1 apps call it after `xrGetVulkanInstanceExtensionsKHR`) loads the layer. The existing `vkGetQueueLockMarkerDXR` handshake engages tier 2 with no app change. | works (tier 2) | Child processes inherit the variable, so unset it at session create. An app that creates its instance before `xrCreateInstance` stays tier 3 (logged, never wrong). The per-call serialisation cost on an enable1 app is **unmeasured** (#2039 must be re-run on a fixed app first). |
| **B. `ANV_QUEUE_OVERRIDE=gc=2`** | Measured here: the graphics family reports 2 queues, and each is its own kernel context, so it is real tier 1 with no layer. The runtime sets it before `xrCreateVulkanInstanceKHR`. | works, **enable2 only** | Driver-private and undocumented in Mesa's docs; an experiment, not a product path. |
| **C. same-adapter fill engine** (#2065) | The repaint runs on the runtime's own `VkDevice` (lever 3). | works for **enable1 and enable2** with no layer | L effort; see lever 3 |
| D. app-side | Apps enable the layer themselves (the shipped native avatar's current opt-in `DXR_VK_QUEUE_LOCK=1`) or move to enable2 (#886) | works | Per app; the runtime cannot enforce it |

**What cannot work on Intel single-queue parts.**
- **Tier 1 does not exist by default**, because the graphics family has exactly one queue.
- A compute-only queue exists, but the weave is the vendor DP's command stream, so the runtime
  cannot assume it is compute-only.
- Any second context (B or C) still shares one render engine. GuC time-slices it, so a repaint
  queued behind a long app batch waits for preemption. `VK_EXT_global_priority` is the only lever
  (`HIGH` needs `CAP_SYS_NICE` on i915/xe), and it has to be measured.
- Under tier 2 the layer serialises submit and present calls, not GPU execution. GPU contention is
  unchanged.

**Effort:** A = S/M, B = S (experiment), C = part of #2065. **Verify.**
- `#902: repaint will submit on the APP's queue …` (A) or `#868: repaint will submit on the runtime-owned queue …` (B/C) for an **unmodified** enable1 demo.
- `#868: repaint loop start ret=0`.
- `DXR_FRAME_WITNESS=5` with the app below refresh: repaint/s > 0, weave/s ≈ refresh, and present/s **not lower** than with the layer off.
- Record the GPU clock (`intel_gpu_top`) and whether a viewer was tracked: both moved #2039's numbers more than the layer did.

## Lever 3 — split weave / weave-on-scanout / the fill engine

### What exists, per path

| path | state | mechanism |
|---|---|---|
| Windows D3D11 / D3D12 / VK | **default on**, hybrid and (ADR-039) same-adapter | VK renders into a D3D11 deposit (`comp_vk_native_deposit.cpp`, VK-0). `comp_xbridge` carries it (D3D12 cross-adapter heap, or a same-adapter shared open) to an egress ring on the scanout adapter. A D3D11 fill arm with its **own device and immediate context** does weave, DP, repaint tick and DXGI present. Planes (Local2D, backdrop, authored mask) ride the same slots (VK-1b). Ordered by an `ID3D11Fence` imported as a VK timeline semaphore. |
| Android | absent | — |
| **Linux** | **absent** | `comp_vk_native_split.cpp` is `#if defined(XRT_OS_WINDOWS) && defined(XRT_HAVE_D3D11)`; no `weave placement:` line |

ADR-039's finding is what makes this matter on a single-GPU Linux laptop, not only on hybrids. The
split's load-bearing property is **headroom**, not the removed copy: the fill arm ticks about
400/s at 1.8–2.3 ms, against 100–205/s with 17–23 ms holes for in-process arms (#1264). Every
in-process remedy has been exhausted with clean attribution. On Linux today, every session is an
in-process arm.

### What Linux is missing, and the substrate

- **Same adapter (#2065).** A second `VkDevice` on the same `VkPhysicalDevice`, plus a transport.
  - Memory: `VK_KHR_external_memory_fd` with the **opaque-fd** handle type, valid between devices with matching `deviceUUID`/`driverUUID`.
  - Sync: a **timeline semaphore** exported as an opaque fd (`VK_KHR_external_semaphore_fd`), the analogue of the shared `ID3D11Fence`.
  - All of it is present on this box. No dma-buf or modifiers are needed.
- **Hybrid PRIME (#2066).** Two drivers, so opaque fds do not cross.
  - Memory: dma-buf (`VK_EXT_external_memory_dma_buf` + `VK_EXT_image_drm_format_modifier`, with `DRM_FORMAT_MOD_LINEAR` as the guaranteed floor).
  - Sync: binary semaphores exported as **sync_file** (`SYNC_FD`), or implicit sync on the dma-buf.
  - Scanout-device detection: on Wayland, `zwp_linux_dmabuf_v1` v4 surface feedback (tranche `target_device`); on X11, the RandR provider of the window's CRTC. Either way, match it with `VK_EXT_physical_device_drm`.

**Which GPU scans out on a hybrid laptop under GNOME.**
- The app renders on the dGPU only under PRIME render offload (`__NV_PRIME_RENDER_OFFLOAD=1` /
  `__GLX_VENDOR_LIBRARY_NAME=nvidia`, or switcheroo-control's `PrefersNonDefaultGPU` launcher hint).
  Mutter composes on its primary GPU, normally the iGPU, unless udev tags another one.
- **The connector decides the scanout GPU.** The internal eDP is usually on the iGPU. External
  HDMI/DP is on many laptops wired to the **dGPU**, and a desktop-class 3D panel is HDMI. Mutter
  then does a secondary-GPU copy, so weave → scanout crosses the adapter boundary inside the
  compositor. That is the Linux form of the ~2.5-period residual #918 removed on Windows (42 vs
  16 ms there).
- The runtime cannot assume "iGPU = scanout". It has to ask: the dmabuf feedback `target_device`
  is the authoritative per-surface answer on Wayland.

### Proposed design (keep the Windows structure, replace D3D with Vulkan on both ends)

```
app VkDevice: vk_native renders the atlas (+ planes) INTO exportable images  (VK-0 analogue, zero copy)
      | opaque-fd (same GPU) or dma-buf + modifier (PRIME); ring of 3, layout-generation stamped
      v
fill-arm VkDevice (runtime-owned, on the SCANOUT GPU): DP weave -> its own VkSwapchain on the app's surface
      ^ timeline semaphore (same GPU) / sync_file (PRIME)        + repaint thread + present-timing feeder
```

- The **fill arm** owns the swapchain on the app's `wl_surface` or X window. Wayland allows one
  swapchain per surface, so the app-device target is not created, as with `c->split == NULL` on
  Windows. The arm also owns the DP (created against the arm's bundle, with ADR-037 §3a consent),
  the repaint thread, and the present-timing feeder from lever 1.
- **Repaint keys on the arm, not on the queue tier.** enable1 apps get repaint with no layer.
  This is option C of lever 2, and the reason #2065 is the structural fix.
- Reuse `comp_split_gate` tokens and emit `weave placement:` on Linux. `DXR_SPLIT_SAME_ADAPTER=0`
  and `DXR_WEAVE_ON_SCANOUT=0` keep their meaning as kill switches.

**Effort:** #2065 L, #2066 L (hardware-gated: needs a hybrid laptop with the panel on each kind of
connector). **Risks.**
- Two contexts on one Intel engine (see lever 2).
- The DP must accept weaving on the arm's device.
- The ring costs 3 × atlas of memory.
- Move-sync (#1748), HW transforms and multi-screen segments must follow the arm.
- On PRIME, NVIDIA ↔ Mesa dma-buf interop for render targets depends on driver versions, and a
  linear modifier costs bandwidth at 4K.

**Verify.**
- `weave placement: … split=1` (new on Linux).
- `DXR_WEAVE_REPAINT_TRACE=1`: fill-loop ticks in the hundreds per second at < 3 ms intervals, which is the ADR-039 signature.
- `DXR_FRAME_WITNESS=5`: weave/s ≈ refresh, with present/s not lower than split-off.
- A ≥ 5 min leg and an eyeball sign-off.
- On PRIME, the #206 residual should drop from ~2.5 to ~1 period.

## Deferred present and late latching

- **Deferred present** (`DXR_DEFER_PRESENT`) was removed from the code after two null
  measurements (#837). Nothing on Linux changes that conclusion, so there is nothing to port.
- **Late latching** is vendor-side. It re-samples the pose into frames already queued, and the
  levers doc records it as unverified on every backend. Repaint + late weave make it redundant by
  construction. On Linux it has the same precondition as everywhere: the VK DP's submit hook, and
  frames actually in flight. It belongs to the vendor plug-in, not this plan.
- **The #206 forward horizon** is a runtime lever. It arrives with any grid feeder (#2061/#2062),
  through the existing `residual_ring` → `set_predicted_scanout` path in `comp_vk_native_compositor.c`.
- **The slot partition** (`DXR_APP_FRAME_DIVISOR`) needs a Linux arm in `part_tier_ok` (#2064).

## Issue map

| # | piece | effort | depends on |
|---|---|---|---|
| #2059 | **epic** | — | — |
| #2060 | late weave via `VK_KHR_present_wait` (un-gate) | S | — |
| #2061 | grid feeder: `VK_EXT_present_timing` | M | Mesa ≥ 26.1 / NVIDIA 595+ |
| #2062 | grid feeder: `wp_presentation` / X11 Present (covers enable1) | M | — |
| #2063 | repaint for enable1: implicit layer / `ANV_QUEUE_OVERRIDE` / re-measure #2039 | S/M | #2039 re-run |
| #2064 | slot partition on Linux + one `m2p:` tier line | S | a trusted grid |
| #2065 | same-adapter fill engine (ADR-039 twin) | L | #2060 or #2062 for its pacing |
| #2066 | PRIME weave-on-scanout (ADR-037 twin) | L | #2065, hybrid hardware |

Related: #868 (repaint), #883 (fleet capability survey, which should gain Linux rows from this
table), #886 (enable1 apps), #902 (queue-lock layer), #1049 / #1339 (fill-steals-app-frames
defect class: the yardstick for every change here), #1905 (mid-frame fill), #2036 (layer
packaging), #2039 (layer cost, to be re-measured).

## How to read a Linux log today

| question | line |
|---|---|
| is the grid fed? | `#902: display-timing feeder — vk=… has_ext=…` (`has_ext=0` means no) |
| is late weave live? | `#902: late weave on the measured grid …` (absent on Linux today) |
| which repaint tier? | `#868: repaint will submit on the runtime-owned queue …` (tier 1) / `#902: repaint will submit on the APP's queue …` (tier 2) / `#868: no runtime-owned VkQueue …` or `#868: repaint loop NOT started` (off) |
| rates | `DXR_FRAME_WITNESS=5`: `[WITNESS] … present/s=… repaint/s=… weave/s=…`. Quote all three, never weave/s alone |
| split? | no `weave placement:` line on Linux until #2065 |

Until #2064 adds a single tier line, a support case has to assemble this from four lines and one
absence.

## Measured outcome and decisions (2026-10-13, Intel reference laptop)

Numbers from the same box as the measured-state table above, every window 1:1 on the panel, GPU clock pinned where stated; full data and method in the session notes (`perf-assessment-2026-10-13.md`).

### Decision table

| lever | Linux state after today | benefit measured | cost measured | recommendation |
|---|---|---|---|---|
| Repaint (#868) | shipped in v2.42.0 (layer); enable2 injected, enable1 opt-in (#2069 merged) | +~19 pose-fresh weaves/s for a 30 Hz app | +10 pts core, +2 pts GPU while filling; 0 at full rate | **keep, as shipped**; make enable1 default via the in-process env route later (#2063) |
| Late weave, present_wait path (enable2) | PR #2068, default on | residual 0.90 period at 60 Hz (Unity) | none (59–60/s on and off) — **but see #2119: that run was on the direct-scanout path; a composited window at depth 1 runs at half rate** | **merge #2068**; depth governed by #2119 |
| Late weave, measured-grid path | PR #2068, opt-in, self-off | none on GNOME (no slack inside a period) | none when off | leave opt-in; only direct scanout / `VK_EXT_present_timing` (#2061) change this |
| Feedback pacing for enable1 (#2077) | stacked PR, opt-in | −0.35 period (~6 ms) | −8 % present rate | **not by default**; revisit with p95 residuals |
| Split weave / weave-on-scanout | not built on Linux (#2065 L, #2066 L) | n/a on single-GPU Intel (every Lenovo target) | large build | **do not build for Lenovo**; NVIDIA-laptop decision, measure there first |
| Anti-aliasing | Unity: none (PR #6 → 4×, build pending); native: 2× default | 4× = clean silhouette (montage) | native 4× −15 % frames, 8× −23 % | native 2× default + 4× option on iGPU; Unity 4× then measure |
| GPU governor (xe) | parks at 400 MHz under the native app's load | — | 20–25 presents/s episodes | product issue in its own right; pin for measurements |

### Anti-aliasing (David's ask: both builds must have decent AA)

Facts: the Unity project renders with **MSAA off** (URP asset `m_MSAA: 1`; URP ignores `QualitySettings.antiAliasing`; the plug-in's FXAA pass is Built-in/MultiPass only) — PR LeiaInc/lenovo-avatar#6 sets 4× and needs a Linux build from Byungju. The native renderer uses true MSAA on its colour target (default **2×**, `DXR_AVATAR_MSAA=1|2|4|8`) plus a 3×3 alpha-weighted silhouette soften pass.

Captures (pre-weave atlas, left view, 4× nearest crops): the capture montages attached to the avatar PR; sources `aa_native_1/2/4/8.png`, `aa_native_{2,4}_rawA.png`, `aa_unity.png`;  Reading: native 1× = stair-steps on every silhouette; 2× = visibly softer but steps remain on the hat brim; 4× = clean; 8× ≈ 4×; Unity RC2 = the hardest steps of all (no AA, and the alpha silhouette is a 0/1 step too).

Cost (native, MSAA buffers at 680x1368 per view): 2× 95 MB, 4× 158 MB, 8× 285 MB of GPU memory. Frame cost: pinned-clock sweep below (GPU min_freq 2500, back to back, same window, idle).

| native MSAA (clock pinned 2.5 GHz) | present/s | app GPU | app CPU |
|---|---|---|---|
| 1× | 55.1–59.6 | 31 % | 75 % |
| 2× (default) | 53.5–56.5 | 33 % | 74 % |
| 4× | 46.3–49.2 | 38 % | 71 % |
| 8× | 42.1–45.0 | 40 % | 69 % |

Reading so far: on this iGPU 4× costs the native app ~15 % of its frames (55 → 47/s) for a clearly better silhouette; 2× is nearly free vs 1×. Recommendation: native default stays **2×** on Intel iGPUs with 4× as the quality option (or 4× by default once the frame-rate headroom exists, e.g. with the pacing/repaint work landed); Unity needs *some* MSAA first (PR #6 at 4×; drop to 2× if Byungju's build shows the same ~15 % cost).

### Pacing enable1 apps on presentation feedback (stacked PR #2077, opt-in `DXR_LATE_WEAVE_LINUX_PACE=1`)

The hypothesis from #2068's author: the native app's residual came from running ahead into the 4-image FIFO swapchain; pacing the weave on the previous frame's `wp_presentation.presented` would recover Unity's 0.9 periods. Clock-pinned A/B (native, same window):

| arm | present/s | weave-to-scanout residual (last samples, n up to 5000) | app CPU / GPU |
|---|---|---|---|
| unpaced | 54–58 | 1.81 → 0.86 → **0.92** periods once warm | 73 % / 34 % |
| paced, latency 1 | **48–52 (−8 %)** | **0.55** periods (9.2 ms); waits ~245 per 5 s, 0 slips, mean wait 0.5–0.7 ms | 74 % / 32 % |
| paced, latency 2 | 55–57 (no cost) | 1.37 periods | 74 % / 34 % |

Reading: the early 2.5–3.1-period native readings were start-up transients (n = 1…138); warm, the unpaced native app sits at ~0.9 periods like Unity. Pacing at latency 1 trims ~0.35 period (~6 ms) and costs ~8 % of the frames on this iGPU; latency 2 costs nothing and gains nothing. Verdict: **marginal — keep opt-in**; worth revisiting only with a distribution (p95) rather than last-sample residuals, which the PR will log next.

### #2119: depth-1 present_wait pacing halves a composited window (2026-10-14, ds1-linux)

Reproduced without a panel: the same RC2 Unity player (native Wayland), runtime 2.43.4 + sim-display claiming the laptop panel (`DXR_PLUGIN_EXCLUSIVE=sim-display SIM_DISPLAY_FAKE_TRACKING=1`), GNOME 50 Wayland, Intel PTL (xe); 30 s per arm, `DXR_FRAME_WITNESS=5` (present/s = app frames, repaint 0), GPU busy from fdinfo.

| arm | pacing log line | present/s median (min–max) | player CPU | player GPU |
|---|---|---|---|---|
| late weave on (default) | `pacing on present id N-0 … MAX_LATENCY=1` | **30.0** (28.8–35.6) | 59 % | 19.5 % |
| `DXR_LATE_WEAVE=0` | — | 55.5 (50.4–60.0) | 78 % | 19.0 % |
| **`DXR_LATE_WEAVE_MAX_LATENCY=2`** | `pacing on present id N-1 … MAX_LATENCY=2` | **58.0** (52.8–60.0) | 77 % | 20.8 % |
| late weave on, GPU clock pinned 2.5 GHz | N-0 | 33.6 (29.8–39.9) | 62 % | 9.8 % |
| `DXR_LATE_WEAVE=0`, clock pinned | — | 58.1 (56.7–60.0) | 80 % | 15.8 % |

Mechanism: the present_wait tier blocks until the previous present is on glass; Mesa resolves that from `wp_presentation.presented`, and GNOME shows a windowed commit about two refreshes later, so with no frame of overlap the loop is render → weave → present → wait ~2 periods (60/2 = 30 fps). The 2026-10-13 59–60 reading was on the Odyssey's fullscreen opaque-region path (direct scanout, 1-period presents). Re-runs show the lag is not even constant per desktop: across identical depth-1 arms the window ran at ~30 in four runs and ~60 in one, and inside one run the 5 s windows alternated 59, 34, 97, 60, 61, 60, 58, 24, 98, 61 — mutter's dynamic double/triple buffering flipping the lag between 1 and 2 periods, with our own pacing feeding back into it. Byungju's 41 (Meteor Lake) and 34–39 (NVIDIA) fit the same mechanism with a different mix.

**Decision:** no per-OS default. The present_wait tier's depth is governed from measurements (base 1, ceiling 4): effective depth = max(commit-to-glass lag floor, slip level). The floor is raised by measurement — the commit-to-glass EMA from presents committed with none of ours outstanding (≥ floor + 0.5 periods), or a starvation window (the pacer's wait ≥ 0.8 period on frames ≥ 1.5 periods long) — and lowered only by **slack**: when ≥ 95 % of the waits on present N−(d−1) over a dwell (5 s, doubling after a wrong lowering) return in < 0.1 period, that present was already on glass and a level can go. After a raise the pacer drains the queue to depth − 1 before admitting a frame. The slip level (shared `comp_feedback_pace.h` machine) banks no level above the floor on this tier. Four CI rounds got here: no lag correction ran away to depth 4 (residual 1.99 → 3.99 periods); subtracting depth − 1, then the outstanding count, misread GNOME's 2-period *pipelined* compositor and dropped to depth 1 (30 present/s for two minutes); a 1 s throughput probe held depth 2 but cost a second at half rate per probe and once left an extra frame queued (residual 2.97 periods for ~50 s, median 52.7 present/s). Depth 2 is cheap — the #206 weave→scanout horizon measured the same at depth 1 and 2. `DXR_LATE_WEAVE_MAX_LATENCY` pins the depth, `DXR_LATE_WEAVE_AUTOBACKOFF=0` holds 1. Reference: `docs/reference/motion-to-photon-levers.md` § *present_wait depth governor*.

### GPU governor finding (Intel xe, PTL)
With the governor free, the native app's light load lets the GPU park at its 400 MHz efficient floor for long stretches (20–25 presents/s with a tracked viewer) and in other sessions run at 2.5 GHz (55–58/s); the Unity app's heavier load keeps the clock up. Pinning `min_freq` to 2500 removes the variance. For any native app on Intel this is a product issue in its own right (a GPU-busy hint / `VK_EXT_global_priority` / a workload floor), independent of the levers.


# VK late-weave on single-graphics-queue GPUs — queue-serialization layer

> Design of record for [#902](https://github.com/DisplayXR/displayxr-runtime/issues/902).
> Status: **DESIGN** (validated on paper 2026-08-10; prototype pending).
> Companions: #868/#873 (repaint), #886 (enable1 forfeit WARN), #884/#850
> (late-weave pacing + governor), #898 (RandR refresh), #899/#900 (INV-5.9
> enable2 enforcement + Linux test-app migration).
> Goal: the #868 repaint becomes the **default VK architecture on Windows and
> Linux, iGPUs included, with zero app awareness** — parity with D3D11/D3D12.

## 1. Problem

The #868 repaint replays the last atlas against fresh eyes at
`display_refresh_rate` while the app is between frames. Weave = f(atlas, eyes);
the display processor re-samples the eyes at weave time, so a replayed atlas is
a *fresher* frame, not a wasted one. The repaint runs on its own runtime thread,
so it must submit GPU work concurrently with the app's render thread — and the
three graphics APIs give the runtime very different tools for that:

| API | Concurrency tool | Result |
|---|---|---|
| D3D11 | `ID3D10Multithread::Enter/Leave` — a **driver-enforced lock every caller respects**, incl. the app's render thread (`comp_d3d11_compositor.cpp` `mt_lock`) | repaint just works |
| D3D12 | per-thread command lists + thread-safe `ExecuteCommandLists`; queues are cheap software objects | repaint just works |
| Vulkan | **none** — a `VkQueue` is *externally synchronized* (the app must serialize submits; there is no driver lock to borrow) and you cannot create more queues than the driver exposes | repaint needs a queue the runtime owns exclusively |

Under enable2 the runtime reserves a second queue from the graphics family at
`xrCreateVulkanDeviceKHR`. That works where the driver exposes one:

- **NVIDIA** — graphics family typically `queueCount = 16`. Dedicated-queue
  path shipped and proven.
- **Intel iGPUs — every generation (Meteor/Lunar/Panther Lake), Mesa ANV *and*
  the Windows driver** — exactly **one** graphics-capable queue. The extra
  families (async-compute, transfer; ANV: `ANV_QUEUE_OVERRIDE=c=0,b=0`) carry
  no `VK_QUEUE_GRAPHICS_BIT`, so they cannot run the weave's graphics pipeline.
  This is a driver *design*, not a per-silicon limit — do not expect a newer
  Intel part to change it. Verified live on an Intel Arc (Meteor Lake) laptop:
  `#868: graphics family 0 is saturated (1/1) — no runtime-owned queue, VK
  repaint stays disabled`.
- **AMD RADV** — also a single graphics queue (+ compute/transfer families) →
  same wall.

Since SR laptops are Intel-iGPU machines, the single-queue case is the
*mainstream* VK case. Today it silently degrades to pacing-only (`DXR_LATE`):
motion-to-photon improves, but a 30 fps app on a 120 Hz panel still holds each
interlace pattern four refreshes.

## 2. Key observation: per-call locking is sufficient in Vulkan

D3D11 needs its lock held across the whole weave *sequence* (bind RTV → set
viewport → draw) because an immediate context is **shared mutable state** — an
app call landing mid-sequence redirects the weave. Vulkan has no submit-time
shared state: the weave is pre-recorded into a command buffer, and the only
externally-synchronized touchpoints are the `vkQueue*` calls themselves.
Submit→present ordering is carried by semaphores, not lock atomicity. An app
submit interleaving between the repaint's submit and its present is legal and
harmless.

So the entire requirement is: **no two threads inside a `vkQueue*` call on the
same queue at once.** A per-queue mutex around the queue-access surface is
exactly the Vulkan spec's external-synchronization contract, discharged
centrally.

A runtime-side lock (`c->mutex`) cannot wrap the app's own `vkQueueSubmit` —
the app submits its scene on its own thread, outside any OpenXR call. But a
**Vulkan layer** can, because the lock lives *inside* the intercepted call.

## 3. Design: runtime-injected queue-serialization layer

### 3.1 The layer

A minimal layer (`VK_LAYER_DXR_queue_lock`) holding one mutex per `VkQueue`,
acquired for the duration of each intercepted call. The interception surface
must be **complete** — one missed entry point silently reopens the race:

- `vkQueueSubmit`, `vkQueueSubmit2`, `vkQueueSubmit2KHR`
- `vkQueuePresentKHR`
- `vkQueueWaitIdle`
- `vkQueueBindSparse`
- `vkQueueBeginDebugUtilsLabelEXT` / `vkQueueEndDebugUtilsLabelEXT` /
  `vkQueueInsertDebugUtilsLabelEXT`

Locks are held only across the call — never across a host-side wait the
runtime controls (the #884/#850 pacing wait is `vkWaitForPresentKHR`, a
*device* call, outside the lock by construction).

### 3.2 Why coverage is total (the load-bearing argument)

Vulkan dispatchable handles carry their dispatch table, built at device
creation **with the enabled layer chain baked in**. Every caller routes through
it, regardless of how the function pointer was obtained (loader export
trampoline or `vkGetDeviceProcAddr`):

- the **app engine's** scene submits,
- the **runtime repaint thread's** weave submits,
- the **vendor display processor's internal submits** — e.g. the SR weaver's
  correction-texture upload + `vkQueueWaitIdle` issued from inside `weave()`,
  which is the *other* threading hazard on record.

Nobody in-process can bypass the chain. The layer therefore retires **both**
recorded hazards at once, including the one that forced the DP-creation
queue-swap trick (`comp_vk_native_compositor.c`, "#868: creating the display
processor against the runtime-owned queue").

### 3.3 Injection — why the app never knows

`XR_KHR_vulkan_enable2` **requires** the app to create its `VkInstance` via
`xrCreateVulkanInstanceKHR`, where the runtime builds the final
`VkInstanceCreateInfo`. The runtime appends the layer to
`ppEnabledLayerNames` and ensures discovery (§5.1). The app requested nothing
and observes nothing. INV-5.9 (lint ERROR since #899; test apps migrated in
#900) makes enable2 the fleet-wide contract, and the Unity/Unreal OpenXR
plugins are already enable2 — so engine apps are covered transparently.

`VK_ERROR_LAYER_NOT_PRESENT` at instance creation → retry once without the
layer → tier 3 (below). Instance creation must never fail because of us.

### 3.4 Handshake, not hope

The layer exposes a resolvable marker entry point (working name
`vkGetQueueLockMarkerDXR`). The runtime starts the shared-queue repaint thread
**only after** resolving the marker on the created device — never on the
assumption that injection worked. Without the marker, the hard disable stays.

### 3.5 Tiered mode selection — automatic, default ON

| Tier | Condition | Mode |
|---|---|---|
| 1 | driver exposes ≥2 graphics queues | **dedicated runtime queue** (current shipped path) |
| 2 | single graphics queue, marker resolves | **shared queue, layer-serialized** |
| 3 | layer absent / failed | repaint off; `DXR_LATE` pacing only (today's behavior) |

Tier 1 stays **preferred** where hardware allows — deliberately not
layer-everywhere:

1. *Weave latency under load.* Within one queue, execution follows submission
   order: a shared-queue repaint lands **behind** the app's heavy scene submit
   — worst exactly when the app is slow, which is when the repaint matters
   most. A second queue lets the GPU scheduler slip the small weave in
   promptly.
2. *Zero tax on the app.* Tier 1 leaves the app's submit path untouched — no
   per-submit lock, no exposure to DP-internal `vkQueueWaitIdle` stalls (those
   drain only the runtime queue).
3. *Blast radius.* Tier 1 is shipped and proven; a layer bug can then only
   affect machines that had no repaint at all before — strictly additive.

Test-matrix divergence is answered with a knob, not a uniform default:
`DXR_VK_QUEUE_MODE=layer` forces tier 2 on any hardware (dev/CI coverage);
`=queue` / `=off` force the others.

### 3.6 Tier-2 behavior change at DP creation

With no runtime queue, the DP is created against the **app's** queue and the
queue-swap trick becomes a no-op. Safe *because* §3.2 covers the DP's internal
submits — but it is a deliberate behavior change, stated here so nobody
"fixes" it back.

## 4. Alternatives considered (and why not)

- **Dedicated queue only** (status quo) — leaves every Intel machine, Windows
  and Linux, without repaint. Rejected as the default story.
- **Repaint from the app thread inside `xrWaitFrame`** — app is parked there
  when between frames, so one thread submits. Covers the steady-slow case but
  not hitches, for comparable plumbing effort. Superseded by the layer.
- **Cooperative submit-lock in our own apps/providers** — works only for the
  app surface we own; arbitrary OpenXR apps stay uncovered. Violates the
  zero-app-awareness requirement.
- **Compute-shader weave on the async-compute queue** — the runtime *can* own
  a compute queue on Intel, but the weave is a graphics pipeline; this needs a
  vendor weaver change (compute interlace + cross-queue sync). Out of scope;
  revisit only if a hitch-proof repaint is ever required where the layer
  cannot load.
- **Second logical device** — a second `VkDevice` on the same physical GPU
  yields an independently-synchronized queue, but sharing the atlas + present
  images across devices needs external-memory export/import plumbing. Strictly
  heavier than the layer for the same outcome.

## 5. Risks

### 5.1 Layer discovery robustness (highest)

`VK_LAYER_PATH` is **ignored for elevated processes** on Windows
(secure-loader behavior — same trap family as the elevated-process
`XR_RUNTIME_JSON` caveat in the build docs). Therefore:

- **Windows:** installer registers the layer manifest under the loader's
  `ExplicitLayers` registry key (elevated-safe). Never rely on `VK_LAYER_PATH`
  outside dev trees.
- **Linux/macOS:** manifest in the standard XDG data path
  (`…/vulkan/explicit_layer.d/`), shipped by the runtime package.
- Dev trees: runtime sets `VK_LAYER_PATH` process-side before
  `vkCreateInstance` (OS env API on Windows — the DLL's static-CRT `getenv`
  caveat does not apply to what the loader reads).

### 5.2 Lock hold-time

Two calls can hold the per-queue lock for non-trivial time, stalling an app
submit behind them:

- DP-internal `vkQueueWaitIdle` (correction upload; calibration/geometry
  changes, not per-frame),
- a blocking FIFO `vkQueuePresentKHR` when the swapchain image queue is full.

Expectation: tens of µs typical submit hold; rare ms-scale spikes.
**Measure on a single-queue Intel iGPU before flipping the default**, and
instrument lock wait/hold times (per-queue max + EMA) behind a debug var so a
regression is observable rather than felt.

### 5.3 Residual

- A pathological app calling the ICD directly (not via loader dispatch) would
  bypass the layer — same practical coverage as every layer-based tool
  (overlays, capture); accepted.
- enable1 apps: runtime is not in-path to inject anything. Already an INV-5.9
  ERROR; unchanged (tier 3 + the #886 WARN).

## 6. Rollout

1. Prototype layer (manifest + per-queue lock + marker), Linux first; unit
   harness = two threads hammering submit/present on one queue with validation
   on.
2. Injection in `xrCreateVulkanInstanceKHR` + `LAYER_NOT_PRESENT` fallback.
3. Tier gate in `comp_vk_native_compositor_create`: replace the hard disable
   with tier 2 when the marker resolves; add `DXR_VK_QUEUE_MODE`.
4. Validate on a single-queue Intel iGPU: `#868 repaints=N` counters pacing at
   `display_refresh_rate` (post-#898 this is the true panel rate), lock
   hold-time measurements, `DXR_WEAVE_REPAINT_FORCE=1` soak with validation
   layers.
5. Windows: installer registry registration; repeat validation (Intel iGPU +
   `DXR_VK_QUEUE_MODE=layer` on NVIDIA).
6. Flip default: tier auto-selection ON — VK repaint default-on everywhere,
   matching D3D11/D3D12.

## 7. Android: per-queue scope and delivery (#1905)

**Status: PROTOTYPE, device-validated** (branch `feat/android-queue-lock-layer`,
PR #1915). Companion: PR #1910, which auto-enables mid-frame fill when this
layer is live.

**Scope: per-queue, decided by measurement.** The first hypothesis was that
on Adreno 740 the app's queue (family 0 idx 0) and the runtime's repaint queue
(idx 1), though distinct `VkQueue`s, collide *across* queues on their shared
kernel GSL context (`Adreno-GSL ctx N: next client ts … must be greater`,
then -3 on the next idx-0 submit), which would have needed one mutex per
`VkDevice`. The device experiment says otherwise. Lume phone, Adreno 740,
60 Hz, layer loaded through the `gpu_debug` settings path, force-repaint probe
(`debug.dxr.weave_repaint_force=1`, which reproduces the -3 at ~0.5/s without
the layer), 30 s runs:

| scope | GSL warnings | submit -3 | weave refresh (CNSDK GetFace) | fills | app fps |
|---|---|---|---|---|---|
| per-queue | 0 | 0 | 41.5 Hz | not recorded | = baseline |
| per-device | 0 | 0 | 22.5 Hz | 10/s | — |

Per-queue locks already clear the race, so the colliding calls are on the
**same** queue, which is exactly §2's contract. Per-device adds a cost:
the app's submits wait behind the fill's blocking present, and fills collapse.
So **per-queue is the default on every platform** (`QL_DEFAULT_PER_DEVICE=0`).
`setprop debug.dxr.ql_scope device` (read at `vkCreateDevice`) keeps the
per-device A/B. Tier selection (§3.5) is unchanged. Adreno gives the runtime
idx 1, so the repaint stays on tier 1 with its own queue, and the layer only
serializes. With the layer live and #1910's mid-frame fill on, there were
also 0 GSL warnings, 0 `Failed to render layers` and 0 CNSDK -3.

**Android loader contract** (checked against AOSP `frameworks/native`
`vulkan/libvulkan/layers_extensions.cpp` + `api.cpp`, main branch):
- There is no JSON manifest and no `vkNegotiateLoaderLayerInterfaceVersion`.
  The loader scans each directory, or `apk!/lib/<abi>` zip path, on its layer
  search path for `libVkLayer*.so`.
- It `dlsym`s `vkEnumerateInstanceLayerProperties` and
  `vkEnumerateInstanceExtensionProperties`. Both are required: without them
  the library is rejected.
- The device-side `vkEnumerateDeviceLayerProperties` and
  `vkEnumerateDeviceExtensionProperties` are nominally optional. However, a
  layer whose device properties do not `memcmp`-equal its instance properties
  is marked non-global, and the loader leaves it out of every **device** chain
  (`IsLayerGlobal`). For this layer they are therefore mandatory.
- GIPA/GDPA resolve as `<layerName>GetInstanceProcAddr` first, then fall back
  to `vkGetInstanceProcAddr`.
- An unknown layer name fails `vkCreateInstance` with
  `VK_ERROR_LAYER_NOT_PRESENT`, which is what the §3.3 retry already handles.
- `gpu_debug_layers` names become implicit layers. An explicit request for the
  same layer name is de-duplicated (`OverrideLayerNames`).
- The NDK ships no `vk_layer.h`. The layer defines Android's
  `vk_layer_interface.h` chain structs locally.

**Delivery.**
- *Experiment:* the `.so` ships in the runtime APK
  (`lib/arm64-v8a/libVkLayer_DXR_queue_lock.so`). A userdebug device loads it
  into an app with `enable_gpu_debug_layers=1`, `gpu_debug_app=<app>`,
  `gpu_debug_layers=VK_LAYER_DXR_queue_lock` and
  `gpu_debug_layer_app=org.freedesktop.monado.openxr_runtime.out_of_process`.
  This path is debug-only: `GraphicsEnvironment.debugLayerEnabled()` requires
  a debuggable build or app, or the inject-layers metadata.
- *Product (#1919):* the `.so` ships inside the DisplayXR client AAR
  (`jni/arm64-v8a/`, built by a standalone layer-only CMake project in
  `displayxr_client/src/main/cpp`; CI asserts it is byte-identical to the
  runtime APK's copy) so it lands in the **app's own** library path. App-side
  contract and verification: `docs/getting-started/android-build-guide.md`
  § *The queue-lock layer ships in the client AAR*. **Verified (AOSP main):**
  `GraphicsEnvironment.setupGpuLayers()` appends the app's library search
  paths to the layer path "in all cases", outside the debug gate. In
  `DiscoverLayers()`, the loader searches `getLayerPaths()` unconditionally;
  only `/data/local/debug/vulkan` is gated on `isDebuggable()`. So a
  non-debuggable app can load a layer bundled in its own APK, and the runtime's
  existing injection names it.
- *Uncompressed libs — verified in AOSP (#1919), android11/13/14-release and
  main:* `LoadedApk.makePaths()` puts both `nativeLibraryDir` and
  `<apk>!/lib/<abi>` on the class loader's library search path, which
  `ApplicationLoaders` hands to `GraphicsEnvironment.setLayerPaths()`;
  `DiscoverLayersInPathList()` walks `!/` zip paths, keeping entries that are
  stored and page-aligned (AGP's uncompressed packaging), and opens them via
  the app namespace. *Not verified:* the OEM builds our devices run. Fallback:
  `extractNativeLibs=true` / `useLegacyPackaging true`.

**Hazards to measure before productizing.**
- With the per-device A/B scope only: a DP-internal `vkQueueWaitIdle`, or a
  blocking present, stalls the **other** queue's submits as well as its own
  (§5.2, widened); this is what the 22.5 Hz row above measured.
  `vkQueueWaitIdle` held under the device lock can also **deadlock** in one
  case: the waited queue has a pending timeline-semaphore wait whose signal the
  other queue has not been submitted yet (wait-before-signal). Binary
  semaphores cannot do this. Per-queue scope (the default) has neither hazard.
- The layer covers every `VkDevice` in the app process, including ones the app
  creates for itself.

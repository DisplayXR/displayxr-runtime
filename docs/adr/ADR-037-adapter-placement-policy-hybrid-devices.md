# ADR-037 — Adapter placement policy on hybrid-GPU devices

**Status:** PROPOSED (2026-08-22) · supersedes the implicit "everything on
HIGH_PERFORMANCE" default · depends on the #918 output-device split
· related: [ADR-035](ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md),
[`docs/reference/adapter-selection.md`](../reference/adapter-selection.md),
[`docs/investigations/hybrid-igpu-weave.md`](../investigations/hybrid-igpu-weave.md)

## Context

A **hybrid device** has more than one adapter, and exactly one of them — the
**scanout adapter** — drives the 3D panel. On the laptop class we ship to, that
is usually the *integrated* GPU while the *discrete* GPU is render-only. On a
desktop, or a MUX'd laptop, the panel may hang off the discrete GPU instead. The
policy below must be correct on all three topologies without being told which
one it is on.

### Three workloads, only one of which is pinned by physics

| workload | what it is | placement constraint |
|---|---|---|
| **App render** | the app's scene, N views | none — goes wherever it is fastest |
| **Composite** | atlas assembly, crop, layer/zone composition, mask raster | wants to be where the app's swapchain images already are |
| **Weave + present** | the DP's lens shader, then present to the panel | **must be on the scanout adapter** |

The weave constraint is not a preference. The panel's framebuffer lives on the
scanout adapter, so weaving anywhere else means a full-frame cross-adapter copy
inside the present path, every frame.

### What we do today, and why

Every app linking `sr_common_base` force-exports `NvOptimusEnablement=1` /
`AmdPowerXpressRequestHighPerformance=1` (`displayxr-common/common/optimus_dgpu_hint.c`);
the Unity plug-in's build processor writes a per-exe `UserGpuPreferences`
entry; and the runtime's own suggestion defaults to
`EnumAdapterByGpuPreference(0, HIGH_PERFORMANCE)` (`oxr_d3d.cpp`), matched by
the D3D11 service compositor. Three mechanisms, all saying "discrete".

That default was adopted for a real reason: engine apps (Unity) that pick their
own adapter *before* consulting the runtime would land on the iGPU, diverge from
the runtime's suggestion, and present black or fail session creation (#240).
Pinning everything to the discrete GPU made the two agree.

It is **coherent**, and it is right about *render*. It is wrong about *weave*,
because a process-wide pin moves all three workloads together.

### What it costs (measured, #918, hybrid laptop with an iGPU-driven panel)

| configuration | motion-to-photon | iGPU cost |
|---|---|---|
| everything on the scanout adapter | **16.0 ms** | whole pipeline ≈ 460 ms/s |
| everything on the discrete GPU *(today's default)* | **42.1 ms** | still **~6.4 ms/frame** executing copy-2 of the two-copy hybrid present |
| **split** — render discrete, weave scanout (#918 Phase 1) | **16.6 ms (p50)** | explicit, pipelined D3D12 cross-adapter heap copy |

The decisive figure: doing *everything* on the scanout adapter cost only
**+1.2 ms/frame more than merely receiving** the discrete GPU's frames. Today's
default pays nearly the full scanout-side price *and* spends the bus and 26 ms
of latency on top.

The split reaches the all-scanout latency **while keeping discrete-GPU render**,
so it dominates both alternatives wherever it is implemented.

## Decision

### 1. One placement rule

> **Render and composite go on the most capable available render adapter.
> Weave and present go on the scanout adapter. The runtime decides both; the
> app is told, not asked.**

This single rule covers every topology, because the two adapters coincide when
there is only one:

| topology | render adapter | scanout adapter | resulting regime |
|---|---|---|---|
| laptop, panel on iGPU, render-only dGPU | dGPU | iGPU | **split** (bridge engaged) |
| desktop / MUX'd, panel on dGPU | dGPU | dGPU | single-adapter (bridge elided) |
| single GPU | that one | that one | single-adapter (bridge elided) |

The bridge is not a special mode to opt into: it is what the rule degenerates
to when the two adapters differ.

### 2. Capability, not "discrete"

"Most capable render adapter" is resolved by the runtime, not hardcoded to
discrete. Ranking inputs, in order: dedicated VRAM, adapter type (discrete >
integrated > software), and exclusion of adapters that cannot present or lack
the required feature level. A discrete adapter that is render-only still wins
render; a software adapter never does. The scanout adapter is resolved by
`QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` → `sourceInfo.adapterId`
(`d3d_scanout_helpers`, #1078) — **never** by walking DXGI outputs, which
misreports on Optimus.

### 3. Fallback ladder when the split is unavailable

The split is not implemented for every graphics API, app class and layer type
yet. When it is not available for the session being created, the runtime must
choose a single adapter for everything, and it falls back **to the render
adapter** (i.e. today's behaviour):

1. **Split available** for this API + app class + layer set → apply the rule.
2. **Split unavailable** → everything on the **render adapter**; the OS carries
   the cross-adapter present. Log the degradation explicitly, naming why.
3. **Render adapter unusable** (absent, excluded, or the bridge cannot be
   built) → everything on the **scanout adapter**.

Rung 2 is chosen over rung 3 deliberately: real content is heavier than a test
cube, and losing render throughput is worse than paying transfer cost. Rung 3
is the correct answer only for light, latency-critical content — see *Open
questions*.

### 4. The runtime decides; the app is not consulted

No manifest field, no app setting, no per-app policy file in the default path.
The runtime already **enforces** placement for D3D (`oxr_d3d_get_requirements`
hands down `suggested_d3d_luid`; `oxr_d3d_check_luid` **rejects** a session
device on any other adapter) and selects the VkPhysicalDevice for Vulkan. The
app's job is to obey the LUID it is given.

Rationale: the app cannot know the machine's topology, the runtime cannot know
the app's content weight, and only one of those two facts is needed to apply
the rule. Content weight only matters on fallback rung 2 vs 3, which is a
transitional state.

Overrides remain, for diagnosis and for the exceptions below — `DXR_D3D_FORCE_GPU`
/ `DXR_VK_FORCE_GPU` (`igpu` | `dgpu` | `scanout` | `<index>`) — and they are
*overrides*, not policy inputs. If an app class turns out to need a durable
opt-out, it gets a manifest field then, with a measurement attached; not
speculatively.

### 5. How the decision is expressed, per graphics API

| API | mechanism | who enforces |
|---|---|---|
| D3D11 / D3D12 | `suggested_d3d_luid` via `xrGetD3D*GraphicsRequirementsKHR`; `oxr_d3d_check_luid` rejects mismatches | runtime, hard |
| Vulkan | runtime selects the `VkPhysicalDevice` (`select_physical_device`) | runtime, hard |
| OpenGL | **no LUID/device-selection API exists** — the per-exe `UserGpuPreferences` entry is the only lever | OS, advisory |
| Metal / Android | single-adapter in practice; rule degenerates | n/a |

**OpenGL and pick-first engines are the two cases that still need the registry
pin**, because the runtime cannot enforce placement for them. For those:

- the pin value is **derived from the runtime's decision** (`displayxr-cli info`
  reports both adapters), never hardcoded;
- it is written at **first run**, and **re-asserted at launch** — pins get swept
  by system policy and by other tooling (observed: 95 of 98 entries on a dev box
  reset to `GpuPreference=2`, including our own binaries);
- under the default rule this derivation yields the **render adapter**, i.e. the
  same value we ship today. Today's pin is right; it is currently right for the
  wrong reason, and would be wrong on any box that lands on fallback rung 3.

### 6. Build-time driver hints are retained but demoted

`NvOptimusEnablement` / `AmdPowerXpressRequestHighPerformance` stay. They are
link-time constants that cannot express "integrated" and cannot know the
topology, so they are **not** policy — they exist only to ensure the discrete
adapter is powered and enumerable. Nothing may depend on them for placement.

### 7. Service / IPC sessions

Under the rule the service holds devices on **both** adapters: ingest and
composite on the render adapter (where client swapchain images live), weave and
present on the scanout adapter. The "client and service must share one adapter"
constraint that motivated the original HIGH_PERFORMANCE pin applies to the
*ingest* device only. The service's own `DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE`
pin (`comp_d3d11_service.cpp`) must become the same resolved render adapter, and
its DP device must be scanout — this is #918 Phase 2 work.

### 8. Window moves between panels

The scanout adapter is a property of *the panel the window is on*. If a window
moves to a panel driven by a different adapter, the weave target changes. Until
live re-placement exists, the runtime resolves scanout at session creation,
logs the panel identity, and warns when the window's panel no longer matches.

## Consequences

- The common laptop case gets discrete-GPU render **and** scanout-adjacent
  weave: ~16 ms motion-to-photon instead of ~42 ms, with the cross-adapter
  transfer explicit and pipelined instead of buried in `Present`.
- Desktop and single-GPU boxes are unaffected — same rule, bridge elided.
- The split becomes load-bearing rather than experimental, so its failure modes
  (bridge build failure, slot drain, watchdog) become product-visible and need
  the fallback ladder in §3 to be real and logged.
- Scanout-placement wins measured on a box whose pins are swept to discrete are
  **partly measuring that box's configuration**; magnitudes quoted externally
  need that caveat, though the direction is unaffected.
- VRAM and bus cost: the bridge holds a ring of slots on both adapters.
- Battery: the rule wakes the discrete GPU for every 3D session, including
  trivial ones. See *Open questions*.

## Open questions (each needs a measurement, not an argument)

1. **Where is the render-weight crossover** at which the split beats
   all-on-scanout, and is it low enough that "always split" is right for light
   content too? The perf-ladder can produce this: adapter-placement arms
   (scanout / render / split) across a light app and a heavy one.
2. **Battery and thermals.** On DC power, is waking the discrete GPU for a light
   overlay a net loss? On shared-package-power parts (Meteor Lake) the CPU/GPU
   budget interacts. If the answer is yes, power state becomes a policy input
   and rung 3 becomes the DC default for light content.
3. **Split coverage.** Phase 1 is D3D11, in-process, projection-only. Vulkan,
   D3D12, the service path, zones/Local2D and mask planes each need the bridge
   before the rule stops falling back for them.
4. **Multi-3D-panel machines** — per-panel scanout adapters, and whether a
   session can migrate its weave target live.

# ADR-037: Adapter placement policy on hybrid-GPU devices

**Status:** PROPOSED (2026-08-22; amended 2026-08-23 by the #918 owner — added
§3a DP consent, corrected §7 to shipped, added the crossover instrument
constraint and current split coverage; amended again 2026-08-23 — **§1 is now
IMPLEMENTED for the covered paths**, see the implementation note below) ·
supersedes the implicit "everything on HIGH_PERFORMANCE" default · depends on
the #918 output-device split
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
inside the present path, every frame. (Weaving elsewhere is *possible* — it is
what we shipped for years and what fallback rung 2 below still does — but it is
never free, and the copy is invisible, unpipelined and inside `Present`.)

There is a fourth constraint the table cannot express, because it is not ours:
**the display processor is a vendor plug-in, and its ability to weave on a given
adapter is a plug-in property, not a runtime choice.** See §3a.

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

**IMPLEMENTED for the covered paths (#918 Phase 3, 2026-08-23).** Until Phase 3
this sentence was true of the design and false of the code: the bridge *was* a
mode to opt into, behind `DXR_WEAVE_ON_SCANOUT=1`, off by default for a release
cycle. It now engages automatically wherever the two adapters differ and the
path implements it — in-process D3D11, the D3D11 service (eligible presenter
kinds per §7), in-process D3D12. `DXR_WEAVE_ON_SCANOUT` is
retained as a **kill switch** (`=0`), not an opt-in; `=1` still parses and is a
no-op restatement of the default.

Everything else takes §3's ladder, and — this is the part the default flip made
load-bearing — **every rung names itself**. One `weave placement:` line per
session, on every graphics API including the two with no split, carrying
`split=1` or `split=0 reason=<token>` from a closed set of snake_case tokens
(`comp_split_gate.h`). The old short reason `env_not_requested` is gone: it was
the honest answer while the split was opt-in and would be a lie now.

§3a is now honoured on **both** in-process legs, by two different mechanisms —
and the difference is worth reading, because it is the difference between
asking and undoing. The D3D12 leg RETIRES an engaged split when the plug-in
declines the scanout adapter (#1164). The D3D11 leg NEGOTIATES instead: its
display-processor create moved into Stage A, on the scanout device, as the last
of Stage A's commit criteria, so a refusal is an ordinary Stage-A failure that
the existing fall-through already handles (#1168). Both report
`reason=dp_refused_scanout`; only the D3D12 one needs a `CHANGED` line, because
only it can discover the refusal after the placement has been announced.

### 2. Capability, not "discrete"

"Most capable render adapter" is resolved by the runtime, not hardcoded to
discrete. Ranking inputs, in order: dedicated VRAM, adapter type (discrete >
integrated > software), and exclusion of adapters that cannot present or lack
the required feature level. A discrete adapter that is render-only still wins
render; a software adapter never does. The scanout adapter is resolved by
`QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS)` → `sourceInfo.adapterId`
(`d3d_scanout_helpers`, #1078) — **never** by walking DXGI outputs, which
misreports on Optimus.

Implemented in `d3d_render_adapter` (the render-side sibling of the scanout
helper). Three points where the prose above was under-specified, settled during
implementation and recorded here so they are not re-litigated:

- **"Cannot present" must NOT be read as "enumerates no DXGI outputs."** On
  Optimus the render-only discrete GPU enumerates **zero** outputs and presents
  perfectly well through the OS hybrid present. Implementing the literal reading
  would exclude the dGPU and elect the *integrated* GPU as the render adapter —
  inverting this ADR on exactly the laptop topology it exists to serve. The
  exclusion means **no local presentation capability at all**: software
  rasterizers and remote adapters. This is the same trap as the scanout-side
  rule above, in the other direction: DXGI outputs describe *scanout*, and using
  them to reason about *render* is a category error either way.
- **Dedicated VRAM is the PRIMARY key**, not a co-equal input — it is the one
  signal a `UserGpuPreferences` sweep cannot reorder, which matters on machines
  whose per-exe pins are managed by policy. Adapter kind is a tiebreak, and
  since DXGI reports no adapter kind it is inferred (software flag, Basic Render
  Driver ids, and a 512 MB dedicated-VRAM watermark) — inference is allowed to
  break a tie and never to exclude.
- **Ties break on lowest enumeration index.** The ADR specified no tiebreak;
  determinism requires one.

Every choice carries a **provenance string** naming which rule decided it
(`most VRAM`, `adapter type`, `lowest index (tie)`, `env-forced: …`,
`only candidate`), logged once per process alongside adapter name and LUID, per
the rule PR #1023 established. Provenance is diagnostic infrastructure, not
decoration: with overrides sanctioned by §4, "why this adapter" must be
answerable from a log.

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

**"Log the degradation explicitly, naming why" is IMPLEMENTED (#918 Phase 3).**
Each rung emits exactly one `weave placement:` line naming both adapters, the
resulting regime, and `split=0 reason=<token>` from a closed set defined in
`comp_split_gate.h`. The line is formatted in one place (`aux_d3d`) and emitted
on every graphics API — including OpenGL, which has no split (no
adapter-selection API exists, so the runtime cannot place a GL context) and so
always reports rung 2. **Vulkan DOES have a split as of #1178** (shipped
v2.12.1/v2.13.0, all layer kinds): it reaches it via a same-adapter D3D11
deposit the compositor renders its atlas straight into, so the cross-adapter
transport is unchanged and no cross-adapter Vulkan work was needed. A VK session
reports rung 2 only when the app's `VkDevice` lacks `VK_KHR_timeline_semaphore`
(`reason=no_timeline_semaphore`). The service repeats the live token on its periodic
`[RENDER] split=0 reason=` line, and the in-process D3D12 path re-emits
`weave placement: CHANGED` when it retires an engaged split mid-session, so the
LAST placement line in a log is always the truth.

### 3a. The display processor must consent to the placement

The runtime picks the adapters; it does **not** get to assume the vendor
plug-in can weave on the one it picked. Placement is therefore a *negotiation*
with the DP, and the negotiation must happen **before** the session commits to
a topology:

- The runtime resolves the scanout adapter, then asks the plug-in to create its
  weaver on a device on that adapter. A plug-in may legitimately fail: its SDK
  may not support that vendor's GPU, that driver, or that API on that adapter.
- **A weaver-creation failure on the scanout adapter is a fallback trigger, not
  a session failure** — degrade to rung 2 (everything on the render adapter,
  where the DP demonstrably worked before) and log the plug-in's reason.
- This is not hypothetical. Weaving Vulkan on an Intel iGPU was *unproven* until
  it was measured on 2026-08-22 (it works — clean, zero errors, maintainer-
  eyeballed); the D3D11 path was proven earlier. Each (vendor × API × adapter)
  cell is an empirical question, and the policy must treat an unproven cell as
  "ask, then fall back", never as "assume".
- Corollary for vendor onboarding: a plug-in that can only weave on one specific
  adapter class must say so by failing weaver creation cleanly, not by weaving
  wrongly or crashing. See `docs/guides/vendor-plugin-onboarding.md`.

**"Before the session commits" is a real ordering constraint, not a figure of
speech** (#1168). Where a path can ask first, it must: the in-process D3D11 leg
creates the weaver on the scanout device as the LAST of Stage A's commit
criteria — after the bridge and the egress ring, immediately before
`split_active` is set — so a refusal falls out through the same teardown as a
failed heap allocation and needs no recovery code of its own. Where a path
cannot (the in-process D3D12 leg builds its target first), a retire is the
correct answer, and it must then re-emit the placement line. Both are
conformant; the negotiation is cheaper, because a refusal it catches has
nothing to undo.

The bind key `(hwnd, device)` constrains either shape: a vendor weaver is
one-per-HWND, so a second create against a window that still holds one is
refused *silently*. A negotiation that creates a weaver must therefore keep it
(it becomes the session's) or destroy it before anything else creates one.

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
| OpenGL | **no device-selection API exists** — the per-exe `UserGpuPreferences` entry is the only lever. Adapter *identity* usually IS readable: `GL_EXT_memory_object_win32` → `glGetUnsignedBytevEXT(GL_DEVICE_LUID_EXT)` (#1159) | OS, advisory |
| Metal / Android | single-adapter in practice; rule degenerates | n/a |

**Amendment (#1159): identity and selection are separate questions for GL, and
only selection is genuinely missing.** This row originally read "no
LUID/device-selection API exists". The selection half stands — nothing lets the
runtime move a GL context. The identity half was wrong: `GL_EXT_memory_object_win32`
has the driver report its own D3D LUID, so the runtime can *read* which adapter
WGL picked on any current NVIDIA / Intel / AMD Windows driver. That matters
because it is what lets the GL compositor place its D3D11 interop devices on the
GL context's adapter instead of on `D3D11CreateDevice(NULL, …)` — DXGI
enumeration order — and it is what makes a cross-adapter GL interop *loggable*
rather than invisible. The GL `weave placement:` line therefore prints the real
`render=` LUID when the driver answers and `render=UNKNOWN` only when it does
not. Where identity is unavailable the compositor degrades explicitly:
`GL_VENDOR` PCI-vendor match, then a `GL_RENDERER`-vs-description match (both
used only when unambiguous), then the §2 resolver — never NULL.

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
*ingest* device only.

**This is SHIPPED, not planned** (#918 Phase 2b, PRs #1083…#1143, merged
2026-08-22): the service holds an out-device on the scanout adapter, composites
and crops on it, binds the DP by `(hwnd, device)` with dwell hysteresis, carries
the recipe with the pixels (#1140), and hardens the transport (adaptive
in-place ingress, per-presenter weave ledgers, DEVICE_REMOVED completeness).
Eligibility is by **presenter kind**, which is the service-path expression of
§3's ladder: `SERVICE_WINDOW` and `APP_HWND` presenters split;
`CLIENT_TEXTURE` and self-presenting clients are *structurally* ineligible
because the client owns the present, so they take rung 2.

The service's own `DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE` pin
(`comp_d3d11_service.cpp`) is **also done** (#1153): `create_system` calls the
§2 resolver, so the capability ranking — not DXGI's idea of "high performance"
— decides where ingest lands, and the choice is logged once with name, LUID,
provenance, and an explicit MATCH/DIVERGES assertion against what the old
hardcode would have picked.

`DXR_D3D_FORCE_GPU` reaches the ingest device too, which is what makes the
all-on-scanout arm of Open Q1 buildable for the service family. **A forced
ingest is never refused.** Ingest is the one device that must share an adapter
with clients, so forcing it to the scanout adapter while the clients stay on
the render adapter is exactly the cross-adapter configuration the arm exists to
measure; the divergence is logged loudly on both sides — `ADR-037 §7: the
service INGEST device was FORCED …` in the service, `CROSS-ADAPTER by explicit
override; proceeding` in `oxr_d3d_get_requirements` — and proceeds. An
eligibility check that assumed the two always agree would have deleted the arm,
so §4's override outranks the assumption. `displayxr-cli info` reports the
resolved ingest adapter next to the split line, so an arm can be verified
before the service is started.

### 8. Window moves between panels

The scanout adapter is a property of *the panel the window is on*. If a window
moves to a panel driven by a different adapter, the weave target changes. Until
live re-placement exists, the runtime resolves scanout at session creation,
logs the panel identity, and warns when the window's panel no longer matches.

Live re-placement is **not** a small feature, and the ADR should not imply it
is: changing the weave target means destroying and recreating the vendor
weaver mid-session (the Leia SR weaver is one-per-HWND and must be destroyed
before recreation), and weaver recreation under load is precisely the failure
class behind the workspace-wedge epic (#925 / leia#144). Any future live
migration needs that teardown to be provably non-blocking on the render thread
before it is attempted.

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

1. ~~**Where is the render-weight crossover** at which the split beats
   all-on-scanout, and is it low enough that "always split" is right for light
   content too?~~ **ANSWERED for the service/D3D family, 2026-08-30 (#1154):
   there is no crossover in favour of all-on-scanout — the split wins or ties at
   every render weight measured, so "always split" stands and §3's ladder needs
   no light-content exception.**

   Measured on the reference box with one fullscreen 3840x2160 client
   (`cube_hosted_d3d11_win`, forced IPC), queue depth pinned across arms
   (`DXR_APP_HWND_LATENCY=2`), render weight swept with `gpu_loadgen` on **each
   arm's own render adapter**, 2 reps x 3 duties, presenter kind and split state
   asserted per run. R is quoted as the fraction of frames landing on ONE
   refresh period (`q1`), because R on this path is bimodal — it quantises to
   whole refresh periods and a mean of it describes no real frame.

   | render weight | render (no split) | **split** | all-on-scanout |
   |---|---|---|---|
   | idle | 54.5 w/s, q1 85% | **70.7 w/s, q1 100%** | 64.4 w/s, q1 99% |
   | 40% duty | 64.0 w/s, q1 92% | 60.2 w/s, q1 90% | 62.7 w/s, q1 93% |
   | 85% duty | **28.7 w/s, q1 2%** | **66.2 w/s, q1 94%** | 50.7 w/s, q1 63% |

   Three things follow. **(a)** All-on-scanout never wins: split R p50 is
   16.00/16.01/16.09 ms against scanout's 16.38/16.41/16.44 at the three
   weights, and split leads on throughput everywhere except the 40% tie — so
   **rung 3 does not earn a default**, at any weight, on this family. **(b)**
   The split's margin is a *load* effect: at 85% duty the scanout adapter runs
   out of headroom carrying the app's render as well as the weave (50.7 w/s,
   q1 63%), while the split keeps the two on separate adapters and holds 66.2
   w/s at q1 94%. **(c)** The no-split arm **collapses** under heavy render
   weight — 28.7 w/s with 2% of frames at one refresh (p50 41.54 ms, and 62% of
   frames at *three* refresh periods in the worse rep) — which is the strongest
   measured argument for Phase 3's default-on.

   Limits that travel with the numbers: n=2 reps over 0/40/85% duty, so the
   claim is "no crossover found between idle and 85%", not "none exists"; the
   40% row is a three-way tie within noise; and the standing
   `UserGpuPreferences` caveat in *Consequences* applies, so directions hold
   while magnitudes are partly a property of this machine. **Family B
   (in-process Vulkan, rung 2 vs rung 3, framed in throughput) is still open**
   — the instrument constraint below is why it must not be framed in R.

   **Instrument constraint that shapes the experiment — read before building
   arms.** Motion-to-photon (R) is *not* measurable on the all-on-scanout arm:
   the harness derives scanout truth from `VK_KHR_present_wait`, and Intel
   iGPUs expose no Vulkan presentation-timing extensions at all, so the pacing
   instrument is dormant there by construction. In-process Vulkan additionally
   emits no steady-state per-frame R even on the discrete arm (measured
   2026-08-22 — only a first-frame outlier), so a latency-framed crossover is
   currently unmeasurable for VK on either side. Therefore: **frame the
   crossover in throughput and frame-time under load (witness counters), not in
   R**, or fund the VK-0 instrumentation first and frame it in R afterwards.
   The endpoints should also be matched on *graphics API*, not just weight — a
   light-D3D11 vs heavy-VK pair would confound API with weight.
   **FAMILY B — in-process Vulkan — ANSWERED 2026-08-31 (#1154): the answer
   FLIPS with render weight, so rung 3 stays a lever and never becomes the
   in-process VK default.** Framed in throughput and GPU busy, never R (see the
   instrument constraint below). Endpoints matched on graphics API:

   | app | rung 2 (render) | rung 3 (`DXR_VK_FORCE_GPU=scanout`) |
   |---|---|---|
   | light (`cube_handle_vk_win`) | 60.0 weave/s, dGPU ~70 ms/s | **60.0 weave/s, dGPU 0** |
   | heavy (gaussian splat) | 59.9 weave/s, 59.9 present/s | **51.4 weave/s, 15.1 present/s** |

   For LIGHT Vulkan content rung 3 is free: identical throughput while the
   discrete GPU goes to exactly zero (the iGPU roughly doubles, to ~134 ms/s,
   nowhere near saturation) — the case that matters on battery and for
   thermals, and overlay-class apps should take it. For HEAVY content the
   scanout adapter cannot carry the app's render *and* the weave at 4K60: it
   holds ~68% busy while presents fall to ~51/s and **weaves collapse to ~15/s**,
   a three-quarters cut in real 3D update rate that a presents-only view
   understates. So the crossover hypothesised above is real and sits between
   these endpoints; two points bracket it rather than locate it.

2. **Battery and thermals.** On DC power, is waking the discrete GPU for a light
   overlay a net loss? On shared-package-power parts (Meteor Lake) the CPU/GPU
   budget interacts. If the answer is yes, power state becomes a policy input
   and rung 3 becomes the DC default for light content.
3. **Split coverage** — the rule falls back for whatever the bridge does not
   yet cover, and **since Phase 3 the coverage boundary IS the safety story**:
   the split is the default, so a path that cannot honestly answer "is the split
   implemented for me?" must answer NO and take rung 2, never half-engage.
   Status 2026-08-23:
   - **shipped, and now auto-on**: in-process D3D11 — including the §3a
     negotiation, which asks the plug-in for a weaver on the scanout adapter
     inside Stage A and falls through to rung 2 (`reason=dp_refused_scanout`)
     if it declines (#1168); the **service/IPC path for every client API** (the
     split lives in the service's compositor, downstream of IPC, so a
     D3D12/VK/GL client benefits whenever its presenter kind is eligible);
     zones/Local2D and mask planes; recipe-with-pixels; transport hardening.
   - **shipped, auto-on**: in-process D3D12 (Unity/Unreal) — ladder D12-0…D12-5
     COMPLETE (#1150, #1151, #1164, D12-4, D12-5). D12-3 shipped it
     projection-only, D12-4 added the 2D plane transports so **zones and Local2D
     composite on the scanout adapter** — which is what the flagship Unity path
     needed, since the shipping avatar sample carries zones plus a Local2D band
     every session and used to retire on frame one — and D12-5 added the
     **app-authored (Tier-3) mask plane** plus the zone-wish publish on the
     DP+target path (#1175), which had been inert on exactly the path an active
     split takes. Every layer kind this leg can submit now composites and
     publishes on the scanout adapter. What still retires the split for the
     session is a **failure**, never a feature: a mask plane this machine could
     not allocate (`reason=authored_mask`) and a DP that declines the scanout
     adapter (`reason=dp_refused_scanout`, §3a). `reason=layers_unsupported` is no
     longer emitted by this leg — a D3D12 session reporting it is a pre-D12-4
     build.
   - **pending a decision, not just work**: in-process Vulkan. The cheap
     alternative measured well — whole-app placement on the scanout adapter via
     `DXR_VK_FORCE_GPU=scanout` eliminated *all* of the app's discrete-GPU work
     including the cross-adapter copy engine, ran clean, and was maintainer-
     eyeballed — so for light VK content the rule's rung-3 answer may simply be
     correct and a VK bridge unnecessary. The VK split's value case now rests on
     heavy-render VK content, which is question 1 above. Phase 3 does **not**
     pre-empt that decision: VK takes rung 2 explicitly and says so
     (`reason=api_unsupported`), which is the same placement it had before, now
     visible in the log instead of inferred from its absence.
   - **not implementable on the runtime's terms**: in-process OpenGL. §5 — no
     device-selection API, so the runtime cannot ask WGL for another adapter.
     Rung 2, `reason=api_unsupported`. Amended by #1159: the runtime *can*
     usually learn which adapter WGL chose (`GL_DEVICE_LUID_EXT`), so `render=`
     names it and `render=UNKNOWN` is now reserved for drivers that do not
     report it — still an honest unknown, never a guess. Knowing the adapter
     does not make the split implementable; it only makes the interop devices
     placeable and the placement legible.
   - **n/a**: Metal / Android (single-adapter; the rule degenerates).
4. **Multi-3D-panel machines** — per-panel scanout adapters, and whether a
   session can migrate its weave target live.

# Motion-to-photon levers and their defaults

Every runtime knob that changes how long it takes a head movement to reach the panel, what each
one actually does, and what it is set to out of the box on each GPU topology.

**The short version:** late weave and repaint are the levers that carry the product. Everything
else is either a fallback for hardware that cannot do those, a probe, or an experiment that has
not paid off yet. If you are reading this to decide what to turn on: turn nothing on. The defaults
are the tuned configuration.

**Scope.** This covers the *runtime* side only. A display processor may also do pose re-sampling
of its own inside the weave — that is DP-internal behaviour, it varies by vendor, and it is
documented in the vendor's own plug-in repo (see [`docs/vendors/README.md`](../vendors/README.md)).
The only thing the runtime guarantees a DP is a well-paced weave with a fresh pose; what the DP
does with it is the DP's business.

## The central trade

There are two ways to stop the viewer seeing a stale eye position, and they are alternatives, not
a stack:

| | Approach | What it costs |
|---|---|---|
| **A** | Let frames queue (`DXR_DEFER_PRESENT`) so that a DP capable of re-sampling the pose at submit time has something to correct | +1 frame of eye-pose staleness, which the DP-side correction is supposed to pay back. Measured null twice. |
| **B** | Never let the weave go stale in the first place — **repaint** re-weaves the last atlas at display rate with a **fresh** eye pose | A repaint costs GPU time even when the app has produced nothing new |

**We ship B.** Repaint runs at the display rate, so the woven pose is at most one refresh old
regardless of how slow the app is. That plausibly makes DP-side submit-time correction redundant
*by construction*: the staleness it exists to remove is already being removed every refresh.

That is the reasoning behind the defaults below — repaint and late weave on, deferred present off.

## Latency levers

### `DXR_LATE_WEAVE` — **default ON** (all paths)

Weave as late as possible before scanout rather than at submit time. This is the single largest
win we have, and it is the product behaviour on every path:

```
VK         96 -> 17 ms
D3D12      62 -> 17 ms
D3D11      32 -> 17 ms
workspace  29 -> 17 ms
```

`DXR_LATE_WEAVE=0` opts out, for A/B or triage only.

| Companion | Default | Effect |
|---|---|---|
| `DXR_LATE_WEAVE_MAX_LATENCY` | `1` | Frame-latency depth, clamped to `1..LATE_WEAVE_MAX_DEPTH` |
| `DXR_LATE_WEAVE_AUTOBACKOFF` | on (`0` disables) | Backs the depth off when probes fail; dwell 30 s, doubling per failure, capped at 5 min |

Late weave depends on present-timing feedback from the swapchain. Where the platform does not
provide it, it is **dormant** rather than wrong — see the topology table.

### `DXR_WEAVE_REPAINT` — **default ON** (#868)

Re-weaves the last atlas at the display rate while the app is between frames, **with a fresh eye
pose each time**. This is what keeps 3D solid when the app runs below refresh: at 15 fps the app
produces 15 atlases a second but the panel still gets ~60 correctly-phased weaves.

A repaint replays *rendering state only* — it never touches app-owned state.

| Probe | Purpose |
|---|---|
| `DXR_WEAVE_REPAINT_FORCE=1` | Repaint every refresh regardless of app rate. Correctness probe; it **will** cost frame rate. |
| `_DIAG`, `_HASH`, `_NO2D`, `_DRAIN`, `_REFLATTEN`, `_APPTHREAD` | Bisect probes from the #868 investigation. Not for production. |

**Repaint weaves outside the app's frame loop, and a DP may not expect that.** A display processor
is entitled to assume a weave cadence tied to app frames, and one that does may degrade — possibly
silently — when repaint drives extra weaves. That is a per-vendor contract question, not a runtime
one; if you change repaint's cadence, re-check it against each vendor's documented constraints.

### `DXR_VK_QUEUE_MODE` — **default `auto`** (#902)

Repaint needs somewhere to submit from. Three tiers, resolved automatically:

| Tier | Condition | Behaviour |
|---|---|---|
| 1 | Driver gave us a dedicated queue | Runtime-owned queue, no layer involved |
| 2 | Single queue only, `VK_LAYER_DXR_queue_lock` live | Shares the app's queue, serialised by the layer |
| 3 | Neither | **Repaint off**, pacing only |

Tier 2 is not assumed to work — the runtime resolves `vkGetQueueLockMarkerDXR` to confirm the
layer is actually in *this* device's chain. The layer had never been packaged until #917, which
meant tier 2 was unreachable on every single-queue GPU and those machines silently fell to tier 3.

Overrides: `queue` = tier 1 only · `layer` = force tier 2 even where a queue exists, so the layer
path is testable on multi-queue GPUs · `off` = no layer injection, no repaint.

### `DXR_DEFER_PRESENT` — **default OFF** (#837)

Return from the weave without waiting on the submit fence, parking the frame's command buffer,
framebuffer and fence and retiring exactly that predecessor at the top of the next call. Net
effect: one frame genuinely in flight instead of zero.

It does **not** change the latency horizon handed to the DP. It is a fence-discipline change.

**Measured as a null twice** — once unloaded (60→60 fps) and once under load with the mechanism
proven live (`#837: first frame PARKED`), 15 fps and identical results either way. It only touches
app frames (`!is_repaint`), so with repaint on it affects roughly 15 of every 60 weaves. Keep it
off.

### Prediction horizon — computed by the DP, no runtime env var

The runtime feeds the DP a **measured weave→scanout residual** through its frame-timing loop. A DP
that predicts eye position can use that instead of assuming a fixed pipeline depth, which means a
change in real present latency is largely self-correcting without any runtime knob.

**Unverified:** whether that residual still means what the DP thinks it means when
`DXR_DEFER_PRESENT` moves the present into the following call. Check before that flag is ever
considered for default-on.

### Adjacent levers that change the picture

| Lever | Default | Note |
|---|---|---|
| `DXR_VK_BRIDGE_PACING` | governor | `0..3` pins the queue depth and disables governor transitions (#912) |
| `DXR_PRESENT_OPAQUE` | `false` | Opaque flip chain instead of the composed chain. Different pacing source: `GetFrameStatistics` vs the DComp compositor clock |
| `DXR_D3D_FORCE_GPU` / `DXR_VK_FORCE_GPU` | unset | Which adapter renders and weaves (#821). See [adapter-selection.md](adapter-selection.md) |
| `DXR_FRAME_STAGE_TIMING` | off | Per-stage CPU timing of the windowed commit. `composite=` is the GPU wait |

## Defaults by GPU topology

| | dGPU (discrete, multi-queue) | iGPU (integrated) | Hybrid laptop |
|---|---|---|---|
| Late weave | **on**, fully effective | **on**, but *dormant on VK* where the driver exposes no VK present-timing extensions | on; effective on whichever adapter owns the present |
| Repaint | **on**, tier 1 dedicated queue | **on**, tier 2 via `VK_LAYER_DXR_queue_lock` (single queue family) | on; follows the adapter the session was created on |
| `DXR_DEFER_PRESENT` | off | off | off |
| `DXR_PRESENT_OPAQUE` | off | off — **pure cost on VK** without timing extensions to exploit; the opposite has been observed for engine apps | off |

**Hybrid is the unresolved one.** On this class of laptop the panel is frequently **scanned out by
the integrated adapter** — it holds the active mode — while the app renders on the discrete one.
That makes the weave a cross-adapter operation, and cross-adapter sharing has its own traps (KMT
handles share no pixels on some integrated drivers; use NT handles). Whether it is better to weave
on the dGPU and copy, or to run the weave and repaint next to the scanout, is **open — see #918**.
There is no hybrid-specific default today; you get the dGPU column with a cross-adapter copy.

## Measuring any of this

Two traps that have each produced a wrong answer here, both worth more than any advice above:

1. **An unfocused window throttles.** The same load reads ~15 fps unfocused and ~35 focused. Take
   every reading, *and* every load calibration, with the app window focused.
2. **Counters are not pixels.** Present counts and frame rates prove nothing reached the panel.
   Confirm on the display.

Synthetic GPU contention is a poor lever for reaching a target frame rate: presents are
vsync-quantised, so the rate steps 60/N rather than sliding, and enough contention to force a low
step starts destabilising the app under test. Prefer an app that is genuinely heavy in its own
render loop.

Vendor-specific verification recipes — debug overlays, forced-off switches, purpose-built load
demos — live in the vendor plug-in repos, not here.

## See also

- [`docs/adr/ADR-007`](../adr/ADR-007-compositor-never-weaves.md) — the compositor never weaves; the DP does
- [`docs/architecture/compositor-pipeline.md`](../architecture/compositor-pipeline.md)
- [`docs/reference/adapter-selection.md`](adapter-selection.md) — GPU placement on hybrid machines
- [`docs/vendors/README.md`](../vendors/README.md) — index; DP-internal behaviour lives in each vendor's repo

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
| `DXR_WEAVE_ON_SCANOUT` | **on (kill switch)** | **DEFAULT ON since #918 Phase 3 (ADR-037 §1).** The output-device split engages automatically wherever the scanout adapter differs from the render adapter **and the path implements it**: in-process D3D11, the D3D11 service (eligible presenter kinds only — `SERVICE_WINDOW` / `APP_HWND`; `CLIENT_TEXTURE` and self-presenting clients are structurally ineligible), and in-process D3D12 projection-only. Vulkan and OpenGL have no split and take ADR-037 §3 rung 2, logged as `split=0 reason=api_unsupported`. Set `DXR_WEAVE_ON_SCANOUT=0` (or `f…`/`n…`/`off`) to KILL it and force the old single-adapter behaviour; `=1` still works and is now a no-op restatement of the default. What it does: the app keeps rendering on its own adapter while the swapchain, the display processor, the HUD and the repaint loop move to the adapter that scans out the panel, with the composited atlas crossing once per app frame through a D3D12 cross-adapter heap. Removes the cross-adapter present, so the weave→scanout residual drops from ~2.5 frame periods to one. Covers **zones, Local2D, authored masks and the 2D-under backdrop** as of Phase 2a on the D3D11 legs (the four mask rasterizers take pure CPU rects and are built on the output device, while the Local2D flatten, the backdrop and a Tier-3 app-drawn mask ride the same egress slot as the atlas as extra planes — dirty-box + change-skip; measured 3 copies per session at rest, zero bridge traffic per repaint tick). The D3D12 leg is projection-only and RETIRES the split for the session on the first zones/Local2D/mask frame (`reason=layers_unsupported`). On the SERVICE it covers **both** paths — direct (the focused client's cropped atlas) and compose (the crop of the combined atlas) — with the panel DP staying on the scanout adapter across a shell attach/detach, the zones wish mask following the DP's device rather than the output half, and unfocused app-HWND flat repaints skipped while it is on. No-op (`reason=same_adapter`) when the scanout adapter already is the app's, and refused outright under `DXR_LEGACY_STANDALONE`. Every fallback names itself on the one `weave placement:` line. `DXR_WEAVE_ON_SCANOUT_DEPTH=1` forces the deterministic seq−1 slot instead of the opportunistic newest-ready pick; `DXR_TEST_SPLIT_FAIL_STAGEA=1` forces Stage A to fail and `DXR_TEST_FAKE_DP_REFUSE=1` forces a DP refusal on the scanout adapter (both in-process legs: D3D12 retires the engaged split, D3D11 fails Stage A before it engages — ADR-037 §3a), for exercising the two halves of the fallback matrix. **#1172:** an ineligible SERVICE client (`CLIENT_TEXTURE` / present-owner) does not merely stay off the scanout adapter — it gets a display processor of its **own** on the render device, bound to its **own** window, because the shared panel DP follows whichever presenter owns the panel and weaving a render-adapter texture through a scanout-adapter weaver faults inside the vendor SDK. Logged as `split=0 reason=presenter_ineligible,weave_on_ingest`. `DXR_TEST_FORCE_WEAVE_INGEST_DP=1` forces that path on with no split at all (so it is exercisable on a single-adapter box, with `test_apps/probes/weave_rpc_probe_d3d11_win`), `=2` forces the DP creation to fail so the refusal guard runs instead of the weave |
| `DXR_SPLIT_INGRESS` | `adaptive` | **#918 Phase 2b PR 6, service only.** What the bridge does with the app-device source texture. `adaptive` reads a source that has held still IN PLACE and stages only the frame a source CHANGE lands on (a focus change, a controller attaching, a crop texture reallocating), with a 250 ms settle before it re-binds so a resize drag cannot drive one shared-handle re-open per frame. `staged` pins the PR 3-5 behaviour — one extra full-content app-device copy every frame — and exists as the A/B control, not as tuning. A source the render thread does not exclusively own is never read in place regardless: a client's own atlas is written by that client's IPC thread and always stages |
| `DXR_FRAME_STAGE_TIMING` | off | Per-stage CPU timing of the windowed commit. `composite=` is the GPU wait |

## Defaults by GPU topology

| | dGPU (discrete, multi-queue) | iGPU (integrated) | Hybrid laptop |
|---|---|---|---|
| Late weave | **on**, fully effective | **on**, but *dormant on VK* where the driver exposes no VK present-timing extensions | on; effective on whichever adapter owns the present |
| Repaint | **on**, tier 1 dedicated queue | **on**, tier 2 via `VK_LAYER_DXR_queue_lock` (single queue family) | on; follows the adapter the session was created on |
| `DXR_DEFER_PRESENT` | off | off | off |
| `DXR_PRESENT_OPAQUE` | off | off — **pure cost on VK** without timing extensions to exploit; the opposite has been observed for engine apps | off |

**Hybrid is the one still being settled — but the answer is now measured.** On this class of laptop
the panel is frequently **scanned out by the integrated adapter** — it holds the active mode — while
the app renders on the discrete one, which makes today's weave a cross-adapter operation: every
woven frame, and every repaint tick, crosses inside `Present`. Measured on the reference box
([hybrid-igpu-weave.md](../investigations/hybrid-igpu-weave.md)): 42 ms present-to-display against
16 ms for a scanout-local pipeline, and a forced repaint burning 327 ms/s of dGPU copy engine
purely on re-transfer.

The **output-device split** (above) is that scanout-local pipeline, and since #918 Phase 3 it is
the DEFAULT rather than a flag. It runs on the D3D11 windowed path, on the in-process D3D12 path
(projection-only), and on **both** of the
D3D11 service's paths — the DIRECT path (PR 3) and the COMPOSE path with a workspace controller
attached (PR 4). So a forced-IPC `_handle` app, a hosted client presenting through the service
window, and the shell composing several of them all weave and present on the scanout adapter, and
the panel display processor stays on that adapter across a shell attach and detach: PR 3's
suspend-under-a-controller is deleted.

Under the service the eligible presenters are exactly the two the runtime owns the surface of
(`SERVICE_WINDOW`, `APP_HWND`); `CLIENT_TEXTURE` and present-owner (`SELF`) clients are
structurally ineligible — the weave destination is a texture the client opened on the app adapter
and presents itself — so the panel DP's bind key carries the device alongside the window, and
migrates (dwell-limited) when focus crosses that boundary. The zones **wish mask** follows the DP's
device rather than the output half (PR 5): it is a CPU-rect raster handed to the vendor as a
hardware control signal, so it wants whichever device the DP is on, which for an ineligible
presenter is still the app device.

`DXR_APP_HWND_LATENCY` **stays at 2** on split sessions: the expectation was that a scanout-local
chain no longer needs depth 2's slack, and the 2x2 said the opposite — off the split depth makes no
difference (16.56 / 16.57 ms p50), on the split depth 1 costs a whole extra refresh (32.70 ms p50)
because the weave consumes a slot the bridge landed a frame ago and a single buffer serialises the
present against the copy legs.

`DXR_SPLIT_INGRESS` (PR 6) is the split's own second lever, and the only one that changes what it
COSTS rather than where it runs. Default `adaptive`: a source texture that has held still is read in
place by the bridge's producer, and only a source CHANGE (focus, a controller attaching, a crop
texture reallocating) stages through the app-device ring for a frame. `=staged` pins the PR 3-5
behaviour — one extra full-content app-device copy on every frame — and is the A/B control, not a
tuning knob.

Cross-adapter sharing has its own traps: D3D11 has no cross-adapter texture path at all on this
stack (all six share flavours fail at the open call, both directions), so the transport is a D3D12
`SHARED | SHARED_CROSS_ADAPTER` heap with D3D11 on both ends; KMT handles share no pixels on some
integrated drivers, so use NT handles.

There is still **no hybrid-specific default** — unflagged, you get the dGPU column with a
cross-adapter copy. #918 stays **open** on Phase 3, which owns default-on, and default-on has a
hard external gate: **SR Platform ≥ 1.37.0+1498**. Below that the scanout weave emits
transparent-black bursts while nobody is tracked (#1134, closed — a non-monotonic pulse-animation
clock in the vendor weavers, fixed as LeiaSR#190 and verified 0/40 against 8/40 pre-fix). Until
Phase 3 lands, read the split as the lever to try first when a hybrid box shows a ~2.5-frame
residual, not as tuning advice.

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

R is unobtainable on the DComp-bridge (transparent VK) presentation path — DXGI frame statistics
are unavailable there (#1044); the plain-swapchain paths are the instrumented ones.

## See also

- [`docs/adr/ADR-007`](../adr/ADR-007-compositor-never-weaves.md) — the compositor never weaves; the DP does
- [`docs/architecture/compositor-pipeline.md`](../architecture/compositor-pipeline.md)
- [`docs/reference/adapter-selection.md`](adapter-selection.md) — GPU placement on hybrid machines
- [`docs/vendors/README.md`](../vendors/README.md) — index; DP-internal behaviour lives in each vendor's repo

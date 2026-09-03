# Investigation: weave/repaint placement on hybrid laptops with iGPU-scanned panels (#918)

**Recommendation: move the weave, repaint, and present to the scanout adapter (iGPU) on
non-MUX hybrid laptops, keeping the app render + session device on the dGPU, bridged by a
D3D12 cross-adapter heap — measured on the reference box, the iGPU-local pipeline does
*everything* for ~1.2 ms/frame more iGPU time than today's path spends merely *receiving*
the dGPU's frames, frees the dGPU entirely, and removes ~26 ms of present-to-display
latency.**

Measured 2026-08-19 on the reference hybrid box (see [Topology](#topology-covered)).
Analysis phase (code audit + external research) is in the
[#918 comment](https://github.com/DisplayXR/displayxr-runtime/issues/918); this doc is
the measurement campaign it called for.

## Topology covered

| | |
|---|---|
| Panel | AUO B194 3840×2160@60 (Leia SR), scanned out by **Intel UHD Graphics** (Comet Lake-H, Gen9.5) — holds the active mode |
| Render GPU | NVIDIA RTX 3080 Laptop (render-only, no output) |
| Hybrid present | **Two-copy** WDDM path — Gen9.5 is below the CASO cutoff (Intel needs 11th-gen Iris Xe + WDDM 3.0) |
| Runtime | Installed v2.7.2-11-g5e49d6953, Leia SR plug-in 2.3.0, in-process `cube_handle_d3d11_win`, fullscreen 3840×2160, focused, High-performance plan, AC |

The conclusion applies to **non-MUX hybrids where the panel is iGPU-scanned**. MUX'd /
Advanced-Optimus machines (panel on dGPU) are unaffected — today's placement is already
local there. On CASO-capable machines the per-app-frame delta shrinks (one copy instead
of two) but the repaint delta (zero vs full re-transfer per tick) is topology-independent.

## Configurations

- **A (today's default):** session + weave on the dGPU; woven output crosses adapters
  inside `Present`.
- **B (proxy for the proposal):** `DXR_D3D_FORCE_GPU=igpu` — entire session on the iGPU,
  local present. This proxy *overstates* B's iGPU cost (it also renders the app there)
  and *omits* the atlas bridge cost, which was measured separately
  ([transport](#cross-adapter-transport)).

## Feasibility gate — SR weaver on Intel

Passed first try: weaver created via the v2 C API on the Intel adapter (LUID `0x24BBF`),
3D mode applied, weave timing feedback engaged, repaint thread live, clean destroy, zero
errors. Log: `weave: target=3840x2160 vp=(0,0 3840x2160) view=1920x1080`.

## GPU cost — `\GPU Engine(*)\Running Time` deltas

Counter: **`\GPU Engine(*)\Running Time`** (cumulative 100 ns ticks; deltas over 30 s
windows — *never* `Utilization Percentage`), attributed per PID + LUID + engine type so
ambient noise (an editor hammering the dGPU copy engine at ~200 ms/s, NVIDIA Share waking
on fullscreen) is excluded exactly. Interleaved B,A,B,A runs. Values = GPU-busy ms per
wall-clock second; app held 60 fps in all normal runs (repaint gated, ~6 repaints total).

| Normal (app @60 fps) | app iGPU 3d | app dGPU 3d | app dGPU copy | iGPU total | dGPU total |
|---|---|---|---|---|---|
| A run 1 | 379.8 | 22.3 | 179.2 | 397.4 | 207.9 |
| A run 2 | 391.5 | 22.1 | 188.7 | 407.6 | 216.1 |
| B run 1 | 449.8 | — | — | 450.6 | 0.6 |
| B run 2 | 469.5 | — | — | 470.2 | 0.1 |

Reading A: the dGPU renders + weaves for only **22 ms/s**; the *transfer* costs
**184 ms/s dGPU-copy (copy 1)** plus **386 ms/s of iGPU 3d (copy 2, executed by the
iGPU, attributed to the app)**. The iGPU burns ~6.4 ms/frame moving pixels it never
computed. Reading B: the whole pipeline — render, composite, 4K weave, local present —
is ~460 ms/s on the iGPU, i.e. **~74 ms/s (≈1.2 ms/frame) more than A's receive cost
alone**, with the dGPU idle. System-wide, B uses ~25 % less total GPU time.

| Forced repaint (`DXR_WEAVE_REPAINT_FORCE=1`, both configs settled at ~30 repaint/s) | app iGPU 3d | app dGPU 3d | app dGPU copy | iGPU total | dGPU total |
|---|---|---|---|---|---|
| A force | 423.4 | 10.7 | **326.8** | 436.9 | 352.2 |
| B force | 427.5 | — | — | 428.2 | ~0 |

The repaint arm, as predicted, is the strongest: every A repaint re-crosses the full
woven frame (dGPU copy nearly doubles), and **A costs the iGPU more (437 ms/s) than B's
entire pipeline (428 ms/s)** — the iGPU pays more to *receive* A's repaints than to
weave them itself — while A additionally burns 352 ms/s of dGPU.

## Latency

PresentMon 2.5.1, 12 s captures, fullscreen. Both configs achieved
`Hardware Composed: Independent Flip` — the A-path tax is the copies, not composition.

| | MsUntilDisplayed mean / p95 | MsGPULatency mean |
|---|---|---|
| A (dGPU, cross-adapter present) | **42.12 / 43.62** | 16.89 |
| B (iGPU, local present) | **15.97 / 16.16** | 6.55 |

Corroborated independently by the runtime's own weave→scanout residual
(`DXR_WEAVE_LATENCY_CSV`, R = SyncQPCTime − T_weave):

| R (ms) | mean | p50 | p95 |
|---|---|---|---|
| A baseline | 38.8 | 43.2 | 43.9 |
| A + 25 % iGPU load | 37.3 | 42.0 | 43.6 |
| A + 40 % iGPU load | 37.0 | 40.7 | 43.4 |
| B baseline | **16.61** | 16.57 | **16.62** |
| B + 25 % iGPU load | **16.58** | 16.56 | 16.62 |

B's residual is one frame period with an almost-zero-variance distribution, and the
DP's eye predictor covers 16 ms instead of ~43 ms — a direct 3D-quality lever.

## Starvation ladder (the falsifier probe)

The one analysis-phase risk that could kill B: loading the iGPU starves the SR
eye-tracking pipeline (leia-plugin `docs/late-latching.md`). A closed-loop synthetic load
(`gpu_loadgen`, timestamp-query-servoed full-screen ALU pass) held 25 % / 40 % duty on
the iGPU while sessions ran:

- iGPU totals reached **79 %** (A+40 %) and **72 %** (B+25 %) with **no degradation**:
  60 fps held, R unchanged (table above), zero tracking-loss events while a viewer was
  present.
- Tracking state changes in later unattended runs correlate with the viewer leaving the
  desk (A/B/A discriminator all-zero unattended), not with config.
- **Attended confirmation (closed):** viewer watched a fullscreen B session with the 25 %
  load active throughout — **iGPU at 78 % total** (session 449 + loadgen 256 ms/s), R
  16.66 mean / 16.61 p95, 60 fps, tracking clean, and head-lock judged good by eye.
- Observation from the attended runs: brief **black frames during session warmup**
  (weaver async-create flat-blit window + the fullscreen swapchain resize, before
  tracking engages, ~5 s). Warmup behaviour, not load- or config-correlated — but an
  output-device split must not lengthen that window.

## Cross-adapter transport (the bridge B needs)

Capability-probed and benchmarked with purpose-built tools
(`scripts/hybrid_gpu_bench/`), pixel-verified (pattern readback on the consumer):

- **D3D11 has no cross-adapter texture path on this stack at all.** All six share
  flavors (NT+keyed-mutex, legacy KMT, plain shared; both directions) fail at the open
  call with `E_INVALIDARG`. Only cross-adapter *fences*
  (`D3D11_FENCE_FLAG_SHARED_CROSS_ADAPTER`) work, both directions.
- **D3D12 cross-adapter heaps fully work** (`SHARED | SHARED_CROSS_ADAPTER` heap →
  placed row-major `ALLOW_CROSS_ADAPTER` texture → `OpenSharedHandle` on the consumer —
  every call S_OK). Measured with a 3840×1080 RGBA8 atlas (15.82 MB):
  - Sustained one-way bandwidth **3.04 GB/s** (unpaced copy) — 3× the 1.0 GB/s a
    per-app-frame atlas at 60 Hz needs.
  - 60 Hz paced, consumer *directly sampling* the cross-adapter texture (the shape a
    real weave has): roundtrip **6.58 ms mean / 11.54 ms p95** — synchronous worst case;
    a shipping bridge must pipeline (produce N while weaving N−1).
  - Caveat: `CrossAdapterRowMajorTextureSupported` did **not** predict behavior (NVIDIA
    reports FALSE yet sampled cleanly). Ship cap-query + pull-copy fallback; don't
    assume direct sampling generalizes.
- **Architecture consequence:** the atlas bridge must be D3D12 (D3D11-on-12 wrap on the
  D3D11 compositor side); the D3D11-only bridge sketched in the analysis comment is not
  buildable.

## Transfer accounting (bytes, 60 Hz, RGBA8)

| | A (today) | B (proposal) |
|---|---|---|
| Per app frame | woven 3840×2160 = 33.2 MB ⇒ ~2.0 GB/s (×2 on the two-copy path) | atlas 3840×1080 = 16.6 MB ⇒ ~1.0 GB/s |
| Per repaint tick | 33.2 MB again, every tick | **0** — atlas already iGPU-resident |
| Legacy apps (Case A compromise) | 33.2 MB | 33.2 MB *unless transferred post-downscale* (parity, not regression) |

## What actually crosses, per plane (#918 Phase 2a, measured)

Phase 1 shipped the split projection-only: the atlas crossed and everything else — zones,
Local2D, authored masks, the 2D-under backdrop — was hard-gated off. Phase 2a carries the
rest, and the per-plane inventory below is what makes that cheap. The reframe is that the
masked composite has to run on the output device no matter what (its destination is the back
buffer, which Phase 1 already moved), so the only real question is which *inputs* have to be
transported — and for three of the four mask sources the answer is **none**.

| Plane | Transport | Measured on the reference box |
|---|---|---|
| auto wish mask / implicit mask / feather mask / Tier-1-2 authored mask | **none** — every rasterizer takes pure CPU rects, so it is simply run again on the output device | 0 B/s |
| Local2D OVER flatten (`local2d_scratch`) | bridge plane, RGBA8, panel-sized, per frame with dirty box + change-skip | **3 copies per session** at rest (see below) |
| 2D-under backdrop (`backdrop_scratch`) | bridge plane, RGBA8, panel-sized, dirty box | **3 copies per session** at rest |
| Tier-3 authored mask (app-drawn RT) | bridge plane, **R8**, **sized at the mask** (not the panel), on `author_seq` change only | one copy per re-authoring; R8 ROW_MAJOR cross-adapter **works** on this stack — no RGBA carrier needed |
| weave snapshot (`weave_scratch`) | **none** — its source is the output-side back buffer, so it is output-local | 0 B/s |

The planes ride the **same egress slot as the atlas**: parallel per-slot arrays, copies
recorded into the same producer/consumer command lists before `Close()`, so atlas and planes
land under one seq and one fence pair. That is not tidiness — it is what makes the Phase-1
layout-generation refusal, the slot-readiness check and the repaint slot republish cover the
planes for free.

Measured with `DXR_XBRIDGE_DIAG=1` (windowed 1280×720 `cube_handle_d3d11_win`,
`DXR_LOCAL2D_PANEL=1 DXR_LOCAL2D_BACKDROP=2`, 31 s, 1862 frames bridged):

```
atlas 0.111 GB/s | local2d 0.000 GB/s (0 copies, 61 skips) |
backdrop 0.000 GB/s (0 copies, 61 skips) | mask 0.000 GB/s | TOTAL 0.111 GB/s
…
d3d11 xbridge: Local2D plane  — 3 copies, 1850 change-skips
d3d11 xbridge: backdrop plane — 3 copies, 1850 change-skips
```

Three copies, not 1853: one per egress slot, and then the content hash stops matching nothing
and every subsequent frame is skipped. The **change-skip is mandatory, not an optimisation** —
a full-window RGBA plane at 4K60 is 1.99 GB/s on its own, which is the whole acceptance budget
for one plane. The hash covers `{swapchain, image_index, source rect, norm_rect, flip_y,
flags}` per layer; `image_index` is what makes it safe rather than optimistic, because
`d3d11_swapchain_acquire_image` hands indices out round-robin, so an app that redraws hashes
differently every frame and an app that stops acquiring genuinely has not changed.

A bandwidth **gate** sums every leg over a rolling second and, above 2.0 GB/s, latches the
largest plane to half rate with one WARN — never the split, whose win is the repaint arm and
the iGPU-local present, neither of which a plane's rate affects. It did not fire in any
session measured here.

The gate aims at **plane-attributable** bytes: the over-budget test is on the transport total,
but a plane is only throttled when the planes contribute at least 0.25 GB/s of it. Otherwise
the atlas is the cost — a 4-view 4K atlas is ~4 GB/s at 120 Hz before any 2D content exists —
and the gate has no lever on it, so it says so once and throttles nothing. The half-rate latch
is pressure relief, not a session-long sentence: it clears once the transport has stayed inside
budget for 5 s, and a plane re-bind clears its own.

Under a forced repaint (`DXR_WEAVE_REPAINT_FORCE=1`, 960 repaints, backdrop active) the
transport stayed at the app's frame rate — 0.055 GB/s against 30 app frames/s — so **repaint
ticks add zero bridge traffic with the planes live**, exactly as they did projection-only.

The two **2D** plane textures are **panel-sized once and never resized**, which puts them
structurally outside the R2 churn hysteresis. A 3 s continuous resize with the Local2D plane
active produced 6 egress rebuilds / 1 churn entry / 1 settle / 77.1 ms longest weave gap,
against 6 / 1 / 1 / 76.8 ms for the same resize with no planes at all.

The **mask** plane is the deliberate exception. An authored mask maps *stretch-to-region* —
the whole mask texture over the whole composite region, whatever its own dimensions, which is
also what the display processor does with the published mask — and an app picks those
dimensions itself (the zones docs recommend a downsampled one). A panel-sized mask plane would
therefore transport the mask into a corner of a texture the composite then stretches whole,
sampling a band no copy ever wrote; for R8 that band reads as "2D everywhere", the exact
inverse of an unauthored mask's all-3D default. So the mask plane is allocated **at the mask**,
and it rebuilds only when the app creates a differently-sized one — an on-change event, like
its transport, and so never the per-frame churn the 2D planes are protected from.

The DP **sideband** views (`set_background_2d`, `publish_local_zone_mask`) are seq-gated on the
slot's plane generation and metered in the DIAG line. They run on the output device rather than
the bridge, so they are not bridge traffic — but they are real per-tick bandwidth, and an
unchanged backdrop under a forced repaint must copy nothing. With the mask plane now
mask-sized, the mask sideband is a passthrough and copies nothing at all.

## Not measured, and why

- ~~Attended eye-pose latency under B~~ — **closed**: attended run with load active
  throughout (see ladder above); tracking and head-lock held at 78 % iGPU. No
  quantitative camera→pose latency instrument exists runtime-side; the check is
  observational.
- **True option-B (split pipeline)** — doesn't exist yet; B was proxied by an all-iGPU
  session. The proxy's biases run *against* B (extra render load) and the bridge was
  measured separately.
- **Service/IPC path, VK path, legacy-app anamorphic case, CASO-capable hardware** —
  out of scope for this pass; the VK gap (no Intel `present_wait`) is moot for the
  recommended D3D11+D3D12-bridge output device.

## Falsifiers

Would have reversed the recommendation, none occurred: B's iGPU busy exceeding A's
receive-cost by more than the freed dGPU time (measured: +74 ms/s vs −210 ms/s);
R or frame rate degrading in B or under load (stable to 79 % iGPU); no workable
cross-adapter transport (D3D12 heaps work at 3× required bandwidth); attended tracking
degradation under B (checked at 78 % iGPU with a viewer present — held). No falsifier
remains open on this topology.

## As shipped

This investigation ended at a recommendation and a proxy measurement. The thing it
recommended now exists, so the honest close is to say where it went and stop treating
this page as the current state of the work — the epic
([#918](https://github.com/DisplayXR/displayxr-runtime/issues/918)) is that, and it
carries the per-phase design comments and the merge evidence.

| Phase | What landed |
|---|---|
| 0 | `scanout` keyword for `DXR_D3D_FORCE_GPU`/`DXR_VK_FORCE_GPU` — the prerequisite this page names (#1078); weave-placement reporting in session logs and `displayxr-cli` (#1093) |
| 1 | The in-process D3D11 output-device split behind `DXR_WEAVE_ON_SCANOUT=1`, projection weave only (#1083 + #1085) |
| 2a | Zones, Local2D, the authored mask and the 2D-under backdrop across the bridge; every Phase-1 gate deleted (#1095 + #1106) |
| 2b | The same split on the **D3D11 service** — direct path, compose/shell path, the zones wish mask on the DP's device, and the hardening pass (#1125, #1127, #1136, #1139, and this PR) |
| 3 | Auto-enable policy, CASO-hardware revalidation, legacy-app post-downscale transfer — **open** |

Two corrections this page's readers should carry forward:

- **The proxy understated the win, as predicted, but the SERVICE numbers are their own
  measurement.** The all-iGPU session measured here renders the app on the iGPU too; the
  shipped split keeps app render on the dGPU. PR 3's rate-normalised service A/B is the
  first real one, and it is not this page's numbers: 9.7 ms iGPU / 5.9 ms dGPU per weave
  split-on against 5.2 / 10.6 split-off, with the delivered weave rate rising 33 → 50/s
  because the split removes the cross-adapter present that was rate-limiting the stock
  arm. Do not quote this page's figures for the service.
- **Default-on is gated on the vendor stack, not on the runtime.** #1134 found the
  scanout weave emitting transparent-black bursts while nobody is tracked — a
  non-monotonic pulse-animation clock in the vendor weavers, not a runtime defect. Fixed
  upstream (LeiaSR#190) and verified 0/40 against 8/40 pre-fix, so **Phase 3's default-on
  requires SR Platform ≥ 1.37.0+1498**.

## Reproduction

Tools in `scripts/hybrid_gpu_bench/` (`build.bat`; VS 2022): `gpu_loadgen`
(duty-servoed per-adapter load), `xbridge_bench` (D3D11 share capability matrix),
`xbridge12_bench` (D3D12 cross-adapter heap benchmark, `--mode=full|copyonly|sample`).
Busy-time sampling: `gpusample.ps1` (same directory). Sessions:
`DXR_D3D_FORCE_GPU=igpu|unset`, `DXR_WEAVE_REPAINT_FORCE=1`, `DXR_WEAVE_LATENCY_CSV`,
fullscreen via F11, PresentMon 2.5.1 for present modes.

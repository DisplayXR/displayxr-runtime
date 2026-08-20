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

| Forced repaint (`DXR_WEAVE_REPAINT_FORCE=1`, both configs settled at ~30 repaints/s) | app iGPU 3d | app dGPU 3d | app dGPU copy | iGPU total | dGPU total |
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

## Not measured, and why

- **Attended eye-pose latency under B** — the SR camera→pose path has no runtime-side
  instrument and the box was unattended for the B ladder; needs one eyeball session
  (viewer watches 3D lock under `DXR_D3D_FORCE_GPU=igpu` + `gpu_loadgen --duty=25`).
  This is the remaining falsifier.
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
cross-adapter transport (D3D12 heaps work at 3× required bandwidth). Still open:
attended tracking degradation under B would reopen the decision.

## Reproduction

Tools in `scripts/hybrid_gpu_bench/` (`build.bat`; VS 2022): `gpu_loadgen`
(duty-servoed per-adapter load), `xbridge_bench` (D3D11 share capability matrix),
`xbridge12_bench` (D3D12 cross-adapter heap benchmark, `--mode=full|copyonly|sample`).
Busy-time sampling: `gpusample.ps1` (same directory). Sessions:
`DXR_D3D_FORCE_GPU=igpu|unset`, `DXR_WEAVE_REPAINT_FORCE=1`, `DXR_WEAVE_LATENCY_CSV`,
fullscreen via F11, PresentMon 2.5.1 for present modes.

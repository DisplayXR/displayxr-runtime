---
status: Accepted
date: 2026-10-24
source: "#2317"
---
# ADR-059: One D3D11 hub compositor on Windows; the per-API in-process compositors become import shims

**Status:** Accepted (David, 2026-10-24; proposed the same day). Phase 1 of epic #2317 is
implemented default-OFF behind `DXR_HUB=1` (PR #2321) with its A/B harness (PR #2323); Phase 2
has landed the planes through the hub (#2335). Defaults still flip only per API behind the
acceptance gates below. Related: [ADR-001](ADR-001-native-compositors-per-graphics-api.md) (amended, not
superseded), [ADR-035](ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md)
D3, [ADR-037](ADR-037-adapter-placement-policy-hybrid-devices.md),
[ADR-039](ADR-039-one-fill-engine-for-every-tier.md),
[ADR-055](ADR-055-weave-sync-gpu-side-semaphores-no-host-drains.md).

## Context

### The question

Would Monado's shape — one compositor, per-API client shims that import swapchain images into
it — beat DisplayXR's five native in-process compositors, now that iGPUs handle interop? The
mac/arch charter (2026-10-23) recommended one hub per platform in that platform's native API,
asked the win box to measure before any ADR, and left two things open: whether the Windows hub
should be D3D11 or D3D12, and whether the handoff costs latency.

### What exists already (the proposal is more than half built)

- **The service is a hub.** ADR-035 D3 landed (#964): one always-on D3D11 pipeline; the
  per-client path survives only behind `DXR_LEGACY_STANDALONE=1`.
- **In-process, the hub exists too, housed in the Vulkan tree.** `comp_vk_native_split.cpp`
  plus `compositor/xbridge/` is Vulkan-free D3D11 code (the "d3d11 fill arm"). Vulkan renders
  *into* a same-adapter NT-shared D3D11 deposit (an `ID3D11Fence` imported as a timeline
  semaphore, GPU-side waits, zero copies) and the arm weaves with the D3D11 display processor
  and presents. The in-process D3D12 compositor reroutes onto the same arm by default on a
  single adapter (#1264, measured 8–9 ms → 1.4 ms weave fires). On `main` the only Windows API
  outside the arm is OpenGL.
- **"Native single-device D3D11" does not exist on `main`.** The in-process D3D11 tier engages
  its own xbridge fill arm by default even on one adapter (ADR-039 same-adapter engage), i.e.
  two transport copies per frame. The baseline the charter called "today's same-context weave"
  is already a separate-device, fence-synchronised path.
- **The browser present-owner route (`XR_DXR_weave`)** is already hub-shaped and service-only.
- **Unity D3D12 standalone** already runs the session on a provider-owned D3D12 device with an
  NT-shared bridge texture and a shared fence (one `CopyTextureRegion` per slice per frame and a
  CPU fence block); Unity's own device is used only under the shell. **Unreal** defaults to one
  tile copy per frame since plug-in 0.9.0 and has **no cross-queue fence** at release.

### What the duplication costs (audit of `main` 6b206f0aa)

In-process compositors total 113.7k lines (d3d11 22.7k, d3d12 21.6k, gl 11.8k, vk_native
45.2k, metal 12.5k); the IPC client shims total 9.8k. On the Windows backends features land as
one multi-backend train (zones, late weave, split, segments: 0–4 days apart), so the cost is
not lag but **N parallel copies**: the late-weave gate ×5, repaint thread ×4, DComp set-up ×5,
placement/window-metrics ×6, zone mask rasterisers ×6. `comp_base` is used by no native
compositor. The genuine drifters are Metal (no repaint, no late weave, no present-wait; depth
budget 47 days late) and GL (no depth budget, no split) — a Windows hub fixes GL only.

### Vendor side (Leia plug-in `origin/main` 44e26fd, SR SDK)

The D3D11 DP is the only mature variant (~35 slots: phase snap, `set_window`, overlay_2d, lift
×5, atlas-encoding query); D3D12 has 21 slots and none of those; VK fills none of its header's
newer slots; GL lags most. SR's D3D11 and GL weavers latch late automatically; the VK weaver
needs a submit hook; **SR's D3D12 late latching is a literal stub**. WGC desktop capture is
D3D11 internally but already shared to every variant by NT handle. A D3D11-only Windows hub
makes ~15k of 41.5k Leia Windows lines dead (the D3D12/GL/VK DPs and the bundled VK weaver
selector that carried the SR 1.36 regression). A D3D12 hub would retire the one mature variant.

### Consumers

| consumer | needs | D3D11 hub + D3D12 shim | D3D12 hub + D3D11 shim |
|---|---|---|---|
| Unity (standalone D3D12) | `ID3D12Resource` on its device; render-target state at acquire/release; `arraySize=2`; zones; Local2D | low–medium: it already does this pattern itself; the lift path proves D3D11→D3D12 opening; the shim could drop Unity's own bridge copy and CPU block | low |
| Unreal (in-process D3D12) | raw resources on UE's device; **no release fence today** | medium–high until the plug-in signals a fence from UE's queue | low (status quo) |
| Browser (`XR_DXR_weave`) | keyed mutex, legacy DXGI handles (low-integrity sandbox, #743), service D3D11 | lowest: unchanged | high: no D3D12 keyed mutex; NT-only handles |

### Options considered

1. **Status quo** — five native compositors (ADR-001). Keeps N copies; GL never gets the split.
2. **Vulkan hub (Monado's shape).** Loses on Windows: the iGPU weave split needs D3D12
   cross-adapter heaps (VK external memory is same-device), transparency is DComp/DXGI, WGC is
   D3D11, Chromium/ANGLE is D3D11, and ADR-001's Intel VK interop failures are documented
   (`VK_ERROR_FORMAT_NOT_SUPPORTED`, DEVICE_LOST on Iris Xe without a drain, no D3D12_FENCE
   import on Intel UHD).
3. **D3D12 hub.** Strictly worse for the vendor (stub late latching, immature DP) and breaks the
   browser's keyed-mutex + legacy-handle contract.
4. **D3D11 hub — this ADR.** Formalises the fill arm the runtime has already converged on.

## Decision

1. **On Windows there is one hub compositor, in D3D11**, `compositor/hub_d3d11/comp_hub_d3d11`
   (extracted from `comp_vk_native_split` in #2321 P1a, a pure move). The hub owns weave,
   present, late-weave pacing, repaint and the output-device split. The in-process D3D11, D3D12
   and Vulkan compositors become **import shims**: they render on the app's device into an
   atlas the hub can open (same-adapter NT share + `ID3D11Fence`, GPU-side waits only —
   ADR-055; cross-adapter via `comp_xbridge`), and hand it over once per frame. OpenGL follows
   through a new WGL interop shim (Phase 3). Metal and desktop Linux/Android keep their native
   compositors — this ADR is Windows-only.
2. **The vendor weaver contract on Windows is the D3D11 DP.** The D3D12, GL and VK DP variants
   stop being load-bearing once every in-process API rides the hub; retirement is the vendor's
   call after one release at the new default.
3. **The hub is the same code in-process and in the service.** The service's
   `comp_d3d11_service` converges onto `comp_hub_d3d11` **last** (Phase 5): it is 39k lines and
   the lift host, and the in-process arm is the small, clean seed.
4. **Defaults flip per API, gated by measurement, never by fiat.** Each API gets a default-on
   with a kill switch and one `hub placement:` WARN line per session naming the rung
   (`hub=1 … ingress=direct|xbridge` or `hub=0 reason=<token>` from a closed set) — the #1264 /
   ADR-039 pattern. The native weave/present legs stay one release, then are deleted.
5. **Fallback is a named rung, not a failure.** Where interop is unproven for a (vendor × API ×
   adapter) cell (ADR-037 §3a), the hub refuses at create or retires mid-session with a reason
   and the native leg composes the frame.
6. **ADR-001 is amended, not superseded.** Its reasons 1 (vendor D3D11 late latching), 6
   (format fidelity), 7 (DXGI present semantics) and 8 (single-API captures) are *why the hub is
   D3D11*; its reason 3 (interop is a driver lottery) is why decision 5 exists.

## Measurements (hybrid laptop, leia-sr 2.17.0, hub build `v2.53.0-7-g745765ed1`)

`scripts/perf-ladder/run_hub_ab.ps1` (#2323): n=3 × 20 s per arm, interleaved, AC power,
60 Hz panel, untracked. Every run proves its path from its own log or is refused. **R** =
weave→scanout residual (`DXR_WEAVE_LATENCY_CSV`), **D** = scanout − promised display time,
**p2d** = PresentMon present→display. Noise band on R: ±1.66 ms.

**Same adapter (everything on the Intel iGPU) — the charter's Q1, "handoff cost on one GPU":**

| app / arm | R p50 / p95 (ms) | D p50 (ms) | p2d p50 (ms) | app GPU % | CPU % | present mode |
|---|---|---|---|---|---|---|
| d3d11 native (own xbridge arm, 2 copies) | 16.60 / 16.63 | +0.05 | 16.00 | 12.4 | 17.1 | Independent Flip |
| d3d11 **hub, DIRECT ingress (0 copies)** | 16.59 / 16.64 | +0.04 | 15.97 | 11.3 | 17.3 | Independent Flip |
| d3d11 hub, xbridge ingress | 16.60 / 16.64 | +0.04 | 16.11 | 11.7 | 17.0 | Independent Flip |
| d3d12 native | 16.60 / 16.65 | +0.12 | 16.15 | 10.8 | 12.9 | Independent Flip |
| d3d12 hub (reroute, 3-copy ingress) | 16.60 / 16.63 | −0.75 | 16.16 | **19.5** | **22.7** | Independent Flip |
| vk native (kill switch) | no R (Intel VK lacks present_wait) | — | **33.19** | 12.5 | 15.4 | **Composed: Copy with GPU GDI** |
| vk hub | 16.60 / 16.63 | −0.78 | 16.16 | 14.0 | 18.0 | Independent Flip |

**Default topology (RTX renders, Intel scans out):**

| app / arm | R p50 / p95 (ms) | D p50 (ms) | p2d p50 (ms) | GPU iGPU / dGPU % | CPU % |
|---|---|---|---|---|---|
| d3d11 native (own split) | 16.60 / 16.64 | +0.06 | 16.03 | 8.3 / 8.4 | 18.9 |
| d3d11 hub (ingress=xbridge) | 16.60 / 16.64 | +0.05 | 16.11 | 7.5 / 8.4 | 18.2 |
| d3d12 native (own split) | 16.61 / 16.64 | +0.11 | 16.15 | 6.1 / 9.0 | 16.7 |
| vk native (kill switch) | 16.59 / 16.69 | — | 14.70 | 4.5 / 19.7 | **181.5** |
| vk hub | 16.61 / 16.64 | −0.74 | 16.20 | 7.2 / 7.4 | 15.4 |

Readings:

- **The handoff costs nothing measurable.** D3D11 hub-DIRECT vs native: R within 0.01 ms, D
  within 0.01 ms, GPU 1.1 points *lower*, no frame of pipelining (R stays one refresh, p2d
  stays 16 ms). The ingress mode (DIRECT vs xbridge) is also within noise.
- **Latency is neutral for every API on both topologies.** The one real latency gain is
  Vulkan on a single adapter: native presents through a composed GDI copy (one extra frame);
  the hub gets independent flip, **−17 ms**.
- **The D3D12 reroute's extra copies show up as GPU/CPU, not latency** (+8.7 / +9.8 points):
  the hub's xbridge ingress copies three times even on one adapter. Phase 2 gives D3D12 and VK
  the DIRECT ingress the D3D11 shim already has.
- **Split, late weave and repaint survive the hub**: hybrid `ingress=xbridge` with identical
  R; R = one refresh everywhere; the frame-divisor test gives 30 weaves + 30 repaints on both.
- **Two findings outside the hub, filed separately:** the Vulkan kill-switch path spins ~1.8
  cores on the default topology; D rows on fill-arm sites read −0.75 ms where native reads
  +0.05 (prediction-feed bias in the arm's display-time feed).
- **Not measured:** a Leia desktop (single dGPU driving the panel) — this box is the hybrid
  laptop; the GL hub arm (Phase 3); the D3D12 hub on the hybrid topology (the reroute gate
  also demands `DXR_APP_FRAME_DIVISOR ≥ 2`, which is not a fair arm); tracked viewers.

Prior data the decision also rests on: #1264 (D3D12 → D3D11 arm 8–9 ms → 1.4 ms weave fires);
ADR-037/#918 (all-scanout 16.0 ms, all-dGPU 42.1 ms, split 16.6 ms motion-to-photon);
`docs/architecture/service-one-pipeline.md` (forced-IPC handle cube 16.6 ms pipeline vs
30.3 ms legacy).

## Consequences

**Positive**

- One late-weave gate, one repaint thread, one DComp/waitable presenter, one placement, one
  mask rasteriser, one capture site on Windows — instead of four or five of each.
- Every Windows app gets the vendor's most mature weaver (D3D11, automatic late latching).
- GL apps gain the output-device split for the first time (Phase 3).
- Vulkan apps on an iGPU-only machine gain a frame.
- The vendor ships one weaver per platform.

**Costs and risks, with the design answer**

- **ADR-001's interop lottery returns for GL and for VK on Intel.** → decision 5: per-API
  gates, named rungs, native leg kept one release. GL is last (Phase 3) and needs a
  LUID-matched interop device (`GL_EXT_memory_object_win32`, #1159).
- **Unreal's missing release fence becomes a race on a separate hub device.** → the Unreal
  plug-in signals from UE's queue before Phase 4 flips its default; the shim requires it.
- **The IPC client shims are not the model** (`client_d3d12` has no effective sync, #2320;
  `client_vk` drains the queue every frame; `client_gl` `glFinish`es and copies). → the
  in-process deposits are the model; #2320 adopts the same fence.
- **Colour honesty (ADR-044) moves to the import boundary** — typeless/`_SRGB` pairs across
  D3D11↔D3D12, Vulkan's no-blit-through-`_SRGB` rule, Unity's two-slice array. → Phase 4
  verification list; the ADR-044 oracle (`DXR_ATLAS_CAPTURE_RAW_ALPHA=1`) is part of every gate.
- **Late latching needs exactly one weave per presented frame** on the immediate context, and
  the D3D11 weaver self-disables on too many frames in flight. → already the hub's contract
  (verified in #2321 review).
- **Mid-session retire is the delicate path** (one weaver per HWND; swapchain re-create on the
  same HWND; the fresh DP must be re-told 3D/eye mode/frame timing). → fixed and re-validated
  in #2321; retire stays a transitional mechanism until planes ride the hub (Phase 2).
- **Vulkan's present_wait extras do not follow a VK app onto the hub, by design** (#2342). The hub
  presents through DXGI on its own `comp_d3d11_target`, so the `vk_native` target is never created
  on a hub route, and every pacing helper it carries is bypassed:
  - The **adaptive grid lead** (`comp_late_weave_adapt.h`, #2068) learns a composited Linux
    desktop's latch-before-vblank. Its own header says the vblank IS the deadline under DXGI flip,
    so it has nothing to learn on Windows.
  - The **present_wait depth floor** (`comp_late_weave_depth.h`, #2119) exists because a composited
    surface shows a commit two refreshes late. The hub's DXGI path has the #850 late-weave governor
    instead: depth on the frame-latency waitable, raised by saturation, capped by
    `DXR_LATE_WEAVE_MAX_LATENCY`. That is the same lever, DXGI-native, and the same governor the
    native D3D11 leg runs.
  - The **#867 lookahead** is already the hub's: VK's `predict_frame` / `wait_frame` read
    `comp_hub_d3d11_get_predicted_lookahead_ns` and mark its ledger (#2336 / #2327), through the
    same `late_weave_lookahead_ns` formula.
  - **Lazy transparency** (`comp_lazy_transparency.h`) is desktop-Linux-only and never ran on the
    Windows VK split.

  What a Windows VK app gains instead is the DXGI present. On an Intel iGPU, VK has no
  `VK_KHR_present_wait`, so the native leg has no late-weave R at all and presents through a
  composed GDI copy; the hub gets independent flip (−17 ms, measured above). Nothing in this list
  is lost relative to native VK on Windows, so nothing is ported. The `[WITNESS]` / R / D harness
  vk arms (`run_hub_ab.ps1 -Apps vk`) are the check.
- **Metal is untouched** — the biggest drifter is out of scope here; macOS keeps its native
  compositor by the same "hub in the platform's native API" rule.

## Phasing

See #2317 for the checklist. P1 (extraction, D3D11 shim, D rows, harness) is done default-off.
P2: the hub becomes the one implementation (planes through the hub, DIRECT ingress for every
shim, one late-weave gate/repaint/presenter/placement/mask/capture, colour at the boundary).
P3: GL shim. P4: engines + browser regression. P5: per-API default flips with kill switches,
retire native legs after one release, converge the service onto the hub library.

## Acceptance for flipping a default (per API)

Harness run on the hybrid laptop **and** a single-dGPU Leia desktop: R and D within the noise
band of native, no added frame of pipelining, app GPU and CPU not worse than native by more
than the copy count predicts (DIRECT ingress: not worse at all), 60/60/0 witness rates, split
engaged on the hybrid, repaint cadence preserved under the frame divisor, the full test matrix
(zones, Local2D, cursor, transparency, capture, multi-screen segments, the ADR-044 oracle,
`displayxr-cli selftest`) green, and — for Unity, Unreal and the browser — the Phase 4 checks.

## Amendment 1 (2026-10-24) — bridge ingress is one frame stale; direct ingress is a frame fresher

The measurements above said the DIRECT and xbridge ingress modes were "within noise". That is
true of the residual R (weave→scanout) and false of content age. Once the hub pairs each weave
with the predicted display time of the frame it actually shows (#2336, the fix for #2327), every
xbridge arm reads D ≈ +16.7 ms — one refresh — while DIRECT reads +0.03 ms: `comp_xbridge_pick_slot`
selects the newest slot whose cross-adapter copy has *completed* on a CPU check, which right after a
submit is the previous frame. The shipped D3D11 and D3D12 own-split routes use the same pick, so the
hybrid-topology motion-to-photon figures in ADR-037 and in the table above carry one frame of transport
latency the old D rows hid. Tracked as #2337 (experiment: GPU-wait the in-flight slot on the app-frame
weave). Consequence for this ADR: on a single adapter the shims take DIRECT ingress (#2336, a default
change on the already-default hub routes, gated on the single-dGPU desktop run and the engine checks);
cross-adapter stays xbridge until #2337 is settled.

## Amendment 2 (2026-10-25) — P2c: the hub is the one implementation for its routes

#2342 (P2c) moves the remaining per-route copies on the hub routes into `comp_hub_d3d11`. The native
legs keep their own copies until P5 deletes them.

- **Ring producers' planes ride DIRECT.** A ring producer that declares
  `comp_hub_d3d11_info::ring_direct_planes` keeps its Local2D, backdrop and mask planes on DIRECT
  ingress. The hub reads them in place, and the frame's recipe is snapshotted beside its slot.
  The single-buffered planes get the atlas's two edges:
  - write → read: the producer writes them before the deposit signal;
  - read → rewrite: a submit that rewrites a plane GPU-waits for the consumer fence's *latest*
    value (`comp_hub_d3d11_get_plane_release_value`).

  Both ring producers declare it, so the P2a `reason=plane_content` move to xbridge is now only
  the fallback for a producer that does not. Keyed-mutex VK deposits keep their documented
  timing-only planes (#1274).
- **Per-segment zone publish and clear** run in the hub on every route, which is the native
  #2218 #4 rule. The D3D11 shim's `segments_planes` refusal is retired.
- **One repaint pacer and admission.** `comp_hub_d3d11_repaint_pace_hold` / `_wait`, then
  `comp_hub_d3d11_repaint`, give every hub route the native D3D11 leg's pacing (scanout wait plus the
  #884 late sleep) and its #1339 admission. The pacer hold is lifetime-safe against a retire. Before
  this, all three routes skipped pacing, so `DXR_WEAVE_REPAINT_FORCE=1` gave 0 repaints/s on the
  hub (#2321 surprise 6). `DXR_LATE_WEAVE` is read once for every hub route, by the hub's target.
- **One capture site.** On a hub route the tier hands its pending `u_capture_intent` to the hub.
  The hub writes the DP input to `displayxr_atlas[.<exe>][.projection].png` and, for full
  triggers, the post-weave back buffer to `…woven.png`: the service twin's names.
- **Colour at the import boundary** (ADR-044 §1). Every hub view of a producer surface uses the
  non-decoding UNORM member of its family; crops are raw copies.
- **VK present_wait extras.** None is lost relative to native VK on Windows; see *Consequences*.

## References

- Epic #2317; PRs #2321 (hub extraction + `DXR_HUB=1` shim + D rows), #2323 (A/B harness),
  #2319 (doc corrections from the audit); bug #2320.
- `docs/architecture/service-one-pipeline.md`, `docs/reference/motion-to-photon-levers.md`,
  `docs/reference/adapter-selection.md`, `docs/specs/runtime/swapchain-model.md`.

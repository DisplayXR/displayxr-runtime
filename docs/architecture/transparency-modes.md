# Transparency modes — canonical vocabulary

How a transparent DisplayXR overlay window gets its see-through pixels is a
**2×2 of two independent axes**, not a single switch. Established 2026-08-03
with the #833/#837 decoupled-presentation work; the numbers below are the
gated iGPU A/B from that work (dxr-perf-study `BENCH-FINDINGS.md`,
2026-08-03 addendum).

## Axis 1 — composition: who completes the transparency

- **Live** (transparent present): DComp premultiplied swapchain; **DWM**
  alpha-blends the window over the live desktop every frame. Zero lag,
  expensive (per-pixel blend, never flip-promotes).
- **Baked** (opaque present, `DXR_PRESENT_OPAQUE=1`): opaque HWND flip-model
  swapchain; DWM blends nothing. The display processor **bakes the
  WGC-captured desktop** (~1 frame old) into every non-opaque pixel
  (compose-under-bg + flattened alpha-gate, leia-plugin#116).

## Axis 2 — shaping: what the window physically covers

- **Unshaped**: the window owns its full rect.
- **Shaped**: `SetWindowRgn` from per-frame content coverage carves the
  all-views-transparent pixels **out of the window** — the desktop there is
  real desktop, not our pixels (`dxr::ClickThroughRegion` in
  displayxr-common ≥ v2.6.0; the Unity provider pioneered the technique).

## The four combinations (measured on Intel UHD, 811×1421 window)

| | Unshaped | Shaped |
|---|---|---|
| **Live** | Classic transparency. Live punch-through everywhere. ~56 pts system GPU, dwm ~32 %, Composed: Flip. | Live everywhere (holes are real desktop; in-region blends DWM-live). dwm ∝ region area. Composed. |
| **Baked** | Bake everywhere the window covers. **Cheapest**: 12–15 pts, dwm ≈ 0, **Hardware Composed: Independent Flip**. | **Live punch-through in the carved holes**; bake only in the in-region blend band (silhouette edges, de-occluded pixels, kept chrome). Cheap region compose. Composed. |

## The default: live + shaped, hybrid composition, governor-paced late-weave

Settled 2026-08-10 (#906 + leia-plugin#139 + #905/#907; each arm eyeballed on
the panel). Transparent overlay apps ship:

- **Live + shaped** — punch-through holes are real desktop (no drag-behind
  smear, rule 5), `SetWindowRgn` carries click-through. **Hybrid** completes
  it: the runtime owns the transparent present, and the DP still bakes the
  **de-occlusion band** (silhouette fringe, partial-alpha pixels) from the
  WGC capture — that band physically cannot be DWM-blended (the weaver
  interlaces to opaque RGB; the post-weave alpha-gate reconstructs only the
  binary all-views-transparent mask), so the band is a ~1-frame-old bake or
  it is BLACK. In-process live passes `client_presents=false`;
  `client_presents=true` (DP composes nothing, gate only) is reserved for
  client-side presents / bandless content (#551). The WGC cost is contained
  by the capture-delivery throttle (`LEIA_DP_CAPTURE_MIN_INTERVAL_MS`,
  default 66 ms, `0` = uncapped) — free on a quiet desktop, ~9 pts saved
  under motion.
- **Late-weave pacing on the live path, all backends** (#905 D3D11/D3D12,
  #907 VK): the composition swapchain carries the frame-latency waitable
  (D3D chains get `bc=3`); on VK the DXGI **bridge** carries it, created
  exactly where `VK_KHR_present_wait` is absent (every Intel iGPU) —
  `DXR_VK_BRIDGE_PACING` unset = governor, `0` = off, N = pinned. The #850
  governor is the single owner of pacing state, extended with: slip-rate
  escalation (GPU-completion jitter the cost EMA can't see), a catch-up
  guard (an overrun frame skips the next tick-align instead of dropping a
  frame), an adaptive align-hold (chronic contention yields the constant
  phase — a dropped frame holds a *stale weave on glass*, the exact
  artifact late-weave prevents), drag-shallow (queue clamps to 1 while the
  present origin moves), and an occlusion guard (waitable timeout ≠
  saturation). Measured on the iGPU avatar: pacing on = 60.0 fps / 39.7 %
  GPU busy vs off = 60.0 / 39.3 % — pacing is free, and it buys the bounded
  weave→glass queue that late weave and app-rate throttling (#868 repaints
  hold panel-rate weave cadence; measured 39.7 % → 17.7 % GPU busy with the
  app idle-throttled) are built on.
- **Baked (`DXR_PRESENT_OPAQUE=1`) is demoted to full-cover content** (the
  cube-class apps, which keep the 3.7–4.7× Independent Flip win) and to
  A/B instrumentation. Launcher recipes for overlay apps no longer set it.

Live-path caveat: the weave→scanout residual is unmeasurable on composed
chains (frame statistics DISJOINT; the compositor clock gives *alignment*,
not a number), so `predictedDisplayTime` keeps the `period*2` fallback and
the stage-2 source is logged one-shot (`Late-weave: live-path stage-2
source = ...`).

### Product decision: shaped is the default; unshaped is not shipped

Overlay-class apps ship **shaped**. Unshaped was evaluated (it is the only
combination that reaches Independent Flip) and **rejected**, because dropping
the region costs two things at once:

1. **Background freshness.** Unshaped, the window covers its whole rectangle,
   so *every* transparent pixel is served by the ~1-frame-old bake — not just
   the silhouette fringe. For an avatar-class app that is most of the window.
   (It is only *visibly* wrong when background pixels change — see rule 5 —
   but that is a property of the content, not a guarantee.)
2. **Click-through.** `SetWindowRgn` *is* the hit-testing mechanism on this
   path. Per-pixel alpha gives see-through visually, but Windows does not
   hit-test on alpha for a flip-model / `WS_EX_NOREDIRECTIONBITMAP` window
   (alpha hit-testing exists only for `WS_EX_LAYERED` + `UpdateLayeredWindow`,
   which is incompatible with our swapchain). Unshaped, the full rectangle
   eats every click, scroll and hover.

Both are recovered by the same mechanism, so the two costs arrive together.
Schemes that enter unshaped conditionally (on cursor proximity, or on
background motion) were considered and are **not** being pursued.

**Consequence for the perf model:** Independent Flip is therefore unavailable
to overlay-class apps, so `DXR_PRESENT_OPAQUE` buys them **no DWM saving**
(measured: opaque gate on a shaped avatar 10.7 dwm vs 10.2 baseline). Its
value on a shaped app is that the opaque flip chain is what carries the
late-weave **waitable** — i.e. pacing and latency, not GPU. Quote it that way.
Full-cover unshaped content (the cube apps) is unaffected and keeps the
3.7–4.7× Independent Flip win.

This also reconciles two measurements that look contradictory. On a **shaped**
avatar, baked shows *no* dwm win (≈38 dwm, same as live) because the region
denies the flip. On a config that is effectively **unshaped** — region-on-hover
with the cursor parked — baked does win the flip (0.93 dwm vs live's 4.80). Same
mechanism, opposite headline, decided entirely by whether a region is set. So a
baked number quoted without stating the shaping state is unusable; always say
which. And since shaped is the product default (above), the flip is off the
table and the smear of rule 5 is the deciding factor.

## Rules (each learned the hard way)

1. **Shaping only punches through what you carve.** Kept surface (OS frame,
   chrome bands/rects) shows the bake wherever it is not opaque content.
   Chrome in a baked+shaped app therefore has exactly three options: hide it
   while transparent (product default for the demos), keep it and accept
   bake in its transparent padding, or make it tight/opaque.
2. **Frame and shaping are exclusive per-moment.** A region suppresses the
   DWM themed frame, and a `WS_EX_NOREDIRECTIONBITMAP` window cannot paint
   the classic fallback — a shaped framed window shows *no* frame. Toggle
   between framed-unshaped (positioning) and borderless-shaped (product).
   Never change window styles while shaped (the flip-chain window vanishes).
3. **Independent Flip belongs to unshaped-baked only.** Any punch-through
   (shaped) window is Composed by construction: a flip surface scans out its
   full opaque rect, and per-pixel-alpha hardware planes are not reachable
   through HWND flip chains (#833, measured dead ends).
4. **Baked composition requires session adapter == the panel's scanout
   adapter** (`DXR_VK_FORCE_GPU=igpu` on Optimus boxes): cross-adapter WGC
   delivers black/no frames → holes flatten to black (the "opaque squared
   Local2D" failure; leia-plugin#119).
5. **Bake lag breaks ordinary desktop interaction — it is not a narrow
   residual.** *(Corrected 2026-08-10; the earlier wording claimed drags were
   clean and animated content behind was an edge case. That was wrong and it
   understated the failure.)* Moving **our own** window over a *static*
   desktop is clean, because the ~1-frame-old full-monitor capture is
   pixel-identical at any position given capture self-exclusion. But
   **dragging any other window behind ours makes the background visibly
   smear** — verified by hand on the panel — and that is ordinary desktop
   use, not a corner case. It is **not tunable**: bisected one knob at a
   time, a fresher capture (uncapped vs 66 ms) changes nothing and painting
   more often (61 Hz vs 22 Hz) is *worse*. It is bake or don't. True dynamic
   live↔baked switching on drag brackets would need runtime swapchain
   recreation — not implemented, by choice.
   **Consequence: live is the shippable composition mode for overlay apps**,
   and baked is only defensible for content with no meaningful background
   behind it. Measured cost of choosing live, cursor parked, six-lever
   avatar config: **+4.90 system points, essentially all dwm** (10.36 baked
   → 15.26 live) — the price of a correct background.
6. **Window-space-stamped chrome under baked composition needs plug-in
   ≥ v2.1.1** (leia-plugin#121, fixed): older DPs dropped it from the gated
   output (present in the input atlas, missing after the gate). Local2D
   chrome (post-weave composite) remains the most portable path.
7. **Late-weave latency-1 pacing halves a saturated pipeline** — governed
   since runtime v2.4.1 (#850/#851): the opaque flip chain enables the
   late-weave waitable (maxFrameLatency=1), which removes all frame
   overlap — measured on a GPU-saturated Unity app: ~4 ms better weave
   R p50 for **−63 % fps** (20 vs 54.5 fps), with the WGC bake exonerated
   by decomposition. The saturation auto-backoff (default-ON) drops to
   depth 2 when the pipeline can't make rate (one-shot WARN;
   `DXR_LATE_WEAVE_MAX_LATENCY=N` pins, `DXR_LATE_WEAVE_AUTOBACKOFF=0`
   opts out). The R metric alone hides this trade — always record fps
   beside it, and prefer trimming the app under one refresh over relying
   on the backoff (it is a safety net, not a target).

## What each app uses

| App | Transparent-mode combo | Notes |
|---|---|---|
| cube test apps | baked + unshaped | Full-cover content ⇒ no holes to punch; gets Independent Flip. `DISPLAYXR_TRANSPARENT_BG=1` + env. |
| avatar (`displayxr-demo-avatar`) | **live + shaped hybrid (the default above)**; framed `B` positioning mode is unshaped | Silhouette+bubble region; de-occlusion band baked by the DP. Known open: RMB-drag 3D stutter under live+shaped, bisected to NOT be the pacing (#908). |
| modelviewer (`displayxr-demo-modelviewer`) | opaque scene default; `Ctrl+T` = baked + shaped, chrome hidden, RMB-drag to move | Region from view-0 alpha via `dxr::ClickThroughRegion`; toast band kept in-region. |
| gaussiansplat (`displayxr-demo-gaussiansplat`) | baked + unshaped *(shaping wiring pending)* | Kit-cached HUD landed; punch-through follows the modelviewer recipe. |
| Unity provider overlay | shaped (region from rendered alpha); present path = whatever the runtime session uses | The origin of the shaping technique; gets baked composition with the same env, no plugin changes. Baked+shaped on the D3D12 DP requires plug-in ≥ v2.1.1 (leia-plugin#126 replaced the alpha-sentinel gate with the D3D11-style sentinel-free compose-under; halo + opaque-bubble defects on older plug-ins). Works on the default D3D11 path too — no `-force-d3d12` needed. |

Present-path env: `DXR_PRESENT_OPAQUE` (default off). App-side building
blocks: displayxr-common ≥ v2.6.0 (`vk_overlay_kit.h`,
`vk_clickthrough_region.h`, `win_window_drag.h`).

### `DXR_PRESENT_OPAQUE` stays opt-in — the default-on question is ANSWERED, not open

#833 landed the flag default-off and deferred the default flip "after HW soak
(drag/phase-snap, 2D/3D transitions, VK-bridge flip eligibility)". That soak ran
on 2026-09-02 and the answer is **no**. Do not re-open it as an oversight.

The mechanism works and reproduces. Four **alternating** runs on `cube_handle_d3d12`
(transparent session, per-process attribution, in-process) gave dwm **114.9 → 15.6
ms/s, −86.4%**, rank-separated with the worst pairing 96.2 ms/s apart; app iGPU
138.7 → 119.0 (−14.2%), also rank-separated; app dGPU unchanged. Alternation matters
here because the dwm term on the reference box is a documented **per-run** coin flip
(~0 ↔ ~150 ms/s regardless of arm, #1144) — within-arm spread was ≤4.5 against a
between-arm gap of 99.3, so it tracked the arm, not the toggle. Cadence is a **pacing**
result, not throughput: 60.00 with sd 0.000 versus 59.67 with sd 0.298, a mean gain of
0.33 weave/s that is not worth quoting as speed.

**It still must not ship on, because the benefit and the cost land on the same app
class** — which is exactly the matrix at the top of this file:

- **unshaped + opaque** → the whole win (Independent Flip, dwm ≈ 0) **and bakes
  everywhere the window covers**. Maintainer verdict on the live panel: *"not good —
  am seeing bg capture everywhere, should be only in deocclusions, rest should be
  punch through."* That is the *Baked × Unshaped* cell behaving exactly as documented.
- **shaped + opaque** → the correct look (live punch-through in the carved holes,
  bake only in the blend band) but the region denies flip promotion, so it stays
  `Composed` and buys **no dwm saving** (10.7 vs 10.2).

So the class that gains the performance is the class that loses the look. On a shaped
app the flag's value is that the opaque flip chain carries the late-weave **waitable**
— pacing and latency, not GPU. Quote it that way.

Not settled by that soak, and still open if anyone wants them: the **D3D11 service**
path (different code from the in-process compositor tested here; the default would
cover both), and **VK-bridge flip eligibility**.

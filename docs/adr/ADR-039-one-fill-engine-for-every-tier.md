# ADR-039: One fill engine for every tier (same-adapter split)

**Status:** ACCEPTED for the VK tier (Phase A, 2026-08-29 — full acceptance record on
#1264: keyed-mutex smoke, partition D=3 exact, 68-window ≥5-min event-immunity leg,
eyeball on the #1257-opening configuration; `DXR_SPLIT_SAME_ADAPTER` default flipped
on, `=0` is the kill switch) **and for the in-process D3D11 tier** (Phase C,
2026-08-29 — pulled forward as Phase B's reroute hypothesis probe and passed on first
contact: engage clean, partition exact, acceptance leg with the event-immunity profile,
eyeball with the planes verified; the tier consults the same accepted switch, and its
bring-up env retired) **and for the D3D12 tier via the heavy-d3d12 reroute** (2026-08-29
— the tier's own-legs arm serializes under real app load (3-of-3 ~105 s event dips;
Intel preemption is draw-granular, so a HIGH-priority queue measured null), so a
same-adapter d3d12 engage routes through the d3d11 fill arm by default
(`DXR_SPLIT_D3D12_ROUTE=own` keeps the own-legs arm as the A/B control and the hybrid
arm): full ladder — own-arm 8–9 ms fires to 1.4 ms through the arm at 60 flat,
acceptance leg with events absorbed, planes bound with correct on-change copy cadence,
and the eyeball including live drag-resize. The tier consults the same accepted switch;
its bring-up env retired. The partition's `tier_supported` on d3d12 keys on the REROUTE
being active — the own-legs arm still refuses the partition, since its event record
never passed the matrix. The gl tier remains PROPOSED. (Designed 2026-08-28; commissioned as epic #1264
workstream S4 —
directive: *"one fill engine for every tier will be the key"*) · extends the #918
output-device split beyond hybrid topologies · depends on the #1257 slot partition and
the #868 repaint fill
· related: [ADR-035](ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md),
[ADR-037](ADR-037-adapter-placement-policy-hybrid-devices.md),
[ADR-029], [`docs/reference/motion-to-photon-levers.md`](../reference/motion-to-photon-levers.md)

## Context

### The law this serves

Perceived smoothness on a 3D panel is **cadence stability of fresh weaves**: every vblank
must carry a weave computed from the current tracked eye position. A vblank showing a
stale weave freezes parallax for a frame and then jumps it — and a *varying* rate of such
misses reads as judder even at high average rates. This was established by the #1257/#206
campaign: five eyeball verdicts, one controlled A/B (per-weave-exact prediction horizons:
null on stutter), and the slot partition's acceptance (steady 60 at an app render rate of
20 — *"tracking is great no stutter very good experience"*).

### One arm passes, the others cannot

The runtime has, in effect, two kinds of fill arm today:

| | **the bridge arm** (#918 split, hybrid) | in-process arms (vk_native, d3d12, gl, d3d11) |
|---|---|---|
| fill thread cadence | ~400 ticks/s @ 1.8–2.3 ms | 100–205 ticks/s with 17–23 ms holes |
| device/timeline | **its own D3D11 device + immediate context** on the scanout adapter; nothing app-side can block it | records on the app's queue (or same device); GPU waits interleave with the app's |
| schedule | vsync-locked (commit-relative over a real flip chain) | same gate code — but starved of ticks to execute it |
| per-fill cost | re-weave the published egress slot; **zero per-fill app traffic** (#918: the atlas crosses once per app frame) | full replay on shared resources |
| partition acceptance | **four-for-four** incl. two eyeball sign-offs | "stutters still" at every attempt |

The in-place remedies are exhausted with clean attribution (#1264 S1–S3 evidence):
single-thread ownership (+35% ticks), the fence-park (+5–6 fills short-timescale, the
right default), MMCSS (negative interaction, opt-in) — and none survives the 5-minute
bar. The residual is an **external ~105 s periodic system event** (measured: total GPU
work *falls* during dips; no process competes on any engine; best-fit an iGPU
power-governor dwell) that slows every GPU consumer proportionally. The bridge arm
absorbs the same event invisibly because its fill costs so little that ~5× headroom
remains; the marginal in-process arms fall off the cadence cliff. **Headroom is the
property; the bridge architecture is what buys it.**

### The one line this ADR changes

`comp_split_gate.c` (the #918 decision): when render LUID == scanout LUID, the gate
declines with `COMP_SPLIT_REASON_SAME_ADAPTER` — *"the weave is already local, so the
split has nothing to do."* That rationale reflects the split's original purpose
(eliminating the cross-adapter copy) and is now obsolete: the split's load-bearing
property on the evidence above is the **decoupled fill engine**, which same-adapter
sessions need just as much. Single-GPU machines (pure-iGPU laptops, the Arc desktop
class) have no hybrid fallback and can *only* get it this way.

## Decision

**Every tier's fill runs on the one proven engine — the #918 split's d3d11 fill arm —
including when render and scanout are the same adapter.**

1. **Gate change**: `same_adapter` stops being a decline. Under S4 policy the gate
   returns `split_active = true` with `out_adapter_luid = scanout_luid` (== render LUID).
   The decline survives only as the behavior of the rollout kill switch.
2. **Ingress**: unchanged in v1 — the app deposits the composited atlas once per app
   frame into the egress ring (the ADR-029/xbridge machinery), now via a **same-adapter
   shared-texture open** instead of a cross-adapter transfer: strictly cheaper (no PCIe
   hop). A later optimization may make same-LUID ingress zero-copy (shared handle without
   the staging copy); v1 deliberately ships the byte-proven path.
3. **Fill + present**: exactly the bridge arm as it runs on hybrid today — own device,
   own immediate context, commit-relative vsync-locked schedule, present on its own flip
   chain into the session's window. The VK side presents nothing, same as hybrid.
4. **Eligibility**: the existing split ineligibility ledger applies unchanged
   (shared-texture sessions, no-HWND, legacy-standalone, DP consent per ADR-037 §3a…).
   An ineligible session keeps today's in-process path — a documented limitation, never
   a regression.
5. **Scope order**: Phase A = the VK compositor (the measured worst tier; the split
   machinery already lives there). Phase B = d3d12 (its #918 split legs learn the same
   same-adapter policy, or d3d12 routes through the same xbridge — decided by
   measurement, with the caveat that its baselines need repeated ≥5-min legs). Phase C =
   gl / d3d11-in-process. The service path (ADR-035) already has its own single pipeline
   and is out of scope.

### Rollout

`DXR_SPLIT_SAME_ADAPTER` (default **on** since the VK tier's acceptance; `=0` is the
kill switch, restoring the same-adapter decline). Acceptance per tier is the #1260
matrix — steady presents at panel rate, weave/repaint pair exact, then the eyeball —
measured over **≥3 of the ~105 s events (≈5 min) per leg** (the #1264 method law).
The VK tier passed the full matrix 2026-08-29 (record on #1264). The partition's
`tier_supported` follows the split's own activity (`c->split != NULL` in the VK
compositor), so the two flip together by construction. Phases B/C re-run the same
matrix per tier before consulting the switch.

The VK tier's acceptance ran in the deposit's KEYED-MUTEX mode (the bring-up box's
ICD imports no D3D12_FENCE; see the mode selection in `comp_vk_native_deposit.cpp`).
Plane deposits in that mode run TIMING-ONLY (#1274): their fence edges no-op and
ordering rides the frame path's per-frame CPU wait (#837) plus the planes' on-change
cadence — the same degrade family as the missing-`ID3D11DeviceContext4` rung. If #837
removes that wait, the keyed-mutex atlas and the timing-only planes must be revisited
together.

## Consequences

- The hybrid acceptance record transfers to every eligible tier **by construction** —
  it is the same code, not a port of its ideas. One arm to profile, one to fix.
- Cost: one full-res copy per **app frame** (not per fill) plus ~2 textures of memory —
  measured cheap on hybrid where it also crossed PCIe; cheaper here. Under a D=3
  partition that is 20 copies/s against 60 fills/s that carry none.
- The in-process compositors' own repaint loops become fallback-only (ineligible
  sessions, non-Windows), shrinking the hot surface the #1257/#1264 campaign kept
  finding bugs in.
- The ~105 s system event stops mattering: the fill arm's headroom absorbs
  governor-class slowdowns, as measured on hybrid. (The event's identification continues
  separately as a box note; it is no longer load-bearing.)
- Risk to watch in bring-up: same-adapter double-device contention (two D3D11/VK devices
  on one iGPU share HW queues). The all-engine sampler evidence says the fill's GPU cost
  is small and there is no engine-level competitor; the bring-up matrix verifies it
  end-to-end before any default changes.

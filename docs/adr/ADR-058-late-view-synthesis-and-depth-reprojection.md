---
status: Accepted
date: 2026-10-22
deciders: David Fattal
issues: []
related: [ADR-042, ADR-048, ADR-053, ADR-054, ADR-057, ADR-047, ADR-055]
---

# ADR-058: Late view synthesis and depth reprojection — converted content is placed at the eyes of the frame it is shown in, not the frame it was converted for

**Status:** Accepted (2026-10-22, owner go) · builds on
[ADR-053](ADR-053-conversion-providers.md) (conversion providers, the composed pipeline S0–S8,
latest-wins streams), [ADR-048](ADR-048-lift-viewpoint-policy.md) + Addendum A (viewpoint
policy, auxiliary depth, `depthToDisplay`) and [ADR-042](ADR-042-vendor-2d3d-conversion-supersedes-default.md)
(pre-weave results, the lifted weave rect) · interacts with ADR-047 (late weave, per-screen
period) and ADR-055 (GPU-side weave sync) · spec: `docs/specs/extensions/XR_DXR_lift.md`
(§4.3 display alignment, §5 the result ring).

## In one paragraph

A conversion result is synthesized for the viewer's eyes **at the moment the frame was
submitted**, then shown for as many display frames as the provider takes to produce the next
one. With a fast provider that gap is one frame and nobody sees it. With a slow one — the open
provider on Apple Silicon runs its composed path at 3–4 fps today, a remote provider adds a
network round trip, and an interactive world model will be slower still — the picture is
placed for eyes that have since moved, so the 3D image is placed for an eye pair the viewer no longer has: the stereo
is wrong for a turned head and the depth budget is wrong for a viewer who leaned in or out. This ADR decouples the two rates. The runtime keeps the
latest **source image and depth** from the provider and re-places them at the **current eye
pose of every display frame**: for composed pipelines by re-running the runtime's own view
synthesis late, just before the weave (*late view synthesis*); for providers that emit views
themselves, by a depth-aware forward warp of the last views to the current viewpoints
(*depth reprojection*). The provider rate then bounds how fresh the *content* is, never how
correct its *geometry* is. Nothing changes for apps, providers or vendors: it is trunk policy
on data the contract already carries.

## Context

- **What a result already carries.** Every view-producing result reports the viewpoints it was
  synthesized for (`XrLiftResultViewpointsDXR`: viewpoints, `rectCenter`, `rectSize`, display
  metres), and a stream created with `XrLiftDepthRequestDXR` gets the same-inference auxiliary
  depth with its decode, intrinsics and — for metric depth — a `depthToDisplay` transform that
  places every texel in display space as the woven result presents it (spec §4.3). So the
  runtime knows, per result, *where it was meant to be seen from* and *where every texel is*.
- **Composed pipelines already do synthesis in the trunk.** For a depth-only provider the
  runtime itself runs S5 (reference view synthesis, `conv_viewsyn_ops`, D3D11 on Windows,
  Vulkan on macOS / Linux since P5 step B) right after S6, on the conversion thread, at the
  eyes S3 snapped for that conversion. The image and depth that S5 consumed are in hand.
- **Eyes are already timestamped and per screen.** The DP's target-time eye query (N6 Part 1,
  #2166) and the eye-pair ring give "the eyes at time T" for the screen a rect sits on; the
  conversion core publishes and binds eyes per display processor (`conv_core_eyes_publish`,
  `conv_core_stream_bind_eyes`).
- **Late weave already exists** (ADR-047): the weave runs as late as the pacer allows before
  scanout. That is the right moment to place converted content too.
- **The measured gap.** macOS open provider, composed `views-mono`: 264–386 ms per frame on an
  M1 Pro (the provider's inference dominates). A remote splat or world-model endpoint is
  hundreds of milliseconds to seconds.
- **What a stale result actually gets wrong** (measured by the phase-1 conformance test, a sim
  provider at 4 fps vs 60 Hz with results 250–483 ms old, against a direct synthesis at each
  frame's own eyes). A purely **lateral** move of both eyes changes no synthesized view: the
  synthesis places the source camera at the eye midpoint, so a sideways sway is already
  absorbed — mean error 0.000 / 255, max 0.3. What a stale result misses is the **eye pair
  itself**: a head turn (the spread and orientation of the pair) on every path, and the viewing
  **distance** on the composed path, whose disparity budget scales with 1 / distance. On a
  sway + lean + ±20° turn path, a stale result is off by up to 132/255 with 8.8 % of pixels
  wrong per frame on average (12.8 % worst frame); late synthesis is exact.

## Decision

1. **Placement is late; content is latest.** For every lifted result shown in a display frame,
   the runtime places it at the eyes for **that frame's predicted display time**, not the
   eyes recorded at conversion. Content freshness is bounded by the provider; geometric
   placement is bounded by the display frame. This applies to the runtime-woven paths (lifted
   weave rects, in-process lift streams the runtime presents). Apps that acquire results and
   present them themselves get the same through item 5.

2. **Composed pipelines: late view synthesis (phase 1).** The conversion thread keeps
   producing depth (S4 → S6) at the provider's rate and stops at S6; S5 moves to the weave:
   on each display frame the presenter runs the reference view synthesis on the **latest**
   source image + depth at the **current** eyes, immediately before the weave. The S5 output
   is per display frame, not per conversion; the ring holds image + decoded depth (+ range),
   not views. Cost: one view synthesis per display frame per lifted rect — measured today at
   a few milliseconds on the reference GPUs (Vulkan viewsyn golden test, D3D11 S5); bounded by
   the existing rect input cap (1920 long edge) and the per-screen period.

3. **View-emitting providers: depth re-placement (phase 2).** When a provider emits views
   itself (a vendor module, a world model), and the result carries aux depth with a valid
   `depthToDisplay`, the runtime re-places the result for the **current eye pair** under the
   same camera model the synthesis uses — source camera at the current eye midpoint, views at
   the current eyes — and warps the last views to those viewpoints (unproject with the depth,
   re-project, depth test, fill holes from the nearest view in the set; an N-view result is its
   own disocclusion source). It is **not** a world-fixed warp: re-projecting from a fixed world
   camera would add look-around that a provider running with zero delay never shows, so the
   target is always "what a zero-latency provider would have produced for these eyes". Without metric depth, or with
   `depthValid` false, the result is **held** as today (no reprojection) — the runtime never
   invents a scale. A provider that wants reprojection advertises aux metric depth; one that
   cannot is shown exactly as now.

4. **Bounded correction, honest degradation.** Reprojection corrects viewpoint motion, not
   content motion: a moving subject still updates at the provider's rate. The runtime limits
   the correction to a configurable viewpoint delta (default 10 cm, the range where a
   single-image DIBR stays clean on the reference content) and beyond it eases the content
   toward the plane (reduced disparity) rather than warp it into tearing; the limit and the
   easing are trunk policy (`DXR_LIFT_REPROJECT_MAX_M`, a Dashboard setting later), logged
   once per stream when first engaged. Result age is reported per stream (`XrLiftStreamStats`
   gains the placement age vs content age split, append-only).

5. **App-presented results.** An app that acquires views and presents them itself
   (`xrAcquireLiftResultDXR`) gets placement at *its* frame's eyes only if it asks:
   a future lift spec revision may add an acquire-time "place at this display time" option
   that runs the same late synthesis or reprojection on the service side before export. Not
   in phase 1 or 2; the woven paths are where the product is.

6. **Scope of the trunk work.** One reprojection/late-synthesis stage per graphics backend that
   already has a view-synthesis glue (D3D11 service, Vulkan service and in-process), sharing
   one pure placement module (`u_lift_reproject.{h,c}`: viewpoint delta, the correction limit,
   the per-texel transform composed from `depthToDisplay` and the current eyes) with a CPU
   reference used by the tests, the same split S5 uses today. No provider ABI change, no new
   class, no app-API change for phase 1 (the present-owner repaint signal of Amendment 1 is an
   additive `XR_DXR_weave` revision, not a lift change).

## Consequences

- **Slow providers become usable on tracked displays.** A 3 fps provider still updates content
  at 3 fps, but look-around stays at display rate; the swim disappears. This is what makes the
  open provider on Apple Silicon, remote providers, and an eventual world-model provider
  (ADR-054 Amendment 1: a conversion task or pack, not a class) acceptable on a tracked panel.
- **Composed pipelines get cheaper on the conversion thread** (no S5 there) and more expensive
  on the presenter (one S5 per display frame per lifted rect). The per-screen period from
  ADR-047 Amendment 9 bounds it; a second lifted rect on a 165 Hz screen is the case to measure.
- **The ring changes shape for composed streams** (image + depth instead of views). The
  provider contract does not; the app-facing result does not (apps acquire views as before,
  synthesized at acquire for app-presented streams, item 5 is a later option).
- **Conformance**: per ADR-054 Amendment 1's deliverable rule, the cross-class test is
  *tracking → conversion → weave*: a sim provider at a forced 4 fps, a scripted eye path, and
  the post-weave capture compared against a direct synthesis at the frame's own eyes (same
  pixels within tolerance in phase 1; bounded error in phase 2). The capture trigger exists on
  the POSIX weave engines (#2207); the Windows present-owner weave route still needs its twin
  before the browser case can be gated there (Amendment 1).
- **Risk: double placement.** Views a provider already placed at explicit viewpoints must not
  be re-placed twice; the stage keys off the recorded viewpoints, not assumptions, and
  explicit-viewpoint streams (`XR_LIFT_OUTPUT_EXPLICIT_VIEWPOINTS_BIT_DXR`) are reprojected
  only when the app asked for runtime placement.

## Phases

| Phase | What | Gate |
|---|---|---|
| 1 | Late view synthesis for composed pipelines on the runtime-woven paths (D3D11 service, Vulkan service + in-process); `u_lift_reproject` pure module + CPU reference; stats split | the tracking → conversion → weave test at 4 fps; mac + Linux + Windows panel eyeball |
| 2 | Depth reprojection for view-emitting providers with metric aux depth; correction limit + easing | same test with a view-emitting sim mode; vendor module on the Windows panel |
| 3 | Acquire-time placement for app-presented results (lift spec revision) | first app consumer (media player) |

## Alternatives rejected

- **Make providers faster.** Necessary and ongoing (GPU inputs, smaller models), never
  sufficient: a network round trip and a generative model have floors no optimisation removes.
- **Reproject in the display processor.** The DP owns pixels of the weave, not the viewer
  geometry of converted content; the runtime already owns viewpoints, depth decode and
  convergence (ADR-048). Putting it in the DP would duplicate it per vendor.
- **Ask each provider to extrapolate.** Moves trunk policy into every provider and still
  cannot see the display frame's eyes.
- **Time-warp the woven output (HMD-style ATW).** A lenticular weave is not a projective image;
  warping after the weave breaks the view-to-subpixel mapping. Placement must happen before it.

## Amendment 1 — scope, straddle and timing rules from the box reviews (2026-10-22)

Folded in from the Windows and Linux reviews on PR #2283 and the first hardware data on #2286.

1. **Two routes, two scopes.** The *compositor-presented* routes (in-process and IPC apps whose
   frames the runtime presents) get late synthesis in phase 1 unconditionally: the runtime owns
   the display frame. The *present-owner* route (`XR_DXR_weave`, the DisplayXR Browser) weaves
   only on the caller's submit and never presents on its own. Phase 1 covers it **only while the
   caller submits** — which the browser does today every vsync while a lifted rect is visible
   (Windows, browser 1.8.0: 301 submits / 5 s on a static page; patch 0329/0423 full-page
   damage). A static page that stops submitting gets no late placement. Removing that cost
   (a full-page redraw per vsync to keep the weave alive) is a separate, additive
   `XR_DXR_weave` revision — an advisory "input unchanged" submit flag with which the runtime
   re-weaves the retained input at current eyes without a page redraw — tracked as its own phase
   (1b) and its own proposal.
2. **Straddling rects.** A lifted rect that spans screens is placed with the eyes of the screen
   that contains the rect's centre, with the per-screen coverage dwell of #2256 as hysteresis;
   per-segment placement is a later refinement. One DP, one eye sample, one placement per rect
   per frame.
3. **One runtime-side eye sample per frame.** Each weave asks the display processor once for
   the eyes at the frame's target time (the DP's target-time query, N6), and every runtime-side
   consumer of that frame — the late synthesis, the eyes returned to the caller, the eyes the
   conversion core publishes — uses that one answer; none samples separately. This does **not**
   reach inside the weaver: a vendor weaver picks its own internal eye position for phase and
   sub-pixel mapping and never exposes it (ADR-033: the weaver owns all phase). The residual
   between the DP's target-time answer and the weaver's internal pose is the same one every
   rendered app already has with the weaver; ADR-058 removes the conversion staleness, not that.
4. **Inside the governor's span.** The late synthesis runs inside the late-weave governor's
   measured span (ADR-047), so its cost moves the weave start like any other weave work instead
   of silently eating the scanout margin.
5. **Adapter-aware size.** The late output cap (`DXR_LIFT_LATE_VIEWSYN_MAX_EDGE`, default 960)
   becomes adapter- or governor-driven: Intel ANV measured 2.30 ms (2 views) / 4.41 ms (4 views)
   at 960, about 1.7–2.2× the M1; ~1 ms at 512. Integrated GPUs default lower (512) until the
   governor sizes it from measured headroom.
6. **Hybrid split.** Under the Windows weave-on-scanout split, a late synthesis per frame is one
   cross-adapter copy per frame per rect; phase 1 is gated on a hybrid-box measurement as well.
7. **Encoding and oracle.** The late output keeps the stream's declared encoding (no new format
   or colour-space decision), and the conformance oracle on the boxes is the #2219 capture PNG.
8. **A wasted re-conversion was found on the way:** on the Windows present-owner route an
   unchanged still is re-snapshotted and re-converted every submit (76 s: 4471 submitted, 1839
   converted, all the same picture) — the still de-duplication of #2030 did not engage. That is
   independent of this ADR and tracked as #2287; fixing it is what makes the browser case
   cheap.

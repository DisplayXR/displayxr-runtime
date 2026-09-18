# ADR-041: Fixed view count with per-frame activity — inactive views alias, they do not disappear

**Status:** Accepted (2026-09-18) · follows
[#1486](https://github.com/DisplayXR/displayxr-runtime/issues/1486) /
[#1499](https://github.com/DisplayXR/displayxr-runtime/issues/1499) · extends
[`XR_DXR_display_info`](../specs/extensions/XR_DXR_display_info.md) to SPEC_VERSION 21 ·
related: [ADR-010](ADR-010-shared-app-iosurface-worst-case-sized.md),
[ADR-027](ADR-027-display-zones.md), [ADR-030](ADR-030-crop-before-dp-zero-copy-only-when-swapchain-equals-atlas.md),
[ADR-032](ADR-032-array-layered-swapchains-first-class.md)

## Context

A 3D display changes its *rendering mode* while an app is running — 2D (1 view), stereo
(2 views), quad (4 views). The app allocates ONE worst-case swapchain (ADR-010) and renders only
the active mode's views into per-view tiles. The open question has always been what that means
for the two counts the OpenXR API exposes: what `xrLocateViews` returns, and what
`XrCompositionLayerProjection::viewCount` must be at `xrEndFrame`.

#1486 fixed the first one. `XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO` now reports exactly 2, as
the spec requires, and the device maximum moved to a DisplayXR view configuration,
`XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR`. Both are **fixed for the session's
lifetime** — neither count moves when the mode does.

It did not fix the second one. `xrEndFrame` kept accepting a projection layer carrying *any*
rendering mode's view count under `PRIMARY_MULTIVIEW_DXR`, so a session that located 4 views was
allowed to submit 2 when the panel happened to be in a stereo mode. Core OpenXR says otherwise,
twice:

> `XrCompositionLayerProjection::viewCount` **must** be equal to the number of view poses
> returned by `xrLocateViews`.

> All views associated with projection layers **must** be supplied, or
> `XR_ERROR_VALIDATION_FAILURE` **must** be returned by `xrEndFrame`.

So the runtime shipped a rule that contradicts two core *musts*. That is the defect this ADR
closes. It is not a theoretical one: any conformance-minded consumer, any validation layer, and
anyone reading the spec to decide what our runtime guarantees, all arrive at a different answer
than our code did.

The thing that makes the fix cheap is that the runtime *already* behaves as if the surplus views
were harmless. `xrLocateViews` locates views `[A, R)` at view 0's pose (A = active mode's count,
R = the reported count), and every compositor backend already clamps a submitted layer down to
the active mode's tile count before compositing. The surplus was already being ignored. It was
simply never *said*.

## Decision

**The view count is a fixed property of the session. What varies per frame is how many of those
views are ACTIVE. The app always submits the full located count; the inactive tail aliases
content the app already rendered, and the runtime ignores it.**

> Counts do not change. Activity does.

Three parts:

1. **`XrViewActivityStateDXR`** (`XR_TYPE_VIEW_ACTIVITY_STATE_DXR`, `XR_DXR_display_info` v21),
   an output struct chained on `XrViewState` at `xrLocateViews`, carrying `activeViewCount`.
   Views `[0, activeViewCount)` carry the active mode's poses and FOVs; views
   `[activeViewCount, viewCountOutput)` are inactive, are located at view 0's pose, and their
   submitted content is ignored. Same shape as the existing `XrViewEyeTrackingStateDXR` — a
   per-frame fact the runtime already computes, published rather than inferred.

2. **One submission rule, for every view configuration type**: submit exactly the count
   `xrLocateViews` returned. An app that renders only the active views satisfies it by pointing
   each inactive view at any valid subimage of a swapchain it rendered this frame (view 0's is
   the obvious choice) while keeping that view's own located pose/FOV. Every core sentence then
   holds verbatim, with no DisplayXR carve-out.

3. **`DXR_UNDER_SUBMIT`**, a three-state staging switch, because one arm of the old rule is
   load-bearing for *released* software: `0` strict; `1` (default) strict plus the deprecated
   `PRIMARY_STEREO` 1-view arm, which logs once per session and names the fix; `2` the
   pre-ADR-041 behaviour, as a kill switch.

### What this is NOT

It is not a change to tiling, to the atlas, to `ADR-010` worst-case sizing, or to what the app
*renders*. The app renders exactly what it rendered before. Only the layer's `viewCount` and the
contents of its tail change — and the tail is free, because it points at pixels that already
exist.

## Consequences

**Zero-copy had to move with it, and that is the one non-obvious edit.**
`u_tiling_can_zero_copy()` — the sole gate (ADR-030) — required `view_count == mode->view_count`.
Under this ADR that equality silently retires the one zero-copy case that actually ships:
Windows Leia's worst-case-filling mode is the **1-view 2D** one, which a `PRIMARY_STEREO` app now
submits **two** views into. The gate now asks for *coverage* (`view_count >= mode->view_count`)
and inspects only the first `mode->view_count` rects. A submission that does not cover the mode
(`PRIMARY_STEREO` in a quad mode) still fails — there is no atlas to hand over.

**A 3D zone layer is a projection layer.** `XR_DXR_display_zones` submits each 3D zone as an
`XR_TYPE_COMPOSITION_LAYER_PROJECTION` with a zone chained on it, and it goes through the same
`xrEndFrame` gate. Every zone layer therefore carries the located count too, aliased per zone.

**The `metal` backend gained a guard it never had.** Its zero-copy path looped
`mode->view_count` with no check that the layer carried that many views, so a submission
*shorter* than the mode read `proj.v[]` slots the app never wrote. That was latent before and
reachable now; the other four backends had the check (as an equality, relaxed here to `>=`).

**Compat is a window, not a promise.** The deprecated 1-view arm exists because released demos
submit one view in 2D mode. The default flips to strict in the first runtime release after
`displayxr-common` and the five demos ship the alias submission — the trigger is that shipment,
not a date.

**CTS is unaffected at every knob value.** A conformance session never enables
`XR_DXR_display_info`, and the deprecated arm requires it, so the switch cannot open a hole under
conformance. `test_XrCompositionLayerProjection.cpp` decrements the located count and expects
`XR_ERROR_VALIDATION_FAILURE`; it gets it.

## Alternatives considered

- **Keep under-submit.** Contradicts two core *musts* verbatim; no amount of documentation makes
  a runtime conformant against a sentence it disobeys.
- **A ladder of fixed view-configuration types (1-view, 2-view, 4-view), one per mode.** The app
  would have to end its session and begin a new one on every 2D↔3D toggle — a mode switch is a
  V-keypress, not a restart.
- **`XR_MSFT_secondary_view_configuration`.** Secondary configurations come in fixed shapes; the
  nearest fit wastes two stereo views in 2D and still cannot express a 1-view primary, so it
  costs more and expresses less.
- **An explicitly variable per-frame view count.** Honest about the hardware, but it relaxes
  three separate core *musts* (the locate/submit equality, "all views must be supplied", and the
  fixed-per-configuration count `xrEnumerateViewConfigurationViews` promises) instead of one — a
  larger deviation to fix a smaller one.

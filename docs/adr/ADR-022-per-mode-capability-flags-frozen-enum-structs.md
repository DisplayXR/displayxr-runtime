---
status: Accepted
date: 2026-06-06
---
# ADR-022: Per-Mode Capability Flags + Frozen Enumerated App Structs

## Context

#441 added per-rendering-mode tracking capability (`has_tracking`) so a vendor
can expose e.g. a "2D tracked" mode alongside tracked 3D modes and untracked
export modes (SBS, anaglyph), and so sim_display can honestly advertise that it
has no tracker at all. That one feature forced two structural questions whose
answers will outlive it:

1. **Vendor side** — `struct xrt_rendering_mode` is embedded **by value** as an
   array in `xrt_device`, which the plug-in's `create_device` builds. Any field
   addition changes the element stride → a plug-in ABI break (ADR-020 major
   bump, coordinated vendor release). Per-mode capabilities will keep arriving
   (low-latency, HDR-weave, requires-canvas, …). Does every one cost an ABI
   break?

2. **App side** — `xrEnumerateDisplayRenderingModesDXR` fills an app-allocated
   array of `XrDisplayRenderingModeInfoDXR` using the **runtime's compiled
   struct stride**. The v12 (tile fields) and v13 (`isActive`/`isRequestable`)
   revisions plain-appended fields — which only worked because every consumer
   in the org rebuilt in lockstep. An app binary compiled against an older
   header (engine plug-ins, shipped demos, any third-party app now that the
   repo is public) would have the runtime write past each element it allocated:
   silent memory corruption, with **no version handshake on the app ABI to
   reject the mismatch cleanly** (unlike the plug-in side, where the loader
   rejects ABI-mismatched DLLs at `xrCreateInstance`).

## Decision

**1. Vendor side: capability bits, not fields — v3 is the last rendering-mode
layout break.**

`xrt_rendering_mode` gained, in the vendor-provided MUST-set section:

```c
uint32_t mode_flags;   // bit 0 = XRT_RENDERING_MODE_FLAG_HAS_TRACKING
uint32_t reserved[3];  // MUST be zeroed by the driver
```

paid for with the `XRT_PLUGIN_API_VERSION_CURRENT` 2 → 3 bump. Every future
per-mode boolean is a **new bit** in `mode_flags`; small future per-mode values
draw from `reserved[]`. Zero-init = all capabilities off = the safe default,
so older-style drivers that calloc and don't know a new bit are automatically
conservative. No further stride changes — no ABI v4 for per-mode capabilities.

**2. App side: `XrDisplayRenderingModeInfoDXR` is frozen at its v13 layout.
All future per-mode fields chain.**

New per-mode data reaches apps via structs chained to each array element's
`next` — starting with `XrDisplayRenderingModeTrackingInfoDXR { hasTracking }`
(header v14). The app opts in per the standard OpenXR input convention by
pre-setting each element's `type` (and chaining); the runtime **only walks the
chain of elements carrying the correct input type**, because v13-and-earlier
binaries leave `type`/`next` uninitialized and walking garbage pointers would
crash them. Non-opted-in callers get the exact v13 fill (`next = NULL`'d).

This is the canonical Khronos pattern for extending enumerated output structs
(cf. `XrViewConfigurationView` + chained per-view extension structs, and our
own `XrEyeTrackingModeCapabilitiesDXR` chaining to `XrSystemProperties`).

## Consequences

- Adding a per-mode capability is now: define a bit (vendor side) + define a
  chained struct or extend an existing one (app side) + header minor bump.
  No plug-in ABI break, no app-binary risk, no coordinated release.
- The v12/v13-style plain append is **prohibited** on
  `XrDisplayRenderingModeInfoDXR`. Reviewers should treat any field added to
  that struct as a correctness bug, not a style preference — the failure mode
  is silent memory corruption in binaries we don't control.
- The opt-in type handshake means chained data is invisible to apps that don't
  ask for it — acceptable: capability discovery is inherently opt-in.
- ABI v3 was a hard break for vendor plug-ins (each tracks its rebuild in its
  own repo per ADR-019); the versions.json ABI gate held the runtime bump
  until a matched pair existed, as designed (ADR-020).

## Amendment 1 (2026-10-10, #1855): sim_display's advertisement under webcam tracking

sim_display's honest "no tracker" advertisement now has a third state. With
`SIM_DISPLAY_WEBCAM_TRACKING=1` **and** a face estimator built into the
plug-in, sim_display really tracks the viewer from a plain webcam, so it
advertises what a tracking vendor does: `supported_eye_tracking_modes |=
MANAGED_BIT`, `default_eye_tracking_mode = MANAGED`, and `has_tracking` on its
3D rendering modes (2D passthrough stays untracked). The consistency rule
(`supported_eye_tracking_modes != 0` ⇔ some mode has `has_tracking`) holds in
every combination: neither toggle → 0 / no tracked mode; `SIM_DISPLAY_FAKE_TRACKING`
→ MANUAL; webcam → MANAGED; both → MANAGED | MANUAL with MANAGED the default.

Only MANAGED is offered for the webcam path: on loss it animates the eyes back
to the nominal viewer itself (the vendor-side collapse of the MANAGED
contract) and it has no hardware 2D/3D switch to hand to an app. Multi-screen
per-monitor descriptions are unchanged — segment display processors keep their
nominal viewer, because the camera's pose is known relative to one panel only.

**The default build carries no estimator** (which model and inference
dependency to ship is #1855's open question), and without one the toggle only
logs a WARN: no capability changes and no camera opens. Nothing an app sees
changes until an estimator lands.

## References

- #441 (umbrella), runtime PRs #443 / #446 / #451; vendor rebuilds tracked in
  the respective plug-in repos
- `docs/roadmap/per-mode-tracking-capability-plan.md` (implementation plan)
- `docs/specs/extensions/XR_DXR_display_info.md` §7c (v14 API surface)
- `docs/specs/vendor/eye-tracking-modes.md` (capability layering + contract)
- ADR-020 (plug-in ABI policy this builds on)
- #1855 (sim_display webcam tracking — Amendment 1)

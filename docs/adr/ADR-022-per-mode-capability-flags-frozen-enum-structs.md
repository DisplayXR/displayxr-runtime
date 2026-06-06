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

2. **App side** — `xrEnumerateDisplayRenderingModesEXT` fills an app-allocated
   array of `XrDisplayRenderingModeInfoEXT` using the **runtime's compiled
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

**2. App side: `XrDisplayRenderingModeInfoEXT` is frozen at its v13 layout.
All future per-mode fields chain.**

New per-mode data reaches apps via structs chained to each array element's
`next` — starting with `XrDisplayRenderingModeTrackingInfoEXT { hasTracking }`
(header v14). The app opts in per the standard OpenXR input convention by
pre-setting each element's `type` (and chaining); the runtime **only walks the
chain of elements carrying the correct input type**, because v13-and-earlier
binaries leave `type`/`next` uninitialized and walking garbage pointers would
crash them. Non-opted-in callers get the exact v13 fill (`next = NULL`'d).

This is the canonical Khronos pattern for extending enumerated output structs
(cf. `XrViewConfigurationView` + chained per-view extension structs, and our
own `XrEyeTrackingModeCapabilitiesEXT` chaining to `XrSystemProperties`).

## Consequences

- Adding a per-mode capability is now: define a bit (vendor side) + define a
  chained struct or extend an existing one (app side) + header minor bump.
  No plug-in ABI break, no app-binary risk, no coordinated release.
- The v12/v13-style plain append is **prohibited** on
  `XrDisplayRenderingModeInfoEXT`. Reviewers should treat any field added to
  that struct as a correctness bug, not a style preference — the failure mode
  is silent memory corruption in binaries we don't control.
- The opt-in type handshake means chained data is invisible to apps that don't
  ask for it — acceptable: capability discovery is inherently opt-in.
- ABI v3 was a hard break for vendor plug-ins (each tracks its rebuild in its
  own repo per ADR-019); the versions.json ABI gate held the runtime bump
  until a matched pair existed, as designed (ADR-020).

## References

- #441 (umbrella), runtime PRs #443 / #446 / #451; vendor rebuilds tracked in
  the respective plug-in repos
- `docs/roadmap/per-mode-tracking-capability-plan.md` (implementation plan)
- `docs/specs/extensions/XR_EXT_display_info.md` §7c (v14 API surface)
- `docs/specs/vendor/eye-tracking-modes.md` (capability layering + contract)
- ADR-020 (plug-in ABI policy this builds on)

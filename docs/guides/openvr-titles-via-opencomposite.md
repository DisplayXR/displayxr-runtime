# Running OpenVR Titles — OpenComposite, Not a SteamVR State Tracker

**Audience:** anyone asking "can a SteamVR/OpenVR title drive DisplayXR
motion controllers?" now that input providers exist (ADR-034 / #823).

## The path

[OpenComposite](https://gitlab.com/znixian/OpenOVR) reimplements the
OpenVR API **on top of OpenXR**: the title loads OpenComposite's
`vrclient` in place of SteamVR's, and every OpenVR call is translated
into OpenXR calls against whatever runtime is active. With DisplayXR set
as the active OpenXR runtime, an OpenVR title therefore reaches:

- the DisplayXR compositor exactly like a native OpenXR app, and
- the input-provider motion controllers through the standard action
  system — OpenComposite suggests bindings for common interaction
  profiles, which the runtime's binding engine resolves against the
  provider's claimed profile (`khr/simple_controller` for the in-tree
  `sim_input` / `net_input` providers) like any other app's.

No DisplayXR-specific work is required; treat OpenComposite as just
another OpenXR application. Caveats are the generic OpenComposite ones
(per-title compatibility, overlay APIs unimplemented), plus DisplayXR's
own app-model note: an OpenVR title arrives as a **legacy** app — no
`XR_DXR_*` extensions — so it gets the legacy compromise scaling of
`docs/architecture/extension-vs-legacy.md`.

## Explicit non-goal

Resurrecting Monado's OpenVR **state trackers** (`st_ovrd` /
`steamvr_lh`, removed in the fork) is a non-goal: that code made Monado
*pose as a SteamVR driver* — the opposite direction — and dragging the
OpenVR API surface into the runtime contradicts the lightweight-runtime
premise. The supported story is OpenVR title → OpenComposite → OpenXR →
DisplayXR, with input flowing from input-provider plug-ins
(`docs/specs/runtime/input-provider-discovery.md`).

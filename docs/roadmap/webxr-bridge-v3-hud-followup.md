# WebXR Bridge — HUD layer follow-up (#139)

## Context

The DisplayXR compositor window has a built-in HUD (TAB key) that displays the qwerty driver's controls and runtime stats. When a WebXR bridge session is active, this HUD is no longer relevant — the page owns input semantics, not qwerty (qwerty is gated off via `g_bridge_relay_active`). The TAB-toggled HUD still shows stale qwerty hints in bridge-relay mode.

## Goal

Allow bridge-aware pages to:

1. **Suppress** the runtime's built-in HUD (auto-hidden when bridge is active).
2. **Submit a window-space layer** for an app-styled HUD that the compositor composites over the rendered scene. Equivalent to how `cube_handle_d3d11_win` draws its own HUD via `XrCompositionLayerQuad` / overlay rendering.

## Proposed mechanism

### Runtime side
- When `g_bridge_relay_active`, skip the built-in HUD draw path in `comp_d3d11_window.cpp` (or have the HUD opt out via a new flag).
- Already done: TAB-key qwerty processing is gated.

### Page → bridge → compositor delivery
Two options:

**(A) Use existing OpenXR composition layers.** Page submits an `XrCompositionLayerQuad` positioned in window space (z=0, sized to window dims). The bridge-relay compositor's existing layer pipeline draws it over the projection layer.
- Pro: zero new protocol surface, reuses standard WebXR APIs.
- Con: Chrome's WebXR may not expose quad layers; would need a polyfill.

**(B) New WS message: `hud-layer`.** Page sends a texture handle (or pixel data) + window-relative rect. Bridge writes to a shared texture, compositor blits over the projection.
- Pro: full control over HUD pixels.
- Con: more plumbing; texture sharing across processes is non-trivial.

Lean toward (A) for v1 (when Chrome supports quad layers). Skip if not viable; fall back to (B).

## Verification
- Before: TAB shows qwerty hints over WebXR scene.
- After: TAB is no-op (or hidden) when bridge active. Page can render its own HUD that appears in correct window-space position even as window resizes/moves.

## Status
**Deferred** — tracked here for the next bridge phase. Current Phase 3 input-forwarding work (raw WASD/mouse pass-through) is sufficient for cube_handle parity minus HUD.

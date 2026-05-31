# ADR-014: Shell Owns Rendering Mode Control

## Status

Accepted. **Mechanism revised 2026-05-31** (issue #376): keyboard handling moved off the
runtime's server-side qwerty handler and onto the workspace controller, completing the
ADR-018 controller migration (`XR_EXT_spatial_workspace` spec_version 22–23). The
**decision below is unchanged** — the workspace owns rendering mode and an app cannot
self-switch while sharing the display — only the *how* was updated to the current model.
Current contract: `docs/architecture/separation-of-concerns.md`,
`docs/specs/extensions/XR_EXT_spatial_workspace.md`.

## Context

In standalone mode, apps control their own rendering mode (2D/3D/multiview) via the `xrRequestDisplayRenderingModeEXT` API, typically triggered by the V key or number keys (1-8). The app and runtime are in the same process, so mode changes take effect immediately.

In workspace mode (multi-app compositor), multiple apps share a single display. The display's physical state (lenticular lens on/off, interlacing pattern) is global — it cannot be different per app. If each app could independently change the rendering mode, they would fight over the display state.

## Decision

**The workspace controller controls rendering mode changes, not the app** (the DisplayXR Shell is the reference controller).

When an app runs inside a workspace:

1. **Input reaches the controller, and the focused app, as events** — the runtime drains keyboard/pointer input to the controller as `XR_EXT_spatial_workspace` KEY events (`xrEnumerateWorkspaceInputEventsEXT`) and also forwards them to the focused app's HWND. The runtime does **not** run a server-side qwerty handler for workspace input (those hooks are gated on `!workspace_mode_active`); the controller owns all shortcut policy.

2. **`xrRequestDisplayRenderingModeEXT` is a no-op for regular workspace clients** — returns `XR_SUCCESS` but does not change the mode, so apps can't fight over the global display state. Only the privileged workspace-controller session actually flips the mode through it.

3. **The controller owns mode policy** — it decides the binding (e.g. Ctrl+V toggles 2D/3D, number keys select specific modes) from the drained KEY events and calls `xrRequestDisplayRenderingModeEXT` on its session; the DP mode flip is acknowledged across all client slots.

4. **The app is notified of mode changes via events** — the runtime publishes the current `active_rendering_mode_index` (readable per-frame and via `XR_EXT_display_info`) and the OXR session poll pushes `XrEventDataRenderingModeChangedEXT` to the app's event queue. The app tracks that single source of truth and adapts (1 view for 2D, 2+ views for 3D).

## Key Routing

In workspace mode the runtime delivers input twice, with the controller owning all policy:

1. Each keypress is pushed to the controller as a workspace KEY event (drained via `xrEnumerateWorkspaceInputEventsEXT`).
2. The same key is forwarded to the focused app's HWND via `PostMessage`, **except** runtime-reserved keys (e.g. ESC while a window is maximised).
3. The controller decides every shortcut from its KEY-event stream — mode (Ctrl+V / number keys), focus-cycle (TAB), maximize (F11), depth-step (`[` / `]`), close (DELETE → `xrRequestWorkspaceClientExitEXT`), launch (Ctrl+O) — and acts via the appropriate extension call. The app still receives the keys for its own use.

## Consequences

### Positive
- Apps keep all their keys — V can mean anything to the app
- No per-app mode conflicts — one global display state
- Clean separation of concerns: shell manages display, app manages content
- Same app binary works in standalone (app controls mode) and shell (shell controls mode) without code changes

### Negative
- Apps cannot programmatically switch modes in shell (e.g., a media player switching to 2D for menus)
- Slight latency (~50ms) for mode change notification via shared memory polling

### Future Considerations
- If apps need to request mode changes in shell, add an advisory API (shell can accept or reject)
- Multi-window shell may need per-window mode overrides (e.g., one window in 2D, another in 3D)

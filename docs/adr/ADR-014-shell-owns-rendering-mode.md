# ADR-014: Shell Owns Rendering Mode Control

## Status
Accepted

## Context

In standalone mode, apps control their own rendering mode (2D/3D/multiview) via the `xrRequestDisplayRenderingModeEXT` API, typically triggered by the V key or number keys (1-8). The app and runtime are in the same process, so mode changes take effect immediately.

In shell mode (multi-app compositor), multiple apps share a single display. The display's physical state (lenticular lens on/off, interlacing pattern) is global — it cannot be different per app. If each app could independently change the rendering mode, they would fight over the display state.

## Decision

**The shell controls rendering mode changes, not the app.**

When an app runs inside the shell:

1. **All keyboard input is forwarded to the app** — including V, P, and 0-9 keys. The app can use these keys for any purpose (game actions, UI shortcuts, etc.).

2. **`xrRequestDisplayRenderingModeEXT` is a no-op in shell/IPC mode** — returns `XR_SUCCESS` but does not change the mode. This prevents apps from overriding the shell's mode.

3. **The shell changes the mode via qwerty controls** — V toggles 2D/3D, number keys select specific modes. The qwerty handler runs server-side in the compositor process.

4. **The app is notified of mode changes via events** — the server writes `active_rendering_mode_index` to IPC shared memory. The client reads it per-frame, and the OXR session poll pushes `XrEventDataRenderingModeChanged` to the app's event queue. The app then adapts (1 view for 2D, 2 views for 3D, etc.).

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

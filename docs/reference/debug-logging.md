# Debug Logging Conventions

DisplayXR uses the Monado `U_LOG_*` macros for runtime logging. Follow these conventions to avoid log bloat.

## Log Levels

| Level | Macro | Use for |
|-------|-------|---------|
| WARN | `U_LOG_W` | One-off init, error, and lifecycle events only |
| INFO | `U_LOG_I` | Recurring/throttled diagnostic logs (per-frame, per-keystroke, etc.) |

## Rules

- **Never add per-frame `U_LOG_W` calls** — they cause massive log bloat. If something fires every frame, it must be `U_LOG_I` or lower.
- Use `U_LOG_W` for events that happen once or rarely: initialization, teardown, error conditions, mode changes.
- Use `U_LOG_I` for events that may fire frequently but are useful for diagnostics.

## Opt-in tracers

A tracer is per-frame `U_LOG_W` that is **off unless its env var is set**, so it does not violate the
rule above (nothing is emitted in a normal run) while still being loud enough to read when armed.
Add one only when the alternative is guessing, and gate it with `DEBUG_GET_ONCE_BOOL_OPTION`.

| Env var | What it traces | Where |
|---|---|---|
| `DXR_QTRACE=1` | The whole head/view pose path, one line per event, tagged `[QTRACE]`: `QD` (qwerty device integration — key state, mouse yaw/pitch deltas, resulting pose), `W32` (Win32 input reaching the driver: focus loss, WASD, RMB capture, mouse-look deltas), `CREATE-REF` (`xrCreateReferenceSpace` type + pose), `LS` / `LSS` (`xrLocateSpace` / `xrLocateSpaces` — target + base space, flags, resulting pose), `LV` (`xrLocateViews` — the head pose it consumed and the per-view pose + FOV it produced, plus which rig branch ran) | `qwerty_device.c`, `qwerty_win32.c`, `oxr_space.c`, `oxr_session.c` |
| `OXR_DEBUG_VIEWS=1` | Monado's built-in `xrLocateViews` pretty-printer | `oxr_session.c` |
| `OXR_DEBUG_SPACES=1` | Monado's built-in space pretty-printer | `oxr_space.c` |

`DXR_QTRACE` answers "the app is not moving — who dropped the pose?" by showing the same frame at
every stage. It is how #999's sibling investigation established that the runtime delivers a correct
moving pose to legacy Unity and that the loss was app-side (Unity cancels the head out of `XrView`
via `inv(headAnchor)` and re-applies it through a `TrackedPoseDriver`, which that probe scene did not
have). Expect a few thousand lines per second — capture to a file and grep by tag.

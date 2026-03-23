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

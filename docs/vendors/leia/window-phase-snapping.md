# Leia SR — Lenticular Phase-Aligned Window Snapping

Vendor-specific behavior of the Leia SR weaver around window positioning on Windows. This is
**one vendor's optional optimization** of the neutral phase-alignment problem described in
[`XR_EXT_win32_window_binding` §2.4](../../specs/extensions/XR_EXT_win32_window_binding.md#24-the-phase-alignment-problem)
— other vendor display processors may handle it differently or not at all.

## The problem (recap)

On a lenticular display, each subpixel projects light at a specific angle determined by its
absolute position relative to the lens array. When a window is dragged to an arbitrary pixel
position, the phase relationship between content and lenses can break, causing crosstalk
(left/right eye images bleeding into each other).

## How the SR weaver handles it

The Leia SR weaver handles this **automatically**. During initialization, the weaver
**subclasses the application's window procedure** via `SetWindowLongPtr`. The subclassed
`WndProc` intercepts:

| Message | Action |
|---------|--------|
| `WM_ENTERSIZEMOVE` | Records initial window position |
| `WM_WINDOWPOSCHANGING` | Snaps proposed position to nearest phase-aligned coordinate |
| `WM_EXITSIZEMOVE` | Clears drag state |

The window moves in small discrete steps (typically 1–2 pixels) that preserve the lenticular
phase, so the 3D effect remains stable throughout a drag while motion feels smooth to the user.

No runtime or application code is needed — any application using a weaver-bound window gets
phase-aligned snapping automatically. This is an optional vendor optimization: without it, 3D
quality degrades during drag but is correct at rest.

## SR SDK `WndProcDispatcher` race condition (resolved)

Previous versions of the SR SDK had a **use-after-free race condition** in `WeaverBaseImpl.ipp`
where `WndProcDispatcher` released the global map lock before calling
`instance->weaverWndProc()`, allowing another thread to destroy the weaver between the lookup
and the call.

**Status: Fixed in SR SDK** (commit `54410d9f`). The fix introduces a per-instance `SRWLOCK`
(`instanceLock`) inside a `WindowObjectData` struct. The dispatcher now acquires a shared
instance lock before releasing the map lock and holds it for the entire `weaverWndProc` call.
`restoreOriginalWindowProc` acquires the instance lock exclusively after removing the entry from
the map, which blocks until all in-flight dispatcher calls complete. Re-entrancy is handled via
an `inDispatcherCount` counter, and same-thread destruction (from within a `WndProc` callback)
skips the exclusive-lock wait to avoid deadlock.

The workaround in the runtime's weaver-destroy path (message pumping and delays) is no longer
needed but remains as a defensive measure.

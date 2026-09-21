# Lens-preference ownership: every 2D request pairs with a restore

**Status:** in force on desktop Linux (vk_native + the Leia srSDK display processor).
**Applies to:** every runtime path that can reach a display processor's
`request_display_mode(false)` or `on_pause`.
**Code:** `src/xrt/auxiliary/util/u_display_mode_hold.h` (the rule),
`src/xrt/compositor/vk_native/comp_vk_native_compositor.c` (its one user today),
`tests/tests_aux_display_mode_hold.cpp` (the test).

## The rule

> After its first explicit lens call, DisplayXR (acting for the app) owns the panel's lens
> preference, and nobody else will ever change it. So **every path that asks for hardware 2D must
> pair with a path that asks for the previous state back when its reason clears**, and a
> runtime-side degrade must restore **the app's own last choice**, never a forced 3D.

"The app's own last choice" is the last value the session sent through the compositor's
`request_display_mode` entry point: `xrRequestDisplayModeDXR`, the hardware state of the mode
picked by `xrRequestDisplayRenderingModeDXR`, the V key, or the zones tier-1 fallback. It is not
the active rendering mode's `hardware_display_3d`: `xrRequestDisplayModeDXR` overrides the
hardware state without changing the mode.

## Why: the vendor rule (Leia srSDK, LeiaSR #266)

SRService keeps one lens preference per client connection, i.e. per SR context, and the last
writer wins: `srCreateLens` and the weaver share one `SwitchableLensHint`.

- Before the application's first `srLensEnable`/`srLensDisable` the **weaver** owns it: it votes
  the lens on at the first woven frame and withdraws the vote only after 500 ms continuously off
  the panel.
- The first explicit call makes it the **application's** for the rest of that context's life.
  From then on the Linux weaver never writes it again: not on panel exit or re-entry, not after
  tracking loss, not from a weaver recreated on the same context. It still picks woven 3D or
  plain 2D output per frame.
- A new context (for example after an SRService restart) starts with the weaver in charge again.

The Leia Linux plug-in keeps one process-wide SR context, so the first runtime or app request
that reaches `srLensEnable`/`srLensDisable` owns the lens for the rest of the process. A path
that turns the lens off and relies on the weaver to turn it back on leaves the panel flat for
good.

The plug-in's side of the contract (in `displayxr-leia-plugin`,
`docs/display-mode-switching.md`):

- no lens call at startup;
- a 3D request before anything has asked for 2D makes **no** call, which keeps the weaver's
  off-panel release for an untoggled session (the runtime requests 3D at every
  `xrBeginSession`);
- the first 2D request is sent and takes ownership, and every request after it is sent;
- the last request sent is re-applied to every new SR context.

## Audit of desktop-Linux callers

Every desktop-Linux path that reaches `request_display_mode(false)` or `on_pause` on a display
processor, as of this change. "Restores" means a matching request for the previous state when the
reason clears.

| Path | Turns the lens off? | Restores? | Fixed here? |
|---|---|---|---|
| `vk_linux_update_surface_not_1to1()`, Wayland refuse-rather-than-resample (#1595) | Yes, `request_display_mode(false)` + `on_pause` on the NOT_1TO1 edge | It asked for **3D** on the clear edge, overriding an app that had chosen 2D. App requests made while degraded went straight to the DP, so a V-key 3D lit the lens over a degraded, un-woven frame. | **Yes.** The degrade is now a `u_display_mode_hold`: the clear edge restores the session's recorded choice, and requests made while held are recorded and applied at release. |
| X11 present-origin refusal (`vk_x11_present_origin_is_panel_native`) and drag-snap refusal (#1609) | No. They only withhold the phase origin / the snap. | n/a | No change needed. With no explicit origin the SDK weaver places the window from its own X11 geometry for the on-panel decision. |
| `xrRequestDisplayModeDXR` / `xrRequestDisplayRenderingModeDXR` (`oxr_session_request_display_mode`) | Yes, when the app asks for 2D | Only by the app's own later request, which is the app's choice by definition | No change. Under #266 the app's first 2D takes ownership, deliberately. |
| V key (qwerty `qwerty_check_display_mode_toggle`, vk_native and multi) | Yes, on V to 2D | Yes, V again restores the saved 3D mode | No change. |
| `xrBeginSession` (`oxr_session_begin`) | Only if the session's mode is 2D | n/a (it is the restore for `xrEndSession`) | No change. Its 3D request makes no lens call in the Leia Linux plug-in until something has asked for 2D. |
| `xrEndSession` (`oxr_session_end`) | Yes, `request_display_mode(false)` | Yes, the next `xrBeginSession` requests the session's mode | No change. A process that ends a session and begins another owns the lens in the second one (see below). |
| `vk_compositor_end_session` / `vk_compositor_begin_session` | `on_pause` / `on_resume` | Yes, paired | No change. The Leia Linux DP implements neither slot. |
| Zones tier-1 fallback (vk_native) | No, only ever requests 3D | n/a | No change. While a hold is on, its 3D request is recorded and applied at release. |
| Deferred request at DP creation (`hw3d_request_pending`) | Only if the session asked for 2D before the DP existed | It is the session's own choice | No change. |
| Multi-compositor / service path (`multi_compositor_request_display_mode`, IPC) | Only on app request, V key or session end | As above | No change. Its visibility / weave-idle `on_pause` (#1039, #1278) is Android-only. |
| Focus loss, visibility change, tracking loss | No path on desktop Linux reaches the lens | n/a | Nothing to fix. MANAGED tracking loss is the weaver's own output decision, not a lens call. |

Out of scope, same shape: the Android `vk_android_update_container_scaled()` degrade also restores
3D rather than the app's choice on its clear edge. Its CNSDK display processor has a different
ownership model, so it was left unchanged.

## Known consequences

- A session that goes to 2D and back (app toggle, V key, or a Wayland NOT_1TO1 episode) owns the
  lens from then on, so the weaver's off-panel release no longer applies to it for the rest of the
  process. On Wayland the NOT_1TO1 gate itself degrades when the surface leaves the panel (with
  the geometry extension installed), which covers that. On X11 nothing does: a toggled X11 session
  dragged off the panel keeps the lens in whatever state it last asked for.
- `xrEndSession` asks for 2D, so a process that runs a second session owns the lens in it.
- A context invalidated by an SRService restart is replaced by the plug-in only once no weaver is
  alive on it, i.e. at the next session. The running session keeps a dead context until then.

## Verifying on hardware

Not verifiable without the panel, and so still open:

1. Untoggled X11 and Wayland sessions: drag the window off the DS1 and back. The lens should drop
   about 500 ms after leaving and come back on re-entry, with no
   `DisplayXR now owns this SR context's lens preference` line in the log.
2. Wayland NOT_1TO1 with the app in 3D: degrade and clear. The lens goes off, then on
   (`NOT_1TO1 cleared: ... restoring the session's own hardware 3D`).
3. Same with the app in 2D (V key first): the lens stays off after the clear.
4. V key during a NOT_1TO1 episode: no lens change until the clear, then the last choice.
5. Restart SRService between two sessions of one long-lived process with the app having chosen 2D:
   the second session's context re-applies 2D (`re-applied to a new SR context`).

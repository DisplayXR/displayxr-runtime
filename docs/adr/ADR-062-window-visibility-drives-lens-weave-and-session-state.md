---
status: Proposed
date: 2026-11-15
---
# ADR-062: Window visibility drives the lens, the weave and the session state — one runtime-owned signal

**Status:** Proposed (2026-11-15). Tier 1 shipped (#2671 / #2673, follow-ups #2675, #2676);
tiers 2 and 4 in progress; tier 3 approved 2026-11-15, being implemented. Related: [ADR-007](ADR-007-compositor-never-weaves.md),
[ADR-027](ADR-027-display-zones.md) (the zone wish), [ADR-035](ADR-035-service-owned-arbitration-single-pipeline-isolated-satellites.md)
(the panel lease), [ADR-047](ADR-047-multi-screen-segments-and-per-screen-views.md) (segments),
[docs/specs/vendor/lens-preference-ownership.md](../specs/vendor/lens-preference-ownership.md),
[docs/specs/vendor/eye-tracking-modes.md](../specs/vendor/eye-tracking-modes.md).

## In one paragraph

A switchable-lens panel's lens is panel-wide, so an app whose window nobody can see must stop
holding the panel in 3D — and, if nobody can see it, it has no reason to weave or even to render.
Only the runtime knows where every 3D window is, whether it is covered, and which windows share a
panel: with client-side weave routing a vendor runtime cannot see those windows at all, and the
runtime already arbitrates 2D/3D across apps. So the runtime owns **one visibility signal per
3D window per screen**, computed off the hot path (`aux_util/u_window_visibility`), and that one
signal drives four consumers, each a tier that can ship alone: (1) the lens wish, (2) skipping the
weave and present, (3) telling the app, through an opt-in extension, (4) on panels that switch locally, a screen-space
mask of the 3D region that is actually visible. The vendor display processor keeps what only it
knows — how deeply its tracker may sleep given its own restart cost — behind the existing
`on_pause` / `on_resume` slots.

## Context

- A windowed 3D app fully covered by 2D windows, or minimized, used to keep the panel in 3D for as
  long as its session ran; the 2D windows on top were shown through an active lens (#2671).
- With client-side routing the vendor runtime casts no lens vote and runs no occlusion test for
  our windows; per vendor connection the last explicit lens call wins and the vendor ORs across
  connections. Each in-process DisplayXR app has its own connection (its own vote); the service
  holds one connection for all its IPC clients and arbitrates them itself (the panel lease).
- The vendor never sees zone masks: a 1×1-grid panel collapses the wish to one lens boolean, and
  that boolean is everything the vendor combines across apps.
- Measured restart costs on the reference vendor (2026-11-15): a live weaver that is not called is
  near free and resumes on the next call; the tracker rests on its own 20 s after the lens goes
  2D; waking it costs ~0.9 s warm and ~6 s cold; the lens reaches 3D ~0.25 s after the request.

## Decision

### D1 — The signal

**Hidden** = the window is minimized, cloaked / on another virtual desktop, the session is locked
or the 3D display is off, or the window's client area on a given 3D screen is covered by opaque
top-level windows above it except for less than a small threshold (≈2% of that area, so a sliver
behind the taskbar or a window edge does not hold the panel). A window that has **not been shown
yet** is `NOT_SHOWN`, never hidden: an app that renders its first frame into a hidden window and
then calls ShowWindow must not be demoted at startup.

**Timing.** Minimize, cloak, lock and display-off are reported hidden at once. Covered is reported
after a ~1.5 s dwell, and not at all while another window is being moved or sized (a window
dragged across a small 3D window must not flip the panel). Visible is reported at once, driven by
window events (foreground, minimize end, show/hide/move of a covering window) with a 1–2 Hz safety
poll. Battery: the poll does not speed up while hidden; events do the work.

**Occluders.** The z-walk rules in `u_window_visibility.h` (cloaked, shell and compositor overlay
classes, overlay processes, same process, click-through and non-opaque layered windows are not
occluders). The taskbar **is** an occluder when it is not auto-hidden. Cheap pre-filter: the window
rect; DWM frame bounds only for windows that intersect.

**Per screen.** The signal is evaluated per 3D screen the window covers (ADR-047). Tier 1 acts per
screen; tiers 2–4 act on "hidden on every screen", so a window half on a visible 2D screen never
stops presenting.

**Platforms.** Windows ships. X11 (map state, `_NET_WM_STATE_HIDDEN`, visibility events, stacking)
and macOS (`NSWindow.occlusionState`) are in review (#2675). Wayland cannot see global stacking and
reports visible.

**Aggregation.** In-process: one window per vendor vote. Service default policy: the panel follows
the lease holder, so the vote is that window's. Under a workspace controller the controller owns
the panel and the signal is not applied.

**Observability** (an app must be able to tell why the lens went 2D):
- the real hardware state is reported: `XrEventDataHardwareDisplayStateChangedDXR` fires when the
  runtime drops the lens for visibility and when it restores it;
- a 3D request made while hidden is recorded and applied on return, and the requester gets a
  denial reason `WINDOW_HIDDEN` where the request path reports one;
- `displayxr-cli status` and the MCP status snapshot show each session's visibility with its
  reason and, when covered, the covering process;
- the switches are persisted settings (`displayxr-cli` settings, read by the service too), not
  only environment variables.

### D2 — Tier 1: the lens wish (shipped; D1 fixes pending)

While every 3D window a vote serves is hidden on a screen, that screen's vote is 2D; on return it
is **the session's own last request** (never a forced 3D — the lens-preference-ownership rule).
Zone wishes are withheld while hidden and re-published from scratch on return; the session's
choice is always re-sent first. One WARN per transition, naming the reason and the occluder.

Side effect to document: with the lens off, the reference vendor rests its tracker after 20 s, so
a return after a long hide shows 0.9–6 s of untracked (nominal-viewer) 3D before tracking resumes.

### D3 — Tier 2: stop presenting (default on)

While the window is hidden on every screen, the compositor **skips the weave and the present**
but keeps the weaver, the swapchains and the target alive. The app keeps rendering and its frame
pacing is unchanged (unpresented frames are paced on the same timer grid). On the visible edge the
compositor **re-weaves the retained atlas at once**, so the screen does not wait for the app's next
frame. Exceptions and rules:
- `_texture`-class and present-owner sessions are exempt (the app presents; skipping our weave
  would leave it presenting stale content);
- captures (the %TEMP% triggers, `xrCaptureAtlasDXR`, MCP) still produce output while hidden and
  log that the session is hidden;
- window-capture sharing (a call app sharing the covered window) sees the last presented frame
  while hidden. This is accepted and documented; an app that must keep presenting opts out (D4's
  extension) or the user turns tier 2 off;
- edge ordering: hidden edge = lens 2D (tier 1) **then** `on_pause`; visible edge = `on_resume`
  **then** the session's lens request **then** the first weave. `on_pause`'s contract ("release
  your lens vote") is satisfied by the tier-1 request already sent; a DP must not re-assert 3D in
  `on_resume` (the runtime sends the lens request itself).

The vendor decides how deep `on_pause` idles. For the reference vendor the recommendation is
nothing beyond the lens: its own 20 s rest gate already sleeps the tracker, and stopping it
outright would make every uncover pay ~0.9 s.

### D4 — Tier 3: telling the app (opt-in extension; NOT approved yet)

The review found the default-on session-state demotion unsafe: there is no in-process event path
today (every in-process session hard-codes visible + focused); apps that render into a not-yet-shown
window would deadlock; dropping FOCUSED kills `xrSyncActions` for a covered window that still has
OS focus; a first-party engine plugin is known to livelock if FOCUSED returns late. So tier 3 is a
proposed extension, `XR_DXR_window_visibility`:
- an event `XrEventDataWindowVisibilityChangedDXR { visible, reason: MINIMIZED | CLOAKED | COVERED
  | NOT_SHOWN | LOCKED | DISPLAY_OFF }`, delivered to every app that enables the extension;
- an opt-in flag (on the window-binding struct or a session create flag) asking the runtime to
  move a hidden session to `SYNCHRONIZED` (`shouldRender = XR_FALSE`). In OpenXR `FOCUSED`
  implies `VISIBLE`, so a demotion always drops focus too; input is protected by two rules
  instead: a session is **never** demoted while its window is the OS foreground window, and never
  before its window has been shown once (`NOT_SHOWN`). On the visible edge it returns to
  `VISIBLE` and then `FOCUSED` as before;
- an app that does not enable it gets tiers 1–2 only and sees no session-state change (superseded
  for minimize by the amendment below);
- needs a new in-process session-event path (the native compositors push state change events).

**Approved by David 2026-11-15.** Being implemented.

**Amendment (David, 2026-11-15): minimize stops rendering by default.** After a compatibility pass
over unmodified apps (the demos, Unity BiRP/URP/HDRP players, a packaged Unreal app), a session that
does not opt in is demoted the same way, with the same two guards, but **only while minimized** —
never for covered, cloaked, locked or display-off. `DXR_HIDDEN_DEMOTE_FORCE=off` is the kill
switch; an app that chains the opt-in struct with `demoteWhenHidden = XR_FALSE` opts out. The
first-party engine livelock risk above was checked on the Unreal plug-in: FOCUSED returned cleanly
after each minimize.

### D5 — Tier 4: the visible-3D-region mask for local-switching panels

On a DP whose zone caps report a grid larger than 1×1, the runtime publishes per screen the
**visible part of every 3D region**: a zones app's wish intersected with its window's uncovered
area, and a non-zones 3D app's uncovered client rect (an implicit wish), unioned across the windows
the vote serves (the service merges its clients). The existing implicit-mask rules still apply and
are the app's opt-out: Local2D layer rects are subtracted, a sticky `xrSubmitLocal3DZoneDXR` mask
overrides, and a session in 2D mode publishes an empty wish. On a 1×1-grid panel this collapses to
D2's boolean, so nothing changes on today's hardware. Verified with unit tests and a sim_display
mode that reports a zone grid; combining masks across vendor connections is the vendor's concern.

## Consequences

- One signal, four consumers: a fix to detection improves all of them.
- Tier 1 fixes the lens; tier 2 saves the weave and, on hybrid laptops, the cross-adapter copy and
  present (real but small); tier 3, once approved, saves the app's render cost for apps that opt in.
- Risks: window-capture sharing of a covered window freezes under tier 2 (documented, opt-out);
  tier 4 cannot be hardware-verified until a local-switching panel exists.
- Not solved: Wayland occlusion; workspace-controller policy; vendor-side mask combination.

## Review

Reviewed 2026-11-15 by an independent app-developer review: YELLOW overall, RED for tier 3 as
first written (default-on session-state demotion). Its findings are folded into D1–D5 above.

## Open questions

None. D4 (`XR_DXR_window_visibility`) was approved by David on 2026-11-15.

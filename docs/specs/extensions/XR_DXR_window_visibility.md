# XR_DXR_window_visibility

| Property | Value |
|----------|-------|
| Extension Name | `XR_DXR_window_visibility` |
| Spec Version | 1 |
| Type Values | `XR_TYPE_EVENT_DATA_WINDOW_VISIBILITY_CHANGED_DXR` (1004999380) · `XR_TYPE_SESSION_WINDOW_VISIBILITY_INFO_DXR` (1004999381) |
| Author | The DisplayXR Project |
| Platform | Windows (in-process D3D11, D3D12, OpenGL and Vulkan sessions, and the service's default policy over IPC). Other platforms advertise the extension and report the window as always visible. |
| Decision | ADR-062 (`docs/adr/ADR-062-window-visibility-drives-lens-weave-and-session-state.md`, #2678) D4 (tier 3), approved 2026-11-15 |

---

## 1. Overview

A switchable-lens 3D panel's lens is panel-wide, so a 3D app whose window nobody can see must not
hold the panel in 3D, and has no reason to weave or present. The runtime already judges every 3D
window off the hot path (ADR-062 D1) and acts on it without asking the app:

- **tier 1:** while the window is hidden on a 3D screen, that screen's lens wish is 2D;
- **tier 2:** while it is hidden on every screen it covers, its weave and present are skipped.

`XR_DXR_window_visibility` is **tier 3: telling the app.** It adds an event that says the window is
hidden or seen again, and why, and an opt-in that lets the runtime move a hidden session to
`XR_SESSION_STATE_SYNCHRONIZED` (`shouldRender = XR_FALSE`) so the app can stop rendering.

**Tiers 1 and 2 behave the same whether or not the extension is enabled.** The extension changes
only what the app is told and, when it opts in, its session state. It never changes the lens or
the weave.

## 2. The signal

**Hidden** means hidden on **every screen the window covers** (the aggregate verdict): a window
half on a visible 2D screen is visible. A window is hidden when

| reason | when | reported |
|---|---|---|
| `MINIMIZED` | its root window is minimized | at once |
| `CLOAKED` | it is cloaked (another virtual desktop, hidden by the shell) | at once |
| `WINDOW_HIDDEN` | its owner hid it after it had been shown | at once |
| `LOCKED` | the user session is locked | at once |
| `DISPLAY_OFF` | the display is off | at once |
| `COVERED` | opaque top-level windows above it cover all but a sliver (≈2% of its area) | after a ~1.5 s dwell, and not while another window is being moved or sized |

`VISIBLE` is reported at once when any meaningful part of the window can be seen again.

`NOT_SHOWN` is not hidden: it says the window **has not been shown yet**. A session may receive it
(`visible = XR_FALSE`) when the runtime first judges a window that is still hidden at startup; it is
followed by `VISIBLE` when the window is shown. It never demotes a session (§4).

What counts as an occluder (the taskbar does, click-through and non-opaque layered windows do not,
the app's own windows do not, and so on) is the tracker's rule in
`src/xrt/auxiliary/util/u_window_visibility.h`.

## 3. The event

```c
typedef enum XrWindowVisibilityReasonDXR {
    XR_WINDOW_VISIBILITY_REASON_VISIBLE_DXR = 0,
    XR_WINDOW_VISIBILITY_REASON_MINIMIZED_DXR = 1,
    XR_WINDOW_VISIBILITY_REASON_CLOAKED_DXR = 2,
    XR_WINDOW_VISIBILITY_REASON_COVERED_DXR = 3,
    XR_WINDOW_VISIBILITY_REASON_NOT_SHOWN_DXR = 4,
    XR_WINDOW_VISIBILITY_REASON_LOCKED_DXR = 5,
    XR_WINDOW_VISIBILITY_REASON_DISPLAY_OFF_DXR = 6,
    XR_WINDOW_VISIBILITY_REASON_WINDOW_HIDDEN_DXR = 7,
} XrWindowVisibilityReasonDXR;

typedef struct XrEventDataWindowVisibilityChangedDXR {
    XrStructureType             type;     // XR_TYPE_EVENT_DATA_WINDOW_VISIBILITY_CHANGED_DXR
    const void*                 next;
    XrSession                   session;
    XrBool32                    visible;  // XR_FALSE: hidden on every screen, or not shown yet
    XrWindowVisibilityReasonDXR reason;   // VISIBLE iff visible
    XrTime                      time;     // when the runtime judged the change
} XrEventDataWindowVisibilityChangedDXR;
```

- Queued only for sessions whose instance **enabled** the extension.
- Queued on every edge of the aggregate verdict, and when the reason of a hidden window changes
  (covered, then minimized). Never per frame.
- A session starts "visible": a session whose window is seen from the start receives no event.
- `time` is the tracker's transition time for an edge, or the time the runtime saw a reason change.

## 4. Opt-in demotion

```c
// Chained on XrSessionCreateInfo::next. Ignored unless the extension is enabled.
typedef struct XrSessionWindowVisibilityInfoDXR {
    XrStructureType  type;   // XR_TYPE_SESSION_WINDOW_VISIBILITY_INFO_DXR
    const void*      next;
    XrBool32         demoteWhenHidden;
} XrSessionWindowVisibilityInfoDXR;
```

With `demoteWhenHidden = XR_TRUE`, while the window is hidden the session moves
`FOCUSED → VISIBLE → SYNCHRONIZED` (both events in the same `xrPollEvent`), so `xrWaitFrame`
returns `shouldRender = XR_FALSE`. When the window is seen again it returns
`SYNCHRONIZED → VISIBLE → FOCUSED`. The app must keep its frame loop running
(`xrWaitFrame` / `xrBeginFrame` / `xrEndFrame`, zero layers is fine), as for any `SYNCHRONIZED`
session; a zero-layer frame is discarded, so the frame the runtime retains for tier 2 is the last
one the app rendered, and it is re-woven at once when the window is seen again.

In OpenXR `FOCUSED` implies `VISIBLE`, so a demotion always drops focus, and with it
`xrSyncActions`. Two guards protect input and startup:

1. **Never while the window is the OS foreground window.** A covered window that still has
   keyboard focus (e.g. a topmost window that does not take focus sits over it) keeps `FOCUSED`.
   The foreground fact is re-checked at every `xrPollEvent` while hidden: if the window becomes
   foreground while still covered the session is released; if it loses foreground while hidden
   it is demoted.
2. **Never before the window has been shown once** (`NOT_SHOWN`). An app that renders its first
   frame into a hidden window and then calls `ShowWindow` must not be held at `SYNCHRONIZED`
   waiting for a frame it will not render.

A session that does not opt in sees **no session-state change at all** from window visibility.
The opt-in, the guards and the state machine all run in the app's process (the state tracker),
in-process and over IPC alike.

### Why a session-create struct, not a window-binding flag

The opt-in is about the session's state, not about the window handoff, and it must work for
every session class:

- a `_hosted` session passes no window binding (the runtime creates the window), yet it is
  judged on that runtime window and gets the event and the opt-in like any other session;
- the binding struct is per platform (Win32, Cocoa, Xlib, Wayland, Android); one chained struct
  on `XrSessionCreateInfo` serves them all, so a platform that gains a tracker needs no new
  field;
- growing an existing binding struct is an ABI break for apps compiled against an older header.

## 5. Which sessions, which window

| session | window judged | event + opt-in |
|---|---|---|
| in-process `_handle` (D3D11 own legs or hub, D3D12, GL, Vulkan) | the app's window (its root) | yes |
| in-process `_hosted` | the window the runtime created for it | yes |
| in-process `_texture` | the app's window, when it passed one (its position anchor) | yes (no window: always visible) |
| IPC session, service default policy (no workspace controller) | the presenter's window (the app's, or the service window for a hosted IPC session); only the presenter is judged — the lease holder | yes |
| IPC session under a workspace controller | — (the controller owns the panel) | never hidden |
| present-owner (`XR_DXR_weave`) | — (own visibility rule) | never hidden |
| AppContainer (WebXR) sessions | — | never demoted |
| macOS, Linux, Android | — (no tracker yet) | never hidden |

A client that stops being the service's presenter after it was told "hidden" is told "visible",
so it is never left demoted.

## 6. Interaction with tiers 1 and 2

- Tier 1 (lens) and tier 2 (skipped weave) are driven by the same tracker and run whether or not
  the extension is enabled; their switches (`DXR_LENS_VISIBILITY`, `DXR_HIDDEN_SKIP_PRESENT`) do
  not change what this extension reports. Tier 3 uses the every-screen verdict regardless of the
  tier-2 switch.
- With `DXR_LENS_VISIBILITY=0` the tracker is not created, so the window always reads visible and
  no event is sent.
- A 3D request made while hidden is still answered with
  `XR_DISPLAY_MODE_DENIAL_REASON_WINDOW_HIDDEN_DXR` ([XR_DXR_display_info](XR_DXR_display_info.md)
  v26) and the lens edges are still reported with `XrEventDataHardwareDisplayStateChangedDXR`.

## 7. Test lever (not a feature)

`DXR_HIDDEN_DEMOTE_FORCE` (a persisted setting, read once per process in the app's process):
`off` (default) | `minimize` | `all`. It applies the demotion to sessions that did **not** enable
the extension: only for `MINIMIZED` with `minimize`, for any hidden reason with `all`. The two
guards still apply. One WARN at session create says it is on. It exists to test unmodified apps
against a possible future default ("a minimized app stops rendering"); it is not part of this
extension's contract.

## 8. Verifying it

`cube_handle_d3d11_win` is the verify vehicle: `DISPLAYXR_WINDOW_VISIBILITY=1` enables the
extension, `=demote` also opts in. The app logs every `shouldRender` edge; the runtime logs each
event (`OXR EVENT: Window visibility changed`) and each demotion edge (`ADR-062: session demoted`
/ `released`).

## Version history

| Version | Date | Author | Changes |
|---|---|---|---|
| 1 | 2026-11-15 | David Fattal | Initial version (ADR-062 D4): the event, the opt-in demotion and its two guards. |

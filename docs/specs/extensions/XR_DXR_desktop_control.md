# XR_DXR_desktop_control

| Field | Value |
|---|---|
| **Name** | `XR_DXR_desktop_control` |
| **Spec version** | 1 |
| **Status** | In development (ADR-056) |
| **Header** | [`XR_DXR_desktop_control.h`](../../../src/external/openxr_includes/openxr/XR_DXR_desktop_control.h) |
| **Decision** | [ADR-056](../../adr/ADR-056-desktop-control-runs-in-the-app.md) |
| **Code** | `src/xrt/auxiliary/desktop/` (common layer + one OS backend), `src/xrt/state_trackers/oxr/oxr_desktop_control.c` |

## 1. Overview

An app's agent observes and operates the user's desktop — windows, pointer, UI text,
keyboard and pointer input — through one API on Windows, macOS and GNOME Wayland. The
runtime executes everything **inside the calling app's process**, through the OS's own
consent path, so the permission is the app's (ADR-056). The header documents every type
and function; this spec states the behaviour.

## 2. Architecture

```
app ─ xr*DesktopDXR ─▶ oxr_desktop_control.c (handles, events)
                         │
                         ▼
               u_desktop_control.c  ── the common safety layer (§5), one per session
                 │        │      ▲
                 │        │      └─ u_desktop_policy.c: the user's store + audit log (§6)
                 │        ▼
                 │   worker thread: actions in submission order
                 ▼
           struct u_desktop_backend (u_desktop.h) — exactly one per build:
           u_desktop_win32.cpp │ u_desktop_macos.m │ u_desktop_gnome.c │ u_desktop_none.c
```

A backend is mechanism only. Everything in §5 and §6 is implemented once, in the common
layer, and is identical on every OS.

## 3. Consent (state machine)

`xrGetDesktopControlStateDXR` reports, and `XrEventDataDesktopControlStateChangedDXR`
announces, one of: `UNSUPPORTED`, `NOT_REQUESTED`, `PENDING`, `GRANTED`, `DENIED`,
`DISABLED_BY_USER`, `SUSPENDED`. The state is evaluated in this order, first hit wins:

| # | Check | State |
|---|---|---|
| 1 | No backend for this OS / desktop | `UNSUPPORTED` |
| 2 | `DXR_DESKTOP_CONTROL=0` in the process environment (admin / dev off switch) | `UNSUPPORTED` |
| 3 | The user's kill switch is on (store `suspended`) | `SUSPENDED` |
| 4 | The store has `deny` for this executable | `DISABLED_BY_USER` |
| 5 | The screen is locked | `SUSPENDED` |
| 6 | Never requested in this session | `NOT_REQUESTED` |
| 7 | A request is in flight | `PENDING` |
| 8 | Backend `remembers_in_store` (Windows) and the store has `allow` for this executable | `GRANTED` |
| 9 | The backend / OS reports granted | `GRANTED` |
| 10 | Otherwise | `DENIED` |

`xrRequestDesktopControlDXR` resolves rows 1–5 without prompting, then calls the backend's
`request_access`: the GNOME portal dialog, the macOS Accessibility (and Screen Recording)
prompt, or on Windows DisplayXR's own dialog — *"<app> wants to control your computer:
see your windows, type and click in other apps. Allow / Don't allow"* plus the app's
`reason`. Windows answers are written to the store (`allow` / `deny`); the OS remembers its
own elsewhere. A refused request is not retried by the runtime; the app may call again on a
new user gesture.

States 3–5 are re-evaluated at every call and before every action, and a change emits the
state event, so a kill switch or a lock takes effect on the very next operation.

## 4. Operations

All operations — observation included — require `GRANTED`, else
`XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR`. A capability the backend lacks returns
`XR_ERROR_FEATURE_UNSUPPORTED` (observation) or completes with it (actions).

- **Observation** (`xrEnumerateDesktopWindowsDXR`, `xrGetDesktopPointerDXR`,
  `xrPickDesktopDXR`, `xrGetDesktopTextAtDXR`) is synchronous, never waits on an action in
  flight, and is fast enough for a frame loop (text-at may take tens of ms).
- **Actions** (`xrSubmitDesktopActionDXR`) are validated at submit (type known, required
  fields present, `targetWindowId` non-zero, `text`/`keys` non-empty, chord parses) — a bad
  action returns `XR_ERROR_VALIDATION_FAILURE` and no id. Valid actions queue (64 per
  session; beyond that `XR_ERROR_LIMIT_REACHED`) and run in order on the session's worker;
  each ends with exactly one `XrEventDataDesktopActionCompletedDXR`. Destroying the session
  drops queued actions without events (the session's event queue is gone); an action
  already executing is allowed to finish its current backend call first.

## 5. The safety layer (every action, in this order)

1. **State** is `GRANTED` (§3 re-evaluated) — else `NOT_GRANTED` / `REFUSED` ("kill
   switch", "screen locked", "disabled by the user").
2. **Never fight the user.** Input actions (text, keys, pointer, scroll) — and `ACTIVATE` —
   wait until the user has been idle for `idle_ms` (store, default 600 ms), polling the
   backend's `user_idle_ms`; input the runtime itself injected does not count (the common
   layer remembers when its own last injection ended and ignores activity up to 150 ms after
   it). Waits at most 10 s, then `REFUSED` "the user is active".
3. **Target exists.** The target window is in the backend's window list — else
   `WINDOW_LOST`.
4. **Target verification** for input actions, immediately before delivery:
   - pointer actions: the backend's pick at `point` is a `WINDOW` whose id is the target or
     whose `parent_id` is the target (its dialog) — else `REFUSED` "target is not under the
     point (found <app>: <title>)". `DRAG` verifies the start point.
   - text / keys: the backend's focused window is the target (or its dialog) — else
     `REFUSED` "target window does not have focus — ACTIVATE it first".
   - After `pointer_move`, the backend confirms the pointer arrived; the common layer
     re-picks before pressing a button.
5. **Execute** through the backend.
6. **Audit** (§6.2) — every action, whatever its result.

Window actions (`ACTIVATE`, `MOVE_RESIZE`, `MINIMIZE`, `RESTORE`, `MAXIMIZE`, `CLOSE`) skip
step 4; step 2 applies to `ACTIVATE` only (moving focus under a typing user is the theft
the rule exists for).

Chords are parsed by `u_desktop_chord_parse()`: `+`-separated, case-insensitive; modifiers
`ctrl`/`control`, `shift`, `alt`/`option`, `super`/`win`/`cmd`/`command`/`meta`; at most one
key from the canonical list in `u_desktop.h`.

## 6. The store and the audit log

### 6.1 Store

One JSON file per user, read on every evaluation when its mtime changed:

| OS | path |
|---|---|
| Windows | `%LOCALAPPDATA%\DisplayXR\desktop_control.json` |
| macOS | `~/Library/Application Support/DisplayXR/desktop_control.json` |
| Linux | `$XDG_CONFIG_HOME/displayxr/desktop_control.json` (default `~/.config/...`) |

```json
{ "version": 1, "suspended": false, "idle_ms": 600,
  "apps": { "<executable full path>": "allow" | "deny" } }
```

Writes are atomic (temp file + rename). A missing or unreadable file means defaults
(nothing suspended, nobody allowed or denied).

### 6.2 Audit log

JSON lines next to the store, `desktop_control_audit.jsonl` (Linux: `$XDG_STATE_HOME/displayxr/`,
default `~/.local/state/displayxr/`), rotated to `.1` at 1 MiB:

```json
{"t":"2026-10-16T18:02:11Z","exe":"/opt/3dluma/3DLuma","pid":4242,"action":"type_text",
 "target":{"app":"gedit","title":"notes.txt"},"chars":12,"result":"ok"}
```

Typed text is **never** logged — only its length. Consent decisions (`request`, `granted`,
`denied`) and kill-switch transitions are logged too.

### 6.3 CLI

`displayxr-cli desktop status | stop | resume | allow <exe> | deny <exe> | forget <exe> |
audit [--tail N] [--json]` reads and writes the same store and log. `stop` / `resume` flip
`suspended` (the kill switch); a running session sees it on its next call.

## 7. Backends

| | Windows | macOS | GNOME Wayland |
|---|---|---|---|
| windows / z-order | `EnumWindows` (visible, not cloaked, not tool windows) | `CGWindowListCopyWindowInfo` (layer 0) | extension `GetAllWindows` (v16) |
| pointer / pick | `GetCursorPos`, `WindowFromPoint` → `GA_ROOT` | `NSEvent mouseLocation`, `CGWindowList` hit test | extension `GetPointer` / `GetWindowAt` |
| text at point | UI Automation `ElementFromPoint` | AX `AXUIElementCopyElementAtPosition` | AT-SPI `GetAccessibleAtPoint` |
| window ops | `SetForegroundWindow` (+ attach-input), `SetWindowPos`, `ShowWindow`, `WM_CLOSE` | AX raise / position / size / minimized / close button | portal input as the user would: Super+click to activate, mutter keybindings, Super+drag with read-back |
| text | `SendInput` `KEYEVENTF_UNICODE` | `CGEventKeyboardSetUnicodeString` | portal clipboard + Ctrl+V (Ctrl+Shift+V in terminals) |
| keys / pointer | `SendInput` | `CGEvent` | libei (from the portal's `ConnectToEIS`) |
| consent | DisplayXR dialog (store) | `AXIsProcessTrustedWithOptions` + `CGRequestScreenCaptureAccess` | RemoteDesktop portal, `persist_mode` 2, restore token kept in memory per session |
| lock | `OpenInputDesktop` fails / WTS session locked | `CGSessionCopyCurrentDictionary` `CGSSessionScreenIsLocked` | `org.gnome.ScreenSaver.GetActive` |
| idle | `GetLastInputInfo` | `CGEventSourceSecondsSinceLastEventType` | `org.gnome.Mutter.IdleMonitor.GetIdletime` |

## 8. Not in v1

Window capture (screenshots), a DisplayXR-drawn in-use indicator on Windows / macOS and a
global kill-switch hotkey (both planned in the service tray, as the stereo camera's §7.3–7.4),
X11 sessions, KDE window listing, Flatpak-sandboxed apps on GNOME.

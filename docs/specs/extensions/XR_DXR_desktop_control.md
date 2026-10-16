# XR_DXR_desktop_control

| Field | Value |
|---|---|
| **Name** | `XR_DXR_desktop_control` |
| **Spec version** | 1 |
| **Status** | In development (ADR-056) — common layer, OpenXR entry points, `displayxr-cli desktop`, the fake backend and `desktop_control_probe` implemented; OS backends in progress (Windows / macOS / GNOME). Until a backend lands, its OS reports `UNSUPPORTED`. |
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
`XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR`. `xrGetDesktopControlStateDXR` is the one exception:
it always succeeds for a valid session. A capability the backend lacks returns
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
2. **Never fight the user.** EVERY action (window ops included: on GNOME they are input
   gestures, and everywhere they move focus) waits until the user has been idle for
   `idle_ms` (store, default 600 ms), polling the backend's `user_idle_ms`. Input the runtime
   itself injected does not count — but only input whose OS timestamp falls inside one of
   the runtime's own injection windows `[start, end + 150 ms]`; activity after that window is
   the user's, however soon the next action follows. (Known limit: a user keystroke landing
   *during* one of our injections is indistinguishable from ours on these OSes.) Waits at
   most 10 s, then `REFUSED` "the user is active".
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
step 4's focus / pick check in the common layer; a backend that performs them through
input (GNOME) re-picks before every gesture under the input guard.

**Delivery guarantee.** Verification (step 4) is repeated by the backend immediately before
every injection batch — each text chunk, the paste keystroke, each Return/Tab, each chord
key-down, each button press, each scroll burst — together with the kill switch and the lock
(the input guard, `u_desktop.h`). A check that fails mid-action stops it with `REFUSED`
("focus moved", "kill switch", "screen locked"); what was already injected stays, and every
pressed key and button is released. Slow setup (opening / restoring the GNOME portal
session) happens before step 4, never between it and the injection.

**Revocation.** A backend that finds the OS permission gone returns NOT_GRANTED; the state
becomes `DENIED` (event) and every further action is refused without reaching the OS.

**Privacy.** Typed text, characters from chords (incl. AltGr / Option combinations, which
produce characters), clipboard contents and UI text read from other apps never reach a
log, an audit record or a message — validation errors name the problem, not the input.

Chords are parsed by `u_desktop_chord_parse()`: `+`-separated, case-insensitive; modifiers
`ctrl`/`control`, `shift`, `alt`/`option`, `super`/`win`/`cmd`/`command`/`meta`; at most one
key from the canonical list in `u_desktop.h`. Letters, digits and punctuation name the character on the user's **active keyboard layout** (as every OS matches shortcuts: `ctrl+z` presses the key labelled Z on AZERTY too); named keys are layout-independent.

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

Writes are atomic (temp file + rename) and serialised across processes by an OS file lock
on `desktop_control.json.lock` (`flock` / `LockFileEx`), with the read-modify-write done
under the lock — so `displayxr-cli desktop stop` can never be overwritten by an app's
concurrent `allow`. A **missing** file means defaults (nothing suspended, nobody allowed or
denied). A file that **exists but cannot be parsed** never means defaults — that would
silently turn off the kill switch and every `deny`: the last good policy read in this
process is kept, and with none, the state is `SUSPENDED` ("the desktop-control settings
file is unreadable") until the file is fixed or removed.

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

## 9. Implementation notes (v1)

Decisions the sections above leave open, as implemented in `u_desktop_control.c`,
`u_desktop_policy.c` and `oxr_desktop_control.c`:

**Consent (§3).**
- `xrRequestDesktopControlDXR` returns `XR_SUCCESS` and **always** announces the outcome with a
  state event, even when it equals the previous state: rows 1–5 announce the blocking state
  (and do not count as "requested" — after a kill switch is lifted the state is
  `NOT_REQUESTED`, not a stale grant); a remembered store grant announces `GRANTED`; a prompt
  announces `PENDING` and then its answer. A request while `PENDING` is a no-op (never two prompts).
- Row 8 vs row 9: for a `remembers_in_store` backend (Windows) the **store is the only grant** —
  the backend's `access_status` is never consulted, so `forget` revokes at once. The in-memory
  answer counts only if writing it to the store failed.
- Row 9: `access_status() == GRANTED`, or `UNKNOWN` (the OS cannot tell without prompting) and
  this session's request was answered `GRANTED`. An explicit `DENIED` (revoked) wins.
- On Windows a "Don't allow" is written as `deny`, so the state is then `DISABLED_BY_USER`
  (row 4), not `DENIED`, until `displayxr-cli desktop forget <exe>`.
- `DXR_DESKTOP_CONTROL=0` is read once, when the session's desktop-control object is created
  (first call on the session).
- `DXR_DESKTOP_BACKEND=fake` selects an in-memory desktop (`u_desktop_fake.h`) on every OS — for
  tests, CI and dry runs; `none` selects no backend. Neither is a user setting.

**Validation (§4).** Besides the listed rules: `text` must be valid UTF-8 and at most 65 536
bytes; `clickCount` 0–3 (0 = 1); `button` LEFT/RIGHT/MIDDLE; `SCROLL` needs a non-zero
`scrollX`/`scrollY`, each within ±100 notches; `MOVE_RESIZE` width/height ≥ 0 (0 = keep size).
`xrEnumerateDesktopWindowsDXR` requires `windows[i].type` to be set (OpenXR's usual array rule);
`XrDesktopPickDXR.window` is pure output.

**Text at a point.** At most 16 KiB are returned (cut on a code point).

**Safety layer (§5).**
- Before step 2 an action whose capability the backend lacks (`WINDOW_ACTIONS` / `INPUT`)
  completes with `XR_ERROR_FEATURE_UNSUPPORTED`. Likewise a pointer action on a backend without a
  hit test, or a keyboard action on one that cannot report focus: never delivered unverified.
- Step 2: the kill switch, a lock or a deny that lands during the idle wait ends it at once
  (state re-checked on every poll; default poll 50 ms). An unknown idle time (backend returns
  false) does not wait. `idle_ms` 0 disables the wait; values above 10 000 are clamped.
- "Our own input" is marked after every injected input call (text, keys, pointer move, button,
  scroll). Window operations are **not** marked: on most OSes they are not input, and marking
  them would hide real user activity for 150 ms; where a backend performs them as simulated
  input (GNOME), the next input action merely waits `idle_ms`.
- `CLICK`, `DRAG` and `SCROLL` move the pointer to `point` first, then re-pick, then press.
  A pressed button is always released, whatever happens next.
- Exact messages: `"kill switch"`, `"screen locked"`, `"disabled by the user"`,
  `"the user is active"`, `"target is not under the point (found <app>: <title>)"` (or
  `found the desktop` / `found the shell` / `found nothing: outside every monitor`),
  `"target window does not have focus — ACTIVATE it first"`,
  `"the target window no longer exists"`; success is `"ok"`. A state that is not
  granted for any other reason completes with `XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR`
  `"desktop control is not granted (state <STATE>)"`.
- Backend errors map to: `UNSUPPORTED` → `XR_ERROR_FEATURE_UNSUPPORTED`, `NOT_GRANTED` →
  `…_NOT_GRANTED_DXR` (and the state is re-announced), `WINDOW_LOST` → `…_WINDOW_LOST_DXR`,
  `REFUSED` → `…_REFUSED_DXR`, `TIMEOUT` / `FAILED` → `XR_ERROR_RUNTIME_FAILURE`.
- On session destroy an action mid-flight finishes its current backend call and stops (audited
  as `aborted`, no event).

**Store and audit (§6).**
- The store is re-read when its stamp changes: mtime (with nanoseconds), size and file identity
  (inode / creation time) — so two writes within one mtime tick are still seen.
- `DXR_DESKTOP_CONTROL_DIR` overrides both the store and the audit directory.
- Audit records: `t`, `exe`, `pid`, `action`, `target` (`id`, `app`, `title`), `x`/`y` for
  pointer actions, `chars` (code points) for `type_text`, `result`, `message` on failure,
  `reason` on `request`, and `by` + `subject` for store changes (`allow` / `deny` / `forget` /
  `suspended` / `resumed`, written by whoever changed the store: `cli`, `prompt`).
- A key chord is logged only when it is a shortcut (`ctrl`, `alt` or `super` held): a bare or
  shift-only key is a typed character, and typed text never reaches the log.
- The audit record type has no text field at all, so the rule cannot be broken by a caller.

**Probe.** `test_apps/probes/desktop_control_probe` (headless, every desktop OS) exposes every
call and runs self-verifying scripts; `e2e_fake.txt` is the fake-backend end-to-end run.

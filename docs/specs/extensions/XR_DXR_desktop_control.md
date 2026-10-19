# XR_DXR_desktop_control

| Field | Value |
|---|---|
| **Name** | `XR_DXR_desktop_control` |
| **Spec version** | 1 |
| **Status** | In development (ADR-056) — common layer, OpenXR entry points, `displayxr-cli desktop`, the fake backend and `desktop_control_probe` implemented; OS backends in progress (Windows / macOS / GNOME). Until a backend lands, its OS reports `UNSUPPORTED`. Screen capture is **proposed** for spec version 2 (§10, ADR-056 Amendment 1), and nothing of it is implemented. |
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
   `idle_ms` (store, default 600 ms), polling the backend's `user_idle_ms`. The wait runs
   AFTER the backend's slow setup (`prepare_input`, e.g. restoring the GNOME portal session,
   0.3–9 s), so a user who starts typing during that setup is still waited for. Input the
   runtime itself injected does not count — but only input whose OS timestamp falls inside
   one of the runtime's own injection windows `[start, end + 30 ms]` (the 30 ms grace absorbs
   the OS processing our own events late); activity after that window is the user's, however
   soon the next action follows. The decision is taken only once the last injection's grace
   window has closed (an action following another by < 30 ms waits out the rest of it), so a
   user who starts typing right after our injection is seen as active rather than absorbed
   into our grace. **Residual window:** user input landing *during* one of our injections, or
   within 30 ms after it ends, is indistinguishable from ours on these OSes and is not waited
   for. The opposite error is safe: a late-processed event of ours mistaken for the user only
   costs one idle wait. Waits at most 10 s, then `REFUSED` "the user is active".
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
session) happens before step 2 (so the idle wait comes after it) and never between step 4
and the injection.

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

macOS notes: CGWindowList's on-screen list drops a window the moment it is minimized, so
minimized windows come from a background AX snapshot (`u_desktop_mini_cache`). The backend's
own window ops pin their outcome into it at once (a `MINIMIZE` lists the window immediately; a
`RESTORE` / `ACTIVATE` / `MAXIMIZE` / `CLOSE` removes it), a snapshot that contradicts a pin
younger than 5 s does not undo it, and `list_windows` called off the main thread (the worker's
"target exists" check) refreshes a snapshot older than 250 ms synchronously — so a window just
minimized, by the agent or by the user, is never a false `WINDOW_LOST`. The main thread never
blocks on it.

GNOME notes: text reaches the target through the clipboard (the portal has no "commit this
string" path that survives an IME), so on GNOME **the agent's typed text passes through the
clipboard, where clipboard managers and clipboard history can see and keep it**; the previous
clipboard text is offered again afterwards, best effort, while the portal session lives. The
paste is sent only after the portal confirms the session owns the selection (otherwise
`RUNTIME_FAILURE`, "the portal did not make this session the clipboard owner — not pasting":
nothing is pasted and the user's clipboard is kept — observed on GNOME 50.1 in ~8% of types
racing a user copy, when the portal never reported this session as the owner), and only if it
still does right before the paste key: ownership is tracked from before the previous clipboard
text is read, through taking the selection, to the moment after Ctrl is down and before V is
sent. A user who copied anywhere in that span gets their own clipboard left alone (never
overwritten by the agent's text, nor later by the restored previous text) and the action is
`REFUSED` ("the clipboard changed hands (the user copied something); nothing was pasted").
One window is the exception: a copy landing in the ~1.5 ms between reading the previous text and
the portal applying the agent's `SetSelection` is replaced by that `SetSelection` — the user's
copied data is gone, and "the clipboard is the user's afterwards" cannot hold. The action is
still `REFUSED`, its message says the copy is gone and must be made again, the agent's text is
never handed to any reader, and the selection is released (`SetSelection` with no `mime_types`:
mutter unsets the session's source), so the clipboard is left empty rather than owned and
silent. More generally the session never keeps a selection it will not serve: when there is no
previous text to offer again, it is released too (mutter's own clipboard keeper may then keep the
agent's text as the clipboard, as it would after the session closes).
Losing the selection after the paste key completes `RUNTIME_FAILURE` — "… the agent's text was
never read — whatever the window pasted was the user's clipboard" when the agent's text was
never served, else "… whether the text arrived is unknown" unless the agent's text was served
**and** the focused field changed — and the previous text is never offered again over the
user's. A residual race remains: a copy whose ownership signal reaches this process only after
V was sent is seen after the key (one of those two failures, never a plain success unless the
agent's text was read). Delivery: when the focused window exposes a focused text field
over AT-SPI, the action succeeds only if that field changed; otherwise a read of the agent's
text after the keystroke is the only evidence, and **a clipboard manager reads too** — so the
action succeeds with the message `"ok (unverified: …)"` (same `XR_SUCCESS`; no new result
code). No read at all fails ("the focused window did not ask for the pasted text").

## 8. Not in v1

Window capture (screenshots) — now **proposed** for spec version 2, see §10. A DisplayXR-drawn in-use indicator on Windows / macOS and a
global kill-switch hotkey (both planned in the service tray, as the stereo camera's §7.3–7.4),
X11 sessions, KDE window listing, Flatpak-sandboxed apps on GNOME.

## 9. Implementation notes (contract v2)

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
- Revocation: once any backend call returns `NOT_GRANTED`, the session is `DENIED` (row 10,
  event) whatever `access_status` says afterwards, and every queued and later action is refused
  without reaching the backend — until the app calls `xrRequestDesktopControlDXR` again.
- An unreadable store (§6.1) sits at row 3: `SUSPENDED`, refusals read
  `"the desktop-control settings file is unreadable"`, and a request never prompts.
- On Windows a "Don't allow" is written as `deny`, so the state is then `DISABLED_BY_USER`
  (row 4), not `DENIED`, until `displayxr-cli desktop forget <exe>`.
- `DXR_DESKTOP_CONTROL=0` is read once, when the session's desktop-control object is created
  (first call on the session).
- `DXR_DESKTOP_BACKEND=fake` selects an in-memory desktop (`u_desktop_fake.h`) on every OS — for
  tests, CI and dry runs; `none` selects no backend. Neither is a user setting.

**Validation (§4).** Besides the listed rules: `text` must be valid UTF-8 and at most 65 536
bytes; `clickCount` 0–3 (0 = 1); `button` LEFT/RIGHT/MIDDLE; `SCROLL` needs a non-zero
`scrollX`/`scrollY`, each within ±100 notches; `MOVE_RESIZE` width/height ≥ 0 (0 = keep size);
`point`, `point2` and the rect's offset within ±1 000 000 and its extent ≤ 1 000 000, for every
action type. Validation messages never echo the input (`"keys is not a valid chord"`).
`xrEnumerateDesktopWindowsDXR` requires `windows[i].type` to be set (OpenXR's usual array rule);
`XrDesktopPickDXR.window` is pure output.

**Text at a point.** At most 16 KiB are returned. A backend that fills the buffer may cut a
code point; the common layer drops the incomplete tail (`u_desktop_utf8_trim_partial`).

**Safety layer (§5).**
- Before step 2 an action whose capability the backend lacks (`WINDOW_ACTIONS` / `INPUT`)
  completes with `XR_ERROR_FEATURE_UNSUPPORTED`. Likewise a pointer action on a backend without a
  hit test, or a keyboard action on one that cannot report focus: never delivered unverified.
- Step 2 applies to every action. The kill switch, a lock or a deny that lands during the
  idle wait ends it at once (state re-checked on every poll; default poll 50 ms). An unknown
  idle time (backend returns false) does not wait. `idle_ms` 0 disables the wait; values above
  10 000 are clamped.
- Our own injections: every backend action call (window ops included) records its window
  `[start, end]`; the last 32 are kept. Input whose OS timestamp (`now − idle_ms`, 1 ms rounding
  allowed) falls in `[start, end + 30 ms]` is ours — **but the decision waits until that window
  has closed**: a user who keeps typing then shows a newer timestamp outside it, so back-to-back
  actions cannot chain past an active user through each other's grace windows. Cost: an action
  that follows another by less than 30 ms waits for the rest of the grace window. **Residual
  window, stated plainly:** user input during one of our injections or within 30 ms after it
  ends is indistinguishable from ours (a user keystroke at +50 ms is seen and waited for). The
  failure direction of the grace is safe: our own synthetic event processed later than 30 ms
  is taken for the user and costs one idle wait, never an injection into an active user.
- Before step 2 (right after step 1 and the capability check) the backend's `prepare_input`
  (if any) runs, so slow setup (the GNOME portal: 0.3–9 s) happens before the idle wait — a
  user who starts typing during it is still waited for — and before verification, never
  between it and the injection.
- Step 5 runs under the input guard (`u_desktop.h`): `still_allowed` re-reads the store stamp
  and checks an unreadable store, the kill switch, the deny, `is_locked`, revocation and session
  destroy, and announces a state change it finds. A stop it causes completes with that reason
  (`"kill switch"`, `"screen locked"`, `"disabled by the user"`,
  `"the desktop-control settings file is unreadable"`); a stop at destroy is not reported.
- `CLICK`, `DRAG` and `SCROLL` move the pointer to `point` first, then re-pick, then press —
  the press and the scroll carry the verified point. A pressed button is always released,
  whatever happens next (a drag releases at `point2` if the move got there, else at `point`).
  A `DRAG` re-asks the guard after its press: if the kill switch, a lock, a deny or a
  revocation arrived in between, it releases at `point` (never moving to `point2`) and
  completes `REFUSED` with that reason.
- Exact messages: `"kill switch"`, `"screen locked"`, `"disabled by the user"`,
  `"the user is active"`, `"target is not under the point (found <app>: <title>)"` (or
  `found the desktop` / `found the shell` / `found nothing: outside every monitor`),
  `"target window does not have focus — ACTIVATE it first"`,
  `"the target window no longer exists"`; success is `"ok"`, or `"ok (unverified: <reason>)"`
when the backend delivered but could not confirm the effect (today: GNOME text with no
accessible text field). A state that is not
  granted for any other reason completes with `XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR`
  `"desktop control is not granted (state <STATE>)"`.
- Backend errors map to: `UNSUPPORTED` → `XR_ERROR_FEATURE_UNSUPPORTED`, `NOT_GRANTED` →
  `…_NOT_GRANTED_DXR` (and the state becomes `DENIED`, see Revocation), `WINDOW_LOST` →
  `…_WINDOW_LOST_DXR`, `REFUSED` → `…_REFUSED_DXR`, `TIMEOUT` / `FAILED` →
  `XR_ERROR_RUNTIME_FAILURE`.
- **Privacy:** for `TYPE_TEXT` and `KEY_CHORD` a backend's own message is never passed on.
  Every outcome maps to a fixed string: `REFUSED` → `"focus moved"` (or the guard's reason),
  `NOT_GRANTED` → `"the OS refused: permission missing or revoked"`, `UNSUPPORTED` →
  `"this desktop backend cannot deliver this input"`, `WINDOW_LOST` →
  `"the target window no longer exists"`, anything else → `"the input could not be delivered"`.
- On session destroy an action mid-flight finishes its current backend call and stops (audited
  as `aborted`, no event).

**Store and audit (§6).**
- The store is re-read when its stamp changes: mtime (with nanoseconds), size and file identity
  (inode / creation time) — so two writes within one mtime tick are still seen.
- An existing file that cannot be read or parsed: the last good policy read in this process is
  kept; with none, everything is `SUSPENDED` (above). Writers never turn it into defaults:
  `allow` / `deny` / `forget` / `resume` fail with a message naming the file; `stop` (the kill
  switch must always work) moves it to `desktop_control.json.corrupt` and writes a fresh
  suspended store. `displayxr-cli desktop status` reports an unreadable store.
- Every read-modify-write holds an exclusive OS lock on `desktop_control.json.lock` (`flock` /
  `LockFileEx`) and re-reads the file under it. If the lock cannot be taken the write is
  refused (it never writes unlocked) — except `stop`, which proceeds unlocked: the kill switch
  must always land.
- Windows: readers open the store with `FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE`
  (`CreateFileW`, not `_wfopen`), so an app reading the store never blocks the writer's atomic
  rename; the rename is still retried for up to ~1 s on a sharing violation / access denied
  (antivirus, indexers). `displayxr-cli desktop stop` exits non-zero with "could not save the
  kill switch — is the store locked?" if it still fails.
- `DXR_DESKTOP_CONTROL_DIR` overrides both the store and the audit directory; the runtime logs a
  WARN once per process when it does, and `desktop status` prints the override and the store
  path in use.
- Audit records: `t`, `exe`, `pid`, `action`, `target` (`id`, `app`, `title`), `x`/`y` for
  pointer actions, `chars` (code points) for `type_text`, `result`, `message` on failure,
  `reason` on `request`, and `by` + `subject` for store changes (`allow` / `deny` / `forget` /
  `suspended` / `resumed`, written by whoever changed the store: `cli`, `prompt`).
- A key chord is logged only when it is a shortcut: `ctrl` or `super` held, and not `ctrl+alt`
  (AltGr on Windows / Linux). A bare, shift-only, alt-only (macOS Option) or AltGr chord produces
  a character, and typed text never reaches the log.
- An action refused at submit because the state is not granted is audited once per state
  (`result: "not_granted"`); an unknown action type is audited as `"unknown"` — also while
  `GRANTED`, where it is refused with `XR_ERROR_VALIDATION_FAILURE` (`result: "invalid"`, one
  record per submit).
- The audit record type has no text field at all, so the rule cannot be broken by a caller.

**Lifetime.** The context handed to `request_access` is a small refcounted block (one
reference for the session object, one per request in flight) that outlives the session object.
Destroy sets `dying`, destroys the backend (which must deliver or cancel an owed answer first),
then detaches the block, so an answer arriving later from a backend that broke that contract
is dropped instead of touching freed memory.

**Probe.** `test_apps/probes/desktop_control_probe` (headless, every desktop OS) exposes every
call and runs self-verifying scripts; `e2e_fake.txt` is the fake-backend end-to-end run.

## 10. Screen capture (PROPOSED — spec version 2)

> **PROPOSED. Not implemented, not in the header.** This section specifies a planned
> `XR_DXR_desktop_control_SPEC_VERSION 2`. Nothing in it is available until the header
> declares version 2. The decision record is
> [ADR-056 Amendment 1](../../adr/ADR-056-desktop-control-runs-in-the-app.md#amendment-1--screen-capture-is-its-own-scope-in-the-same-session-proposed-2026-10-19),
> and its design decisions were DECIDED by David on 2026-10-19 (listed in §10.8). Names and
> numeric values may still change when the header lands. Sections 1–9 describe
> version 1 and are unchanged by this one.

### 10.1 Additions to the header (proposed)

Structure types use the four remaining values of this extension's block (1004999400–409; see
`src/external/openxr_includes/openxr/README.md`, which is updated when the header lands):

```c
#define XR_DXR_desktop_control_SPEC_VERSION 2

#define XR_TYPE_DESKTOP_CAPTURE_INFO_DXR                      ((XrStructureType)1004999406)
#define XR_TYPE_EVENT_DATA_DESKTOP_CAPTURE_COMPLETED_DXR      ((XrStructureType)1004999407)
#define XR_TYPE_DESKTOP_CONTROL_REQUEST_SCOPE_DXR             ((XrStructureType)1004999408)
#define XR_TYPE_EVENT_DATA_DESKTOP_CAPTURE_STATE_CHANGED_DXR  ((XrStructureType)1004999409)

// The backend can capture screen pixels (and the platform has the libraries for it).
static const XrDesktopControlCapabilityFlagsDXR XR_DESKTOP_CONTROL_CAPABILITY_CAPTURE_BIT_DXR = 0x00000040;

typedef XrFlags64 XrDesktopControlScopeFlagsDXR;
//! Everything in spec version 1: observation, window actions, input.
static const XrDesktopControlScopeFlagsDXR XR_DESKTOP_CONTROL_SCOPE_CONTROL_BIT_DXR = 0x00000001;
//! Screen pixels (§10).
static const XrDesktopControlScopeFlagsDXR XR_DESKTOP_CONTROL_SCOPE_CAPTURE_BIT_DXR = 0x00000002;

//! Chained on XrDesktopControlRequestInfoDXR::next. Absent = CONTROL only (version 1 behaviour).
typedef struct XrDesktopControlRequestScopeDXR {
    XrStructureType                type;   //!< XR_TYPE_DESKTOP_CONTROL_REQUEST_SCOPE_DXR
    const void* XR_MAY_ALIAS       next;
    XrDesktopControlScopeFlagsDXR  scopes; //!< non-zero
} XrDesktopControlRequestScopeDXR;

typedef enum XrDesktopCaptureTargetDXR {
    XR_DESKTOP_CAPTURE_TARGET_DESKTOP_DXR = 0, //!< the union of every monitor
    XR_DESKTOP_CAPTURE_TARGET_REGION_DXR  = 1, //!< `region`, desktop coordinates
    XR_DESKTOP_CAPTURE_TARGET_WINDOW_DXR  = 2, //!< `windowId`'s frame as it is on screen
    XR_DESKTOP_CAPTURE_TARGET_MAX_ENUM_DXR = 0x7FFFFFFF
} XrDesktopCaptureTargetDXR;

typedef struct XrDesktopCaptureInfoDXR {
    XrStructureType            type;      //!< XR_TYPE_DESKTOP_CAPTURE_INFO_DXR
    const void* XR_MAY_ALIAS   next;
    XrDesktopCaptureTargetDXR  target;
    uint64_t                   windowId;  //!< WINDOW
    XrRect2Di                  region;    //!< REGION
    uint32_t                   maxWidth;  //!< 1..8192: the image is scaled down to fit, never up
    uint32_t                   maxHeight; //!< 1..8192
} XrDesktopCaptureInfoDXR;

typedef struct XrEventDataDesktopCaptureCompletedDXR {
    XrStructureType            type;       //!< XR_TYPE_EVENT_DATA_DESKTOP_CAPTURE_COMPLETED_DXR
    const void* XR_MAY_ALIAS   next;
    XrSession                  session;
    uint64_t                   captureId;
    XrResult                   result;     //!< XR_SUCCESS or an XR_ERROR_* (as for actions)
    uint32_t                   width;      //!< image size (0 on failure)
    uint32_t                   height;
    XrRect2Di                  sourceRect; //!< what the image covers, desktop coordinates
    XrTime                     frameTime;  //!< when these pixels were current
    char                       message[XR_MAX_DESKTOP_MESSAGE_SIZE_DXR];
} XrEventDataDesktopCaptureCompletedDXR;

typedef struct XrEventDataDesktopCaptureStateChangedDXR {
    XrStructureType            type;  //!< XR_TYPE_EVENT_DATA_DESKTOP_CAPTURE_STATE_CHANGED_DXR
    const void* XR_MAY_ALIAS   next;
    XrSession                  session;
    XrDesktopControlStateDXR   state; //!< the CAPTURE scope's state
} XrEventDataDesktopCaptureStateChangedDXR;

//! The CAPTURE scope's state. Never gated, like xrGetDesktopControlStateDXR (which keeps
//! reporting the CONTROL scope, plus the capability bits, CAPTURE_BIT included).
XrResult xrGetDesktopCaptureStateDXR(XrSession session, XrDesktopControlStateDXR* state);

//! Start one capture. Returns at once with a captureId (never 0); exactly one
//! XrEventDataDesktopCaptureCompletedDXR follows while the session exists.
XrResult xrCaptureDesktopDXR(XrSession session, const XrDesktopCaptureInfoDXR* info, uint64_t* captureId);

//! Fetch the image of a completed capture (two-call idiom, in BYTES:
//! countOutput = width * height * 4).
XrResult xrGetDesktopCaptureImageDXR(XrSession session, uint64_t captureId,
                                     uint32_t capacityInput, uint32_t* countOutput, uint8_t* rgba);
```

`xrGetDesktopControlStateDXR` and `XrEventDataDesktopControlStateChangedDXR` keep their
version-1 meaning: the CONTROL scope. A version-1 app sees no change.

### 10.2 Consent (per scope)

Each scope has its own state, evaluated with the §3 table. Rows 1–7 are shared by both
scopes. Rows 8–10 read that scope's own grant:

- **Requesting.** `xrRequestDesktopControlDXR` with an `XrDesktopControlRequestScopeDXR`
  chained asks for the listed scopes in **one** OS / DisplayXR prompt and announces each
  listed scope's outcome on that scope's event (the §9 "always announce" rule applies per
  scope). A scope already `GRANTED` is not asked for again. A request is a no-op only while a
  scope it lists is `PENDING`. `xrCaptureDesktopDXR` **never prompts**.
- **GNOME.** One portal session carries both scopes. The `Start` response decides each one
  separately: `devices` non-zero ⇒ CONTROL granted, `streams` non-empty ⇒ CAPTURE granted.
  Asking for a scope later opens a **new** combined session, and the dialog asks about input
  again (a restore token cannot be widened). Apps that will capture should ask for both
  scopes in their first request. A silent restore that fails (revoked, or the shared
  monitors changed) puts **both** scopes in `DENIED`, as in v1.
- **Windows.** The existing consent MessageBox. Its text lists only the scopes asked for
  ("see your screen", "type and click in other apps"). The answer is stored per scope (§10.6).
- **macOS.** CAPTURE's OS grant is Screen Recording (`CGPreflightScreenCaptureAccess`,
  requested with `CGRequestScreenCaptureAccess`). CONTROL already asks for it, so CAPTURE is
  usually granted without a new prompt.
- **Revocation.** A backend capture call that returns `NOT_GRANTED` puts CAPTURE in `DENIED`
  (event), and only CAPTURE, except on GNOME where it closes the shared session (both).

### 10.3 Capture behaviour

- **Validation** at `xrCaptureDesktopDXR`: `target` known; `maxWidth` / `maxHeight` in
  1..8192; REGION with `width` / `height` > 0 and the §9 coordinate limits; WINDOW with a
  non-zero `windowId`. A bad call returns `XR_ERROR_VALIDATION_FAILURE` and no id.
- **Gating.** It needs CAPTURE `GRANTED`, else `XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR`
  (CONTROL is not required). It needs the CAPTURE capability bit, else
  `XR_ERROR_FEATURE_UNSUPPORTED`.
- **One in flight.** A second `xrCaptureDesktopDXR` while one is pending returns
  `XR_ERROR_LIMIT_REACHED`. Starts are at least 250 ms apart (4 Hz per session). An earlier
  call is accepted and waits, and the wait counts toward the completion time.
- **Its own worker.** Captures run on a per-session capture thread, never queued behind
  actions, and not subject to the §5 idle wait (capture injects nothing).
- **Checks** (re-evaluated just before the OS capture, and again before the frame is
  handed over): state `GRANTED`, kill switch off, not locked, no secure desktop. On failure
  the capture completes with `XR_ERROR_DESKTOP_CONTROL_REFUSED_DXR` and the §9 message
  (`"kill switch"`, `"screen locked"`, ...), and any frame already taken is discarded.
- **Target resolution.**
  - DESKTOP is the bounding box of every monitor.
  - REGION is clipped to that box. A region entirely outside it is refused
    (`"found nothing: outside every monitor"`).
  - WINDOW is the window's current frame from the backend's window list (`WINDOW_LOST` if it
    is gone, `REFUSED` `"the window is minimized"` if minimized), captured **as on screen**:
    windows overlapping it are in the image.
- **Freshness.** The pixels were current at or after the call (`frameTime`). On GNOME a stream
  with no frame since the call is current, because mutter sends frames on damage only. The
  first frame of a newly started stream is always awaited (bounded: 2 s, else `TIMEOUT` →
  `XR_ERROR_RUNTIME_FAILURE`).
- **The image.**
  - RGBA8, sRGB-encoded, straight alpha, row-major, top row first, stride `width * 4`.
  - Scaled down with an area filter so that `width ≤ maxWidth` and `height ≤ maxHeight`,
    keeping the aspect ratio. Never scaled up: a source smaller than the maximum comes back
    at its own size, in **device** pixels where the OS gives them.
  - Every real pixel has alpha 255. **Withheld** pixels are `(0,0,0,0)`: off every monitor,
    on a monitor the user did not share (GNOME), inside an excluded window (§10.4), or inside
    one of the app's own windows (`OWN` bit, every OS).
  - Mapping back to desktop coordinates:
    `x = sourceRect.x + (px + 0.5) * sourceRect.width / width` (and the same for y). The
    result can be fed straight to `xrPickDesktopDXR` or a pointer action.
- **Retention.** The runtime keeps the image of the **most recent** successful capture only,
  until it is fetched, the next capture completes, the session ends, or 10 s pass. Fetching
  any other id, or an expired one, returns `XR_ERROR_VALIDATION_FAILURE`. One fetch may be
  repeated (two-call idiom). The memory is freed when retention ends.
- **The 3D panel.** A region over a spatial display returns the panel's **woven** pixels,
  which is what is scanned out. Substituting views is not in v2 and is future work (§10.8,
  decision 5).

### 10.4 Privacy

- **No disk, no logs.** The runtime never writes a frame, a thumbnail or a hash to disk or to a
  log at any level. Messages name failures, never content.
- **Exclusion.**
  - Every OS: the app's own windows (`OWN` bit) are always withheld over their whole frame,
    whether or not the display processor excluded them (§10.8, decision 7).
  - Windows: Windows.Graphics.Capture returns `WDA_EXCLUDEFROMCAPTURE` windows as the OS
    renders them for capture, and the runtime adds nothing.
  - macOS: what ScreenCaptureKit returns for `NSWindowSharingNone` windows is to be measured
    (§10.7) and documented here.
  - GNOME: every window that the extension's `GetAllWindows` reports as `capture_excluded`
    is withheld over its whole frame, because portal monitor streams contain them
    (wayland-window-geometry §6.2). When the extension is missing or the window list cannot
    be read, the capture is refused (`"cannot honour capture exclusion"`) and never returned
    unmasked.
- **Audit (§6.2).** One record per capture **burst** (closed after 2 s without a capture):
  `{"action":"capture","target":{...},"w":1280,"h":720,"count":7,"result":"ok"}` plus the
  first and last time. A failure always closes its burst with its own record. Consent records
  carry `scope`.
- **Not re-exported.** An app must not hand captured frames to other processes over MCP or
  IPC (ADR-056 D4, and the app rules).

### 10.5 Backends

| | GNOME Wayland | Windows | macOS |
|---|---|---|---|
| capture | the RemoteDesktop session of §7 plus `ScreenCast.SelectSources` (MONITOR, `multiple`, cursor HIDDEN, no persist keys) before `Start`; `OpenPipeWireRemote` → PipeWire streams, SHM (BGRx/BGRA) only | Windows.Graphics.Capture `CreateForMonitor` per touched monitor, a one-frame pool on the backend's own D3D11 device, staging read-back | `SCScreenshotManager` (macOS 14+) with a display `SCContentFilter`, cursor hidden |
| CAPTURE only (no CONTROL) | a standalone ScreenCast portal session (`persist_mode` 2 on `SelectSources`) | as above | as above |
| library | `libpipewire-0.3.so.0` dlopen'd, no `DT_NEEDED` (absent ⇒ no CAPTURE bit) | Windows 10 1903+ | ScreenCaptureKit, weak-linked (macOS < 14 ⇒ no CAPTURE bit) |
| indicator | GNOME's screen-sharing indicator while the session is open | the Windows.Graphics.Capture border (kept on) | the system screen-recording indicator |
| coordinates | stage px; stream `position` / `size` from the `Start` response map stage → stream px | physical px (per-monitor-v2 DPI context) | points; the display's scale gives pixels |

The backend interface (`u_desktop.h`) gains, in the implementation PR: a per-scope
`access_status` / `request_access`, and a `capture` call made only from the capture worker,
which fills a runtime-owned RGBA buffer and is bounded (2 s plus any session restore).

### 10.6 Store v2 and CLI

`"version": 2`. An app's entry is either a string (v1: the CONTROL answer) or an object
`{"control": "allow" | "deny", "capture": "allow" | "deny"}`. v1 files are read as is and
rewritten as v2 on the next write. `displayxr-cli desktop allow | deny | forget <exe>` take
`--scope control|capture|all` (default `all` for `deny` / `forget`, `control` for `allow`).
`status` lists each scope. The kill switch covers both.

### 10.7 Test plan delta

- **Every OS (CI, fake backend).** `DXR_DESKTOP_BACKEND=fake` returns a deterministic
  pattern. Unit tests cover:
  - validation, the one-in-flight and 4 Hz rules, retention and expiry;
  - scaling and the `sourceRect` → pixel mapping, and alpha 0 for withheld areas;
  - the per-scope state machine (a v1 request ⇒ CONTROL only);
  - the kill switch and the lock arriving between the capture and the hand-over (the frame
    is dropped);
  - audit coalescing, with no pixel data in any record;
  - store v1 → v2.

  Add a capture script to `desktop_control_probe` (`e2e_fake.txt`). The probe writes a PNG
  only with `--save`.
- **GNOME (Ubuntu 24.04 and 26.04, plus a 22.04 Wayland session for the oldest
  portal).**
  - One dialog covers both scopes. Captures in a loop show **no flash**, and `dbus-monitor`
    shows no `org.gnome.Shell.Screenshot` call.
  - The screen-sharing indicator is up only during bursts. Silent restore works within the
    process.
  - The user unticks remote interaction ⇒ CONTROL `DENIED`, CAPTURE `GRANTED`.
  - With one of two monitors shared, a region outside it comes back with alpha 0.
  - Hotplugging a shared monitor ⇒ re-prompt, both scopes `DENIED` until requested again.
  - At 100 %, 125 % and 200 % (mixed) scale: capture → find a button → `CLICK` at the
    mapped point lands on it.
  - Masking: a `capture_excluded` window (a transparent DisplayXR app) is alpha 0. With the
    extension disabled the capture is refused.
  - Lock ⇒ refused.
  - With `libpipewire` uninstalled ⇒ no CAPTURE bit.
  - No file is created (an `inotifywait -r` on `$HOME` and `/tmp` during a 100-capture run).
    The logs contain no frame data.
- **Windows (11, and 10 22H2).**
  - The consent text lists the scopes. `allow` / `deny` are stored per scope.
  - The yellow capture border is visible during a burst.
  - A `WDA_EXCLUDEFROMCAPTURE` window comes back as the OS renders it for capture.
  - Mixed-DPI monitors: the mapped click lands.
  - A UAC prompt or a lock ⇒ refused.
  - The hybrid-GPU box: the panel on the iGPU, the app on the dGPU.
  - The app's D3D device and queue are untouched (the D3D11 debug layer stays clean).
- **macOS (14, 15, 26).** The last two items below are decision 8's hardware checks.
  - Screen Recording denied ⇒ `NOT_GRANTED`. Revoked in System Settings mid-session ⇒
    `DENIED` on the next capture.
  - Retina and a non-Retina external display: the mapping is right.
  - The menu-bar indicator appears.
  - **Measure** what `NSWindowSharingNone` windows of another app look like (reports suggest
    macOS 15 no longer hides them from ScreenCaptureKit), and record the result in §10.4.
  - Record whether macOS 15's periodic "continue to allow screen recording" confirmation
    fires for a ScreenCaptureKit still.

### 10.8 Decisions (DECIDED 2026-10-19, David)

The section stays **PROPOSED** until implemented. These design points are no longer open
(ADR-056 Amendment 1, "Decisions"):

1. CAPTURE is usable without CONTROL. GNOME gets a second path, a standalone ScreenCast
   portal session (§10.5).
2. A later CAPTURE request is allowed. On GNOME it shows a second, combined dialog (§10.2).
3. A WINDOW capture is the window's frame as it is on screen. There is no occluded-content
   mode (§10.3).
4. The 4 Hz start cap and 10 s retention are fixed in v2, not settings (§10.3).
5. Swapping woven 3D-panel pixels for unwoven views is not in v2 and is future work (§10.3).
6. The Windows.Graphics.Capture yellow border stays on as the indicator (§10.5).
7. The app's own windows are always masked (alpha 0) on every OS (§10.3, §10.4).
8. The two macOS ScreenCaptureKit behaviours (`NSWindowSharingNone`, macOS 15's periodic
   confirmation) stay hardware-verification items (§10.7).

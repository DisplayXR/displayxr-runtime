# XR_DXR_desktop_control — GNOME Wayland backend: manual test plan

The GNOME backend (`src/xrt/auxiliary/desktop/u_desktop_gnome*.c`, ADR-056) can only be
partly exercised in CI: the RemoteDesktop portal's consent dialog cannot be answered
headless, and input has to land on real panels at real scales. This plan is the
**hardware run** that has to pass before the backend is called validated. It drives the
backend directly with `desktop_backend_smoke` (no OpenXR, **no safety layer** — no idle wait,
no target verification, no audit: it is for the person at the machine).

What a headless container already showed (GNOME Shell 50.1, Ubuntu 26.04 arm64, virtual
1280x800 monitor, two GTK4 windows): caps `0x3f`, `list` / `pick` / `focused` / `text`
correct, `text` in ~8 ms end to end, `request` reaching the GNOME remote-desktop dialog, and
every input / window op returning `not-granted` without a grant. Everything **after** consent
is untested until this plan runs.

## 0. Machines and setup

| Box | GNOME | Why |
|---|---|---|
| A | Ubuntu 26.04, GNOME 50, **fractional scaling on** (mutter "logical" layout) | the prototype's platform; DS1 at 1x + laptop panel at **1.33x** |
| B | Ubuntu 24.04, GNOME 46, integer scaling (mutter "physical" layout) | stage px == device px; a 2x panel |
| C | Ubuntu 22.04, GNOME 42 (X11 session recommended for weaving — but this test needs **Wayland**) | no libei: observation-only, consent-only portal |

On each box:

```bash
# from a checkout of the branch
./scripts/build_linux.sh --clean        # or any configured tree with libdbus-1-dev
SMOKE=build/src/xrt/auxiliary/desktop/desktop_backend_smoke
export XRT_LOG=info                      # the backend's INFO lines are the diagnostics
# The DisplayXR GNOME extension, version 16 (runtime >= v2.46.0)
gnome-extensions info window-geometry@displayxr.org | grep -i version   # → 16
```

If the extension is older, install it from `contrib/gnome-shell/window-geometry@displayxr.org`
into `~/.local/share/gnome-shell/extensions/`, then **log out and in** (Wayland cannot restart
the shell in place).

Optional, to see portal attribution / persistence by app id (xdg-desktop-portal >= 1.19):

```bash
export DXR_DESKTOP_APP_ID=org.displayxr.SmokeTest   # else: "no app id" WARN, approval not persisted across runs
```

**One grant per process.** The restore token lives in the process's memory only (ADR-056: never
written to disk by DisplayXR). Run every input / window-op section below in **one** smoke
process with `script` mode, starting with `request` (answer the dialog with "Share"):

```bash
cat > /tmp/s.txt <<'S'
request
type "hello"
key 1 z
S
$SMOKE script < /tmp/s.txt
```

The commands in the sections below go into such a script (`sleep MS` lines pause; `target ID`
pins the guard's target window). `prepare_input` never prompts: without a grant every action
returns `not-granted` and **no dialog appears** (only `request` shows one).

## 1. Capabilities and state

```bash
$SMOKE info
```

Expected:

| Box | line |
|---|---|
| A, B | `backend=gnome caps=0x3f remembers_in_store=0 access=unknown locked=0 idle=<n> ms` |
| C | `caps=0xf` (no INPUT 0x20 / WINDOW_ACTIONS 0x10), and the INFO line says `libei no` |
| extension < 16 / disabled | `caps=0x20` (A/B: input only) or `0x0` (C), WARN "older than version 16" / "not running" |

and one INFO line `desktop/gnome: caps 0x.. (extension v16 yes, RemoteDesktop v2, clipboard yes, libei yes, a11y yes)`.
`idle` must grow while you keep your hands off and drop to ~0 when you touch the mouse.

## 2. Observation (no grant needed by the smoke tool)

Open gedit / gnome-text-editor, a terminal (gnome-terminal or ptyxis), and Settings; overlap two
of them.

```bash
$SMOKE list
$SMOKE focused
$SMOKE pointer          # hover over each window in turn
$SMOKE pick X Y         # inside a window, on the top bar, on the desktop, off every monitor
```

Expected:
- `list`: one line per window, **top-most first** (`z=0` is the window on top; the focused one
  has `state=0x1`); dialogs carry `parent=<owner id>`; minimized windows have `state` bit `0x2`,
  maximized `0x4`; no "ding"/desktop-icons window; frames are stage px — on box A the 1.33x
  panel's windows have **logical** sizes (a full-width window on a 2560-px panel is ~1920 wide).
- `pick` on the top bar → `shell`; on the wallpaper → `desktop`; at `-5000 -5000` → `outside`;
  over the lower of two overlapping windows where the upper covers it → the **upper** one.
- Lock the screen (Super+L), run `$SMOKE pointer` from an ssh session: `result: refused — the screen is locked`;
  `$SMOKE info` → `locked=1`.

## 3. Text at a point (AT-SPI)

```bash
$SMOKE text X Y   # over a gedit paragraph, a button, a terminal line, the Settings sidebar
```

Expected: the paragraph's text (whole buffer for a text view), the button's label, the
sidebar row's name; `""` + `result: ok` over the desktop. Two windows of the same app with
different titles: each point reads its own window's text; a window whose accessible frame name
differs from its title (rename the document) → `""` (never another window's text). Measure with `time`: < 50 ms after
the first call. GTK3/Qt/Electron apps answer only once accessibility is on
(`gsettings set org.gnome.desktop.interface toolkit-accessibility true`, then restart them).

## 4. Pointer — mixed scales (box A, and the 2x panel on B)

Find a small target on each monitor (a toolbar button), note its centre from `pick`.

```bash
$SMOKE move X Y                      # approve the dialog the first time
$SMOKE click X Y                     # left click
$SMOKE click X Y right
$SMOKE click X Y left 2              # double click
$SMOKE scroll 0 3                    # 3 notches down over a scrollable view
```

Expected: `result: ok` and the click lands **exactly** on the target on the 1x DS1, on the
1.33x laptop panel and on a 2x panel (the prototype's check — the portal's own
NotifyPointerMotionAbsolute path clicked the wrong window here; libei must not). A point
between monitors → `refused — (x,y) is on no monitor`. The GNOME remote-access indicator
appears in the top bar while acting and disappears ~2 s after the last command — after `key`,
`type` and `scroll` too (it used to stay ~12 s: prepare_input's hold outlived the injection).

## 5. Keys — layout-aware chords

Focus gedit with some text.

```bash
$SMOKE key 1 a          # Ctrl+A: select all
$SMOKE key 1 c          # Ctrl+C
$SMOKE key 0 end
$SMOKE key 1 z          # Ctrl+Z
$SMOKE key 8 ""         # Super alone: the overview opens (Escape it)
$SMOKE key 0 Escape     # key names are case-insensitive: Escape = escape, F5 = f5
```

Then switch the input source to **French (AZERTY)** and repeat `key 1 z` and `key 1 a`: they must
still undo / select all (the key labelled Z/A is pressed — a US-position mapping would send
Ctrl+W = close the tab). On AZERTY `key 1 1` presses Shift+& (the digit is on the Shift
level). A character the layout lacks (e.g. `key 1 grave` on a layout with no ` key without
AltGr) → `unsupported — no key produces '`' on the active keyboard layout`.

**Keypad, locks, media and right-hand modifiers** (spec §5 key names). Watch what arrives with
`wev` (or `xev` in an Xwayland window) next to gedit:

| command | expected |
|---|---|
| `key 0 numpad7` with NumLock on | `KP_7`, gedit types 7 |
| `key 0 numlock`, then `key 0 numpad7` | NumLock toggles (the shell's indicator / LED); `KP_Home`: the caret goes to the line start |
| `key 1 numpadadd` / `key 1 numpadsubtract` | gedit zooms in / out (Ctrl+KP_Add / KP_Subtract) |
| `key 0 numpadenter`, `numpadmultiply`, `numpaddivide`, `numpaddecimal` | `KP_Enter` (a line break), `KP_Multiply`, `KP_Divide`, `KP_Decimal` / `KP_Delete` by NumLock |
| `key 0 pause`, `key 0 scrolllock` | `Pause`, `Scroll_Lock` |
| `key 0 audiovolumeup` / `audiovolumedown` / `audiovolumemute` | GNOME's volume OSD moves / mutes |
| `key 0 mediaplaypause` / `mediatracknext` / `mediatrackprevious` / `mediastop` with a player (Videos, Rhythmbox) playing | play/pause, next, previous, stop |
| `key 0 rightalt` with the Korean input source (Hangul on Right Alt) | Hangul / English toggles; on US-intl it is AltGr (nothing typed by itself) |
| `key 2 rightctrl`, `key 0 rightshift`, `key 0 rightsuper` | `Shift_L` + `Control_R`; `Shift_R`; `Super_R` (the overview opens: Escape it) |
| the probe: `act key target=ID keys=rightctrl+c` | `XR_ERROR_VALIDATION_FAILURE` at submit (two keys) |

## 6. Text — IME-safe paste

```bash
$SMOKE type "Hello, 世界 — ünïcödé"        # into gedit
```

- **Korean IME** (ibus-hangul or the GNOME Korean input source) active, Hangul mode on: the
  exact string appears — not jamo soup (it goes through the clipboard, never key codes).
- **Terminal** (gnome-terminal, ptyxis, kgx; also kitty / alacritty / foot if installed):
  focus it, `type "echo pasted"` — it appears on the prompt (Ctrl+Shift+V was used). In a
  non-terminal app the same command uses Ctrl+V.
- **Clipboard restore** (best effort): copy "ORIGINAL" in gedit, `type "x"`, then paste in
  gedit with Ctrl+V within 2 s → "ORIGINAL". After ~2 s (the portal session closes) the
  clipboard may be **empty** — that is the documented limitation (the portal's selection dies
  with its session). Record what you see.
- A window that takes no text (Settings' sidebar) → `failed — the focused window did not ask
  for the pasted text`.
- **Delivery evidence (re-run item 4).** A clipboard read after the keystroke is not proof: a
  clipboard manager reads too. When the focused window exposes a focused text field over
  AT-SPI, the paste counts only if that field changed (length, caret or the text around the
  caret); with no such field a read gives `ok (unverified: …)` through the extension (the
  smoke tool prints the backend message `unverified: …` after `result: ok`). Run with a
  clipboard manager (GPaste, or the Clipboard Indicator extension) enabled:
  | focus | expected |
  |---|---|
  | gedit / gnome-text-editor text | `ok` (verified; no message) |
  | gnome-terminal prompt | `ok` (VTE exposes its text) |
  | Settings' sidebar, Files' icon view (no text field) | `ok` + `unverified: …` (was a plain `ok`), never a crash |
  | a GTK text entry with the text `abc` selected, `type "abc"` | `failed — the focused text field did not change` is ACCEPTABLE (identical replacement); record it |
  | an app with toolkit accessibility off (an Electron app without a11y) | `ok` + `unverified: …` |
  Record any case where the text visibly arrived but the result is `failed — … did not change`
  (a field whose AT-SPI text updates late or never): that is a false failure to fix.
- **Clipboard changing hands mid-type (re-run items 2 + 3).** A helper copies on a timer
  after `type` starts, e.g. `wl-copy USERTEXT` from a script that sleeps D ms after launching
  the smoke tool's `type "agent text"` (or `xdotool`-free: `gdbus`-driven wl-copy). Run 10× for
  each D in 8, 11, 15, 22, 25 ms, into gedit:
  | outcome | expected |
  |---|---|
  | the process | never dies (it used to die of SIGPIPE at ~8–10 ms: EPIPE on the SelectionWrite fd is now a cancelled transfer) |
  | the copy landed before the paste keystroke | `refused — the clipboard changed hands (the user copied something); nothing was pasted`, nothing appears in gedit, and the clipboard afterwards is `USERTEXT` (not restored over) |
  | the copy landed after the keystroke, the agent's text never read | `failed — the clipboard changed hands right after the paste keystroke … whatever the window pasted was the user's clipboard, not the agent's text`; the clipboard afterwards is `USERTEXT` |
  | the copy landed after the keystroke, the agent's text read | `failed — lost the clipboard during the paste … whether the text arrived is unknown`, or `ok` only if gedit's text visibly changed; the clipboard afterwards is `USERTEXT` |
  | the portal never reported this session as the owner | `failed — the portal did not make this session the clipboard owner — not pasting`; nothing pasted, clipboard `USERTEXT` (honest; re-check 2 saw 12/145) |
  | never | `USERTEXT` pasted into gedit with a plain `ok` (re-check 2: 37/80 at D = 8–25 ms, 10/10 at 15 ms) or `failed — did not ask` (the old 7/40) |
  Count how often each row occurs per D. `USERTEXT` appearing in gedit is acceptable only
  with one of the two "after the keystroke" failures (the copy raced the keystroke itself) —
  never with `ok`.
- **Re-check 2 rows (fixes for item 3).** Same helper, gedit, plus a clipboard manager:
  | case | how | expected |
  |---|---|---|
  | (a) copy lands between Ctrl-down and V | D = 8, 11, 15, 22, 25 ms, 20× each (the owner-change signal arrived ~9 ms before V in the D-Bus trace) | `refused — the clipboard changed hands …; nothing was pasted`, Ctrl released with no V (nothing typed, nothing pasted); never `ok` with `USERTEXT` in gedit. Count residual `ok` + `USERTEXT` (a signal reaching the app only after V was sent): expected ~0 |
  | (b) copy lands after V, target reads late | a target that reads the clipboard 300 ms after the key (re-check 2: 20/20 pasted `USERTEXT` with `ok`), 20× | `failed — … whatever the window pasted was the user's clipboard, not the agent's text` (the agent's text was never read); never `ok` |
  | (b') random timing | D uniform in 0–60 ms, 40× (re-check 2: 8/40 `ok` + `USERTEXT`) | no `ok` with `USERTEXT` in gedit; every failure message is one of the rows above |
  | (c) copy between reading the previous clipboard and taking it | D = 0–5 ms, or a helper copying the moment `SelectionRead` shows on `dbus-monitor` (re-check 2: 7/185 restored the previous text over the user's copy) | `refused — the clipboard changed hands …`; the clipboard afterwards is `USERTEXT` — never the previous text, never the agent's |
  | (c') copy after the key, during the ~1.2 s serve | D = 500–1500 ms | the clipboard afterwards is `USERTEXT`, never the previous text |
  | (N1) copy between reading the previous clipboard and our SetSelection (re-check 3: 9/185 at 10.4–12.5 ms after the command; refused correctly but the clipboard was left owned and EMPTY — reads unanswered for 3 s) | D ≈ 10–13 ms, 40× (log: `our offer replaced the user's copy; releasing`) | `refused — … the offer had already replaced what the user copied, which is gone (copy it again); the clipboard was released and is empty`; afterwards `wl-paste` returns at once with no owner / empty — never a 3 s hang, never the agent's text, never the previous text. `USERTEXT` is NOT expected: it is gone. This ~1.5 ms window is the one place "the clipboard is the user's afterwards" cannot hold |
  | nothing to restore | `wl-copy --clear` so the clipboard is empty, then `type "x"` | `ok`; afterwards `wl-paste` answers at once (empty, or "x" kept by mutter's clipboard keeper) — never a hang |

### 4b. Clicks with a moving pointer (re-run item 5)

While a helper wiggles the real mouse (a USB mouse on a stand-held vibrator, or a second
libei / `ydotool` client sending ±5 px relative motion every 5 ms), click a 10×10 px target
20 times: `$SMOKE click X Y`. Expected: every `ok` click lands on the target's exact pixel
(was 3/20 landing 3 px off); a click whose pointer was moved off the verified point before the
press → `refused — the pointer moved off the verified point` (never `ok` elsewhere). The press
now carries its own absolute motion in the same libei frame, so it cannot be shifted after the
check; record the count of `ok` vs `refused`.

## 7. Session lifetime and consent

```bash
$SMOKE request
```

- Click **Cancel** → `access: denied`. Click **Share** → `access: granted`.
- Inside an app (Unity / native, through the runtime): grant once; act; wait > 2 s (indicator
  goes away); act again → **no dialog** (silent restore with the in-memory token), indicator back.
- Lock the screen **while a session is open** (act, then Super+L within 2 s — the indicator is
  still up), unlock, act again → still no dialog and `ok`, never `not-granted`; the log shows
  `the screen lock closed the RemoteDesktop session` then `session restored`. (The portal closes
  the session as the lock starts, before `org.gnome.ScreenSaver.GetActive` turns true; the
  backend waits up to 3 s for it before reading the close as a Stop.) Repeat with a lock that
  happens with no session open: same result.
- **Stop from GNOME's indicator** (top bar → remote-access menu → Stop) while a session is
  open, then act → `not-granted — remote control was declined or revoked` within ~3 s (the lock
  check above; no 15 s wait, no silent restore — the log says `treating it as a revoked grant`);
  `request` asks again.
- Revoke the permission (Settings → Apps → the app, or `flatpak permission-reset`), act → the
  first action returns `not-granted` within ~5 s (`did not restore the session silently`), the
  next ones at once.
- Restart the app → the dialog comes back once (the token is never written to disk). With
  `DXR_DESKTOP_APP_ID` registered on xdg-desktop-portal >= 1.19 the dialog names that app.

## 7b. Delivery guarantee (contract v2) — must hold

Each needs `script` mode after `request`, and a helper to move focus / copy at the right
moment (a second terminal with `sleep 1; wmctrl`-style or just your hands, with `sleep` lines
in the script to give you the window).

- **Paste never happens unless we own the clipboard.** Copy "SECRET" in gedit. Make the portal
  clipboard unavailable for the session (a portal without Clipboard, or kill
  `xdg-desktop-portal-gnome` right after `request`), then `type "x"` into another gedit: result
  `failed — the portal did not make this session the clipboard owner — not pasting` /
  `unsupported`, and the target must NOT receive "SECRET".
- **Clipboard changes hands before Ctrl+V.** Run `type "AGENT"` with a clipboard manager or a
  script that copies "OTHER" at the same instant; either "AGENT" is pasted or the result is
  `refused — the clipboard changed hands …` (or one of the two "after the keystroke" failures);
  "OTHER" must never be pasted with an `ok`.
- **A clipboard manager reading first never makes the target paste the previous clipboard.**
  Copy "PREVIOUS" in gedit; with GPaste / Clipboard Indicator running, `type "AGENT\n"` into a
  slow target (a large gnome-text-editor document, or a terminal under load): "AGENT" is
  pasted, never "PREVIOUS" (the agent's text is served to every reader for ~1.2 s after Ctrl+V,
  only then is "PREVIOUS" offered again). `type` takes ~1.2 s longer than before — expected.
- **A clipboard manager is not a delivery.** With GPaste / Clipboard Indicator running, `type`
  into a window that takes no text (Settings sidebar): result `failed — the focused window did
  not ask for the pasted text`, not `ok`.
- **Focus stolen just before Ctrl+V.** `target <gedit id>`, then `type "long text…"` while a
  timer (`sleep 0.3; gnome-text-editor &` in another shell) raises a new window: result
  `refused — focus moved`, nothing pasted into the new window; the clipboard text from before is
  offered again.
- **Chords.** `target <id>`, `key 1 a` while focus moves away mid-chord → `refused — focus
  moved`, and Ctrl is NOT left held (type in the new window: no Ctrl shortcuts fire).
- **Trailing newline.** `type "ls\n"` into a terminal → `ls` pasted, then Enter as a separate,
  checked key press (the command runs). Move focus between the paste and the Enter → refused, no
  Enter in the new window.
- **Clicks / scroll at the verified point.** `target <id>`, `click X Y` while you move the mouse
  → `refused — the pointer moved off the verified point` or `target no longer under the point`.
- **Super+L protection.** During `op move ID X Y` and `op minimize ID`, hammer the L key: the
  screen must NOT lock (Super is held only across the button press, ~20 ms) — record the result;
  also verify mutter keeps the Super+drag grab after Super is released (the window still moves
  to the target). If the move fails here, report it: Super would have to be held for the drag.
- **Kill switch mid-action.** In the app path (common layer), `displayxr-cli desktop stop` while a
  long `type` / `op move` runs → the next batch is refused (`stopped: desktop control was
  suspended…`), no key or button left pressed.
- **Lock fails closed.** `info` while `org.gnome.ScreenSaver` does not answer (e.g. right after
  shell start, or `kill -STOP` the shell for a second) → `locked=1`.
- **Privacy.** With `XRT_LOG=debug`, grep the app's log for the typed strings, chord letters,
  the clipboard text and the AT-SPI text read in §3 → no hits.

## 8. Window ops on other apps (Super gestures)

Get ids from `$SMOKE list`. For each op, check the window, and that **no overview** and no
tiling popup is left open afterwards, and that the pointer is back where it was.

```bash
$SMOKE op activate ID       # a window partly covered by another: it comes to front with focus
$SMOKE op maximize ID       # Super+Up → state 0x4 in list
$SMOKE op restore ID        # Super+Down → not maximized
$SMOKE op minimize ID       # Super+H → state 0x2
$SMOKE op restore ID        # → unsupported — minimized windows of other apps cannot be restored on GNOME
$SMOKE op move ID 200 150   # Super+drag; list shows frame=(200,150 ...) within 2 px
$SMOKE op move ID 200 150 800 600   # → unsupported (resize of other windows)
$SMOKE op close ID          # gedit with unsaved text: ok — "the app asked for confirmation"
$SMOKE op close ID          # gnome-text-editor (libadwaita) with unsaved text: the prompt is
                            # INSIDE the window — still ok — "the app asked for confirmation
                            # (its window stayed open and is responding …)"
```

`close` of a window whose app is hung (`kill -STOP <pid>` first) → `timeout — … its app is not
responding`; of an app without an accessibility tree that shows an in-window prompt →
`timeout — … cannot be told apart` (record which apps do this).

Move specifically: move a window **across the boundary** between the DS1 and the laptop panel
(different scales) and to just below the top bar of the other monitor; the result must be exact
(one read-back correction allowed — two when the log says `snapped by edge tiling`), Ubuntu's
tiling popup must not stay up, and no window may end up maximized or tiled by edge tiling. Do
each move **twice: once with the pointer on the window's monitor, once with it parked on the
other monitor** (the hardware run failed this way: Tiling Assistant quarter-tiled the window at
the monitor's top-left, then `pointer did not arrive at (2400,16) — it is at (1920,16)`). The
pointer now crosses between monitors through the middle of their shared edge and the drop point
stays ≥ 48 px inside its monitor. If a move still fails, the result must be `refused` /
`timeout` with the reason, never `ok` with the window elsewhere; record the log lines. Activate a window that is entirely covered → `refused — no
visible part of the window to click`; a window on another workspace → the same.

**Cross-monitor move under Ubuntu's Tiling Assistant (re-run item 1).** Tiling Assistant's
`monitor-switch-grace-period` (on by default) judges a drag against the PREVIOUS monitor for
150 ms after the pointer changes monitors; the drop now dwells ≥ 300 ms on the target monitor
after the last crossing. And a Tiling Assistant popup (or any shell UI) that a move opens over
the screen is now detected (picks that saw windows / the desktop before the gesture see shell
UI after it), dismissed with Escape and re-checked before any correction; if it stays up the
move is `refused — the move opened shell UI … not correcting through it`. Run 10 cross-monitor
moves (5 each direction, alternating the pointer's starting monitor) for each of:
| Tiling Assistant | grace period | expected |
|---|---|---|
| enabled (Ubuntu default) | default (on) | 10/10 `ok`, exact, not tiled (base f1651f7d8 was 8/10; the first fix pass was 0/10) |
| enabled | off (`gsettings set org.gnome.shell.extensions.tiling-assistant monitor-switch-grace-period false`) | 10/10 |
| disabled | — | 10/10 |
Also once: a window dragged so that it DOES get tiled (drop near the target monitor's left
edge by hand with `op move` to x = monitor.x + 50) — the log shows `shell UI (a tiling
popup?) … Escape` when the popup came up, the popup is gone afterwards, and the result is
either `ok` (corrected) or `refused`/`failed` with the reason — never a popup left on screen.
And once with a polkit / password prompt already on screen before the move: it must NOT be
dismissed (it was shell UI before the gesture).

**Activate with the pointer starting on the other monitor (merge-check M1).** In Ubuntu's
session (Tiling Assistant + dock) a Super+click right after the pointer changed monitors
sometimes MOVED the window to the other monitor and still returned `ok`: mutter's grab took a
stale pointer position ~4–5 ms after the press, putting the window where the first waypoint from
the other monitor would (eb1379b47: 4/65; with the avatar window 4/18; plain GNOME 0/25). Now
the pointer settles ≥ 300 ms at the press point after the last crossing before Super goes down,
and the frame is read back after every activate: a moved window is put back by a verified move
(log: `activating moved the window … — putting it back`) or the activate fails with
`failed — the Super+click moved the window from (…) to (…) and it could not be put back: …`.
Before each run, park the pointer on the monitor the window is NOT on (`$SMOKE move X Y`):
| session | window | runs | expected |
|---|---|---|---|
| Ubuntu (Tiling Assistant + dock) | gedit, partly covered | 25 | 25/25 `ok` with the frame unchanged (`list` within 2 px of before); 0 jumps; count any `putting it back` log lines (each must end with the frame back) |
| Ubuntu (Tiling Assistant + dock) | the avatar window (the app's own window via the Super+click fallback, and another app's avatar) | 25 | same; the old 4/18 must be 0 |
| plain GNOME | gedit | 25 | same (control) |
| any | a window whose put-back is impossible (force it: maximize the target first, or note any natural case) | — | `failed — the Super+click moved the window …` or `… changed the window's maximized state …`, never a plain `ok` |
Each activate after a crossing now takes ~300 ms longer — expected.

## 9. Window ops on the app's own window

Needs an app with a window (the smoke tool has none): run a DisplayXR test app using the
extension (e.g. `cube_handle_vk_linux` under Wayland) with an agent harness, or the Unity
binding. Expected, **once access is granted**: `ACTIVATE` / `MINIMIZE` / `RESTORE` / `MOVE` go
through WindowPlacement1 with no portal session (no remote-access indicator, no input
injected); `MAXIMIZE` → unsupported; a resize → unsupported; `CLOSE` of a minimized own window
un-minimizes it first. **Without** a grant every one of them → `not-granted`, like any other
action (spec §4: all operations require `GRANTED`; the consent table has no own-window
exception). An earlier version of this section said they work "without the portal dialog",
which read as "without a grant" — it is the input path that differs, not the consent.

## 10. Extension-version matrix

| Extension | Expected caps (A/B) | Notes |
|---|---|---|
| 16 | `0x3f` | full |
| 15 | `0x20` | no list / pick / text → no other-window ops (WINDOW_ACTIONS is withheld); own window ops would work |
| <= 14, or disabled | `0x20` | WARN "older than version 16" / "not running" |
| any, on box C (22.04) | `0xf` with v16 | no libei: `request` shows the dialog once for observation consent, then closes the session |

Report per box: the `info` line, a pass/fail per section, and for failures the backend's log
lines (`XRT_LOG=info`).

## 11. Screen capture (spec v2 §10, ADR-056 Amendment 1) — the capture delta

Untested on hardware when it landed: the portal / PipeWire path compiled on Ubuntu 22.04 and
26.04, the SPA pods were checked byte for byte against libspa 1.6.2 and the stream ABI against
the 26.04 headers, but **no frame has been grabbed by this code yet**. The Python prototype in
the Lenovo app class (GStreamer, DS1, GNOME 50) is the reference: pull 82–204 ms, compose
~165 ms at 7680x2400, 0.3–0.45 s per capture. This C path (libpipewire directly, no GStreamer,
nearest-sample compose) should beat the compose figure; report both numbers.

Setup: `libpipewire-0.3-0` installed (`info` shows caps `0x7f` on box A/B; without it `0x3f`).
The DS1 headless GNOME harness is fine for everything but the dialogs (answer those by hand, or
with the harness's portal-dialog driver). Run in ONE `script` process, as in §0, with
`XRT_LOG=info`. Both the smoke tool (backend, no safety layer) and `desktop_control_probe`
(the OpenXR API, with the safety layer) have capture commands:

```bash
$SMOKE script <<'S'
request-scopes all
monitors
capture 0 0 7680 2400 /tmp/desk.ppm
capture 100 100 400 300
S
# through OpenXR (fake-free): consent, 4 Hz cap, audit, masking of the app's own windows
desktop_control_probe script my_capture.txt   # request-scopes all / capture desktop max=1920x1080 --save /tmp/a.png
```

| # | Check | Expected |
|---|---|---|
| 11.1 | `request-scopes all` on a fresh process | ONE dialog: a screen picker + "allow remote interaction". `access: control GRANTED, capture GRANTED` |
| 11.2 | 20 captures in a loop (`capture …` × 20, or the probe at 4 Hz) | **no white flash**, `dbus-monitor --session "interface='org.gnome.Shell.Screenshot'"` silent; per-capture time logged |
| 11.3 | The screen-sharing / remote indicator | up during the burst, gone ~2 s after the last capture |
| 11.4 | `capture` after 3 s idle (session closed) | restores silently (no dialog), first frame arrives on an idle screen |
| 11.5 | Untick "remote interaction" in the dialog (`request-scopes all+again`) | `control DENIED, capture GRANTED`; input actions refused, captures work |
| 11.6 | Share one of two monitors | a region over the unshared one comes back with alpha 0 (`withheld px` = its area) |
| 11.7 | Mixed scale (1x + 2x, and 1.33x + 2x) | `capture` over both: size = region × 2; `--save` image lines up; the probe's `expect-desktop-pixel` on a known button maps to it, and a CLICK at the mapped point lands on it |
| 11.8 | Gaps of a non-rectangular layout (monitors of different heights) | alpha 0 there (never opaque black) |
| 11.9 | A capture-excluded window (a transparent DisplayXR app, or `CaptureExclusion1` on a test window) | alpha 0 over its whole frame. Disable the extension → `refused: cannot honour capture exclusion` |
| 11.10 | The app's own window (probe run from a DisplayXR app / harness window) | alpha 0 over its frame |
| 11.11 | Lock the screen, capture | `refused: screen locked` (common layer: NOT_GRANTED at submit, state SUSPENDED); after unlock captures resume without a dialog |
| 11.12 | Uninstall libpipewire (`sudo dpkg -r --force-depends libpipewire-0.3-0` in a container, or `LD_LIBRARY_PATH` to an empty dir does not hide it — use the container) | caps without `0x40`; capture state UNSUPPORTED; an existing screen grant is still restored WITH screens (log: SelectSources sent) |
| 11.13 | **Never downgrade**: grant `all`, then only input actions for a while (sessions restore), then capture | the capture restores silently with screens; GNOME's saved approval still includes the screens (`flatpak permission-show` / the portal permission store) |
| 11.14 | **Monitor blink**: reboot the DS1 (HDMI handshake), capture right after login | no restore while the panel is missing (log: `a monitor the screen grant covers is missing`), then a silent restore once it is back; the saved approval still lists BOTH monitors |
| 11.15 | Unplug a shared monitor, capture | refused (TIMEOUT: monitor missing), the grant is kept; replug → works again |
| 11.16 | **Stale approval**: delete the approval from the permission store, then capture | no dialog stays up (the restore's dialog is closed with `Request.Close`), result NOT_GRANTED, capture state DENIED; `request-scopes capture` then shows the dialog |
| 11.17 | CAPTURE only, no CONTROL (`request-scopes capture` in a fresh process) | a ScreenCast-only dialog (no input question); captures work; input actions not granted |
| 11.18 | CAPTURE after CONTROL (`request-scopes control`, then `request-scopes capture`) | a second, combined dialog that asks about input again; cancelling it keeps the CONTROL grant |
| 11.19 | `request-scopes capture+again` while granted | the dialog again; choose different monitors → the next capture shows the new set |
| 11.20 | Slow portal at login: capture immediately after login | a ScreenCast probe that times out is retried (log: `retrying in 10 s`), never a session restored without the screens |
| 11.21 | No file written | `inotifywait -m -r $HOME /tmp` during 100 captures without `--save`: nothing but the audit log line(s) |
| 11.22 | Audit | `displayxr-cli desktop audit --tail 5`: one `capture` record per burst with `count`, `w`/`h`, target, first/last — no pixel data |
| 11.23 | 22.04 Wayland session (box C) | CAPTURE works through the standalone ScreenCast session (RemoteDesktop v1 has no persistence); `request-scopes all` shows two dialogs there (consent-only remote desktop + screen cast) |
| 11.24 | **Parent window** (spec §10.2): put another window on top first, then request with the app's handle — `parent wayland:<handle>` / `parent x11:<hex>` before `request` / `request-scopes` in the smoke script, or `--parent=…` on the probe's `request` / `request-scopes`. A Wayland handle has to come from a live xdg-foreign export, so use the Unity player of displayxr-unity#385 (`displayxr_linux_window_handle`, logged by the plug-in); an X11 handle is `printf 'x11:%x' $(xdotool getactivewindow)` on an X11 / Xwayland window. **Quote the handle**: a mutter handle is 32 random printable ASCII characters and about half contain a SPACE (also quotes, `#`, `\`), anywhere including the ends. Shell: `--parent='wayland:<handle>'`; probe script: `--parent="wayland:<handle>"`; smoke script: `parent "wayland:<handle>"`. A handle that itself contains `"` (or to be sure of every byte): `--parent-hex=$(printf '%s' "$H" \| xxd -p -c 256)` on the probe, `parent-hex <hex>` in the smoke script. Both echo `parent window: [<handle>] (N bytes)` — check N is 8 + 32 = 40 for a mutter handle, so no space was lost | with the handle (a handle WITH spaces too — run it until one has a space; §11 run N1 failed exactly there): the dialog opens centred on (modal to) that window, above everything, and **no** `Failed to associate portal window with parent window ''` line in `journalctl --user -u xdg-desktop-portal-gnome`; for the combined `RemoteDesktop.Start` (`request-scopes all`) AND the ScreenCast-only `Start` (`request-scopes capture` in a fresh process). Without one (the old behaviour): that log line, dialog wherever new windows land. A stale handle (the exporting app closed, or a made-up `wayland:nonexistent`; an `x11:` handle in a pure-Wayland portal) ⇒ the portal logs that it cannot associate the parent, the dialog opens unparented, the request completes normally (no error, no hang). A malformed one through the probe (`--parent=hwnd:1`) ⇒ `XR_ERROR_VALIDATION_FAILURE`, no dialog |

Report per box: the `info` caps, pass/fail per row, capture times (min / median / max for
11.2), and the backend log for every failure.

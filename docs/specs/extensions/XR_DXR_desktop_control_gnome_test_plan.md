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
  `refused — lost the clipboard before the paste`; "OTHER" must never be pasted.
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

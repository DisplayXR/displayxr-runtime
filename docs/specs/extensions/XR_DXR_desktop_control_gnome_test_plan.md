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
written to disk by DisplayXR), and `desktop_backend_smoke` runs one command per process. So
for every input / window-op command below, set the bring-up knob, which makes the action
itself open the portal dialog — **you then have 15 s to click "Share"** in GNOME's dialog:

```bash
export DXR_DESKTOP_GNOME_PROMPT_ON_ACTION=1
```

(Inside an app the common layer calls `request_access` first and the session then restores
silently for the rest of the process — section 7 tests that path.)

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
sidebar row's name; `""` + `result: ok` over the desktop. Measure with `time`: < 50 ms after
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
appears in the top bar while acting and disappears ~2 s after the last command.

## 5. Keys — layout-aware chords

Focus gedit with some text.

```bash
$SMOKE key 1 a          # Ctrl+A: select all
$SMOKE key 1 c          # Ctrl+C
$SMOKE key 0 end
$SMOKE key 1 z          # Ctrl+Z
$SMOKE key 8 ""         # Super alone: the overview opens (Escape it)
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
$SMOKE request          # without DXR_DESKTOP_GNOME_PROMPT_ON_ACTION
```

- Click **Cancel** → `access: denied`. Click **Share** → `access: granted`.
- Inside an app (Unity / native, through the runtime): grant once; act; wait > 2 s (indicator
  goes away); act again → **no dialog** (silent restore with the in-memory token), indicator back.
- Lock the screen while a session is open, unlock, act again → still no dialog; the log shows
  `the portal closed the RemoteDesktop session (screen lock / revoked)` then `session restored`.
- Restart the app → the dialog comes back once (the token is never written to disk). With
  `DXR_DESKTOP_APP_ID` registered on xdg-desktop-portal >= 1.19 the dialog names that app.

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
```

Move specifically: move a window **across the boundary** between the DS1 and the laptop panel
(different scales) and to just below the top bar of the other monitor; the result must be exact
(one read-back correction allowed), Ubuntu's tiling popup must not stay up, and no window may
end up maximized by edge tiling. Activate a window that is entirely covered → `refused — no
visible part of the window to click`; a window on another workspace → the same.

## 9. Window ops on the app's own window

Needs an app with a window (the smoke tool has none): run a DisplayXR test app using the
extension (e.g. `cube_handle_vk_linux` under Wayland) with an agent harness, or the Unity
binding. Expected: `ACTIVATE` / `MINIMIZE` / `RESTORE` / `MOVE` go through WindowPlacement1
**without** the portal dialog; `MAXIMIZE` → unsupported; a resize → unsupported.

## 10. Extension-version matrix

| Extension | Expected caps (A/B) | Notes |
|---|---|---|
| 16 | `0x3f` | full |
| 15 | `0x20` | no list / pick / text → no other-window ops (WINDOW_ACTIONS is withheld); own window ops would work |
| <= 14, or disabled | `0x20` | WARN "older than version 16" / "not running" |
| any, on box C (22.04) | `0xf` with v16 | no libei: `request` shows the dialog once for observation consent, then closes the session |

Report per box: the `info` line, a pass/fail per section, and for failures the backend's log
lines (`XRT_LOG=info`).

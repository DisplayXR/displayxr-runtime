# XR_DXR_desktop_control — Win32 backend test plan

Bring-up checks for `src/xrt/auxiliary/desktop/u_desktop_win32.cpp`, driven with
`desktop_backend_smoke` (no OpenXR, **no safety layer**: no consent check, kill switch,
idle wait or target verification — it acts immediately). The backend was written and
MinGW-compiled on macOS; none of this has run on Windows yet.

## Setup

```bat
scripts\build_windows.bat build
set SMOKE=%CD%\build\src\xrt\auxiliary\desktop\Release\desktop_backend_smoke.exe
```

(`build_windows.bat` uses Ninja Multi-Config; if the path differs, `where /r build desktop_backend_smoke.exe`.)
Run from a **non-elevated** terminal unless a step says otherwise. Open **Notepad** and type a
line into it. Keep the terminal and Notepad side by side, both fully on-screen.

The smoke exe has no DPI manifest, so it runs DPI-*unaware*: that is deliberate. Every
coordinate it prints must still be **physical** pixels, because the backend pins a
per-monitor-v2 thread context around every geometry call. Run the geometry steps once more
with `set __COMPAT_LAYER=~ DPIUNAWARE` to force the point (docs/reference/dpi-awareness.md):
the output must not change.

## 1. Info, lock, idle

```bat
%SMOKE% info
```
Expect `backend=win32 caps=0x3f remembers_in_store=1 access=unknown locked=0 idle=<n> ms`.
Run it again without touching anything for 5 s: `idle` ≈ 5000+. Then
`timeout /t 15 & %SMOKE% info` and press Win+L during the wait (unlock afterwards): the
second `info` must print `locked=1`. (A UAC prompt up during the call must also give
`locked=1`.)

## 2. Consent prompt

```bat
%SMOKE% request
```
A top-most "DisplayXR" message box: *"desktop_backend_smoke wants to control your
computer. … The app says: "bring-up test" … To stop it, run: displayxr-cli desktop deny
"<smoke exe path>" (or 'displayxr-cli desktop stop' to pause every app)"*, Yes/No. Yes →
`access: granted`; No → `access: denied`. The terminal must keep running while the box is
up (it is on its own thread). Also try it while another app is in front: the box must
still be top-most.

- **No is the default, and the box ignores the first 500 ms.** Run `request` and press
  Enter (or Space, or click Yes) *immediately*, while still holding the key from launching
  the command: nothing happens. Wait a second, press Enter without touching the buttons:
  `access: denied` (No had focus).
- **Destroy with the prompt up.** Run `request` and leave the box alone. After 120 s the
  smoke prints `access: no answer in 120 s` and calls `destroy()`: the box must close by
  itself, the process must exit within ~1 s with no crash, no hang and no second line
  printed afterwards. (destroy() answers UNKNOWN, joins the prompt thread, and done()
  is never called after it returns.)

## 3. Window list

```bat
%SMOKE% list
```
Expect Notepad, the terminal, and other visible app windows top-most first (`z=0,1,…`),
with `app="Notepad.exe"` (exe base name), the UTF-8 title, `pid`, `state` (the terminal,
which is foreground, carries `0x1`), and `frame` = the **visible** frame (no 7-8 px invisible
border: compare with a screenshot / Spy++). Must NOT appear: `Progman`, `WorkerW`, the
taskbar(s), tool windows, cloaked windows (UWP apps suspended on another virtual desktop,
closed-but-cached Settings), zero-size windows. Minimize Notepad and list again: its entry
has `state` `0x2` and a frame at its **restore** position, not (-32000,-32000). Maximize it:
`0x4`. A borderless-fullscreen video/game: `0x8`. Note Notepad's `id` as `NP` below.

Unicode: rename a file to `тест 日本 😀.txt`, open it in Notepad — the title must print intact
(the terminal needs `chcp 65001`). A >255-byte title must be truncated without a broken
trailing character.

## 4. Pointer, pick, focus

Put the mouse over Notepad's text area, then `%SMOKE% pointer`: `pointer: (x,y)` and
`pick … window` with Notepad's entry. Over the desktop wallpaper → `desktop`; over the
taskbar / Start menu / tray overflow → `shell`. `%SMOKE% pick -100000 -100000` → `outside`.
`%SMOKE% focused` → the terminal's id (it is in front).

## 5. Text at a point

Hover Notepad's text area, read the pointer position, then `%SMOKE% text X Y`: the document
text (ValuePattern) or the element name. Over a button (Notepad's title-bar Close) → `Close`.
Over a browser password field: must print `""` (never the password). Over the smoke's own
terminal: works, never hangs. Each call should return well under 200 ms; if a call prints
`result: timeout`, record which app.

## 6. Window ops (on `NP`)

```bat
%SMOKE% op activate NP          → Notepad comes to front, result ok
%SMOKE% op minimize NP          → minimized, ok
%SMOKE% op restore NP           → back, ok
%SMOKE% op maximize NP          → maximized, ok
%SMOKE% op move NP 100 100 800 600
```
The move must put Notepad's **visible** top-left corner at exactly (100,100) and its visible
size at 800×600 (check with `list`). `op move NP 100 100` keeps the size. `op activate`
must work even though the terminal is foreground (foreground lock): run it 10× in a loop
alternating with clicking the terminal. Last: `op close NP` with unsaved text → Notepad's
"Save changes?" dialog appears, result `ok`. A bogus id (`op restore 12345`) → `window-lost`.

## 7. Input

Activate Notepad first and wait for the result before each step (the smoke has no
idle/focus checks: anything you do meanwhile is fair game).

```bat
%SMOKE% op activate NP && %SMOKE% type "héllo 日本 😀"
```
Caveat: the smoke uses `main()`, so on Windows its argv arrives in the ANSI code page, not
UTF-8. Unless the box has *Settings → Time & language → Administrative language settings →
"Beta: Use Unicode UTF-8"* enabled, non-ASCII arguments reach the backend mangled
(`result: failed — text is not valid UTF-8` is then the backend correctly rejecting them).
With that setting off, test `type "hello world"` here and the non-ASCII path through the
OpenXR API instead.

Notepad shows exactly `héllo 日本 😀` (the emoji is a surrogate pair). Repeat with a 2 000-char
string (several SendInput batches): every character arrives, in order.

```bat
%SMOKE% op activate NP && %SMOKE% key 1 a        :: ctrl+a selects all
%SMOKE% op activate NP && %SMOKE% key 0 end
%SMOKE% op activate NP && %SMOKE% key 3 left     :: ctrl+shift+left
```
Chord keys are the **character on the active layout** (u_desktop.h). Add French (AZERTY)
in Windows settings, switch Notepad to it (Win+Space), type `abc`, then:

```bat
%SMOKE% op activate NP && %SMOKE% key 1 z        :: Ctrl + the key LABELLED Z -> undo
%SMOKE% op activate NP && %SMOKE% key 1 a        :: select all (the key labelled A)
%SMOKE% op activate NP && %SMOKE% key 1 1        :: Ctrl+VK_1 (no Shift added for digits)
%SMOKE% key 1 bracketleft                        :: '[' is AltGr+5 on AZERTY
```
Expect: `key 1 z` undoes the typing (it must NOT close Notepad's tab, which is what a
US-position Ctrl+W would do); `key 1 a` selects all; the last prints
`result: unsupported — this keyboard layout has no key for that character (it needs AltGr)`
(no character in the message: privacy rule, u_desktop.h). `key 1 slash`
(Shift+`:` on AZERTY) must send Ctrl+Shift+that key. Switch back to US and repeat `key 1 z`:
undo again.

The keypad, lock, media and right-hand modifier names (spec §5). A key viewer that shows the
VK, the scan code and the extended flag (Microsoft's `Keyboard` demo, PowerToys Keyboard
Manager's "Select" box, or a `WM_KEYDOWN` logger) next to Notepad:

```bat
%SMOKE% op activate NP && %SMOKE% key 0 numpad7      :: types 7 — with NumLock OFF too (VK_NUMPAD7)
%SMOKE% op activate NP && %SMOKE% key 0 numpadenter  :: a line break; the viewer: VK_RETURN, extended (Num Enter)
%SMOKE% op activate NP && %SMOKE% key 1 numpadadd    :: Notepad zooms in
%SMOKE% key 0 pause                                  :: VK_PAUSE, scan 0x45, NOT extended (not Right Ctrl)
%SMOKE% key 0 numlock                                :: NumLock toggles (VK_NUMLOCK, extended)
%SMOKE% key 0 audiovolumeup                          :: the volume flyout
%SMOKE% key 0 mediaplaypause                         :: a playing video (Media Player) pauses
%SMOKE% op activate NP && %SMOKE% key 0 rightalt     :: with the Korean IME: 한/영 toggles
%SMOKE% key 0 rightsuper                             :: the Start menu (VK_RWIN)
```

```bat
%SMOKE% move X Y                   :: X,Y = a point in Notepad's text
%SMOKE% click X Y                  :: caret moves there
%SMOKE% click X Y left 2           :: double click selects a word
%SMOKE% click X Y right            :: context menu
%SMOKE% scroll 0 3                 :: with a long document: scrolls DOWN 3 notches
%SMOKE% scroll 0 -3                :: up
```

## 8. Mixed DPI (150 % + 100 %)

Two monitors, primary at 150 %, secondary at 100 % (and once the other way round, and once
with the secondary to the LEFT of the primary, so x < 0):

- `list` frames must match physical pixels on both monitors (a window maximized on a 150 %
  2560×1440 panel reports 2560×1440-ish, not 1707×960).
- `move X Y` to a point on each monitor: `pointer` reads back the same point (±2 px).
- `op move NP X Y 800 600` with X,Y on the *other* monitor: Notepad crosses the DPI
  boundary, gets `WM_DPICHANGED`, and must still land at the visible (X,Y). Its size may
  differ (the app rescales itself) — result must still be `ok`.
- Repeat `list` / `pointer` / `move` with `__COMPAT_LAYER=~ DPIUNAWARE`: identical numbers.

## 9. Elevated target (UIPI)

Start a second Notepad **as administrator** (`id` = `NPA`), keep the smoke non-elevated:

- `op activate NPA` → `ok` (foreground changes are allowed).
- then `type "x"` → `refused — target runs as administrator…`, nothing typed.
- `key 1 a` → `refused`.
- pointer over NPA: `click X Y` → `refused`; `scroll 0 3` → `refused`.
- `op move NPA …`, `op minimize NPA`, `op close NPA` → `refused` (or `ok` if Windows lets
  that particular call through — record which; nothing may claim `ok` without the effect).
- Run the smoke elevated: all of the above succeed.

## 10. Delivery guarantee (contract v2)

The smoke's guard always allows; its target is the focused window (text, keys) or the
window under the point (click, scroll), read when the command starts — unless pinned with
`target ID` in a script. Pinning a window that is NOT focused makes the re-check fire
deterministically. Use script mode (one process):

```bat
:: focus.txt — Notepad's id = NP, the terminal stays in front
target NP
type "should not appear anywhere"
key 0 enter
key 1 a
```
`%SMOKE% script < focus.txt` → each line prints `result: refused — focus moved away from
the target window`; nothing is typed into the terminal, no Enter is pressed, and no
modifier stays held (type in the terminal afterwards: plain lowercase letters).

- **Focus stolen mid-type (manual race).** In PowerShell, with Notepad in front:
  `$s = ('x' * 20000) + "`n" + 'END'; Start-Sleep 2; & $env:SMOKE type $s`, and during the
  typing Alt+Tab to another window (e.g. a second Notepad). Expect
  `result: refused — focus moved away from the target window (after N of 20002 characters)`,
  the first Notepad ends with N `x` and NO newline, the second window receives at most one
  32-character batch that was already in flight and never the Enter or `END`.
- **Pointer re-check.** `target NP` then `click X Y` with (X, Y) over the terminal:
  `refused — target no longer under the point`, no click. With (X, Y) on Notepad but the
  smoke's `move` landing elsewhere is not reproducible by hand; covered by the 2 px rule in
  code review.
- **Cross-process popup is not the target's dialog.** With Notepad pinned, open a window
  owned by Notepad from ANOTHER process (rare; e.g. a shell `IFileDialog` hosted out of
  process) — if you can produce one, typing into it must be refused. Notepad's own Find
  dialog (Ctrl+F, same process) must be accepted: `target NP`, open Find, `type "abc"` →
  `ok`, text lands in the Find box.

## 11. Spec version 3 (PROPOSED, spec §11) — held input

Through `desktop_control_probe` (the safety layer) against Notepad (`NP`) unless noted.

| # | Check | Expected |
|---|---|---|
| 11.1 | `act key-down target=NP keys=shift`, `act key-down target=NP keys=a`, wait 1 s, `act key-up target=NP keys=a`, `act key-up target=NP keys=shift` | one capital A (injected key-downs do not auto-repeat), Shift up afterwards (`GetAsyncKeyState(VK_SHIFT)` 0; typing by hand gives lowercase) |
| 11.2 | Alt held across Tabs: `key-down alt`, `key-down tab`, `key-up tab`, then the next Tabs (the switcher takes the foreground on Windows 10/11, so the target window no longer has focus: record what the next `key-down tab` answers), `key-up alt` | the switcher stays up and steps; releasing Alt switches; no Alt left down |
| 11.3 | ctrl+click in Explorer's file list: `act click … mods=ctrl` on three files | three selected |
| 11.4 | `button-down` / `move-pointer` × 2 / `button-up` on a Paint canvas | a polyline drawn through the stops |
| 11.5 | `key-down ctrl`, then `displayxr-cli desktop stop` | Ctrl released within ~100 ms |
| 11.6 | `key-down shift`, then type on the physical keyboard | Shift released at once; queued actions refused "held input was released (the user is active)" |
| 11.7 | `key-down shift`, nothing for 3 s; `key-down shift` with a `move-pointer` every second for 12 s | released at ~2 s; released at ~10 s |
| 11.8 | Elevated foreground (UIPI) and `key-down` | `REFUSED` (`target runs as administrator`), nothing pressed |
| 11.9 | Kill the probe with a key held | the key stays logically down until pressed physically (an OS limitation: record it) — at most until the user presses it |

### 11b. The shell target (§11.5)

| # | Check | Expected |
|---|---|---|
| 11.20 | `act click target=shell` on the Start button / a pinned taskbar icon | Start opens / the app launches |
| 11.21 | `act click target=shell` on the desktop (all windows minimized) | a click on the desktop (icon selected) |
| 11.22 | `act click target=shell` over Notepad | `REFUSED "the shell is not under the point (found …)"` |
| 11.23 | Click the taskbar first (the taskbar is the foreground), then `act key target=shell keys=super` | Start opens |
| 11.24 | Notepad in the foreground, `act key target=shell keys=super` | `REFUSED "an app window has keyboard focus (notepad.exe) …"` |
| 11.25 | The same with a Notepad target while the taskbar is the foreground | `REFUSED` (focus) — version 2's behaviour |
| 11.26 | `act type target=shell text="notepad"` with Start open | typed into Start's search (record whether Start's search is the foreground root and which class it reports) |

## What to send back

The full stdout of each step, the Windows build (`winver`), the per-monitor scale factors,
and for any `timeout` / unexpected `refused`, the target app. Highest-value first:
§10 delivery re-checks, §2 destroy with the prompt up, §6 activate under the foreground lock, §8 cross-DPI move, §9 UIPI, §5 timing, §7 AZERTY.

# XR_DXR_desktop_control — macOS backend manual test plan

Backend: `src/xrt/auxiliary/desktop/u_desktop_macos.m`. Driver:
`desktop_backend_smoke` (no OpenXR, **no safety layer** — it acts immediately, so run it
only on a machine you are sitting at). Spec: [`XR_DXR_desktop_control.md`](XR_DXR_desktop_control.md) §7,
interface: `src/xrt/auxiliary/desktop/u_desktop.h`, decision: ADR-056.

Every claim below is marked **[verified]** (observed on macOS 26.3, MacBook Pro 1512×982 pt
built-in display, 2026-10-16) or **[assumed]** (Apple documentation / widely reported
behaviour, not reproduced here).

## 0. Build

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
ninja -C build desktop_backend_smoke
S=build/src/xrt/auxiliary/desktop/desktop_backend_smoke
```

(zsh: pass coordinates as separate words, `$S pick 500 10`, not a quoted `"500 10"`.)

## 1. Who gets the permission (TCC attribution) — read this first

ADR-056 D2 says the OS consent belongs to **the app**. On macOS that is TCC's
*responsible process*, which is not always the process that calls the API:

| How the process is started | TCC client the grant / prompt names | status |
|---|---|---|
| CLI binary (`desktop_backend_smoke`) from Cursor's integrated terminal | **Cursor** (`com.todesktop.230313mzl4w4u92`): `AXIsProcessTrusted()` was already true because Cursor holds Accessibility in `/Library/Application Support/com.apple.TCC/TCC.db` (`auth_value=2`), and an Apple-Events prompt raised from that shell read *"“Cursor.app” wants access to control “TextEdit.app”"* | **[verified]** |
| CLI binary from Terminal.app / iTerm | the terminal app (`com.apple.Terminal`, …) | [assumed — same mechanism as the row above] |
| `.app` bundle launched by Finder / Dock / `open MyApp.app` | the app's own bundle id | [assumed — Apple: the responsible process of a launchd-launched app is itself] |
| `.app`'s inner binary run directly from a terminal (`MyApp.app/Contents/MacOS/MyApp`) | the **terminal**, not the app | [assumed — widely reported; this is the easy mistake when testing] |
| Unity / Unreal **editor** play mode | the editor's bundle (`com.unity3d.UnityEditor5.x`, `com.epicgames.UnrealEditor`) | [assumed] |
| Unity / Unreal **built player** launched normally | the player `.app` bundle | [assumed] |

Consequences for testing and for ADR-056:

- **Test the real attribution from a `.app` launched with `open`**, never from a terminal —
  a terminal that already holds Accessibility makes every check pass and proves nothing about
  the app (this machine's run is exactly that case).
- The grant is keyed by bundle id **and** code-signing requirement (`csreq`). A rebuilt,
  ad-hoc-signed app can silently lose its grant and must be re-approved [assumed]. Sign test
  builds with a stable identity.
- The prompt names the responsible app; the backend cannot relabel it, so `request_access`
  ignores `display_name` / `reason` (the app should explain *before* calling).
- An Accessibility grant also covers posting CGEvents (`kTCCServicePostEvent` is implied for
  Accessibility-trusted processes) [assumed; consistent with typing working here].

## 2. Observation (no permission needed for most; safe on any machine)

| # | command | expected | observed here |
|---|---|---|---|
| O1 | `$S info` | `backend=macos caps=0x7f remembers_in_store=0 access=granted\|denied locked=0 idle=N ms` (`0x3f` before spec v2, or on a macOS without ScreenCaptureKit) | `backend=macos caps=0x3f remembers_in_store=0 access=granted locked=0 idle=56047 ms` [verified, 2026-10-16, before capture]; `caps=0x7f` [verified, 2026-10-20] |
| O2 | `$S list` | layer-0 on-screen windows, front-to-back, `z` = index; bundle id as `app`; `FOCUSED` (0x1) on the key window; `MAXIMIZED` (0x4) on windows filling the visible frame; minimized windows of regular apps appended last with `0x2` and their last frame | 17 windows; Cursor's front window `state=0x1`; Photos/Slack/Chrome at `(0,33 1512x855)` `state=0x4` [verified]; a minimized TextEdit window listed last with `state=0x2` [verified] |
| O3 | `$S pointer` | pointer in top-left global points + its pick | `pointer: (1306,547)` → `window id=37 … title="displayxr-runtime"` [verified] |
| O4 | `$S pick 500 10` (menu bar) | `shell` | `shell` [verified] |
| O5 | `$S pick 1400 15` (menu-bar clock, Control Center) | `shell` | `shell` [verified] |
| O6 | `$S pick 756 960` (a Dock icon) | `shell` | `shell` [verified] |
| O7 | `$S pick 20 960` (Dock strip, no icon, wallpaper) | `desktop` (with AX); `shell` without AX (heuristic: outside every visible frame) | `desktop` [verified, AX granted] |
| O8 | `$S pick 700 400` (inside a window) | `window` + that window | `window id=37 …` [verified] |
| O9 | `$S pick -50 -50`, `$S pick 3000 500` | `outside` | `outside` ×2 [verified] |
| O10 | `$S focused` | id of the key window (0 if none) | `37` [verified]; `0` while the only candidate was minimized [verified] |
| O11 | `$S text 1400 15` | the clock's AX value | `"Fri Oct 16  11:49 AM"` [verified] |
| O12 | `$S text 756 960` | Dock item title | `"Photos"` [verified] |
| O13 | `$S text 500 10` | menu-bar item title | `"Window"` [verified] |
| O14 | `$S text X Y` over an Electron app with no AX tree | `""`, result ok | `""` over Cursor [verified] |

A system alert (UserNotificationCenter, window level 8) picks as `shell` and is what
`focused` reports while it is key (`focused: 676`), but it is not in `list` (layer ≠ 0)
[verified].

### Measured cost (warm, 100 calls each, AX + Screen Recording granted) [verified]

| call | mean | max | note |
|---|---|---|---|
| `list_windows` (17 windows) | 8.3 ms | 63 ms | `CGWindowListCopyWindowInfo` alone is 2.8 ms mean / 93 ms max on this machine |
| `pick` | 6.2 ms | 39 ms | CG walk + one AX hit test |
| `focused_window` | 0.95 ms | 14 ms | |
| `text_at` | 7.7 ms | 18 ms | |
| first `list_windows` of a process | ~0.8 s | | waits (≤ 1.5 s) for the AX refresher's first snapshot |

The `< 5 ms` target in `u_desktop.h` is not reachable for `list_windows` on macOS: the
window server call alone is ~3 ms. The common layer should not call it every frame.

## 3. Permission flow (needs a person: it shows system prompts)

Use a signed `.app` that links the backend (or the smoke tool from a terminal that does
**not** already hold Accessibility — check System Settings ▸ Privacy & Security ▸
Accessibility first).

| # | step | expected |
|---|---|---|
| P1 | `$S info` | `access=denied` |
| P2 | `$S list` | windows listed; titles of other apps **empty** if Screen Recording is not granted (no AX fallback without Accessibility) |
| P3 | `$S text 500 10` | `not-granted — Accessibility permission not granted` |
| P4 | `$S op activate <id>` / `$S type x` | `not-granted` — nothing happens on screen |
| P5 | `$S request` | Screen Recording prompt (once per responsible app), Accessibility prompt *"<app> would like to control this computer using accessibility features"*; the command keeps polling |
| P6 | Approve Accessibility in System Settings within 120 s | `access: granted` within ~250 ms of the toggle (the backend polls `AXIsProcessTrusted`, which reflects the change without a relaunch [assumed — not observed here because this machine was already granted]) |
| P7 | Let 120 s pass without approving | `access: denied` |
| P8 | Turn Accessibility **off** while an app is running, then `$S info` | `access=denied`; actions return `not-granted` |
| P9 | Screen Recording: grant it, then `$S list` in a **new** process | other apps' titles appear. A process that started before the grant may keep seeing empty titles until relaunched [assumed]; with Accessibility granted the backend fills titles from `AXTitle` anyway |

## 4. Actions (needs Accessibility; act ONLY on a window you opened)

Use a scratch document: `: > /tmp/dxr-dc-test.txt; open -a TextEdit /tmp/dxr-dc-test.txt`
(do not script TextEdit with `osascript`: that raises an Automation prompt — see §6).
Get its id with `$S list | grep TextEdit` → `W`. Confirm `$S focused` prints `W` before
every input step.

| # | command | expected | observed here |
|---|---|---|---|
| A1 | `$S type "desktop control test 你好 😀"` | text appears verbatim (CJK + emoji: surrogate pairs never split) | `$S text` over the document read back `"desktop control test 你好 😀"` [verified] |
| A2 | `$S key 8 a` (Cmd+A) | all text selected | AX `AXSelectedText` = the whole string [verified] |
| A3 | Cmd+A from a non-main thread in a process whose main queue is not serviced | works, ~300 ms slower (layout lookup times out on the main queue and runs locally) | `r=0`, 340 ms [verified] |
| A4 | `$S key 8 f21` | `unsupported — key "f21" has no macOS key code` | [verified] |
| A5 | AZERTY: switch input source to French, `$S key 8 z` in the doc | the key labelled Z is pressed (undo) — not the US-Z position (W) | not run (single layout on this machine) |
| A5b | `$S key 0 numpad7`, `$S key 0 numpadenter`, `$S key 8 numpadadd` in the doc | `7` typed (kVK_ANSI_Keypad7, numeric-pad flag); a line break; zoom in | not run yet (added with the keypad names) |
| A5c | `$S key 0 pause`, `numlock`, `scrolllock`, `mediaplaypause`, `audiovolumeup`, `rightalt`, `rightsuper` | `unsupported — this key has no macOS key code` (no key code for them on macOS) | not run yet |
| A6 | `$S op move W 300 200 700 450` then `$S list` | `ok`; list shows `(300,200 700x450)` immediately | [verified] |
| A7 | `$S op move W 320 220` | `ok`, size kept | `(320,220 700x450)` [verified] |
| A8 | `$S op maximize W` | fills the visible frame (below the menu bar, above the Dock), `state` has `0x4`; **not** a full-screen Space | `(0,33 1512x854)` `state=0x5` [verified] (TextEdit rounds its height to 854 of 855) |
| A9 | `restore` after `maximize` **in the same process** | previous frame back | `(300,200 700x450)` [verified]. Across two smoke-tool runs RESTORE only unminimizes / leaves full screen: the pre-maximize frame lives in the backend instance [verified] |
| A10 | `$S op minimize W`, `$S list` | `ok`; window listed last with `state=0x2` and its last frame | [verified] |
| A11 | `$S op restore W` | back on screen | [verified] |
| A12 | `$S op activate W` with another app frontmost | `ok`, `$S focused` = W within 500 ms; else `refused — the window did not take focus within 500 ms` | `ok`, `focused: 685` [verified] |
| A13 | `$S click X Y` inside the doc, twice quickly | caret moves; the second click is a double click (word selected) | posted ok [verified]; selection effect not read back |
| A14 | `$S scroll 0 3` over a long document | scrolls **down** 3 lines | posted ok [verified]; direction not checked on a long doc (natural-scrolling interaction [assumed: synthetic line events are not inverted]) |
| A15 | `$S move X Y` | pointer lands within 2 pt | [verified] |
| A16 | `$S op close W` | `ok`; window gone (a dirty untitled doc shows a save sheet: it lists with `parent=W`) | `ok`, gone; the `.txt` had been autosaved [verified]; sheet/`parent_id` not exercised |
| A17 | `$S op activate 999999` | `window-lost` | not run |

## 5. Known limitations (by design or OS)

- **Click-through overlays**: CGWindowList cannot tell an `ignoresMouseEvents` window from a
  normal one. With Accessibility the AX hit test wins (it follows the window server's mouse
  routing); without it, pick reports the overlay. Our **own** windows on the main thread are
  checked through AppKit (`ignoresMouseEvents`) instead.
- **AX to our own process from the main thread** would block until the messaging timeout
  (our AX server runs on that thread). The backend never does it: focus uses `NSApp.keyWindow`,
  `text_at` over our own window returns `""` with a message, and `pick` trusts CG there.
- **Electron apps** (Cursor, Slack, …) often expose no AX tree: `text_at` returns `""`, and the
  system-wide `AXFocusedApplication` fails (`kAXErrorCannotComplete`, verified) — focus then
  falls back to `NSWorkspace.frontmostApplication`, which is notification-fed and can be stale
  in a process that never runs its main run loop (fine in an app).
- **Unresponsive apps** (System Settings answered nothing for 150 ms, verified) are skipped by
  the AX refresher for 10 s: their titles fall back to CG names, sheets/minimized state may lag.
- **Windows on other Spaces** are not in AX's `AXWindows`: window ops on them fail with
  `failed — window not reachable through Accessibility`.
- Without Accessibility and with the Dock visible, pick on the bare Dock strip says `shell`
  (heuristic: outside every visible frame).
- `printscreen`, `f21`–`f24`, `pause`, `numlock`, `scrolllock`, the media keys and the
  right-hand modifiers (`rightctrl` …): no macOS key code here → `unsupported`. Media keys are
  system-defined events (`NX_KEYTYPE_*`), not key codes, and a right-hand modifier's key-down
  would need its device-specific modifier flag; both are possible later, untested now.

## 6. Incident note from the automated run (2026-10-16)

The first attempt opened the scratch document with `osascript -e 'tell application
"TextEdit" …'`. That raised a TCC **Automation** prompt ("“Cursor.app” wants access to
control “TextEdit.app”") on the unattended screen; it was not answered, `osascript` timed out
(-1712), and TCC recorded a **deny** for `kTCCServiceAppleEvents`, client
`com.todesktop.230313mzl4w4u92` (Cursor) → `com.apple.TextEdit`. Nothing else changed. To
clear it: System Settings ▸ Privacy & Security ▸ Automation ▸ Cursor, or
`tccutil reset AppleEvents com.todesktop.230313mzl4w4u92` (resets all of Cursor's
Automation answers). This is why §4 opens the document with `open -a` instead.

## 7. Screen capture (spec v2 §10, ADR-056 Amendment 1)

Backend: `u_desktop_macos_capture.m` (ScreenCaptureKit), geometry in
`u_desktop_macos_logic.c` (unit-tested on every OS: `tests_desktop_control "[macos]"`).
Capture injects nothing, so it is safe to run while someone uses the Mac — but it needs
**Screen Recording** for the responsible process (§1: the terminal / IDE for the smoke tool
and the probe, the `.app` for a real app). Check before running anything:
`CGPreflightScreenCaptureAccess()` (a three-line C program, or `$S request-scopes capture`
only if you accept a prompt). The backend never prompts from a capture.

Saved images (`$S capture … OUT.ppm`, the probe's `--save`) can contain private screen
content: keep them in a scratch directory, never attach or post them, delete them after.
Statistics (`withheld px`, sizes, timings) are enough for every check below.

```bash
$S info                                   # caps=0x7f
$S monitors                               # global points, top-left of the primary display
$S capture 0 0 3432 1080                  # whole desktop: WxH device px, withheld = layout gaps
$S capture 100 100 400 300                # region on the 2x built-in display: 800x600
$S script <<'X'                           # warm timings + the macOS 12.3-13 path
capture 100 100 400 300
capture 100 100 400 300
sck-stream on
capture 100 100 400 300
X
# Through OpenXR, with the safety layer (in-process runtime + sim-display from the build dir):
XRT_FORCE_MODE=native XRT_PLUGIN_SEARCH_PATH=build/src/xrt/drivers \
XR_RUNTIME_JSON=build/openxr_displayxr-dev.json desktop_control_probe script caps.txt
#   caps.txt: request-scopes capture / capture desktop / capture region 100 100 400 300 /
#             capture window <id from `list`> max=640x640 / capture region 5000 5000 100 100
```

| # | check | expected | observed here (macOS 26.3, MBP 1512×982 pt 2× + external 1920×1080 pt 2×, 2026-10-20) |
|---|---|---|---|
| C1 | `$S info` | `caps=0x7f` | `caps=0x7f` [verified] |
| C2 | `$S monitors` | one rect per display, mirrors left out | `(0,0 1512x982)`, `(1512,0 1920x1080)` [verified] |
| C3 | desktop capture | the bounding box at the densest scale; the strip under the shorter display is withheld | `6864x2160`, `592704 withheld px` = 1512 × 98 pt × 4 exactly [verified] |
| C4 | region on one display | region × that display's backing scale | `800x600` for 400×300 pt on either display [verified] |
| C5 | region across both displays, over the gap | gap pixels alpha 0 | `1400 900 300 150` → `600x300`, `30464 withheld px` = 112 × 68 pt × 4 exactly [verified] |
| C6 | geometry: a region vs the same rect cut from a desktop capture, and vs `screencapture -x -R` | identical | byte-identical (mean \|Δ\| 0.00) for both; shifting by 1 pt gives \|Δ\| ≈ 5 [verified] |
| C7 | timings | well under the 2 s bound | region ~120 ms warm, ~270 ms first in a process; desktop ~280 ms warm; through OpenXR (probe) desktop 1.1 s first, region 0.2–0.35 s [verified] |
| C8 | `sck-stream on` (the one-shot `SCStream` path of macOS 12.3–13) | same image | byte-identical to `SCScreenshotManager`; region 165–215 ms [verified] |
| C9 | the app's own window | alpha 0 over its whole frame | a 200×120 pt window of the capturing process → exactly `96000 withheld px` (= 400×240 px), no window pixel visible, for `NSWindowSharingReadOnly` and `None` alike [verified] |
| C10 | **another app's `NSWindowSharingNone` window** (decision 8) | not shown | macOS 26.3: CGWindowList reports it with `kCGWindowSharingState` 0 to other processes; raw ScreenCaptureKit (`captureImageInRect`) shows **none** of its pixels (what is beneath shows); the backend withholds its whole frame (`96000 withheld px`), 8/8 runs with the window up; the very first run, the freshly built test binary's first launch, captured `0 withheld px` with the raw grab also showing no window pixels — most likely before the window was on screen [verified]. A `ReadOnly` window of the same app is captured normally [verified]. **Open: macOS 14 and 15** |
| C11 | probe: `request-scopes capture` with Screen Recording granted | `PENDING` → `GRANTED`, no prompt | as expected [verified] |
| C12 | probe: `capture window <id>` / `max=640x640` / unknown id / region off every display | the window's frame as on screen / scaled down / `WINDOW_LOST` / `REFUSED "found nothing: outside every monitor"` | `1840x928` from `(447,76 920x464)`; `640x323`; `WINDOW_LOST`; `REFUSED` [verified] |
| C13 | lock the screen, capture | `refused — screen locked` | `$S capture` while the screen was locked: `refused — screen locked`; `monitors` still answered from the online displays [verified] |
| C14 | Screen Recording **not** granted, `$S capture …` | `not-granted — Screen Recording permission is not granted`, **no prompt** | not run (this terminal holds the grant) |
| C15 | revoke Screen Recording in System Settings while a probe session runs, capture | `NOT_GRANTED`, CAPTURE `DENIED` (event); later `request-scopes capture` does not prompt again | not run |
| C16 | `request-scopes capture+again` in the probe | System Settings opens on Privacy & Security ▸ Screen Recording; answered at once when already granted | not run (would open System Settings on a working screen) |
| C17 | the menu-bar screen-recording indicator | shows during a capture burst | not observed (no one looking at the screen) |
| C18 | macOS 15's periodic "continue to allow screen recording" confirmation (decision 8) | record whether it fires for stills | not observable on 26.3 in one session; open on 15 |
| C19 | a non-Retina (1×) external display | region × 1 on it; a region spanning 1× and 2× is ×2 with the 1× part sampled up | not run (both displays here are 2×); the geometry is pinned by the unit test |
| C20 | macOS < 12.3 (or without ScreenCaptureKit) | runtime loads, `caps=0x3f`, CAPTURE `UNSUPPORTED` | not run; `otool -L` shows ScreenCaptureKit as `LC_LOAD_WEAK_DYLIB` [verified] |

## 8. Spec version 3 (PROPOSED, spec §11) — held input

Needs Accessibility, like §4. Through `desktop_control_probe` against TextEdit unless noted.

| # | check | expected |
|---|---|---|
| H1 | `act key-down keys=shift`, `act key-down keys=a`, wait 1 s, `act key-up keys=a`, `act key-up keys=shift` | one capital A (record whether a posted key-down auto-repeats), Shift up afterwards |
| H2 | cmd+click in a Finder list: `act click … mods=super` on three files | three selected (the click events carry the Command flag) |
| H3 | `act key-down keys=super`, `act key-down keys=tab` × 3 (key-up between), `act key-up keys=super` | the Command-Tab switcher steps and switches |
| H4 | `button-down` / `move-pointer` / `button-up` in a drawing canvas | a drag through the stops |
| H5 | ctrl+scroll (`act scroll … sy=-3 mods=ctrl`) with Accessibility zoom on | the screen zooms (the scroll events carry the Control flag) |
| H6 | kill switch / lock / user typing / 2 s idle / 10 s while a key is held | released each time (as the Windows plan §11) |
| H7 | `act click target=shell` on a Dock icon and on the menu bar (Apple menu) | the app launches / the menu opens |
| H8 | `act click target=shell` over a window | `REFUSED "the shell is not under the point (found …)"` |
| H9 | `act key target=shell keys=super+space` | `FEATURE_UNSUPPORTED` (keys for the shell are not supported on macOS) |

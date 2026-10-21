---
status: Proposed
date: 2026-10-16
deciders: David Fattal
issues: []
related: [ADR-019, ADR-040, ADR-046, ADR-042, ADR-054]
---

# ADR-056: Desktop control is a first-class capability that runs in the app — the OS's consent belongs to the app, never to DisplayXR

**Status:** Proposed (2026-10-16) · siblings: [ADR-046](ADR-046-depth-aware-cursor.md)
(depth-aware cursor: the agent's pointer reuses it), [ADR-040](ADR-040-rear-depth-budget.md)
(what is behind the window: the avatar stays out of the way), [ADR-042](ADR-042-vendor-2d3d-conversion-supersedes-default.md)
(lift: "show this in 3D") · extension spec: to be written (`XR_DXR_desktop_control`, working name).

## In one paragraph

AI-agent apps on spatial displays — a desktop avatar that is also an assistant — want to
operate the computer: find a window, read the text under the pointer, type into another app,
move a window onto the 3D panel. Every desktop OS except Windows guards this behind a
**per-app user consent** (GNOME/KDE: the RemoteDesktop portal; macOS: Accessibility and Screen
Recording). Today each app works around this on its own. This record makes desktop control a
first-class DisplayXR capability, built on one fact: **the runtime is loaded inside every
DisplayXR app's process**, so when the runtime asks the OS for control, the OS asks the user
about *that app*, by name. DisplayXR provides one API on every OS, the per-OS backends, and a
uniform safety layer; it never holds the permission itself and never offers it to other
processes. MCP tools stay inbound only (agents operate an app), never a route to desktop
control.

## Context

### The ask

A desktop-companion app (a transparent 3D avatar driven by a realtime voice agent) ported from
Windows to GNOME Wayland. Its agent calls the OS to:

- list windows, find a window by title, find the window and the UI text under the pointer;
- activate, move, maximize, minimize and close other apps' windows;
- type text (any language) and send key presses and clicks into other apps.

On Windows, all of this is plain Win32 (`SetWindowPos`, `SendInput`, UI Automation) from any
unelevated process. On GNOME Wayland none of it is available to a client. A prototype got
everything working without new privilege: the RemoteDesktop portal + libei for input (the user
approves once, then sessions restore silently), simulated user gestures for other windows'
geometry, AT-SPI for text, and three **read-only** additions to the DisplayXR GNOME extension
(`GetPointer`, `GetWindowAt`, `GetAllWindows`, extension v16, runtime v2.46.0). It works, but
as app-side glue: a Python helper embedded in C#, re-done by every app that wants it.

### Why DisplayXR must not just "do it" in a shared component

Two DisplayXR components could technically control any window:

- the **GNOME extension** runs inside the compositor (mutter), where every window is reachable;
- the **runtime's MCP servers** answer any process of the same user, with no authentication.

Either one acting on other apps' windows would let **any program the user runs** ask DisplayXR
to type into a banking tab or click "Send", and the OS would never ask the user — because the
request would arrive through a component the user never approved for that program. The OS
consent prompt asks "do you trust *this app* to control your computer?"; a shared DisplayXR
service answering yes for everyone defeats the question, on every machine DisplayXR is
installed on. That is why the extension acts only on the caller's own window (PID from the bus
daemon, v15) and is otherwise read-only (v16), and why this record exists.

### The fact that makes a first-class version possible

The OpenXR runtime is a shared library loaded **into the app's process** (`_handle`, `_texture`,
`_hosted`; the Unity and Unreal plug-ins load it directly). A RemoteDesktop portal session the
runtime opens is attributed to the app's `.desktop` id; a TCC prompt the runtime triggers on
macOS names the app's bundle. The permission lands exactly where the OS intends it, with no
broker in between.

## Decision

### D1. One cross-platform API, implemented in the runtime, in the app's process

A new extension, `XR_DXR_desktop_control` (working name), offers:

| group | operations |
|---|---|
| **observe** | list top-level windows (id, pid, app id, title, frame, monitor, state, z-order); window at a point / under the pointer; pointer position; UI element text at a point (UIA / AT-SPI / AX); capture a window |
| **windows** | activate, move/resize, minimize/restore, maximize/restore, close, move to a screen (incl. the 3D panel) |
| **input** | type text (IME-safe: committed as text, never as key codes), key chords, pointer move/click/drag/scroll |

Every operation is asynchronous and reports a result; every mutating operation names its
**target window** and is refused if that window is not where the call expects it (D3).

### D2. The OS's own consent, attributed to the app

| OS | backend | consent |
|---|---|---|
| Windows | Win32 + UI Automation + `SendInput` | none from the OS → **DisplayXR's own first-run prompt**, naming the app (D3) |
| GNOME / KDE (Wayland) | RemoteDesktop portal + libei; window list / pointer / pick from the DisplayXR GNOME extension (read-only, v16); AT-SPI | portal dialog, remembered per app id; the OS's remote-access indicator while a session is open |
| X11 sessions | XTest / EWMH | DisplayXR's own prompt (as Windows) |
| macOS | AX API + `CGEvent` + ScreenCaptureKit | Accessibility + Screen Recording, granted to the app bundle in System Settings |

The runtime never stores or forwards a grant. A session belongs to the process that opened it
and dies with it. *(Amendment 2, proposed: on GNOME the runtime keeps the portal's restore token
for the same app across launches, in a private file of that app's; it never forwards it.)*

### D3. A DisplayXR safety layer, identical on every OS

The OS prompts differ in strength (Windows has none). DisplayXR evens them out:

1. **Visible while acting.** An indicator (the OS's where it exists, DisplayXR's own overlay
   otherwise) is shown only while an operation runs; sessions open per action and close after
   a short idle (the prototype: 2 s).
2. **A kill switch.** One global hotkey stops every agent session on the machine and refuses
   new ones until the user re-enables it.
3. **Never on the lock screen**, never while a secure desktop / UAC prompt is up.
4. **Target verification.** Before each click or keystroke: the pick at that point is the
   intended window (the extension's `GetWindowAt` on GNOME, `WindowFromPoint` on Windows, AX
   hit-test on macOS), and for typing, that window has focus. A failed check drops the queued
   input rather than sending it somewhere else.
5. **No focus theft while the user types.** Activating a window without recent input to the
   requesting app degrades to "demands attention" (closes the v15 open question the same way).
6. **Per-app switch + audit log.** The Dashboard lists apps that used desktop control, with an
   off switch per app; every mutating operation is logged locally
   (`displayxr-cli audit desktop`).

### D4. MCP is inbound only

App-registered MCP tools (`XR_DXR_mcp_tools`) let agents operate **an app**. They must not
re-export desktop control: the per-app MCP endpoint answers any local process, so doing so
would hand the app's grant to all of them — the exact hole D2 closes. The runtime will not
expose desktop-control operations as MCP tools, and the app-authoring rules
(`docs/guides/displayxr-app-rules.md`) gain a rule saying apps must not either. Should a future
case need it, it requires per-client authorisation in the MCP framework first and its own ADR.

### D5. The GNOME extension stays read-only for other windows

No new extension method mutates another app's window. Other-window geometry on GNOME goes
through the app's own portal session (user gestures today). If a cleaner primitive is wanted
later, it must be gated on proof that the caller holds an approved RemoteDesktop session — see
Open questions.

## Spatial-display use cases (why this is DisplayXR's job, not a generic library's)

1. **The agent points in depth.** "Click here": a pointer floats over the real button at the
   right disparity (ADR-046 cursor machinery), so tutoring and "show me how" work on the panel.
2. **The avatar stays out of the way.** The runtime already measures what is behind the
   transparent window (ADR-040). The avatar can step aside from the window being acted on or
   worked in.
3. **Presence-aware agents.** Eye tracking says whether someone is in front of the panel: speak
   when looked at, pause when the user leaves, never act on an empty room.
4. **"Show this in 3D."** Move a window, photo or video to the spatial display and lift it
   (ADR-042), so 2D content becomes 3D on request.
5. **Assistants across OSes** — "open my last report, summarise it, reply" from one codebase on
   Windows, GNOME and macOS.
6. **Accessibility** — voice control of the whole computer.
7. **Kiosks and demos** — scripted agents on a spatial display.
8. **Test automation** of DisplayXR apps and 3D content, in CI, on every OS.

## Consequences

- Apps stop shipping per-OS glue; the prototype's helper becomes runtime code (C, libei, D-Bus,
  AT-SPI) with one API for Unity (`DisplayXRDesktop`), Unreal and native apps.
- The consent story is the product story: "DisplayXR apps can operate your computer only with
  your permission, visibly, with one off switch." It has to be told that way from day one.
- Windows gains a prompt it never had. Apps that already call Win32 directly keep working; the
  extension is the supported path, not an enforcement point.
- Per-OS maintenance: GNOME gesture emulation depends on default keybindings and timing (three
  races were found and fixed in the prototype). Owned and tested by DisplayXR instead of each
  app.

## Phasing

1. **P0 — Spec.** `XR_DXR_desktop_control` header + spec; app-rules entry for D4.
2. **P1 — Windows + GNOME backends**, safety layer items 1–4, Unity binding. GNOME reuses the
   prototype's measured behaviour (IME-safe clipboard typing, libei absolute pointer on mixed
   scales, gesture-based window geometry with read-back).
3. **P2 — Dashboard switch + audit log (D3.6), focus policy (D3.5)**, the depth-aware agent
   pointer.
4. **P3 — macOS backend**, Unreal binding.

## Open questions

1. **A cleaner other-window primitive on GNOME.** Can the extension verify that a caller holds
   an approved RemoteDesktop session (mutter knows sessions exist, not which client opened
   them)? Until then, gestures.
2. **Flatpak/Snap apps.** Portal attribution works; the extension's PID gate does not (the
   proxy's PID). Observe-only via the extension may need the portal's app id instead.
3. **Windows prompt UX.** A runtime-drawn first-run dialog in a `_hosted` vs a `_handle` app;
   where the remembered grant is stored (per user, per app id).
4. **Service-mode (IPC) apps.** The client library is still in the app's process, so D2 holds;
   confirm nothing routes desktop control through `displayxr-service`.

## Amendment 1 — Screen capture is its own scope, in the same session (ACCEPTED, 2026-10-20)

**Status:** Accepted — spec version 2. Implemented: the common layer, the fake backend, the
probe, the CLI, GNOME (P0b + the GNOME half of P1b) and macOS (ScreenCaptureKit); Windows
capture is still to do and reports no CAPTURE capability until then. Spec text:
[`XR_DXR_desktop_control.md` §10](../specs/extensions/XR_DXR_desktop_control.md#10-screen-capture-spec-version-2).

**Why an amendment and not a new ADR.** D1 already lists "capture a window" under *observe*,
D2 already names ScreenCaptureKit and Screen Recording for macOS, and spec §8 deferred capture
from v1. Capture runs in the app's process, under the app's consent, behind the same safety
layer, kill switch and audit log. A new ADR would repeat D2–D4. What is new is a second
consent **scope**, a path that hands pixels to app code, and one new GNOME dependency
(PipeWire). Those are recorded here.

### Context: the white flash

The first app to use desktop control, a desktop-avatar agent on GNOME Wayland (3DLuma), lets
its agent see the screen through the xdg **Screenshot** portal with `interactive=false`. Every
call plays gnome-shell's white screenshot flash. The cause is in xdg-desktop-portal-gnome, not
in the app. On `main` (read 2026-10-19), the non-interactive path builds a `ScreenshotDialog`
whose constructor calls `do_take_screenshot()` at once. That calls
`org.gnome.Shell.Screenshot.Screenshot(include_pointer, TRUE, "Screenshot")`, and the second
argument is `flash`. `permission_store_checked` only skips *showing* the dialog. It does not
change the call. `ScreenshotWindow` and `ScreenshotArea` also pass `flash = TRUE`. Nothing an
app passes turns the flash off. **Confidence: high** for current xdg-desktop-portal-gnome,
because this was read from the source. **Medium** for the older builds that ship on Ubuntu
22.04 and 24.04, which were not checked line by line. The Screenshot portal has two other
problems. It also returns a **file URI**: gnome-shell writes the frame to disk before the app
reads it. And it is a **second consent**, separate from the RemoteDesktop approval that
desktop control already holds.

### D6. Capture is a capability and a consent scope of desktop control

- A new capability bit `CAPTURE`, and a new **scope** next to the v1 one. `CONTROL` covers
  everything in v1. `CAPTURE` covers screen pixels. Each scope has its own state
  (`XrDesktopControlStateDXR` values), its own state-changed event and its own store entry.
  `CAPTURE` does not require `CONTROL`, so a "look at my screen" assistant can ask for pixels
  only. Targeting a window by id still needs an id, and ids come from the window list, which
  is under `CONTROL`.
- **The API is still frames on request.** The app asks for one frame of a desktop region,
  a window's on-screen frame, or the whole desktop, and gives a maximum size. The result comes
  back asynchronously. The image is RGBA8 sRGB, top-down and tightly packed. It is downscaled
  to fit the maximum size and never upscaled. It comes with the source rectangle in desktop
  coordinates, so the app can map an image pixel back to a click point. Pixels the runtime
  withholds have alpha 0: off every screen, on a screen the user did not share, inside an
  excluded window, or inside one of the app's own windows (decision 7). **Latest-frame semantics:** the frame is no older than the request. At
  most one capture is in flight per session, and the runtime keeps at most one result per
  session until the app fetches it, the next capture completes, or 10 s pass.
- **No continuous stream reaches the app.** No PipeWire node or fd, no `IDXGISurface`, no
  `SCStream`. A backend may keep its OS stream open between stills in a burst, because that is
  how GNOME delivers frames, but the app only ever receives stills. Starts are capped at 4 Hz
  per session, and a faster request waits. This keeps the work bounded and keeps "stills"
  true. If an agent ever needs live video, that needs its own ADR.

### D7. Consent: the app asks for its scopes; the runtime never prompts on its own

`xrRequestDesktopControlDXR` takes the scopes in a chained struct. Without the struct, the
request is for `CONTROL` only, which is the v1 behaviour. The runtime prompts only from that
call and never from `xrCaptureDesktopDXR`, keeping the v1 rule that a prompt follows a user
gesture. Per OS:

| OS | what the user sees | asking for CAPTURE after CONTROL |
|---|---|---|
| GNOME | **One** portal dialog for both: a screen picker plus "allow remote interaction". The approval is remembered for the life of the process (the restore token stays in memory, as in v1; Amendment 2 proposes keeping it per app across launches). | A new dialog. A restore token cannot be widened, so the new combined session asks about input again. **Apps that will capture should ask for both scopes at once.** |
| Windows | DisplayXR's existing consent MessageBox, with text that names the scopes asked for ("see your screen", "type and click in other apps"). The answer is stored per scope. | A second, capture-only prompt. |
| macOS | Screen Recording (TCC). v1 already requests it at `CONTROL` time (for window titles), so it is usually already granted. | Usually no prompt. |

On GNOME the user can untick remote interaction, or share only some monitors. The `Start`
response says what was granted (`devices`, `streams`), so `CONTROL` can be `DENIED` while
`CAPTURE` is `GRANTED`, and a region outside the shared monitors comes back as alpha 0 or is
refused.

**The dialog opens on the app's window.** A request may name the app window the consent
dialog belongs to (`XrDesktopControlParentWindowDXR`, decision 9). On GNOME it is the portal's
`parent_window`, so the dialog opens centred on and modal to that window. Without it the
dialog opens wherever new windows land, which for an agent app that is fullscreen or kept on
top can be behind it, and the user never sees the question (seen on a Lenovo user's machine).

**Honesty about strength.** On Windows, and on macOS once Screen Recording is granted, an app
could capture through the OS directly. There, the `CAPTURE` scope is a DisplayXR contract, not
an OS enforcement point, which is the same as v1's Windows prompt. It gives the user a visible
"yes", the kill switch, the audit log and the same behaviour on every OS. On GNOME it is
enforced: the portal is the only consented path an app has.

### D8. Privacy rules for capture

1. **Frames never touch disk.** The runtime does not write, cache or log any pixel. Frames
   live in process memory until the app fetches them or they expire. The only file this
   feature can ever produce is one the app writes itself (the probe does this behind an
   explicit flag).
2. **The audit log records the event, not the content.** One record per capture *burst*
   (closed after 2 s idle, like the GNOME session): `action: "capture"`, the target (region, or
   the window's app and title), output size, count and result. It never stores pixels,
   thumbnails or hashes, because a hash is a fingerprint.
3. **Capture-excluded windows stay excluded.** Where the OS honours an exclusion, the runtime
   uses only capture APIs that honour it. On Windows, Windows.Graphics.Capture respects
   `WDA_EXCLUDEFROMCAPTURE`. GNOME has no OS-level exclusion. DisplayXR's own
   `CaptureExclusion1` (wayland-window-geometry spec §6) hides windows from *off-screen*
   paints, but a portal **monitor** stream copies the on-screen view and still contains them
   (§6.2 "Scope"). So the GNOME backend **masks** every window that the extension's
   `GetAllWindows` reports as `capture_excluded`: its whole frame comes back as alpha 0, even
   where another window overlaps it. When the mask cannot be computed, for example when the
   extension is missing, the capture is refused, because a frame that might contain an
   excluded window is never returned. The runtime never tries to recover excluded content.
4. **Locked screen means refused.** The same applies on a secure desktop or UAC prompt, and
   with the kill switch on. These are checked when the capture starts and again before the
   frame is handed over. A frame finished just as the screen locks is dropped.
5. **D4 applies.** Apps must not re-export capture over MCP or any other IPC. The app-rules
   entry for D4 names capture explicitly.
6. **An OS indicator is visible during capture on every OS:** GNOME's screen-sharing indicator
   (shown while the portal session is open), Windows.Graphics.Capture's yellow border (kept
   on, because it is the indicator Windows otherwise lacks, D3.1), and macOS's
   screen-recording indicator in the menu bar.

### D9. Per-OS backends

| | GNOME Wayland | Windows | macOS |
|---|---|---|---|
| source | ScreenCast **inside the v1 RemoteDesktop session**: `CreateSession` → `SelectDevices` (`persist_mode` 2, `restore_token`) → **`ScreenCast.SelectSources`** (`types` MONITOR, `multiple` true, `cursor_mode` HIDDEN, **no** persist options, because the portal spec says persistence for a combined session belongs only to `SelectDevices`) → `RequestClipboard` → `Start` → `ConnectToEIS` + **`ScreenCast.OpenPipeWireRemote`**. CAPTURE without CONTROL uses a standalone ScreenCast portal session (persist options on `SelectSources`). | **Windows.Graphics.Capture**: `CreateForMonitor` per monitor the region touches (`CreateForWindow` is never used, so WINDOW means "what is on screen there" on every OS), a one-frame `Direct3D11CaptureFramePool` on the backend's **own** D3D11 device (never the app's device or queue), CPU read-back. | **ScreenCaptureKit** `SCScreenshotManager` (macOS 14+, one-shot, no stream; a one-shot `SCStream` on 12.3–13), one display `SCContentFilter` per display the region touches, excluding the app's own applications, `showsCursor` false. |
| pixels | PipeWire, **dlopen'd** (`libpipewire-0.3.so.0`, no `DT_NEEDED`, like libei). SHM buffers only (MemFd/MemPtr, BGRx/BGRA), so there is no DMA-BUF import and no GPU dependency. One stream per shared monitor, placed by the `Start` response's `position` / `size` (logical px; the stream is device px, which gives the scale). Mutter sends frames on damage, so with no new frame since the stream went live, the last frame *is* the current screen. | BGRA8 → RGBA8, crop, scale. | BGRA `CVPixelBuffer` in sRGB at the display's backing scale (SCK crops with `sourceRect`) → RGBA8, scale. |
| consent | the portal dialog (D7) | the DisplayXR MessageBox (D7), store per scope | Screen Recording TCC |
| exclusion | mask `capture_excluded` windows (D8.3) | OS (`WDA_EXCLUDEFROMCAPTURE`) | `NSWindowSharingNone` windows (`kCGWindowSharingNone` in CGWindowList) withheld over their whole frame by the runtime; ScreenCaptureKit itself also leaves them out (measured on macOS 26.3, spec §10.4) |
| not available | no `libpipewire` / no ScreenCast portal ⇒ no CAPTURE bit | DXGI Desktop Duplication: not used (one duplication per output per process, no indicator) | macOS < 12.3 (no ScreenCaptureKit, weak-linked) ⇒ no CAPTURE bit |

The **GNOME session lifetime** is unchanged. It is opened per burst and closed after 2 s idle,
and a capture counts as use. A capture that needs the session re-opened pays the silent
restore (0.3–9 s) plus the first PipeWire frame. If the shared monitor set changed since the
token was issued (hotplug), the portal prompts instead of restoring silently. v1 already
treats that as a revocation, so both scopes go `DENIED` and the app asks again.

Capture runs on its **own worker** per session, never behind the action worker. A 13 s
`TYPE_TEXT` must not delay a still, and a still must not run between an action's target
verification and its injection. It skips the idle wait: capture injects nothing, so it cannot
fight the user.

### Rejected alternatives

1. **The Screenshot portal (status quo).** It plays the white flash on every call (above),
   writes a file to disk, and needs a second consent unrelated to desktop control.
2. **Capture inside the DisplayXR GNOME Shell extension.** It would have no flash and no
   dialog. But the extension answers **every** process of the user over D-Bus, so any local
   program could silently capture the whole desktop. That contradicts the extension's rule
   that other apps' windows are read-only (runtime PR #2225, wayland-window-geometry spec
   §12.4) and D2. It is the same reasoning that keeps control out of the extension.
3. **`org.gnome.Mutter.ScreenCast` directly from the runtime.** It shows no dialog, and any
   session process can call it, which is how the Leia plug-in's Linux background capture
   works (`leia_mutter_capture_linux.c`: `RecordArea` over the 3D panel's rectangle). That
   case is **narrower and different**. It records only the panel area behind the app's own
   window, its pixels go to the display processor's compose-under-transparency pass and never
   to app code, and it needs `RecordArea` because only off-screen paints honour the capture
   exclusion. Desktop control hands pixels to the app's **agent**. Taking them through a path
   with no consent attributed to the app is exactly what D2 rules out. The DP's use is not a
   precedent.
4. **Handing the app a PipeWire node, fd or OS stream.** It would turn "a still on request"
   into covert video with nothing for the runtime to rate-limit, audit or cut off at the kill
   switch.
5. **Routing capture through `displayxr-service`.** Ruled out by D2 / open question 4: the
   grant would no longer belong to the app.

### Phasing (amends the list above)

- **P0b — spec v2 + header**, the fake backend's capture (a deterministic test pattern), the
  probe, and the store v2 migration.
- **P1b — GNOME** (the motivating case) **and Windows**.
- **P3b — macOS**, together with the P3 macOS backend.

### Decisions (DECIDED 2026-10-19, David)

These were open questions when the amendment was drafted. David accepted every proposed answer
on 2026-10-19.

1. **Capture works without CONTROL.** An app may ask for CAPTURE alone. On GNOME this costs a
   second code path, a standalone ScreenCast portal session, because the smaller grant should
   not cost the bigger one.
2. **Asking for CAPTURE later is allowed.** The app asks for its scopes and the runtime never
   prompts on its own. On GNOME a later CAPTURE request shows a second, combined dialog (a
   restore token cannot be widened). The spec tells apps to ask for both scopes at once.
3. **A WINDOW capture is the window's frame as it is on screen**, including anything
   overlapping it. That is the same on all three OSes and matches what the user sees and what
   a click would hit. There is no "occluded content" mode (Windows.Graphics.Capture
   `CreateForWindow`, an SCK window filter) in v2. GNOME cannot offer one without asking the
   user to pick that window.
4. **The limits are fixed in v2:** 4 capture starts per second per session, and the result is
   kept for 10 s. They are not store settings.
5. **Swapping woven 3D-panel pixels out is not in v2.** A capture over a spatial display
   returns the woven pixels that are scanned out. A future revision may put in the composed
   center view of DisplayXR windows. The runtime has that view for the app's own window, and
   the service has it for composed clients.
6. **The Windows.Graphics.Capture yellow border stays on.** It is the capture indicator Windows
   otherwise lacks (D3.1). Borderless capture (Windows 11, `GraphicsCaptureAccess`) is not
   used.
7. **The app's own windows are always masked**, on every OS. Every window with the `OWN` bit
   comes back as alpha 0, whether or not the display processor excluded it, so agents see the
   same thing everywhere.
8. **Two macOS behaviours are left for hardware.** Whether ScreenCaptureKit on macOS 15+ still
   hides `NSWindowSharingNone` windows, and whether macOS 15's periodic "continue to allow
   screen recording" confirmation fires for one-shot stills, are both measured on hardware
   (spec §10.7). The results are recorded in spec §10.4.
9. **The consent dialog takes a parent window** (DECIDED 2026-10-20, David). A request may
   chain `XrDesktopControlParentWindowDXR` (structure type 1004999420, the first of a second
   decade for this extension because 1004999400–409 is full), whose string is the
   xdg-desktop-portal `parent_window` convention verbatim: `"wayland:<xdg-foreign handle>"` or
   `"x11:<hex XID>"`, `""` / absent for none. A chained struct keeps
   `XrDesktopControlRequestInfoDXR`'s layout as in v1. It is a placement hint for that one
   request: it never changes what is asked or granted, and a handle the OS cannot resolve is
   the OS's to ignore, never a failure. A malformed string is a validation error. On GNOME it
   is the `parent_window` argument of `Start`, both the combined `RemoteDesktop.Start` and the
   standalone `ScreenCast.Start` (the portals take it there, not in `CreateSession`). macOS and
   Windows ignore it today. A handle form of their own is reserved for a later revision, and
   until one is specified any other prefix is refused, not ignored. The Unity plug-in gets the
   string from `displayxr_linux_window_handle` (displayxr-unity#385).

## Amendment 2 — On GNOME the grant survives a restart of the app (PROPOSED, 2026-10-21)

**Status:** Proposed — needs David's decision. Implemented on the branch
`feat/desktop-control-persist-grant` (spec §6.4); nothing changes on Windows or macOS.

### Context

D2 says the runtime never stores a grant. On Windows and macOS that costs nothing: the OS (or
DisplayXR's store) remembers the answer, so an app asks once. On GNOME the portal hands the app a
**restore token** (`persist_mode` 2) and remembers the approval under it, but only a process
that passes the token back gets the silent restore. The runtime kept the token in memory, so
**every launch of an agent app showed GNOME's dialog again**. The app-side helper that desktop
control replaces stored the token itself (a 0600 file) and asked once, ever; moving to the
runtime made the product worse on exactly the OS where the dialog is the consent.

### Decision

1. **Always save the restore token, per app.** After every successful `Start`, the GNOME
   backend writes the token and the grant's shape (portal, input, screens, the covered
   connectors — the shape decides what a restore asks for, and a downgraded restore is saved by
   GNOME, D9) to `$XDG_STATE_HOME/displayxr/desktop-control/<key>.json`: directory 0700, file
   0600, atomic. It is read at the app's first request or restore in a process. This holds on
   every portal that issues restore tokens, Ubuntu 24.04's xdg-desktop-portal 1.18 (no host
   registry) included.
2. **The key is the app's own identity:** the app id
   `org.freedesktop.host.portal.Registry.Register` accepted; else the app's own id
   (`DXR_DESKTOP_APP_ID`, or the `.desktop` basename) even when registration failed or the
   portal has no registry; else the executable (`exe-<basename>-<hash of the absolute path>`).
   The file records, for diagnostics only, which id the portal used when known (the registered
   one) and the systemd unit the process ran in.
3. **The portal stays the judge.** It honours a token only for the app id it bound it to: the
   registered id, or one derived from the launch context (the process's systemd scope — in the
   headless run, the IDE that started the test, `com.microsoft.VSCode`). So the first request of
   a process restores the saved grant with the dialog allowed: silent when the app is launched
   as before; launched from another context, the portal shows its dialog once and its new token
   replaces the saved one. The docs say: launch the app the same way, ideally from its `.desktop`
   file. A `.desktop` file is not needed for "ask once"; registering makes GNOME name the app in
   the dialog and in Settings.
4. **Removed whenever the grant ends:** the user declines a dialog that had no grant to keep,
   the dialog of that verifying restore, or an `ASK_AGAIN` (whose answer replaces the grant,
   spec §10.2 — the in-memory grant is dropped too, which also corrects the old behaviour of
   putting it back); a silent restore that is not silent or is refused (the portal no longer
   honours it); the session closed by anything but a lock (the indicator's Stop); the kill
   switch, which removes every saved grant; `displayxr-cli desktop forget <exe>`, which removes
   the grants recorded for that executable. `desktop status` lists the saved grants (never the
   token).
5. **Never forwarded.** The token is used only by the runtime inside the app's own process; it
   is never logged, never sent over IPC or MCP (D4), and each app has its own file.
6. **The kill switch ends running grants too** (proposed; it was an open question). Turning it on
   increments `kill_switch_count` in the store; every session that sees the count move — even
   after the switch is off again — revokes the scopes its app had requested (`DENIED` until a
   new request) and tells the backend to drop its OS grant (GNOME: close the portal session,
   forget the restore token). This holds on every OS: on Windows and macOS the new request is
   answered without a prompt (the store / the OS still says yes), on GNOME it shows the dialog
   because the saved grant is gone. A process whose saved file is removed any other way (by
   hand, `forget`) keeps its in-memory grant until it exits but never writes the file again.

### Consequences

- One dialog per app on GNOME, as on the other OSes (plus one more if the app is launched from a
  different context); after a kill switch, a decline, a Stop or a withdrawn approval, the next
  launch asks again.
- **The token on disk is a capability.** The threat is a process of the same user that reads
  the 0600 file and replays the token from a context the portal accepts — the same launch
  context, or a process that registers the same app id, which any same-user process can do once
  a `.desktop` file with that id exists. That exposure is the same whether or not the app
  registered, and the same as every app that persists a portal restore token (OBS Studio, the
  app-side helper this replaces). The portal still refuses the token to other app ids, sandboxed
  apps and other users, and GNOME's indicator shows every session. Before, the token lived only
  in the app's memory, and reading it needed `ptrace` on the app.
- A new process's first request costs a restore (the remote-access indicator shows for ~2 s,
  as after any action) instead of being answered from memory, so a withdrawn approval or a new
  launch context surfaces at the request, as the dialog, not as a failed action.

### Rejected alternatives

1. **Keep the token in memory (status quo).** A dialog at every launch: the reason for this
   amendment.
2. **Save only for an app id the portal's registry accepted.** It leaves Ubuntu 24.04
   (xdg-desktop-portal 1.18 has no registry) and every app without an installed `.desktop` file
   with a dialog at every launch, and buys no security: registering an id needs only a
   `.desktop` file, which a same-user process can install.
3. **The Secret Service (gnome-keyring).** Encrypted at rest, but unlocked for the whole session
   and readable by any same-user process through the same D-Bus API, so it adds no barrier
   against the threat above; it adds a dependency and a failure mode (a locked or absent
   keyring).
4. **The service keeps the token.** Routes the grant through `displayxr-service` — ruled out by
   D2 and open question 4.

### Proposed in this amendment (were open questions; the maintainer may still change them)

- The kill switch ends the grant of a process already running (decision 6), on every OS.
- `displayxr-cli desktop forget <exe>` also removes the saved grants recorded for that
  executable, and `desktop status` lists the saved grants (decision 4).

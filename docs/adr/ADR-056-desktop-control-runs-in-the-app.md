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
and dies with it.

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

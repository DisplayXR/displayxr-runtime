# Wayland Window Geometry Provider (windowed weaving under Wayland)

- **Issues:** #817 (the provider), #1596 (the logical→device conversion), #1595 (refuse rather than resample)
- **Status:** Prototype — pending on-hardware validation. What is proven: the D-Bus wire, the PID match, and that the conversion and the present-origin path are exercised (the arithmetic is isolated in `u_wayland_geom.h`, platform-free, and pinned by `tests/tests_aux_wayland_geom.cpp` against both of the measured box's scales — 1.6667 and 2.0). What is open: the weave's phase and scale on a real 3D panel in a Wayland session — sim_display cannot establish either, since its anaglyph/SBS output degrades gracefully under exactly the errors this document exists to prevent.
- **Scope:** desktop Linux, Wayland sessions, apps using `XR_DXR_wayland_surface_binding`
- **Governing boundary rule:** [ADR-033](../../adr/ADR-033-placement-reports-geometry-weaver-owns-phase.md) — geometry crosses the runtime↔vendor boundary; phase math (incl. snapping) never does

## 1. Problem

Windowed weaving anchors the lenticular interlacing phase to the window's
**absolute position on the panel**. On X11 the runtime polls
`xcb_translate_coordinates` per frame and feeds the result down the
present-origin chain:

```
get_window_metrics → vk_update_present_origin → DP set_present_origin
  → vendor SDK (panel-space phase origin, position-source-agnostic)
```

Wayland deliberately never tells a client where its surface sits — so the
Wayland present path (`XR_DXR_wayland_surface_binding`, runtime v2.1.2) could
only weave **display-scoped**: correct when the window covers the panel from
its top-left, wrong anywhere else.

The one component that always knows every window's global geometry is the
Wayland compositor itself. This feature ships a compositor-side publisher and
a runtime-side consumer.

### 1.1 The rule: logical on the wire, device pixels past the boundary

**Wayland reports geometry in LOGICAL coordinates. Everything the weaver
consumes is DEVICE pixels. The conversion is ours to perform, and it happens
exactly once, where the geometry crosses into the runtime** — in
`comp_vk_native_wl_geom.c`, through `auxiliary/util/u_wayland_geom.h` (#1596).

That is not a style preference. The vendor ships the same guarantee from its
side: every geometry its SDK reports or accepts is device pixels,
unconditionally ([`xrt_plugin_iface`](../../reference/xrt_plugin_iface.md),
[`XR_DXR_weave`](../extensions/XR_DXR_weave.md)). So there is exactly one place
in the whole stack where the two spaces meet, and it is this one. Either it
converts, or two coordinate spaces travel under one name.

Three things make this worth stating at the top rather than in a footnote:

- **`wl_output.scale` is not the conversion factor.** It is an **integer** by
  protocol. On the measured box (2026-09-20) the laptop runs at a fractional
  1.6667 and still advertises `scale = 2`. Its logical width is 1728 and its
  real device width is 2880 (1728 × 1.6667); the integer scale says 3456 — a
  20 % error, in a quantity nothing downstream sanity-checks, that looks
  entirely plausible. The two honest sources are
  `wl_output.mode` ÷ `xdg_output.logical_size`, or a compositor-published
  fractional scale (`wp_fractional_scale_v1`; GNOME's
  `Meta.Display.get_monitor_scale()`, which this extension already forwards as
  `monitor.scale`).
- **A scale error does not cancel anywhere downstream.** A *translation* error
  cancels the moment two quantities in the same wrong frame are subtracted,
  which is why so much of this chain is robust to one. A halved displacement
  does not: it lands on a plausible-looking but wrong lattice, so it fails
  silently rather than visibly.
- **It is why one root cause produced three faults at once.** In the measured
  session, `wl_output` put the DS1 at logical x=1728 while the runtime's panel
  rect had it at device x=3456, so the app's output match could never succeed;
  the surface fullscreened on the laptop instead; and the compositor wove
  anyway, six frames, at a phase anchored to nothing.

## 2. Architecture

```
GNOME Shell (Mutter)                        DisplayXR runtime process
┌──────────────────────────────┐            ┌────────────────────────────────┐
│ window-geometry@displayxr.org│  session   │ comp_vk_native_wl_geom         │
│ (contrib/gnome-shell/)       │───D-Bus───▶│  cache + PID match             │
│ Meta.Window position/size/   │  JSON      │   └▶ get_window_metrics        │
│ focus signals, coalesced     │            │       └▶ vk_update_present_origin │
└──────────────────────────────┘            └────────────────────────────────┘
```

- **Publisher** — GNOME Shell extension `window-geometry@displayxr.org`
  (`contrib/gnome-shell/`). Owns session-bus name
  `org.displayxr.WindowGeometry`, object `/org/displayxr/WindowGeometry`,
  interface `org.displayxr.WindowGeometry1`:
  - `GetWindows() -> (s)` — JSON snapshot of all normal windows.
  - `WindowsChanged(s)` — same JSON on any position/size/focus/lifetime
    change, coalesced to at most one signal per compositor redraw.

  Payload (version 1): per window `pid`, `app_id`, `title`, `focus`,
  `xwayland`, `frame` `[x,y,w,h]` (`get_frame_rect()`), `buffer` `[x,y,w,h]`
  (`get_buffer_rect()`), `monitor` `{x,y,w,h,scale}`. Coordinates are Mutter
  global (stage) coordinates in logical pixels — identical to X11 root
  coordinates at monitor scale 1.0.

  **`monitor` is what makes the logical payload convertible, and the consumer
  reads all five fields** (#1596). `x/y/w/h` are the window's monitor's rect in
  the same logical stage coordinates; `scale` is Mutter's **fractional**
  `Meta.Display.get_monitor_scale()` — 1.6667 on the measured box's laptop,
  never the integer `wl_output.scale` (§1.1). Together they give the consumer
  the one factor to multiply by, the monitor-relative displacement to multiply,
  and the monitor's own device size, which is how it can also tell whether the
  window is on the 3D panel at all. All five have been published since schema
  v1; before #1596 the consumer simply never read four of them. Mutter omits
  the `monitor` object only for a window on no monitor.

- **Consumer** — `comp_vk_native_wl_geom` (`src/xrt/compositor/vk_native/`,
  built when `XRT_HAVE_WAYLAND && XRT_HAVE_DBUS`, libdbus-1). Private
  session-bus connection; one blocking `GetWindows` at create (200 ms cap),
  then a non-blocking signal pump per query. The pump also watches
  `org.freedesktop.DBus.NameOwnerChanged` (arg0-matched on the well-known name)
  so it notices the publisher coming and going. Created by the compositor when
  the app binds a Wayland surface; queried from the existing
  `get_window_metrics` Linux branch as a third window source next to the two
  XCB ones. Match policy: windows of `getpid()`, focused first, else largest.

## 3. Degradation ladder (all paths end at pre-#817 behavior)

| Condition | Behavior |
|---|---|
| Built without libdbus / wayland | Provider not compiled; display-scoped |
| No session bus | Provider create returns NULL (one WARN); display-scoped |
| Extension not installed/enabled | Snapshot empty; bounded retry every 5 s; display-scoped until it appears |
| No window matches our PID | `get_window_metrics` invalid; display-scoped |
| Monitor scale ≠ 1.0 | **Converted**, not refused (#1596): the rect is multiplied to device pixels using `monitor` + its fractional `scale`, one INFO naming both spaces. Weaving proceeds with a real phase |
| Payload carries no `monitor` object | Rect **refused**, one WARN; display-scoped. Without it there is no scale to apply and no way to name the space the rect is in. Mutter omits it only for a window on no monitor |
| The window's monitor is not the 3D panel | Rect **refused**, one WARN; display-scoped — *and* the weave itself degrades to flat 2D (#1595), because a surface on another output cannot be 1:1 with this panel by any phase |
| The presented buffer ≠ the window's device extent | Geometry is still valid and still served; the **weave** degrades to flat 2D (#1595), one `NOT_1TO1:` WARN, and the present-origin feed is suppressed so no stale phase stays latched |
| Publisher goes away / comes back (screen lock) | Cache **dropped** on `NameOwnerChanged`, so display-scoped while it is gone; on its return the consumer re-takes `GetWindows` immediately rather than waiting for the next change. One WARN per transition |

The scale row has been rewritten twice, and the history is the argument. It
originally returned the rect with a warning, so the outcome was not pre-#817
behavior but *windowed weaving at a known-wrong phase* — visibly broken 3D
rather than a clean fallback. #1557 then **refused** any rect from a non-1.0
monitor, which restored the ladder's promise but conflated two separate
failures under one condition: the units being wrong, and the pixels being
resampled. Only the second is actually fatal, and the first was never
unfixable — the payload has always carried `monitor` and a fractional `scale`
(§2), so the rect was convertible the whole time. #1596 converts it, and the
part of the old refusal that was really about pixels lives on as the 1:1 gate
below.

The ladder's promise is therefore unchanged but re-founded: **every failure
path still ends at display-scoped, and none of them ends at a known-wrong
phase.** What changed is which conditions are failures. A fractionally-scaled
monitor is now a *supported* configuration; what is not supported is a buffer
that the compositor resamples on its way to the panel, and the two new rows
above — the window being on the wrong output, and the buffer not matching the
window's device extent — are the cases where the runtime can measure that it
would.

Both of those are enforced in the compositor rather than here, by
`vk_linux_update_surface_not_1to1()` (`comp_vk_native_compositor.c`, #1595) —
this provider only reports geometry. The gate is transition-only, logs one WARN
naming both extents and which reason fired, calls
`request_display_mode(false)` plus the display processor's `on_pause`/`on_resume`
pair, and collapses the effective layout to tile 0; it is the Wayland twin of
the Android `vk_android_update_container_scaled()` precedent, and it is
reversible (`NOT_1TO1 cleared:`). It has **three** states and not two:
cannot-be-1:1 degrades, is-1:1 weaves, and *don't know* keeps whatever state
the session is in — so with no publisher there is no destination extent, and
the session behaves exactly as it did before the gate existed. Degrading on
ignorance would be its own kind of wrong answer.

The lock-screen row exists because the publisher is **intermittent by design**:
GNOME disables user extensions whenever the screen shield is up and re-enables
them on unlock, so the bus name really does disappear and reappear during a
normal session. Absence was always handled (bounded retry, display-scoped), but
two things about the *edges* were not. On the way out, the last snapshot stayed
cached and kept anchoring the phase — and a window moved or resized behind the
shield made that cached origin quietly wrong. On the way back, the publisher
only emits `WindowsChanged` on the next geometry *change*, so the stale origin
survived until the user happened to move the window again. Tracking
`NameOwnerChanged` fixes both edges: losing the owner invalidates the cache
(display-scoped is the honest answer once we cannot vouch for the origin), and
gaining one invalidates it *and* re-takes the same bounded `GetWindows` the
retry path already makes. These are rare lifecycle events, so each transition
logs exactly one WARN; the per-frame pump still never blocks for anything else.

## 4. Packaging contract — the publisher is a shared asset

The publisher is deliberately **not** a DisplayXR-coupled component: it is a
compositor-side shim that reports window rectangles and knows nothing about
weaving, lenses, or any runtime. A **vendor runtime package may ship it** (a
vendor SDK runtime needs the same geometry to serve its own non-DisplayXR
apps — one publisher, many consumers, per
[ADR-033](../../adr/ADR-033-placement-reports-geometry-weaver-owns-phase.md)).
The following is what any shipping package must honor.

**Canonical identity — never fork these.** Extension UUID
`window-geometry@displayxr.org`; bus name `org.displayxr.WindowGeometry`;
object `/org/displayxr/WindowGeometry`; interface
`org.displayxr.WindowGeometry1`. The `displayxr` prefix names *who defines the
schema*, not who ships the bits — the `org.freedesktop.*` / `org.gnome.*`
convention. A vendor package shipping the publisher keeps these identifiers
unchanged; renaming forks the ecosystem and strands consumers.

**Exactly one installed owner.** A D-Bus well-known name is singly owned: a
second publisher binds nothing, queues silently, and leaves consumers on
whichever copy won the race — possibly an older schema. So multiple packages
may *be able* to ship it, but only one may be installed at a time. The Debian
idiom for that is a virtual package — every package that ships the extension
declares all three of:

```
Provides:  displayxr-window-geometry-publisher
Conflicts: displayxr-window-geometry-publisher
Replaces:  displayxr-window-geometry-publisher
```

so a vendor runtime .deb and a DisplayXR .deb can each satisfy the dependency,
dpkg refuses to install both, and consumers depend on the virtual name rather
than on any particular vendor. Files install system-wide to
`/usr/share/gnome-shell/extensions/window-geometry@displayxr.org/` (not the
per-user `~/.local/share/...` path used for manual dev installs). Note the
extension still has to be *enabled* per user session — a package can seed this
via a dconf default for `org.gnome.shell enabled-extensions`, but it cannot be
force-enabled for users who have opted out. A package must also **say in its
post-install output that a newly installed extension only takes effect at the
user's next login** — a Wayland session cannot restart GNOME Shell the way Alt+F2
`r` does under X11, so until the user logs out the publisher is simply absent and
every consumer silently falls back to display-scoped weaving.

**Schema evolution is additive within a version.** Publishers may add fields
freely; consumers look up only what they know and ignore the rest. Anything
that changes the *meaning* of an existing field — logical → physical pixels,
a different coordinate origin — is a breaking change and MUST bump `version`.

This cuts both ways, and #1596 is the worked example in each direction. A
consumer beginning to read `monitor.x/y/w/h` is an **additive read** of fields
schema v1 has published from the start: no field changed meaning, no publisher
has to do anything, and **no version bump is required** — a v1 publisher that
predates the runtime consumer serves it correctly. Conversely, the wire stays
**LOGICAL**, deliberately and permanently: the conversion belongs on the
consumer side, where the panel's device geometry is known (§1.1). A publisher
that decided to emit physical pixels instead would be changing the meaning of
`frame`, `buffer` and `monitor` at once, would silently square the scale factor
in every consumer that already converts, and is exactly the breaking change
this clause requires a bump for.
Consumers refuse a payload whose `version` exceeds what they understand
(`WLG_SCHEMA_VERSION_MAX`) and fall back to display-scoped rather than weave at
a silently wrong phase; a payload with no `version` is treated as v1.

**No reverse dependency.** The extension must remain pure GNOME Shell JS with
no import from, or runtime dependency on, DisplayXR or any vendor stack — that
is what lets any package ship it and any runtime consume it.

## 5. Known limitations / follow-ups (#817)

- **Frame vs buffer rect** — the phase needs the rect where the *surface
  pixels* land. For CSD toolkits the buffer rect includes shadow margins;
  both rects are published, `frame` is consumed. Hardware validation decides
  whether a per-toolkit correction is needed.
- **PID matching** assumes the in-process app path (window owner ==
  runtime process). IPC/service mode needs the client PID plumbed through.
- **GNOME only** — KDE could be served by the existing
  `plasma-window-management` geometry events; other compositors need their
  own publisher speaking the same D-Bus interface (the runtime side is
  compositor-agnostic by construction).
- **Packaging** — the extension is not yet installed/enabled by the .deb /
  bundle; manual install per `contrib/gnome-shell/.../README.md`.
- Mutter emits geometry transactionally with its own redraw, so tracking
  during interactive drags is expected to be at least as good as the X11
  per-frame poll; validate visually (phase lock while dragging).

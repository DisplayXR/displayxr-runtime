# Wayland Window Geometry Provider (windowed weaving under Wayland)

- **Issues:** #817 (the provider), #1596 (the logical→device conversion), #1595 (refuse rather than resample); capture exclusion (§6) is the GNOME equivalent of `WDA_EXCLUDEFROMCAPTURE` for Linux transparency (#757)
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
  - Since extension version 2 the same bus name also serves
    `org.displayxr.CaptureExclusion1` at `/org/displayxr/CaptureExclusion`
    (§6), and each window entry carries an additive `capture_excluded`
    boolean. The geometry payload stays schema `version: 1` — the new field
    changes the meaning of nothing.

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

  **`scale` is the factor only in Mutter's LOGICAL layout mode** (extension
  version 11, §4.1). "Logical" above means Mutter's *stage* coordinates, and
  in Mutter's PHYSICAL layout mode the stage is device px: a 3840x2160
  monitor at 200 % is published as `{w: 3840, h: 2160, scale: 2}`. Version 11
  therefore adds a top-level `layout_mode` (`"logical"` / `"physical"`) and
  `monitor.device_scale`, the device-px-per-stage-px factor (the scale of the
  stage view painting the monitor). The consumer converts by `device_scale`,
  then by `layout_mode`, then, for an older publisher, by asking Mutter
  (`org.gnome.Mutter.DisplayConfig.GetCurrentState` → `layout-mode`: 1
  logical, 2 physical). It falls back to `scale` only when none of those
  answers (`u_wl_stage_to_device_scale`).

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
| Mutter in its PHYSICAL layout mode (stage = device px; Ubuntu 24.04 / GNOME 46 at an integer scale) | Factor 1, not the monitor scale (§4.1). One WARN naming the source (`device_scale`, `layout_mode`, or Mutter's own `layout-mode` for a publisher older than version 11). Before this, a 3840x2160 panel at 200 % read as 7680x4320, so the window was "not on the panel" and presented flat 2D |
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
per-user `~/.local/share/...` path used for manual dev installs).

**Ship both entry-point forms, and select by the running shell.** GNOME Shell
45 loads an extension as an ES module; 40–44 (Ubuntu 22.04 is GNOME 42) loads
it with the legacy importer, for which ESM syntax is a parse error. The
publisher therefore carries two thin entry points — `extension.js` (45+) and
`extension-gnome42.js` (40–44) — over one shared, import-free `lib.js` that
both module systems can read, and one `metadata.json` whose `shell-version` is
the union of both ranges. A directory holds one `extension.js`, so the
installer chooses: `displayxr-gnome-extension-enable` reads
`gnome-shell --version` and, below 45, puts the legacy form in that slot — in
place where the install is user-writable, otherwise as a per-user shadow copy
of the system one, which GNOME prefers — and reverses that on a 45+ shell so a
distribution upgrade needs no repair. A package that ships only the ES module
is not wrong, but it silently serves no pre-45 desktop; a consumer there sees
the bus name simply absent and degrades per §3. Note the
extension still has to be *enabled* per user session. A dconf default for
`org.gnome.shell enabled-extensions` reaches only users who have never written
that key, which excludes anyone who has toggled an extension. The
`displayxr-runtime` `.deb` therefore enables it from an `/etc/xdg/autostart`
entry, once per user at login. It skips a user whose `disabled-extensions` lists
the UUID, and it never uses a dconf lock
(`scripts/linux/displayxr-gnome-extension-enable`). No package may force-enable
the extension for a user who has opted out. A package must also **say in its
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

### 4.1 Worked example: Mutter's layout mode (extension version 11)

The clause above says the wire is LOGICAL. That was true only on the desktops
it had been measured on. The wire is Mutter's **stage** coordinates, and Mutter
has two layout modes:

| `layout-mode` | when | stage space | 3840x2160 monitor at 200 % |
|---|---|---|---|
| 1 LOGICAL | fractional scaling enabled; mutter 50's default | logical px | rect 1920x1080, scale 2, stage view scale 2 |
| 2 PHYSICAL | Ubuntu 24.04 / GNOME 46 at an integer scale, out of the box | device px | rect 3840x2160, scale 2, stage view scale 1 |

(Measured on a headless mutter 50.1, switching only `gdctl set --layout-mode`.)
In PHYSICAL the monitor scale says how big clients draw. It is not a
coordinate factor, so a consumer that multiplied by it read the panel as a
7680x4320 output. The window was then never "on the panel": `NOT_1TO1`, flat
2D, lens off.

Version 11 fixes this **additively**. No field changes meaning: `frame`,
`buffer` and `monitor.{x,y,w,h}` remain stage coordinates, and `scale` remains
`get_monitor_scale()`. What is new is two fields that name the stage's space:

- top-level `layout_mode`: `"logical"` / `"physical"`, left out when it
  cannot be told. With every monitor at scale 1 the two modes agree.
- `monitor.device_scale`: device px per stage px. This is the scale of the
  stage view painting the monitor (`Clutter.StageView.get_scale()`), so it is
  the factor in either mode.

Mutter does not introspect its layout mode. The view scales give it away:
any view scale other than 1 means LOGICAL, and every view at 1 while some
monitor is scaled means PHYSICAL (`StageScale` in `lib.js`, pinned by
`scripts/test_gnome_extension_stage_scale.js`). The schema stays `version: 1`.
Bumping it would make every shipped consumer refuse a payload that is still
correct for it on every LOGICAL desktop. A consumer without the new fields
asks Mutter for `layout-mode` instead, as the runtime does for a publisher
older than version 11 (`wlg_query_mutter_layout_mode`). That case is not
hypothetical: after a package upgrade, the running shell keeps the old
extension until the user logs out.

Everything else in the extension already worked in stage coordinates, so it
follows the mode with no change: `MoveWindow`, the pointer drag, and the
move-sync history. The move-sync tag is read from subsurface actor positions,
and those are in *surface* units in both modes. Measured in PHYSICAL at
200 %: the tag actor sits at `(x mod 256, y mod 256)` of the buffer's stage
position, exactly as the runtime set it. The drag-lattice choice and the stamp
audit weigh device px, and now use `device_scale`.

**No reverse dependency.** The extension must remain pure GNOME Shell JS with
no import from, or runtime dependency on, DisplayXR or any vendor stack — that
is what lets any package ship it and any runtime consume it.

## 5. Known limitations / follow-ups (#817)

(Capture-exclusion limitations are listed with it, in §6.6.)

- **Frame vs buffer rect — resolved (#1654): the `buffer` rect is consumed.**
  The phase, the Kooima canvas and the 1:1 check all need the rect where the
  bound surface's pixels land, and that is Mutter's buffer rect
  (`get_buffer_rect()`, the main surface), not the frame (`get_frame_rect()`,
  the window geometry). An undecorated surface has the two equal, so nothing
  changed for it. A client-side-decorated one differs by the title bar. The
  test apps draw that bar in a subsurface above the bound surface and set the
  window geometry to bar + content. Measured on GNOME 50 at 1.6667, a
  1280x720 windowed cube reports frame `[257,173,1280,766]` and buffer
  `[257,219,1280,720]`, and the present origin follows the buffer. The frame
  is kept for two things. It is the fallback when a publisher reports no
  buffer. And a surface that spills **past** the frame is the #1653
  signature of a buffer not mapped to its configured size, which the 1:1
  gate still degrades on (`u_wl_surface_within_frame`). A toolkit whose main
  surface carries shadow margins would weave into those margins too. That is
  a property of the app's surface, not of this provider.
- **PID matching** assumes the in-process app path (window owner ==
  runtime process). IPC/service mode needs the client PID plumbed through.
- **GNOME only** — KDE could be served by the existing
  `plasma-window-management` geometry events; other compositors need their
  own publisher speaking the same D-Bus interface (the runtime side is
  compositor-agnostic by construction).
- **GNOME 42 is shipped but not hardware-validated** (#1663). The legacy
  entry point is syntax-checked in CI and every mutter/Clutter API it uses is
  present in 42.9, but no 22.04 desktop has run it. The measurement that
  settles the one genuinely uncertain part — whether the
  `before-paint`/`after-paint` bracket still discriminates off-screen paints
  on mutter 42 — is `CaptureExclusion1.GetState`'s `paints.skipped` going
  non-zero while an area screencast runs. Steps in the extension's `README.md`.
  Separately from whether it loads: GNOME 42 predates `wp_fractional_scale_v1`,
  so a native-Wayland client on a scaled 22.04 desktop cannot present a buffer
  at its device extent, and the 1:1 gate
  (`vk_linux_update_surface_not_1to1()`, #1595) degrades the session to flat 2D. **X11 is the recommended session on Ubuntu 22.04**, and this
  provider is not needed there.
- Mutter emits geometry transactionally with its own redraw, so tracking
  during interactive drags is expected to be at least as good as the X11
  per-frame poll; validate visually (phase lock while dragging).
- **Multi-screen segments need every monitor, not just the window's own
  (multi-screen M2 follow-up, ADR-047 D2).** M2 weaves a window that spans
  screens per segment — `canvas ∩ screen`, each by that screen's display
  processor with its own present origin — on X11/XWayland, where root
  coordinates are desktop-absolute and RandR gives every monitor's rect in the
  same space (`docs/architecture/comp-segments.md`). Native Wayland stays on
  the primary-segment-only behaviour (#1654 bands), because this payload cannot
  place the second segment:
  - `monitor` describes only the window's OWN monitor. The rect of the part of
    the window that lands on another monitor, in THAT monitor's device px,
    needs that monitor's stage rect and its own `device_scale` — a
    mixed-scale layout (this box: 1.6667 laptop + 2.0 DS1) makes a single
    factor wrong for one of the two.
  - The runtime's screen registry is keyed by connector / RandR output name,
    and the payload names no connector, so even the window's own monitor is
    matched only by its device size today (`wr.monitor_width_px ==
    panel_px_w`), which is ambiguous for two identical panels.

  **Required extension change** (not installed on any dev box; the runtime
  keeps refusing until a publisher announces it): add a top-level
  `monitors: [{connector, x, y, w, h, scale, device_scale}]` array — every
  monitor in stage coordinates, `connector` = the DRM connector / Mutter
  monitor connector (`Meta.MonitorManager` → `get_monitor_for_connector`,
  e.g. `"HDMI-1"`, which is what the registry's `device_name` holds) — plus
  `monitor.connector` on each window. Additive, so the schema stays
  `version: 1` and an older consumer ignores it; bump the extension version
  so `org.displayxr.WindowGeometry1` capability probing can tell. The consumer
  then intersects the window's `buffer` rect with each monitor in stage
  coordinates, converts each piece to that monitor's device px with that
  monitor's `device_scale`, and hands `comp_segments` a segment table in
  which every rect is already per screen. Note that Mutter paints a straddling
  surface at ONE buffer scale (the window's main monitor's), so the part on
  the other monitor is resampled: the per-segment 1:1 policy applies (a
  resample-tolerant DP weaves it, a lenticular weave gets flat 2D).

## 6. Capture exclusion — `org.displayxr.CaptureExclusion1` (extension version 2)

### 6.1 Why it exists

A transparent 3D window needs the desktop **behind** it: the weaver flattens
alpha, so the display processor composes a captured desktop under the stereo
fringe before weaving (the band where some views are transparent and others
are not). On Windows the capture excludes the app's own window with
`SetWindowDisplayAffinity(WDA_EXCLUDEFROMCAPTURE)`. GNOME has no such API, and
a capture that contains our own window composes our **previous woven frame**
under the new one, putting both views into both eyes — the double image
confirmed on a DS1 panel. Without a clean capture the only safe choice is to punch
where *any* view is transparent (silhouette intersection), which shrinks the
silhouette by the disparity and — for the rear depth budget (ADR-040) — leaves
no background to measure, so the runtime clips permanently.

This interface is that missing API, implemented in the compositor where it has
to live. It lives in this extension rather than a second one because this
extension is already installed and already required for windowed weaving; one
extension also means one lifecycle (a single logout activates both).

### 6.2 Mechanism

A `Clutter.Effect` is attached to each excluded window actor. Its `paint`
forwards to the actor only while the stage is painting a view **on screen** and
swallows every other paint:

- **On-screen paints** happen once per stage view per frame and always run
  between the stage's `before-paint` and `after-paint` signals
  (`clutter-stage-view.c`).
- **Off-screen paints** — `clutter_stage_paint_to_framebuffer()` /
  `_to_buffer()` — happen outside that bracket: mutter's ScreenCast
  `RecordArea` source re-renders the recorded area from an idle callback after
  the frame, and Shell screenshots / out-of-frame window grabs do the same.

The discriminator is therefore *timing*. The more obvious test, "is this paint
context's framebuffer a stage view's", is not reachable from JS (the typelib
exposes `get_framebuffer`, not `get_base_framebuffer`). The spike cross-checked
the two on ~8,800 paints with zero disagreements.

The window **behind** an excluded one is not left with a hole: mutter culls
what an opaque window fully covers before painting, but an actor with an active
effect is exempt from that culling (`meta-cullable.c`), so the occluded windows
are still painted and the off-screen paint shows them intact.

**Scope.** `RecordArea` (what DisplayXR uses) and screenshots exclude the
window. A `RecordMonitor` stream that mutter serves by blitting the on-screen
view framebuffer is a copy of the on-screen paint and **does contain** the
window — this is weaker than `WDA_EXCLUDEFROMCAPTURE`, which also hides from
full-monitor capture. Consumers that need exclusion must use `RecordArea`.

### 6.3 D-Bus surface

Service `org.displayxr.WindowGeometry` (the same, singly owned name), object
`/org/displayxr/CaptureExclusion`, interface `org.displayxr.CaptureExclusion1`:

| Member | Semantics |
|---|---|
| `Exclude(u pid) -> (u windows)` | Exclude every window of `pid` — present now **and mapped later** — from off-screen paints. `pid = 0` means the caller. Returns how many windows are excluded right now (`0` is normal before the caller's window maps). |
| `Release(u pid)` | Drop the caller's registration for `pid` (`0` = caller). |
| `GetState() -> (s json)` | Diagnostics: `{version, clients:[{sender,pids}], windows:[{pid,title}], paints:{onscreen,skipped}}`. |

**Identification — by registered PID, not guessed.** The extension does not
decide which processes are "DisplayXR" ones: it is a shared, runtime-agnostic
asset (§4) and has no way to know which process is driving a 3D panel. The
display processor (in the app's process for in-process apps) registers its own
PID. Every window of that PID gets the effect, including dialogs and popups,
and a window that maps later gets it from the window manager's `map` signal —
before its first on-screen frame, so no off-screen paint ever sees it
un-excluded.

**Only your own PID.** A caller may exclude only its own process; anything
else returns `org.freedesktop.DBus.Error.AccessDenied`. Hiding another process's
windows from screen recording is not something an unrelated client should be
able to do silently. IPC/service mode, where the window owner is not the display
processor's process, needs the client PID plumbed — the same open item as §5's
PID matching.

**Lifetime is the caller's bus connection.** The registration is dropped when
the caller's connection goes away (exit, crash, a closed private connection),
so a dead app can never leave a window invisible to capture. There is no timer
and no heartbeat.

**Disable removes everything.** `disable()` removes every effect the extension
added, whether or not its owner is still registered. GNOME disables user
extensions whenever the screen shield is up, so this happens routinely: the bus
name disappears, every window becomes capturable again, and on unlock the name
returns with **no** registrations. Consumers must watch `NameOwnerChanged` and
re-register (§6.5).

### 6.4 Detecting it

A consumer calls `Exclude(0)` and reads the outcome:

| Result | Meaning |
|---|---|
| success | Exclusion live for this connection. |
| `ServiceUnknown` / `NameHasNoOwner` | Extension not installed / not enabled (or the screen is locked). |
| `UnknownMethod` / `UnknownObject` / `UnknownInterface` | A version-1 extension: geometry only, no exclusion. |

`metadata.json` carries `"version": 2`; `GetState()` reports the protocol
revision of `CaptureExclusion1` (`version: 1`).

### 6.5 The consumer: the Leia Linux display processor

`displayxr-leia-plugin` `src/drv_leia_linux/leia_bg_capture_linux.c` +
`leia_mutter_capture_linux.c`, on one private session-bus connection:

1. `CaptureExclusion1.Exclude(0)` — **first**. If it fails, no capture is started
   at all; the DP logs once that installing/updating this extension is what
   enables correct transparency, and runs silhouette intersection.
2. `org.gnome.Mutter.DisplayConfig.GetCurrentState` — the panel's rectangle in
   **logical** (stage) coordinates, which is what `RecordArea` takes. The panel
   is matched by EDID vendor + product (serial breaks ties), then by connector
   name, then by being the only external monitor at the panel's resolution.
   Never derived from X11: XWayland's root space is itself scaled.
3. `org.gnome.Mutter.ScreenCast` `CreateSession` → `RecordArea` over the panel's
   **full** logical rectangle (the area is fixed when the stream is created, so
   the panel is recorded, not the window) with `cursor-mode` hidden → `Start` →
   the stream's `PipeWireStreamAdded(node)`; the PipeWire node is consumed over
   the default PipeWire socket. Torn down with `Stop` (and by mutter when the
   connection closes).

**Units, end to end.** `RecordArea` takes logical coordinates and streams
`area × the highest overlapping monitor scale`, so an area that is exactly the
panel comes back in the panel's **device** pixels: at 200 % a 1920×1080 logical
area is a 3840×2160 stream; at 5/3 a 2304×1296 area is 3840×2160. The window
rectangle the DP receives (present origin + target extent) is already device
pixels relative to the panel (§1.1), so it is normalised by the panel's device
size — never by a logical size — to address the stream.

**Trust.** The captured desktop is used only while the exclusion is live. When
the name owner disappears the capture is distrusted immediately (silhouette
intersection); when it returns the DP re-registers and additionally skips as
many frames as the PipeWire pool holds, because *publish* order is not *record*
order: a frame mutter recorded before the effect was back can still be in the
pool (observed in the nested-shell test). A session closed by mutter — e.g. the
user pressing *Stop Screen Sharing* — ends the capture for that session.

### 6.6 Limitations

- **Screen-sharing indicator.** Any mutter ScreenCast session registers a
  remote-access handle, so while a transparent DisplayXR app runs GNOME shows
  its "screen is being shared" indicator, whose *Stop* button ends every such
  session, ours included. The DP treats that as "no capture for the rest of the
  session" and falls back cleanly. (The portal path had the same indicator, plus
  a consent dialog.)
- **Cost.** Each captured frame is one extra off-screen render of the recorded
  area — the whole panel — by mutter, plus a CPU copy of the frame into the
  display processor. Mutter records only on damage, and the Leia DP caps
  delivery at 66 ms by offering that as the stream's `maxFramerate` (Windows
  parity, `LEIA_DP_CAPTURE_MIN_INTERVAL_MS`; 0 = uncapped). So the worst case
  is about 15 full-panel re-renders a second under motion, and nothing on a
  quiet desktop. The effect itself is a few JS calls per paint. GPU cost at
  4K on the panel is not yet measured.
- `RecordMonitor` streams served by a view blit are not covered (§6.2).
- GNOME only; another compositor needs its own implementation of the same
  interface.


## 7. Window placement — `org.displayxr.WindowPlacement1` (extension version 3)

### 7.1 Why the compositor has to move the window

The interlace phase is a function of the window's position in panel pixels, so
after a drag the window must sit on a position the lens can be phased to. On
X11 the app owns the drag and snaps **every step** before the window moves
(#1588, #1609); on Wayland it owns none of it — the move runs inside the
compositor's grab, and a client cannot position itself at all. So the snap
happens **once, after the drop**, and the move has to be asked of the
compositor.

```
Method  MoveWindow(u pid, i x, i y) -> (b moved)
Object  /org/displayxr/WindowPlacement      Interface  org.displayxr.WindowPlacement1
```

`x`/`y` are the window FRAME's top-left in the same logical stage coordinates
`GetWindows` reports. The PID is taken from the **bus connection**, never from
the argument (which must match, or be 0 for "me"): a client may only move its
own windows. The extension refuses while an interactive grab is running on the
window — the user is still dragging it — and for fullscreen windows.

Additive schema field for the same reason: every window now carries
`"moving"`, true while an interactive grab (move or resize) is in progress on
it. A consumer that repositions windows waits for it to go false. A publisher
older than version 3 omits it, and the runtime then settles on stillness alone
(six unchanged polls), which is less exact but never wrong.

### 7.2 What the runtime does with it

`vk_wayland_phase_snap` (`comp_vk_native_compositor.c`), once per settled move:

1. The window's content origin comes from the geometry payload as usual, in
   device pixels relative to the panel.
2. **Reachability, at any scale.** Mutter positions windows at integer LOGICAL
   pixels and draws each at `roundf((content - monitor) * scale)` device px
   (mutter 50, `meta-window-actor-wayland.c`, `surface_container_apply_transform`),
   which is `u_wl_logical_to_px()`. So every logical move lands on a KNOWN
   device position: every q-th pixel at an integer scale q, an irregular but
   equally known set at 1.25, 1.5 or 1.6667. The search runs over logical
   moves through that mapping; it used to refuse any non-integer scale.
3. **The phase-correct position** is searched over the logical moves nearest
   the display processor's own answer (49 within 3 logical px, in the same ring
   order the X11 drag's `vk_snap_search_lattice()` uses): the display
   processor stays the only owner of the lens math (ADR-019) — the runtime
   never computes a phase, it only chooses which positions to offer
   `snap_window_rect`. The DP is given the pre-move origin as the phase
   reference and the drop position as the reachability anchor; on X11 those
   coincide, after a compositor-run drag they do not.
4. If the answer differs from the drop, the runtime calls `MoveWindow` with the
   frame origin plus the chosen logical move, and logs one line either
   way. At most two attempts per settle, so a compositor that constrains the
   move cannot make it oscillate.

Every failure ends where the session was before: the window keeps the phase it
was dropped on, which is the pre-#1609 behaviour.

## 8. Phase-snapped drag — the drag lattice (extension version 6)

### 8.1 Why

§7 snaps a window once, after the drop, so the 3D is correct only when the
window is parked. While it moves, the compositor runs the drag and places the
window at positions the lens does not accept, and the interlace visibly
shimmers. On Windows and X11 the app owns the drag and snaps every step before
the window moves. A Wayland client cannot: it has no positioning protocol, and
it learns where its window is only after the compositor has put it there.
The display vendor's own SDK example refuses windowed mode on native Wayland
entirely, so a compositor extension is the only known route to a phase-clean
Wayland drag.

### 8.2 Mechanism: correct after apply

1. **At the title-bar press, in the app.** Before `xdg_toplevel.move`, the app
   probes the display processor's own snap (`xrWeaveSnapWindowRectDXR`) over a
   grid of displacements from the drag start (±192 logical px, every 3 px:
   16,641 probes, 1.6–6 ms). It keeps the phase-correct entries that the
   compositor can actually reach (whole logical px, so an integer output scale
   q), and sends them with `SetDragLattice`. The table holds displacements
   only. It carries no lens pitch, slant or viewing distance, and nothing is
   published: it is the app's own answer set, for this drag only.

   **Probe with the grid call.** Per point, the probe is one service round
   trip each for an IPC client (a weave present-owner, the browser's GPU
   process, any forced-IPC app): 2.3–2.5 s per press measured, long enough to
   stall the frame loop and send `xdg_toplevel.move` with a stale serial. The
   preferred probe is `xrWeaveSnapWindowGridDXR` (XR_DXR_weave spec v11,
   #1723): the same per-point snap, looped by the runtime next to the display
   processor, the whole table in one round trip (1.4–2.7 ms for 129 × 129
   headless). Since displayxr-common v2.24.0 the helper sends
   `xdg_toplevel.move` at once and builds the table on its worker from a few
   grid calls through its grid snap provider (`set_snap_grid_provider` +
   `DxrWeaveSnap::grid_callback`, per point on a pre-v11 runtime). See
   [XR_DXR_weave.md §5c](../extensions/XR_DXR_weave.md).
2. **During the compositor's drag, in the extension.** Mutter emits
   `position-changed` synchronously inside `move_resize`. The handler moves the
   window on to the nearest table entry with `move_frame` before the stage
   paints again, and a re-entry guard ignores its own move. The grab places the
   window from the pointer's displacement against the grab anchor, not from the
   window's current position, so the correction never fights the grab. The next
   event proposes the raw position again, and it is corrected again.
3. **Coverage.** A drag that leaves the table moves unsnapped and signals
   `DragLatticeNeeded`. The app answers with the next piece (`extend = true`).
   The table is dropped at grab end.

The drag keeps everything the compositor's own drag has: feel, latency, edge
tiling and drag-to-workspace. No call goes into the app during the grab, so a
busy app cannot stall the desktop's move path.

The app enables this whenever `GetPlacementCapabilities` reports bit 0.
`DXR_WL_DRAG_LATTICE=0` turns it off. An older extension, or a maximised or
fullscreen window, gets the plain compositor drag, which is the pre-#1609
behaviour.

### 8.3 Why not `Meta.ExternalConstraint`

It is the proper hook: external constraints run last, receive `MOVE` for
grab-driven moves, and their rect is final. But from GJS on mutter 50 it
cannot change anything. The constraint receives `info.new_rect` as a copy,
and writing the field throws
`Writing field Meta.ExternalConstraintInfo.new_rect is not supported`.
The code stays behind `DISPLAYXR_LATTICE_CONSTRAINT=1` for when mutter makes the
rect writable (an upstream request is drafted). Correcting in `position-changed`
relies on that signal staying synchronous inside `move_resize`.

### 8.4 Verified

- **Headless mutter 50, programmatic move:** a raw (697,355) was corrected to
  (696,356), the nearest entry in both axes. The paint audit counted 13 frames:
  13 on the lattice, 0 off.
- **Hardware, 3840x2160 lenticular panel at scale 2:** native drag feel,
  stable 3D while moving, and no stutter.
- The per-frame audit under a real grab is available with `DISPLAYXR_DEBUG=1`
  in the shell's environment. The extension README explains how to get it
  there: environment.d is not re-read at logout.

### 8.5 Version 7 — follow-ups from hardware

- **Mid-drag tables.** A drag that starts on a fractionally scaled output has
  no reachable lattice there, so the app sends no table at the press. When
  the window reaches an integer-scale output (the 3D panel) during the drag,
  the app derives a table then. The extension accepts it mid-grab, with its
  origin where the window is on arrival. Leaving the panel drops it
  (`ClearDragLattice`).
- **Ask ahead.** The next piece is requested once the drag is half-way from
  the table's centre to its edge, centred ~150 ms ahead in the direction of
  motion, not on the first miss. The app probes on a worker thread, so
  neither a mid-drag table nor an extension stalls rendering.
- **Grab-only correction.** A table corrects only moves made by a grab. The
  runtime's drop-time `MoveWindow` is never pulled towards the table.
- **One owner per drop.** `lattice_drop: true` in the snapshot means the last
  drag ended on its table and the window has not moved since. The drop-time
  snap accepts such a position as is.
- **Drop-snap verification.** `MoveWindow` is asynchronous, so the landing is
  judged once the window reaches the target (or after ~0.5 s), and every
  change until then is the snap's own. Judging it on the next poll made a
  drop ping-pong between two phase-correct neighbours.
- **`DragLatticeDone`** carries one drag's statistics, so the app logs a
  one-line summary in its own log.

### 8.6 Version 8 — any output scale

The drag lattice used to be built only at an integer scale ("every q-th device
pixel"). At 150 % the panel dragged unconstrained, and stuttered. Now:

- **The table is built over LOGICAL displacements.** Each displacement is
  mapped to the device displacement Mutter actually produces,
  `roundf((surface - monitor) * scale)` (§7.2). The display processor's snap
  is asked about that device displacement, and a logical entry is kept when
  the device position it lands on is phase-correct. At an integer scale this
  is identical to the old build, and a unit test in displayxr-common pins
  that.
- **A table is valid only for its start.** At a fractional scale the device
  displacement of a logical move depends on where the move starts (the
  rounding): at 1.5, one logical px from an even position is 2 device px, and
  from an odd one it is 1. So the app reads its start (frame, buffer, monitor
  and scale) from `GetWindows` and hands it back with the table:
  `SetDragLatticeAt(pid, startX, startY, …)`, capability bit 1. A table built
  mid-drag, for a position the window has since left, stays correct.
- **The trigger is the panel, not the scale.** A table is built at the press
  when the window is on the 3D panel's output, and mid-drag when it reaches
  it: on a `wl_surface.enter`, and re-checked every ~1/3 s until the window's
  monitor is the panel.
- The ask-ahead, the search window and the correction were already in logical
  pixels and are unchanged.

An app against a v7 extension keeps the integer-scale-only behaviour.

### 8.7 Drag smoothness — the dense table (#1748)

A slow drag on the panel moved in visible sideways steps. That was measured in
a private headless GNOME Shell 50.1 running the v8 extension. The drag was a
real `xdg_toplevel.move` grab, driven by pointer injection through
`org.gnome.Mutter.RemoteDesktop`. The table came from the helper's own probe,
over a best-phase snap of the vendor weaver's shape. Distances are in device px.

- **Correct-after-apply is not the cause.** Of ~1,000 audited frames across
  200 % and 150 %, 0 were painted off the table. `position-changed` runs
  synchronously inside `move_resize`, so the correction lands before the next
  paint, as §8.2 relies on.
- **The table was coarser than the lens.** A 3 px probe keeps one entry per
  cell and leaves out 18 % (200 %) to 31 % (150 %) of the reachable
  phase-correct positions. So the nearest entry was up to 6.3 px away where
  4.5 would do. Along a straight drag, the choice of entry flips between
  neighbours on either side of the drag line, which shows as a wiggle across
  the drag direction.
- **Fix (displayxr-common, dense table):** on the grid-snap path the helper
  probes every logical px and keeps every reachable answer. The display
  processor does no extra work at a non-unit scale: the grid calls already
  cover every logical px. At 100 % the one call grows from 129×129 to 385×385
  points. The table stays ~13k entries, and the wire `cell` (the extension's
  bucket size) stays 3, so the extension is unchanged.

| scale | table | correction rms / max | across-track max |
|---|---|---|---|
| 200 % | 3 px | 3.8 / 6.3 | 6.0 |
| 200 % | dense | 3.1 / 4.5 | 4.0 |
| 150 % | 3 px | 3.1 / 5.4 | 4.7 |
| 150 % | dense | 2.5 / 4.5 | 4.5 |
| Windows, app-owned (reference) | — | 1.9 / 2.8 | 2.0 |

**What remains is reachability.** Mutter places a window at whole logical px,
so at 200 % only every other device px in each axis can be reached. Any table
at the same phase precision therefore sits about 1.6× coarser than the Windows
drag. Closing that gap would mean accepting a looser phase, which is the
display processor's decision, not the table's.

### 8.8 Version 9 — which entry a move lands on (#1748)

Every table entry is equally phase-correct, so the only freedom left during a
drag is which entry each move lands on. Up to version 8 that was the plain
nearest one. Along a straight drag, nearest picks entries on either side of
the drag line in turn, and the window wiggles sideways. A denser table
(displayxr-common's probe at every logical px, §8.7) removes the entries it
used to miss, but it cannot close the gap to Windows: mutter places windows at
whole logical px, so above 100 % only some device px are reachable.

Version 9 spends the remaining error where it shows least (`LatticeChoice` in
`lib.js`):

- **The drag direction** is estimated from the raw positions mutter proposes,
  in device px, with a memory of about 8 px of travel. It is an axis, so a drag
  that reverses along the same line keeps it, and a turn moves it.
- **The cost** of an entry is `along² + 9 · across²`, both measured from the
  raw position: a sideways error counts three times a lead or lag along the
  drag.
- **The lead or lag is capped at 6 device px.** Entries past it are not
  eligible. If none is eligible, or until the drag has travelled 4 device px,
  the choice is plain nearest.
- Everything is in device px (logical × the window's monitor scale), so the
  rule is the same at any output scale.
- `DISPLAYXR_LATTICE_NEAREST=1` in the shell's environment restores plain
  nearest, for A/B comparison on a panel.

Nothing on the wire changes, and every landing is still a table entry.
`scripts/test_gnome_extension_lattice.js` (gjs, in `lint.yml`) checks the
rule directly.

**Measured** in a private headless GNOME Shell 50.1, over the dense table
(displayxr-common#65), with a real `xdg_toplevel.move` grab driven through
`org.gnome.Mutter.RemoteDesktop` and a best-phase snap of the vendor weaver's
shape. Distances are in device px; sideways travel is the total sideways
motion of the window per 100 px dragged.

| scale | drag | sideways rms | sideways max | sideways travel /100 px | largest sideways jump | lead/lag max |
|---|---|---|---|---|---|---|
| 200 % | horizontal | 2.56 → 1.64 | 4.0 → 4.0 | 73 → 31 | 8.0 → 4.0 | 4.0 → 6.0 |
| 200 % | vertical | 2.02 → 1.46 | 4.0 → 2.0 | 45 → 22 | 6.0 → 4.0 | 4.0 → 6.0 |
| 200 % | 20° | 2.07 → 1.50 | 4.4 → 3.8 | 52 → 31 | 6.2 → 6.2 | 4.4 → 5.6 |
| 200 % | 45° | 1.94 → 1.77 | 4.4 → 3.8 | 34 → 35 | 5.9 → 5.9 | 4.4 → 5.6 |
| 200 % | reversing | 2.56 → 1.62 | 4.0 → 4.0 | 73 → 32 | 8.0 → 4.0 | 4.0 → 6.0 |
| 150 % | horizontal | 1.87 → 0.99 | 4.5 → 1.5 | 58 → 22 | 7.5 → 3.0 | 3.0 → 6.0 |
| 150 % | vertical | 1.84 → 1.21 | 4.5 → 3.0 | 46 → 24 | 6.0 → 4.5 | 3.0 → 6.0 |
| 150 % | 20° | 1.76 → 0.88 | 4.2 → 1.9 | 72 → 26 | 8.1 → 3.5 | 3.8 → 5.3 |
| 150 % | 45° | 1.64 → 1.33 | 3.2 → 2.1 | 51 → 34 | 5.3 → 4.2 | 3.2 → 5.3 |
| 150 % | reversing | 1.87 → 1.00 | 4.5 → 1.5 | 62 → 26 | 7.5 → 3.0 | 3.0 → 6.0 |
| Windows, app-owned (model, same snap) | all four | 1.31–1.41 | 2.0–2.8 | 45–81 | 4.0–4.8 | 2.0–2.8 |

Sideways rms and travel drop by a third to a half, and the largest sideways
jump halves on straight drags. The worst single sideways offset still reaches
4 device px at 200 % where the table has no closer entry within the cap. The
price is the lead or lag along the drag (≤ 6 px, from ≤ 4.5) and a larger
correction (rms 3.1 → 3.6 at 200 %, 2.5 → 3.3 at 150 %). Edge tiling (left half),
the top-edge maximize and a drag starting from rest behave as before, and no
frame inside the table's coverage was painted off it.



### 8.9 Version 9 — the pointer drag (a content drag on a secondary button)

DisplayXR apps drag their window from anywhere in the content with the RIGHT
button (the left one belongs to the scene), the same gesture as on X11 and
Windows. Handed to mutter with `xdg_toplevel.move`, that drag never ended on
the release: mutter accepts the request with any pressed button's serial
(`meta_wayland_pointer_get_grab_info` only asks for `button_count > 0`), but
its move grab ends only on the release of button 1 or of the resize button
(mutter 50.1, `src/compositor/meta-window-drag.c`, `process_pointer_event`,
`CLUTTER_BUTTON_RELEASE`: `button == 1 || button ==
meta_prefs_get_mouse_button_resize ()` — 2 by default). The window kept
following the pointer until the next click, and the client never saw the
release. Reproduced in a private headless GNOME Shell 50.1 with pointer
injection through `org.gnome.Mutter.RemoteDesktop`: after the right button
went up, a further 300 px of pointer motion moved the window 300 px, in 3 of 3
drags. Nothing outside mutter can end its grab (no introspected
`end_grab_op`; its grab is an input-only `ClutterGrab` with its own handler),
so the app no longer starts one for such a button.

Instead the extension follows the pointer itself — capability bit 2 (value 4),
`BeginPointerDrag(u pid, u button) -> (b started)` and `EndPointerDrag(u pid)`:

- **No grab.** The client keeps its implicit pointer grab, so it sees the
  motion and the release. Every move goes through `move_frame`, and so
  through the drag table's correction exactly as a compositor drag's does.
  While it runs, the window is reported `moving` and a table sent for it is
  active; it ends with `DragLatticeDone` like a grab.
- **It ends on the button mask.** The pointer is read every 4 ms
  (`global.get_pointer()`), and the drag is over on the first read whose
  modifier mask lacks the button — however the release happened, wherever
  the pointer is. Not an event filter: one added by an extension runs after
  mutter's own, which consumes every event it delivers to a Wayland client.
- **mutter files the middle and right buttons under each other's mask.** Its
  table (`src/backends/native/meta-seat-impl.c`, `maskmap`) is indexed by the
  Clutter button number but written in evdev order, so a held right button
  reads as `BUTTON2_MASK` (measured: mods `0x200`). For buttons 2 and 3
  either bit is accepted at the start, and the one actually set is watched.
- **Refused** when the button is already up at the call (a quick click),
  when a grab or another pointer drag is running, or for a fullscreen or
  unmovable window. A compositor grab that starts during it ends it.
- `EndPointerDrag` is the app's belt and braces (it saw the release); the
  mask ends the drag either way.

The app (displayxr-common `dxr_drag.h`) keeps `xdg_toplevel.move` for button 1,
and for any button on a compositor that is not mutter. On mutter without this
capability a secondary-button content drag does not move the window at all,
with one WARN, rather than start a drag nothing can end. Same measurement with
this version: the window followed the pointer while the button was held
(+300,+120 for a +300,+120 drag) and stayed put after the release, in 3 of 3
drags including a release far outside the window after a fast flick.

`scripts/test_gnome_extension_pointer_drag.js` (gjs, in `lint.yml`) checks the
pure part (`PointerDrag` in `lib.js`).

## 9. Move-synchronised re-weave (extension version 9, #1748; tag-gated in version 10)

**On by default** on native Wayland whenever the extension offers it
(`EnableMoveSync`, placement capability bit 3 = value 8; GNOME 45+). The drag
lattice (§8) is the fallback, unchanged, wherever move sync is not available:
an extension older than version 9, the GNOME 40–44 entry point (which answers
`false`), X11/XWayland, no extension at all, or `DXR_WL_MOVE_SYNC=0` in the
app's environment.

### 9.1 Idea

The lattice keeps a stale weave correct by allowing only the positions where
it stays phase-correct. Move sync inverts that: the window may sit on any
logical pixel, and the stage never paints a frame anywhere but where it was
woven for. mutter keeps its grab (or the extension its pointer drag, §8.9)
and moves the `MetaWindow` as usual, so the published geometry, edge tiling
and workspaces all see the real position. Only the window's **actor** is held
back: on every stage frame, in `before-update` (after that frame's input and
commits, before layout and paint), the extension places it at the position
the buffer it is about to paint was woven for.

Because every painted frame is woven for exactly where it is painted, the
result is exact for any lens, not only where a stale weave happens to be
phase-equivalent. It needs no table probe, and every logical position is
reachable at any scale.

### 9.2 Binding a buffer to its woven position

GJS has no per-commit identifier. `MetaWindowActor::damaged` fires inside the
commit's apply, before the subsurface state is synced, and
`meta_window_actor_freeze()` cannot hold a Wayland window: freezing also stops
mutter syncing the surface actors (the new texture and the subsurface
positions) until the thaw.

So the runtime tags the commit itself (`comp_vk_native_wl_move_sync`):
- A 1x1 clear **synchronised** subsurface of the bound surface, placed below
  it, with an empty input region.
- It sits at `(x mod 256, y mod 256)` of the logical content position the
  frame was woven for — the snapshot `vk_get_window_metrics` used for that
  frame's present origin (`comp_vk_native_wl_geom_last_rect`), captured per
  weave and set **immediately before that weave's present, under the
  compositor lock**. A fill's fence-park releases the lock between its weave
  and its present (§9.6), so the position is carried in the weave's own
  storage, never read back from shared state.
- A synchronised subsurface's position is parent state: it lands atomically
  with the buffer the WSI commits in `vkQueuePresentKHR`.
- While the window covers its whole monitor (fullscreen) the tag's pixel is
  detached, so the window stays a single surface for direct scanout; a
  fullscreen window is never dragged.

The encoding is `comp_vk_native_wl_move_sync_encode` (pinned by
`tests/tests_comp_wl_move_sync_tag.cpp`); the extension's decode is
`MoveSyncChoice.decode` (pinned by `scripts/test_gnome_extension_move_sync.js`
against a transcription of the same function).

The extension reads the tag off the window's surface actors (`_readTag`): the
1x1 actor is the tag, and the **main surface** is the actor whose content is
the window's texture — `MetaWindowActor.get_texture()` is the main surface's
`MetaShapedTexture`, and mutter sets that same object as the surface actor's
content, so the pairing is structural: the toplevel's own surface, the one the
tag is a subsurface of (runtime and present-owner alike, §9.8). The tag is
decoded as its position minus the main surface's, both relative to the window
actor (`MoveSyncChoice.pick`, extension version 12, pinned by the same test).
Versions 9–11 took the **largest** surface actor instead. Under libdecor-gtk —
GLFW 3.4's and SDL2/SDL3's stock decorations on GNOME — the shadow subsurface
is larger than the content, so every tag decoded against the shadow's origin,
no frame ever resolved against the window's history, and the hold timed out on
every title-bar drag: stale frames and visible 3D stutter, found on the LeiaSR
OpenGL example (the same drag was smooth with libdecor disabled). An app that
draws its own chrome in a smaller subsurface, as the test apps and the browser
do, was never affected. Without a texture match (none handed out, or no actor
carrying it — not seen on mutter 45–50) the largest actor still stands in.

### 9.3 Placement rules (`MoveSyncChoice` in `lib.js`)

The extension resolves the tag against the positions the window had in the
last second, **newest first**, and shows the frame there:
- **By position.** Every tagged frame is shown where its tag says. A tag names
  a position, and which moment of the history it came from is ambiguous only
  when the window revisits a position (a reversal) — where either answer is
  the same pixel. Commits reach mutter in present order, each woven for a
  position the window had at or after the previous one's. (The prototype
  instead ignored a tag that resolved OLDER than the one shown; at a reversal
  that painted frames woven on the way to the turn off their origin — 1.1 %
  of a reversal's frames.)
- **Stall fallback.** After 100 ms *and* 6 stage frames behind the window with
  no newer woven frame, the actor follows the window, showing stale frames
  (counted as a timeout). It re-syncs on the first frame woven for a position
  the window had within the last 60 ms.
- **Tick at a stop.** While the actor is behind a window that has stopped, a
  redraw is queued each frame; otherwise nothing schedules a stage update and
  the frame woven for the final position waited ~300 ms.
- **Both movers.** mutter's move grab (title bar, Super+drag) and the pointer
  drag (§8.9, a right-button content drag) start and end the same hold.
- **Geometry every frame.** A move of a window in a hold is published
  (`WindowsChanged`) at once. The coalescing `BEFORE_REDRAW` later, added from
  `position-changed`, only runs in the NEXT frame — so the runtime saw a new
  position every other frame and the window advanced on at most half the
  paints. That, not the runtime's weave rate, was the prototype's 30 Hz
  cadence.

### 9.4 Registration and fallback

- The runtime creates the tag first, then calls `EnableMoveSync(0)`.
  `UnknownMethod` (an older extension) or `false` → the tag is destroyed and
  the lattice path runs. No answer (no extension yet) → the request is
  remembered.
- The registration lives with the caller's bus connection. GNOME disables user
  extensions while the screen shield is up, so the runtime registers again
  whenever the service name reappears.
- Per frame, the runtime trusts the snapshot's `move_sync` for its window: only
  when it is set does it skip the drop-time snap (#1609). Until the extension has
  (re)accepted, the window is a lattice window.
- The extension refuses drag tables for a registered process, and
  displayxr-common (the app side) reads `move_sync` from the same snapshot it
  already fetches at the press and derives no table for that drag: each table
  costs the frame loop ~90 ms of snap probing.

### 9.5 Cadence: a weave per refresh

The window advances only when a frame woven for its new position is painted.
The runtime therefore re-weaves during a move-sync drag whenever no weave has
chosen an origin for 7/8 of a refresh period (`vk_wl_move_fill_due`,
`DXR_WL_MOVE_SYNC_FILL=0` to disable): the repaint loop bypasses its quiet gate
and phase hold for that, but never `app_frame_in_progress` or the submit
window, and adds at most one weave per period. An app that is FIFO-bound
already weaves every slot the queue drains, so the fill rarely fires; with a
30 Hz app the difference it made stayed within run-to-run noise in the
harness, because the ordinary repaint gate already refills most of the gaps.

### 9.6 Found on the way: the fill fence-park leaked swapchain images

`DXR_WEAVE_REPAINT_FORCE=1` hung the prototype's app within seconds: the app
thread sat in `vkAcquireNextImageKHR` (waiting on the explicit-sync release
point) holding the compositor lock, and the repaint thread waited for that
lock. Cause: a fill whose fence-park lost the race to an app frame returned
without presenting the image it had acquired, and Vulkan hands an acquired
image back only through a present, so each lost race leaked one image for the
life of the swapchain. The target now holds such an image and hands it to the
next acquire (`comp_vk_native_target_hold_unpresented`; a recreate drops the
hold). With the fix, `DXR_WEAVE_REPAINT_FORCE=1` runs a full drag suite with
no hang and 0 frames off their woven origin. The default gate hit the same
race rarely, so long sessions were exposed too.

### 9.7 Measured (private headless GNOME Shell 50.1)

**Setup.** A private headless GNOME Shell 50.1 (own display and session bus),
real drags through that instance's `org.gnome.Mutter.RemoteDesktop`:
left-button title-bar drags (mutter's grab) and right-button content drags
(the pointer drag). `cube_handle_vk_linux` on sim_display only
(`DXR_PLUGIN_EXCLUSIVE=sim-display`), windowed 1600x1000 device px, its
content drag on the right button (`DXR_CUBE_DRAG_BUTTON=3`, as the demos). The
harness is `scripts/linux/move_sync/` (`run.sh default|lattice|reweave SCALE
OUT DRAG...`).

**Stamp audit.** With `DXR_WL_ORIGIN_STAMP=1` the runtime writes a barcode of
each frame's present origin into its top-left 352x8 px (a measurement hook: it
overwrites content). With `DISPLAYXR_STAMP_AUDIT=1` in the shell's environment
the extension reads it back after every stage paint and compares it with
where the stage painted the window. "Off-origin" = painted at a device origin
other than the one it was woven for. Lag = pointer to painted content. "Still"
= paints on which a moving window did not advance. Slow = 1 logical px per
8 ms, fast = 10 logical px per 8 ms. Device px.

| scale | build | off-origin | lag slow p50 | lag fast p50 | still (slow LMB / RMB) |
|---|---|---|---|---|---|
| 200 % | prototype (#1752) | 0 % (reversal 1.1 %) | 14 | 160 | 51 % / — |
| 200 % | **this** | **0 %** (1,313 frames, reversal included) | 12 | 140 | 22 % / 18 % |
| 150 % | prototype (#1752) | 0 % | 9 | 90 | 51 % / — |
| 150 % | **this** | **0 %** (1,526 frames) | 10.5 | 90 | 22 % / 26 % |
| 200 % | lattice (§8, `DXR_WL_MOVE_SYNC=0`) | 92 % (phase-correct by table) | 2 | — | 68 % |

Every drag type at both scales reads 0 frames off their woven origin: straight
(horizontal and a 20° diagonal), fast, reversal, start from rest, release in
motion, and the same set as right-button pointer drags. No timeout fires.
- **Reversal:** 0 % off-origin (the prototype: 1.1 %).
- **Edge tiling:** a drag to the left edge tiles to the left half, to the top
  edge maximises, at both scales; the hold ends as the window is resized.
- **App frozen (SIGSTOP 670 ms mid-drag), both buttons:** one timeout per
  drag; the window follows the pointer with stale frames (27–28 % of that
  drag's frames), then re-syncs with one backward hop of ≤ 120 device px
  (200 %) / ≤ 67 (150 %).
- **Extension version 8** (the published one before this): `EnableMoveSync`
  answers `UnknownMethod`, the runtime logs it once and runs the lattice path;
  the app derives and sends tables, 0 frames painted off-table. A right-button
  content drag does not move the window (displayxr-common's one WARN), since
  that extension has no pointer drag either.

**Lag.** About 2.9 frames at 200 % (3.4 in the prototype) and unchanged at
150 %. It is the pipeline depth: publish → the app's next weave → a FIFO queue
that, with a FIFO-bound app, holds up to three frames → paint. Measured levers
(not shipped): a minimum-size swapchain (3 images instead of 4) cut it to ~2
frames; `MAILBOX` to ~1 frame, but then nothing paces the app (it wove ~700
frames/s). Either changes every Wayland app's presentation, not only drags, and
needs a panel run first.

### 9.8 Present-owners tag their own commit (XR_DXR_weave v12, browser-pvt#180)

Everything above keys off a window-bound `vk_native` session: the runtime owns
the surface, so it creates the tag subsurface and sets it before its own
present. A **present-owner** (an `XR_DXR_weave` caller on the service path —
the DisplayXR browser) presents the woven buffer itself, through its own
surface, so the runtime cannot tag that commit. Present-owners tag their own
commit from the v12 origin: every `xrWeaveSubmitDXR` returns, in a chained
`XrWeaveOutputOriginDXR`, the origin that submit's output was woven for — the
geometry the weave fed the display processor, captured per weave under the
engine lock, the service-side twin of `comp_vk_native_wl_geom_last_rect` in
§9.2 — plus a per-output serial. The runtime never converts device px back to
logical (the service knows no output scale, and a fresh rounding is how a
150 % output lands one pixel off, #1609): the present-owner binds its logical
content origin and scale beside its device geometry
(`XrWeaveWindowLogicalOriginDXR`) and gets them echoed verbatim per output.
It then places its own 1×1 synchronised subsurface at
`(wovenOriginLogical mod 256)` — the same encoding as
`comp_vk_native_wl_move_sync_encode`, `COMP_VK_NATIVE_WL_MOVE_SYNC_TAG_MOD` =
the extension's `MOVE_SYNC_TAG_MOD` — and commits it with the buffer that
carries that output. Registration (`EnableMoveSync`) and the §9.4 fallback are
the present-owner's too; the extension authorises by caller pid, which for a
multi-process browser is a question for its side. API and wire:
`docs/specs/extensions/XR_DXR_weave.md` §5e.

### 9.9 Tag-gated hold (extension version 10)

`EnableMoveSync` is per **process**, but a present-owner (§9.8) often shows
nothing woven — a browser on a 2D page tags no commit. Under version 9 every
drag of such a window was held with no tagged frame to resolve, so the actor
stood still until the stall fallback (§9.3, 100 ms and 6 frames) let it
follow: a hitch at the start of every 2D drag. Version 10 holds a synced
window **only while a tag subsurface is mapped** (`MoveSyncChoice.gate` in
`lib.js`, checked on every stage frame before `onFrame`):

- **Move start** (mutter's grab or the pointer drag, §8.9): the hold starts
  only if the buffer on screen carries a tag. Otherwise the window drags as a
  plain window — the actor is wherever mutter puts it. The move is tracked
  either way: its position history is kept and a move is published every
  frame, as for a held one.
- **A tag appears mid-move** (a 3D element scrolls into view): the hold
  starts on that frame, from the window's current position. The kept history
  resolves that very frame's tag, so it is shown where it was woven for — one
  step back by the pipeline lag (measured below).
- **The tag goes away mid-hold** (unmapped, or its actor hidden): the hold ends
  on that frame — the actor is put at the window's real position, with no
  timeout and no hop back — counted per drag as a *tag-lost release*
  (`… N hold(s), M tag-lost release(s)` in the `DISPLAYXR_DEBUG` summary). The
  hold starts again if a tag reappears before the move ends.
- **Stall fallback unchanged:** a mapped tag whose frames stop coming (the app
  is stalled) still takes the §9.3 timeout.
- **Unchanged for an app that keeps its tag mapped** — every in-process
  `vk_native` window does, except fullscreen, which is never dragged (§9.2).

**Capability:** placement capability bit 4 (value 16),
`PLACEMENT_CAP_MOVE_SYNC_TAG_GATED`, only ever set together with bit 3. A
present-owner that leaves its tag unmapped while it shows nothing woven checks
it to know that such a drag does not hitch; with bit 4 clear (a version-9
extension) it should keep the tag mapped, or accept the hitch.

**Test hook** (off by default, never in a real session):
`DXR_WL_TEST_TAG=off` never maps the runtime's tag (a synced process that
tags nothing); `DXR_WL_TEST_TAG=toggle:ON_MS:OFF_MS` maps it for `ON_MS`, then
unmaps it for `OFF_MS`, repeating. The registration is unchanged.

**Decoding against the parent surface: not possible from GJS on GNOME 50 —
the main surface is found by its texture instead (version 12).** The parent
surface would be the exact reference, but mutter 50 flattens a window's
surface actors into siblings under one `MetaSurfaceContainerActorWayland` (the
tag's Clutter parent is the container, not its parent surface),
`MetaSurfaceActorWayland` exposes no surface accessor or property to GObject
introspection, and `Meta.WaylandSurface` exposes only `get_window()`. Only the
sibling order hints at the parent (a `place_below` tag sits just before it),
which a client may reorder. What *is* reachable is the toplevel's own surface:
`MetaWindowActor.get_texture()` returns its `MetaShapedTexture`, which is that
surface actor's content (§9.2). So a present-owner keeps its tag a subsurface
of the toplevel's (root) surface — the browser's root surface already is — and
that surface no longer has to be the largest; versions 9–11 decoded against
the largest surface, which a client-side shadow bigger than the content broke.

**Measured** (the §9.7 harness, private headless GNOME Shell 50.1,
`cube_handle_vk_linux` on sim_display only, stamp audit after every paint;
LMB = mutter's grab, RMB = the pointer drag):

| run | scale | frames | held frames off their woven origin | plain frames with the actor not at the window | holds / tag-lost / timeouts |
|---|---|---|---|---|---|
| always tagged (in-process, 17 drags LMB + RMB, reversals) | 200 % | 1,645 | **0** | — | 17 / 0 / 0 |
| always tagged | 150 % | 1,698 | **0** | — | 17 / 0 / 0 |
| never tagged (`DXR_WL_TEST_TAG=off`, 11 drags) | 200 % | 1,034 | — | **0** | 0 / 0 / 0 |
| never tagged | 150 % | 1,058 | — | **0** | 0 / 0 / 0 |
| never tagged, **version 9** (6 drags) | 200 % | 673 | 594 (held, then following stale) | — | 6 timeouts: every drag hitched, up to 110 logical px behind |
| toggled 300/300 ms (8 drags) | 200 % | 992 | **0** of 495 | **0** of 497 | 37 / 34 / 0 |
| toggled 300/300 ms | 150 % | 988 | **0** of 487 | **0** of 501 | 35 / 34 / 0 |

- Always tagged, lag p50 slow / fast: 12 / 120 device px at 200 %, 9 / 90 at
  150 %; paints where a moving window stood still: 22 % / 25 % — within the
  §9.7 run-to-run spread.
- Never tagged: the actor is on the window on every paint (lag 0 — a plain
  mutter drag).
- Toggled: a hold starting mid-move steps back by up to 60 device px at
  200 % (45 at 150 %), and a tag-lost release steps forward to the window by
  up to 120 (81) — each the pipeline lag at that moment, on the fastest
  toggled drag (6 logical px per 8 ms). That is the cost of a tag toggling
  during a drag; it never takes the timeout.
- App frozen mid-drag with the tag mapped: one timeout per drag, as in §9.7.

## 10. Workspace hotkey — `org.displayxr.WorkspaceHotkey1` (extension version 13)

A Wayland client cannot grab a global chord, and the DisplayXR service that owns
the workspace-controller launch chord is socket-activated and exits 30 s after its
last client — so it is usually not running when the chord is pressed. The extension
is the one long-lived process in the session that can hold a keybinding, so it
holds it **for** a client:

| member | meaning |
|---|---|
| `Configure(s accelerator, s unit) -> (b pending)` | grab `accelerator` (GTK syntax, `<Control>space`; `""` releases and forgets) for the caller, and remember it with `unit` (a systemd **user** unit, `""` = none) across the caller's exit and shell restarts (`$XDG_STATE_HOME/displayxr/workspace-hotkey.json`). `pending`: a press happened while no caller was registered, within the last 30 s |
| `Suspend(b suspend, u timeoutMs)` | release the grab until `Suspend(false)` or the timeout (capped at 60 s) — a hotkey-capture box must see the chord |
| `GetState() -> (s)` | JSON diagnostics: `version`, `accelerator`, `unit`, `registered`, `grabbed`, `suspended` |
| signal `Activated(u timestamp)` | the accelerator was pressed |

The caller is registered exactly as long as its bus connection. A press while it is
registered only emits `Activated`; a press while it is gone also calls
`org.freedesktop.systemd1.Manager.StartUnit(unit, "replace")` and is reported back
by that client's next `Configure`. The grab is `Meta.Display.grab_accelerator`
(mutter's own path, Wayland and X11 GNOME alike), allowed in the NORMAL and
OVERVIEW action modes. Nothing is grabbed until a client configures.

**Bootstrap.** So a freshly installed workspace controller gets its chord without the
client having run since, the extension also watches the DisplayXR workspace-controller
manifest roots (`$XRT_WORKSPACE_CONTROLLER_PATH`, `$XDG_DATA_HOME/DisplayXR/WorkspaceControllers`,
`/usr/local/share/displayxr/WorkspaceControllers`, `/usr/share/displayxr/WorkspaceControllers`
— the runtime registry's order): 5 s after enable and 2 s after a change in a watched
root, if the `*.json` set differs from the one recorded at the last `Configure` (the cache
records it, including for a `Configure("")`), it starts the last configured unit — else
`displayxr.service` — once per set per shell session. The bookkeeping
is the pure `WorkspaceHotkey` object in `lib.js`, unit-tested by
`scripts/test_gnome_extension_workspace_hotkey.js`; the consumer is
`src/xrt/targets/service/service_hotkey_linux.c`. Contract and lifecycle:
`workspace-controller-registration.md` § Linux.

Shared-asset note (§4): the interface itself knows nothing about DisplayXR — it holds one
accelerator for one client and starts the unit that client named. The unit name comes
from the client, the systemd manager is the user's own, and only a `*.service` name is
accepted. The bootstrap is the one DisplayXR-specific piece (the manifest roots and the
default unit name); it does nothing on a box without workspace-controller manifests.

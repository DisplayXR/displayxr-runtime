# Wayland Window Geometry Provider (windowed weaving under Wayland)

- **Issue:** #817
- **Status:** Prototype — pending on-hardware validation
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

- **Consumer** — `comp_vk_native_wl_geom` (`src/xrt/compositor/vk_native/`,
  built when `XRT_HAVE_WAYLAND && XRT_HAVE_DBUS`, libdbus-1). Private
  session-bus connection; one blocking `GetWindows` at create (200 ms cap),
  then a non-blocking signal pump per query. Created by the compositor when
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
| Monitor scale ≠ 1.0 | Rect returned, one WARN — weave phase will be wrong until the display is set to 100 % scale (same constraint as X11 windowed weaving) |

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
force-enabled for users who have opted out.

**Schema evolution is additive within a version.** Publishers may add fields
freely; consumers look up only what they know and ignore the rest. Anything
that changes the *meaning* of an existing field — logical → physical pixels,
a different coordinate origin — is a breaking change and MUST bump `version`.
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

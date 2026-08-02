# DisplayXR Window Geometry (GNOME Shell extension)

Wayland deliberately never tells a client where its window sits on the desktop
— but windowed weaving needs exactly that: the lenticular interlacing phase is
anchored to the window's absolute panel position. The compositor (Mutter) is
the one component that authoritatively knows every window's global geometry,
so this extension publishes it over the session D-Bus for the DisplayXR
runtime to consume (`comp_vk_native_wl_geom` provider, runtime#817).

On X11 this is unnecessary — the runtime queries `xcb_translate_coordinates`
directly. This extension only matters for Wayland sessions with apps using
`XR_DXR_wayland_surface_binding`.

## Install (manual, until packaged)

```bash
UUID=window-geometry@displayxr.org
mkdir -p ~/.local/share/gnome-shell/extensions/$UUID
cp extension.js metadata.json ~/.local/share/gnome-shell/extensions/$UUID/
# Log out/in (Wayland cannot hot-reload the shell), then:
gnome-extensions enable $UUID
```

Verify it's live:

```bash
gdbus call --session --dest org.displayxr.WindowGeometry \
  --object-path /org/displayxr/WindowGeometry \
  --method org.displayxr.WindowGeometry1.GetWindows
```

## D-Bus surface

- Service `org.displayxr.WindowGeometry`, object
  `/org/displayxr/WindowGeometry`, interface `org.displayxr.WindowGeometry1`.
- `GetWindows() -> (s)`: JSON snapshot of all normal windows.
- `WindowsChanged(s)`: same JSON, emitted (coalesced per redraw) on any
  position/size/focus/lifetime change.

Schema and coordinate-space notes are documented at the top of
`extension.js` and in `docs/specs/runtime/wayland-window-geometry.md`.

## Packaging (for distributors — including vendor runtime packages)

This extension is a **shared asset, not a DisplayXR-private component**: it
reports window rectangles and knows nothing about weaving, lenses, or any
runtime, so a **vendor SDK runtime package may ship it** (that runtime needs
the same geometry to serve its own non-DisplayXR apps). The full contract is in
[`docs/specs/runtime/wayland-window-geometry.md`](../../../docs/specs/runtime/wayland-window-geometry.md)
§4; the short version:

- **Keep the identifiers.** UUID `window-geometry@displayxr.org`, bus name
  `org.displayxr.WindowGeometry`, interface `org.displayxr.WindowGeometry1`.
  The prefix names who defines the schema, not who ships it. Renaming forks
  the ecosystem.
- **Exactly one installed owner** — a D-Bus well-known name is singly owned, so
  a second publisher binds nothing and silently strands consumers. Any package
  shipping it declares `Provides:` + `Conflicts:` + `Replaces:` on the virtual
  package `displayxr-window-geometry-publisher`, and consumers depend on that
  virtual name rather than on any vendor's package.
- **Install system-wide** to
  `/usr/share/gnome-shell/extensions/window-geometry@displayxr.org/` (the
  per-user path above is for manual dev installs). Enabling is still per user
  session; seed it via a dconf default for `org.gnome.shell enabled-extensions`.
- **Schema is additive within a version.** Add fields freely; bump `version` in
  the payload only when the *meaning* of an existing field changes (e.g.
  logical → physical pixels). Consumers refuse a version they don't understand
  and fall back to display-scoped rather than weave at a wrong phase.
- **No reverse dependency** — keep it pure GNOME Shell JS, with no import from
  or dependency on DisplayXR or any vendor stack.

## Constraints

- Coordinates are logical pixels; windowed weaving requires monitor scale 1.0
  (the provider reports the scale so the runtime can warn otherwise).
- Runtime matches windows by PID → works for in-process apps; IPC/service
  mode needs the client PID plumbed (tracked in runtime#817).

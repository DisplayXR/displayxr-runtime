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

**Version 2 adds capture exclusion** — the GNOME equivalent of Windows'
`WDA_EXCLUDEFROMCAPTURE`. A process can ask for its own windows to be left out
of every off-screen stage paint (mutter ScreenCast `RecordArea`, screenshots)
while they keep drawing on screen. The Leia Linux display processor uses it to
capture the desktop *behind* a transparent 3D window, which is what removes the
double image and the halo and gives the rear depth budget a real background to
measure. Without version 2 the display processor refuses to capture and falls
back to silhouette intersection. Full contract:
`docs/specs/runtime/wayland-window-geometry.md` §6.

## Install

Both Linux packages install it. Only GNOME Shell loads it, so it has no effect
on other desktops.

| Package | Installs to | Enabled for |
|---|---|---|
| `displayxr-runtime` `.deb` | `/usr/share/gnome-shell/extensions/window-geometry@displayxr.org/` | Every user, at their next GNOME login, by `/etc/xdg/autostart/displayxr-gnome-extension-enable.desktop`. It never re-enables the extension for a user who disabled it. |
| Tarball, `./install.sh` | `~/.local/share/gnome-shell/extensions/window-geometry@displayxr.org/` | The installing user, straight away (in their settings). |
| Tarball, `sudo ./install.sh --system` | `/usr/local/share/gnome-shell/extensions/window-geometry@displayxr.org/` | Every user at their next login, as for the `.deb`. |

**Then log out and back in.** A Wayland session cannot reload GNOME Shell, so
a newly installed extension, or a new version of an installed one, loads only
at the next login. Until then the runtime logs that the extension is installed
but not active, and falls back to display-scoped weaving (and, for
transparent apps on a Leia panel, silhouette intersection).

**How "enabled for every user" works, and why this way.** GNOME keeps the list
of enabled extensions per user (`org.gnome.shell enabled-extensions`). A dconf
system default for that key reaches only users who have never written it, and
anyone who has ever toggled an extension has, so for most people it would do
nothing. The package therefore runs
`/usr/lib/displayxr/bin/displayxr-gnome-extension-enable` at each GNOME login.
The script:

- acts **once per user**. After the first time it records a stamp in
  `~/.local/state/displayxr/` and does nothing more, so the extension is never
  re-added once the user has removed it, however they removed it;
- does nothing for a user whose `disabled-extensions` lists the UUID. That is
  GNOME's own record of "I switched this off";
- does nothing while the user has switched off all extensions
  (`disable-user-extensions`), and tries again at a later login;
- does nothing if an administrator has locked the key;
- writes the setting directly instead of calling `gnome-extensions enable`,
  because a running shell refuses to enable an extension it did not see at
  startup.

No dconf lock is involved. Users can always disable it.

To turn it back on after disabling it:

```bash
gnome-extensions enable window-geometry@displayxr.org   # then log out and back in
```

Manual install from this directory (development):

```bash
UUID=window-geometry@displayxr.org
mkdir -p ~/.local/share/gnome-shell/extensions/$UUID
cp extension.js metadata.json ~/.local/share/gnome-shell/extensions/$UUID/
# Log out/in (Wayland cannot hot-reload the shell), then:
gnome-extensions enable $UUID
# Updating an already-enabled copy: copy the files, then log out/in. A running
# shell keeps the old code until then (`gnome-extensions info $UUID` shows the
# version it loaded).
```

A copy in `~/.local/share` takes precedence over the system one, so a leftover
manual install shadows a packaged update. Remove it when switching to a
package.

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
- Version 2: object `/org/displayxr/CaptureExclusion`, interface
  `org.displayxr.CaptureExclusion1` — `Exclude(u pid) -> (u windows)` (pid 0 =
  the caller; only your own PID), `Release(u pid)`, `GetState() -> (s)`. The
  exclusion lasts as long as the caller's bus connection, and `disable()`
  (including the lock screen) removes every effect.

Verify capture exclusion is live:

```bash
gdbus call --session --dest org.displayxr.WindowGeometry \
  --object-path /org/displayxr/CaptureExclusion \
  --method org.displayxr.CaptureExclusion1.GetState
```

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
  per-user path above is for manual dev installs). Enabling is still per user.
  A dconf default for `org.gnome.shell enabled-extensions` reaches only users
  who never set that key. The runtime `.deb` enables it at each user's login
  instead, as described under *Install*.
- **Schema is additive within a version.** Add fields freely; bump `version` in
  the payload only when the *meaning* of an existing field changes (e.g.
  logical → physical pixels). Consumers refuse a version they don't understand
  and fall back to display-scoped rather than weave at a wrong phase.
- **No reverse dependency** — keep it pure GNOME Shell JS, with no import from
  or dependency on DisplayXR or any vendor stack.

## Constraints

- Coordinates are logical pixels; windowed weaving requires monitor scale 1.0.
  The provider reports the scale, and at anything other than 1.0 the runtime
  **refuses** the rect and stays display-scoped rather than weave at a phase it
  knows is wrong (runtime#1557) — the compositor also resamples the surface at
  non-unit scale, which destroys an interlace regardless of phase.
- GNOME Shell versions 45–50 (`shell-version` in `metadata.json`). Validated
  live on GNOME 50.1 / Ubuntu 26.04 (runtime#817). That covers Ubuntu 24.04
  (GNOME 46) and 26.04 (GNOME 50), but **not 22.04 (GNOME 42)**: the extension
  is an ES module, which GNOME only loads from 45 onwards. There it installs
  but GNOME reports it as out of date and never loads it. A newly installed
  extension is only picked up at the next login — Wayland cannot restart the
  shell.
- Runtime matches windows by PID → works for in-process apps; IPC/service
  mode needs the client PID plumbed (tracked in runtime#817).

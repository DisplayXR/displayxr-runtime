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

**Version 3 adds window placement** — `org.displayxr.WindowPlacement1`, one
method that moves a window of the *calling process* to a given logical
position, plus a `moving` flag on every published window. A Wayland client
cannot position itself, but a weaving window has to land on the interlace
lattice after a drag or the 3D shimmers, and only the compositor can put it
there. Without version 3 the runtime keeps whatever phase the window was
dropped on. Full contract:
`docs/specs/runtime/wayland-window-geometry.md` §7.

## Two entry points, one set of logic

GNOME Shell 45 changed how an extension is loaded. From 45 it is an **ES
module** (`import Clutter from 'gi://Clutter'`, `export default class extends
Extension`); before that — GNOME 40 to 44, which is what Ubuntu 22.04's GNOME
42 is — it is the **legacy importer** (`imports.gi`, a top-level `init()`),
and ESM syntax is a parse error there. Every mutter and Clutter API this
extension uses exists in both, so the only obstacle is the module system.

| File | Loaded by | What it is |
|---|---|---|
| `lib.js` | both | **All** of the logic: the three D-Bus objects, the capture-exclusion effect, the snapshot. |
| `extension.js` | GNOME 45+ | ~20 lines: imports `gi://…`, loads `lib.js`, subclasses `Extension`. |
| `extension-gnome42.js` | GNOME 40–44 | ~20 lines: `imports.gi`, loads `lib.js`, returns the service from `init()`. |

`lib.js` is shared verbatim because it contains **no `import` and no `export`
statement**, which makes it simultaneously a valid (side-effect-only) ES
module and a valid legacy GJS module. It cannot export in either idiom, so it
publishes itself on `globalThis` and takes the GI namespaces as an argument —
only the entry point knows which spelling its shell understands. CI compiles
it under both goals (`scripts/check_gnome_extension.py`), so that property
cannot quietly rot.

A GNOME extension directory holds exactly one `extension.js`, so **the
installer picks**: `scripts/linux/displayxr-gnome-extension-enable` reads
`gnome-shell --version` and, below 45, puts `extension-gnome42.js` in that
slot — in place for a user-writable install, or in a per-user *shadow* copy
(which GNOME prefers over the system one) when the files belong to root. On a
45+ shell it undoes that again, so a distribution upgrade repairs itself. Both
forms declare themselves in one `metadata.json`, whose `shell-version` is the
union `42`–`50`; CI asserts that list covers every release the `.deb` is
installed into.

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
cp extension.js extension-gnome42.js lib.js metadata.json \
   ~/.local/share/gnome-shell/extensions/$UUID/
# On GNOME 40-44 only, put the legacy entry point in the slot GNOME reads
# (or run displayxr-gnome-extension-enable --install, which does it for you):
#   cp extension-gnome42.js ~/.local/share/gnome-shell/extensions/$UUID/extension.js
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
- **Ship the whole file set** — `metadata.json`, `lib.js` and **both** entry
  points (`extension.js`, `extension-gnome42.js`) — and select between them by
  the running shell's version, as described under *Two entry points*. Shipping
  only the ES module silently drops every pre-45 desktop; shipping only the
  legacy one drops every 45+ desktop.
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
- GNOME Shell versions 42–50 (`shell-version` in `metadata.json`), covering
  Ubuntu 22.04 (GNOME 42), 24.04 (GNOME 46) and 26.04 (GNOME 50). Validated
  live on GNOME 50.1 / Ubuntu 26.04 (runtime#817). **The GNOME 42 path is not
  hardware-validated yet** — the APIs are all present in mutter 42.9 and the
  form is syntax-checked in CI, but nobody has watched it run; see
  *Validating on GNOME 42* below. A newly installed extension, or a newly
  selected entry point, is only picked up at the next login — Wayland cannot
  restart the shell.
- The entry-point swap happens at login, from `/etc/xdg/autostart`, which runs
  *after* GNOME Shell has already loaded its extensions. So the first login
  after installing on a pre-45 shell logs a JS parse error (the shell tried the
  ES module) and the extension is inactive; the swap it makes takes effect at
  the second login. Same "log out and back in" story as any other install here,
  one login later.
- Runtime matches windows by PID → works for in-process apps; IPC/service
  mode needs the client PID plumbed (tracked in runtime#817).

## Validating on GNOME 42

No CI machine and no dev box here runs GNOME 42, so this is the by-hand pass
that an Ubuntu 22.04 box (Wayland session) has to go through. Steps 4 and 5 are
the ones that prove the two non-obvious mechanisms actually work on mutter 42,
rather than merely loading.

1. Install the `.deb`, log out, log back in, log out and back in once more
   (the first login is when the autostart script swaps the entry point; see
   *Constraints*).
2. `gnome-extensions info window-geometry@displayxr.org` — state `ACTIVE`, and
   `journalctl --user -b -u org.gnome.Shell@wayland` free of JS errors from the
   extension.
3. Geometry:
   `gdbus call --session --dest org.displayxr.WindowGeometry --object-path /org/displayxr/WindowGeometry --method org.displayxr.WindowGeometry1.GetWindows`
   returns JSON listing the open windows with plausible `frame`/`buffer`
   rectangles, and they change as a window is dragged.
4. Capture exclusion — the measurement that proves the trick:
   have a process call `CaptureExclusion1.Exclude(0)`, then
   `gdbus call --session --dest org.displayxr.WindowGeometry --object-path /org/displayxr/CaptureExclusion --method org.displayxr.CaptureExclusion1.GetState`
   and read `paints`. **`skipped` must be non-zero** (and growing while
   something records the screen area) with `onscreen` also growing: that is the
   off-screen paints being dropped while the on-screen ones are forwarded. A
   `skipped` stuck at 0 means the `before-paint`/`after-paint` bracket is not
   the discriminator it is on 45+, and the exclusion silently does nothing.
   The window must stay visible on screen throughout.
5. Placement: `WindowPlacement1.MoveWindow(0, x, y)` returns `true` and the
   window moves, and returns without moving while the window is being dragged.

# Omarchy support (Arch Linux + Hyprland)

**Status:** written and covered by CI. The CI job is "SteamOS + Omarchy
(Arch)", which runs a fake Hyprland IPC on a private session bus.
**No run on a real Hyprland or on Omarchy hardware yet.** The work stacks on
the SteamOS branches ([steamos-support.md](steamos-support.md)), which already
build, package and install on Arch.

[Omarchy](https://omarchy.org) is an opinionated Arch Linux setup:

- the **Hyprland** Wayland compositor, started through **uwsm**, which manages
  `graphical-session.target`;
- XWayland on by default;
- its configs under `~/.config/hypr/`, which belong to the user.

On Linux the runtime is Vulkan-only. That applies to Omarchy as to any
distribution.

| | Ubuntu (shipping) | Omarchy |
|---|---|---|
| Base | Debian packages | Arch (rolling), writable `/usr` |
| Desktop | GNOME, Mutter | Hyprland (wlroots-free since 0.42, own IPC) |
| Window geometry for windowed weaving | GNOME Shell extension | `displayxr-hypr-geometry-bridge` (this work) |
| Connector modes and scale for the panel lookup | Mutter `DisplayConfig` | Hyprland `monitors` (this work), else DRM sysfs |
| Install | `.deb` or tarball | tarball (user-level) or `packaging/arch/PKGBUILD` |

## What works without anything Hyprland-specific

- **XWayland apps and the runtime's own window.** The runtime reads the window
  rect from XCB, so no publisher is needed. The panel's output must be at
  **scale 1**: at any other scale XWayland's pixels are resampled, and the X11
  1:1 gate presents 2D (`NOT_1TO1`). The setup script warns about any output
  not at scale 1.
- **Native-Wayland apps without the bridge.** They weave **display-scoped**:
  the phase is right only for a window at the panel's origin, or full screen.
- **Rotated outputs** (#2844). A portrait-native panel turned to landscape
  (`monitor = …, transform, 1` or `3`) matches its 3D panel in the presented
  orientation. On Hyprland the transform comes from the `monitors` reply
  (item 2 below), and the Wayland window reads it from `wl_output`, so neither
  path guesses from sysfs. `tests_os_hyprland` covers a Deck-like 800×1280
  output at transform 3.
- **Gamepads** (#2845). The evdev reader works without Steam, which is the usual
  case on Omarchy:
  - It picks up any pad the kernel tags as a joystick: xpad, hid-playstation,
    hid-nintendo, and 8BitDo pads in X-input mode.
  - It maps the face buttons by Xbox position, whatever the pad prints on them.
  - When Steam is running, it prefers Steam Input's virtual pad over the raw
    device, so input isn't doubled.
  - It needs read access to `/dev/input/event*`. On Arch, systemd's
    `uaccess` rule grants that to the seated user for joysticks, so nothing is
    added to the `input` group.
  - The installer registers the provider (`150-gamepad-input-provider.json`),
    and the PKGBUILD ships it in the tarball.

## What this work adds

1. **`displayxr-hypr-geometry-bridge`** (`src/xrt/targets/hypr_geometry_bridge/`).
   - It serves `org.displayxr.WindowGeometry1`, read from Hyprland's IPC
     (`j/clients`, `j/monitors`, event socket).
   - Native-Wayland apps then weave windowed at any position and at fractional
     scales.
   - Contract and mapping: [wayland-window-geometry.md](../specs/runtime/wayland-window-geometry.md) §4.5.
   - The name-owning code is shared with the KWin bridge: `src/xrt/targets/wgeom_bus/`.
2. **Panel lookup on Hyprland** (`aux_os` `os_hyprland_*`,
   `os_display_connector_linux.c`). With no Mutter, the connector modes, the
   fractional scale and the transform come from Hyprland's `monitors` reply.
   - Before this, Hyprland fell straight to DRM sysfs, which knows no scale and
     only infers rotation.
   - Plain text, not JSON, because `aux_os` has no JSON parser.
3. **Window rules** (`scripts/linux/hyprland/`, written by
   `displayxr-hyprland-setup`).
   - Windows of class `DisplayXR` (the runtime's X11 window) and app_id
     `displayxr-service` (its Wayland window) get no border, rounding, shadow,
     blur, dimming, transparency or animation. Any of those resamples or
     covers a weave.
   - The syntax follows the installed Hyprland:

     | Hyprland | Syntax | Rules file |
     |---|---|---|
     | 0.48–0.52 | hyprlang, `windowrule = noborder, class:…` | `displayxr.conf` |
     | 0.53–0.54 | hyprlang, `windowrule = border_size 0, match:class …` | `displayxr.conf` |
     | 0.55+ with `hyprland.lua` | Lua, `hl.window_rule({…})` | `displayxr.lua` |

   - Re-run the script after a Hyprland upgrade.
   - It never edits `hyprland.conf` unless you pass `--source`, and it only
     replaces files that still carry its "managed" marker.
4. **Install.** `install.sh` detects a Hyprland session
   (`XDG_CURRENT_DESKTOP` or `HYPRLAND_INSTANCE_SIGNATURE`), or takes
   `--hyprland`. It then:
   - installs the bridge as the systemd user unit
     `displayxr-hypr-geometry.service` (`WantedBy=graphical-session.target`);
   - runs `displayxr-hyprland-setup`. With `--hypr-source` it also adds the
     `source =` line.

   `uninstall.sh` undoes both. `scripts/build_linux.sh` lists the pacman
   packages, and warns on Arch when build tools are missing.
5. **`packaging/arch/PKGBUILD`**, a template, not on the AUR.
   - It builds from git and unpacks the tarball into `/opt/displayxr-runtime`.
   - Each user then runs `/opt/displayxr-runtime/install.sh`.
   - It writes no ActiveRuntime itself, because that's a per-user choice.

## Install on Omarchy

```sh
sudo pacman -S --needed git cmake ninja pkgconf python binutils vulkan-headers \
    vulkan-icd-loader vulkan-validation-layers glslang eigen systemd-libs libxcb \
    libx11 libxrandr libxxf86vm wayland dbus mesa libglvnd
./scripts/build_linux.sh --hybrid --bundled-cjson
./scripts/package_linux.sh --no-build
tar -xzf dist/displayxr-runtime-linux-*.tar.gz -C ~ && cd ~/displayxr-runtime-linux-*/
./install.sh --hypr-source        # from inside the Hyprland session
```

Then give the 3D panel scale 1 in `~/.config/hypr/monitors.conf`
(`monitor = <connector>, preferred, auto, 1`).

## Not covered

- **Leia SR plug-in.** It is a vendor `.deb`. On Arch it would need
  repackaging, or `debtap`. Its paths (`/usr/lib/displayxr/plugins`,
  `/opt/leiasr`) are writable on Arch, unlike SteamOS. Untested.
- **GNOME-only extras.** These have no Hyprland counterpart yet:
  - window placement and the drag lattice;
  - move-synchronised re-weave;
  - capture exclusion;
  - the service-grabbed workspace hotkey.

  The installer's config has a commented `bind` for
  `displayxr-cli workspace launch <id>` in place of the hotkey.
- **`moving`.** Hyprland reports no interactive move. Consumers settle on
  geometry alone, and the bridge polls at 50 ms while a floating window is
  visible.
- **OpenGL and D3D apps.** Blocked as on all Linux: the runtime is Vulkan-only
  there.

## First hardware run

Do these in order, on Omarchy:

1. Run with sim-display:
   - `displayxr-cli selftest`;
   - `busctl --user call org.displayxr.WindowGeometry /org/displayxr/WindowGeometry org.displayxr.WindowGeometry1 GetWindows`.
     Each window's rect and scale should match `hyprctl clients -j` and
     `hyprctl monitors -j`.
2. A Vulkan XWayland test app, windowed on the panel at scale 1.
3. A native-Wayland `weave_present_vk_linux`, then repeat with the panel at
   scale 1.25. Check for a one-pixel phase error, which would point at
   `buffer_device` (spec §4.5).
4. Drag a floating 3D window and check that the weave keeps up. The 50 ms poll
   is the floor.
5. Upgrade Hyprland across a syntax change, if one is available, and re-run
   `displayxr-hyprland-setup`.

Assumptions to check on that run (from the Hyprland wiki, not observed):

- the `j/clients` and `j/monitors` field names;
- the plain-text `monitors` layout;
- the uwsm environment reaching the systemd user manager (the bridge falls back
  to the single live instance);
- the 0.48–0.52 rule names.

# Vendored Wayland protocol XML

`xdg-shell.xml` — the stable xdg-shell protocol definition, copied verbatim from
[wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols)
**1.49** (`stable/xdg-shell/xdg-shell.xml`). MIT licensed; the copyright block at
the top of the file is the upstream one and must be kept.

## Why it is vendored

`libwayland-dev` ships `wayland-scanner` but **not** the protocol XML — that is a
separate `wayland-protocols` package which is not installed on every dev box or
CI image. The test apps' Wayland window backend (`test_apps/common/dxr_linux_window.cpp`)
needs xdg-shell to give its `wl_surface` a toplevel role, so the XML travels with
the tree. Vendoring also pins the protocol revision, so a box with a newer
`wayland-protocols` cannot silently change the generated glue.

## Regenerating the C glue

Do **not** edit the XML or the generated files. CMake runs `wayland-scanner` on
this XML at build time (see `test_apps/common/dxr_linux_window.cmake`) and writes
`xdg-shell-client-protocol.h` + `xdg-shell-protocol.c` into the build directory —
nothing generated is checked in. By hand, the equivalent is:

```sh
wayland-scanner client-header xdg-shell.xml xdg-shell-client-protocol.h
wayland-scanner private-code  xdg-shell.xml xdg-shell-protocol.c
```

## Updating

Replace the file with the same path from a newer wayland-protocols release and
update the version above. The apps only use `xdg_wm_base` v1 requests
(`get_xdg_surface`, `pong`) plus `xdg_toplevel.set_fullscreen`, so any stable
xdg-shell revision works.

## viewporter and fractional-scale-v1

`viewporter.xml` (`stable/viewporter/`) and `fractional-scale-v1.xml`
(`staging/fractional-scale/`) are copied verbatim from wayland-protocols
**1.47**, same MIT terms. They are what makes a DEVICE-pixel buffer land 1:1 on a
scaled output: the helper attaches a buffer of `logical size x output scale` and
sets the `wp_viewport` destination to the logical (configure) size, so the
compositor maps it onto exactly the configured region. Without them a
3840x2160 buffer on a 200 % output is a 3840x2160-LOGICAL surface — twice the
output — and spills onto the neighbouring monitor.
`wp_fractional_scale_v1.preferred_scale` supplies the scale for a windowed
surface; both are optional (the helper falls back to `wl_surface.set_buffer_scale`
for an integer scale, and to no mapping otherwise).

## xdg-decoration, cursor-shape, tablet-v2, ext-background-effect (#1654)

The window chrome (`test_apps/common/dxr_wl_chrome.cpp`) uses four more
protocols. All are copied verbatim from wayland-protocols **1.47**, under the
same MIT terms:

- `xdg-decoration-unstable-v1.xml` (`unstable/xdg-decoration/`): asks for
  server-side decorations where the compositor offers them. GNOME does not, so
  there the helper draws its own title bar.
- `cursor-shape-v1.xml` (`staging/cursor-shape/`): resize and default cursors
  over the bar and the window edges, with no cursor-theme loading.
- `tablet-v2.xml` (`stable/tablet/`): only because cursor-shape-v1 references
  `zwp_tablet_tool_v2`. The generated glue has to define that interface or the
  link fails. The helper never uses tablets.
- `ext-background-effect-v1.xml` (`staging/ext-background-effect/`): blurs the
  background behind the translucent title bar where the compositor supports it.
  It is the standardised successor of KDE's `org_kde_kwin_blur`. GNOME offers
  no client blur protocol, so there the bar is plain translucency.

Every one of them is optional at runtime. A missing global only disables the
corresponding nicety.

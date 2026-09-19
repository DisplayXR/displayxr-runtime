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

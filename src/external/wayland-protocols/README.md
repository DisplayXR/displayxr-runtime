# Vendored Wayland protocol XML

Copied verbatim from [wayland-protocols](https://gitlab.freedesktop.org/wayland/wayland-protocols)
(the same copies `displayxr-common/common/linux/wayland-protocols/` carries):
`xdg-shell.xml` (1.49, `stable/xdg-shell/`), `viewporter.xml` (1.47,
`stable/viewporter/`), `fractional-scale-v1.xml` (1.47,
`staging/fractional-scale/`), `xdg-output-unstable-v1.xml`
(`unstable/xdg-output/`), `cursor-shape-v1.xml` (1.47, `staging/cursor-shape/`)
`tablet-v2.xml` (1.47, `stable/tablet/`, only because cursor-shape-v1
references `zwp_tablet_tool_v2`) and `presentation-time.xml` (1.47,
`stable/presentation-time/`). MIT licensed; the copyright block at the top
of each file is the upstream one and must be kept.

Used by the desktop-Linux service window's native-Wayland backend
(`src/xrt/compositor/main/comp_window_linux_wayland.c`, #710): an xdg-shell
toplevel fullscreened on the 3D panel's output, a device-pixel buffer mapped
1:1 through `wp_viewport` + `wp_fractional_scale_v1`, and the pointer hidden or
restored with cursor-shape-v1.

`presentation-time.xml` is used by the Vulkan compositor's present-feedback
feeder (`src/xrt/compositor/vk_native/comp_vk_native_present_feedback.c`, #902):
`wp_presentation` feedback on the app's surface anchors the vblank grid that
late weave aims at. Ubuntu 22.04 ships version 1 of this protocol; the vendored
copy is version 2, and the feeder binds whichever the compositor offers.

## Why vendored

`libwayland-dev` ships `wayland-scanner` but not the protocol XML, and the
release artifacts are built on Ubuntu 22.04, whose `wayland-protocols` (1.25)
predates fractional-scale-v1 and cursor-shape-v1. CMake runs `wayland-scanner`
on these files at build time (`src/xrt/compositor/CMakeLists.txt`); nothing
generated is checked in.

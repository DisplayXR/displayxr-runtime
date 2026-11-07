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

`kde-stereo-content-v1.xml` is not part of wayland-protocols: it is KDE's
stereo declaration as Stereo KWin implements it (Sparky Stereo OS), copied
verbatim from [libstereo-declare](https://github.com/Sparky-OS/libstereo-declare)
(`protocol/kde-stereo-content-v1.xml`, commit `fe4b338`), MIT-CMU licensed
(Daniel Campos Ramos; the copyright block at the top is the original one). The
Vulkan compositor uses it (`comp_vk_native_wl_stereo.c`) when a display
processor's output is full side by side (`xrt_dp_scanout_caps::stereo_layout` = 3):
the app's surface is declared value 3, side by side with full width per eye, left
first, so a stereo compositor shows each half to its own eye. The protocol was born in
KDE (Stereo KWin is its first consumer) and keeps its name; any compositor can
implement it unchanged, and it carries one value, so a `stereo_layout` other than 3
puts nothing on the wire. The runtime binds
version 2, the first in which a declared surface without a viewport destination
is one view wide; a compositor without the global is told nothing.

## Why vendored

`libwayland-dev` ships `wayland-scanner` but not the protocol XML, and the
release artifacts are built on Ubuntu 22.04, whose `wayland-protocols` (1.25)
predates fractional-scale-v1 and cursor-shape-v1. CMake runs `wayland-scanner`
on these files at build time (`src/xrt/compositor/CMakeLists.txt`); nothing
generated is checked in.

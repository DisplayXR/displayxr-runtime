# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

# Serving org.displayxr.WindowGeometry1 from a stand-alone bridge (KWin,
# Hyprland). See wgeom_bus.h and docs/specs/runtime/wayland-window-geometry.md.

add_library(wgeom_bus STATIC wgeom_bus.c wgeom_bus.h)
target_include_directories(wgeom_bus PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(wgeom_bus SYSTEM PUBLIC ${DBUS_INCLUDE_DIRS})
target_link_libraries(wgeom_bus PUBLIC ${DBUS_LIBRARIES})

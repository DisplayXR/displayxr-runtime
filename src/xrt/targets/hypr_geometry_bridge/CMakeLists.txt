# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

# displayxr-hypr-geometry-bridge: org.displayxr.WindowGeometry1 on Hyprland
# (Omarchy), read from Hyprland's IPC sockets. See
# docs/specs/runtime/wayland-window-geometry.md, "Hyprland publisher", and
# docs/roadmap/omarchy-support.md. hgb_payload.c is pure (it is also compiled
# into tests_hgb_payload).

add_executable(displayxr-hypr-geometry-bridge main.c hgb_payload.c hgb_payload.h)
target_link_libraries(displayxr-hypr-geometry-bridge PRIVATE aux_os aux_util xrt-external-cjson wgeom_bus m)
# LINKER_LANGUAGE CXX: parts of aux_util are C++.
set_target_properties(displayxr-hypr-geometry-bridge PROPERTIES LINKER_LANGUAGE CXX)

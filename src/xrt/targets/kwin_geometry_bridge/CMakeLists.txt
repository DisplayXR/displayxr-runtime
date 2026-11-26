# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0

# displayxr-kwin-geometry-bridge: org.displayxr.WindowGeometry1 on a stock KWin
# (SteamOS Desktop Mode), fed by contrib/kwin/displayxr-window-geometry. See
# docs/specs/runtime/wayland-window-geometry.md, "KWin script bridge".
# kwgb_payload.c is pure (it is also compiled into tests_kwgb_payload); cJSON's
# implementation comes from aux_util (bundled) or the system library.

add_executable(displayxr-kwin-geometry-bridge main.c kwgb_payload.c kwgb_payload.h)
target_link_libraries(displayxr-kwin-geometry-bridge PRIVATE aux_util xrt-external-cjson ${DBUS_LIBRARIES})
target_include_directories(displayxr-kwin-geometry-bridge SYSTEM PRIVATE ${DBUS_INCLUDE_DIRS})
# LINKER_LANGUAGE CXX: parts of aux_util are C++.
set_target_properties(displayxr-kwin-geometry-bridge PROPERTIES LINKER_LANGUAGE CXX)

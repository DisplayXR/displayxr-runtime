# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# weave_present_vk_linux — a desktop-Linux XR_DXR_weave PRESENT-OWNER with a
# real window (#1699): the app renders a stereo scene into a dma-buf, the
# weave service (displayxr-service, IPC) weaves it, and the app presents the
# woven dma-buf in its own Vulkan swapchain. The browser's GPU process in
# miniature. Window: displayxr::linux_window (native Wayland or X11, one
# binary). Only builds on desktop Linux (dir suffix _linux).

cmake_minimum_required(VERSION 3.21)
project(weave_present_vk_linux LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(_dxr_root "${CMAKE_CURRENT_SOURCE_DIR}/../../..")

find_package(Vulkan REQUIRED)
find_package(X11 REQUIRED)

# OpenXR headers (incl. the DisplayXR extension headers) from the source tree.
set(OPENXR_INCLUDE_DIR "${_dxr_root}/src/external/openxr_includes")

# OpenXR loader: CMake package -> system library -> parent build tree (same
# ladder as the other Linux test apps).
set(OPENXR_FOUND FALSE)
find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
endif()
if(NOT OPENXR_FOUND)
    find_library(OPENXR_LOADER_LIB NAMES openxr_loader)
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        set_target_properties(OpenXR::openxr_loader PROPERTIES IMPORTED_LOCATION "${OPENXR_LOADER_LIB}")
    endif()
endif()
if(NOT OPENXR_FOUND AND EXISTS "${_dxr_root}/build/src/external/openxr_loader/OpenXRConfig.cmake")
    list(APPEND CMAKE_PREFIX_PATH "${_dxr_root}/build/src/external/openxr_loader")
    find_package(OpenXR REQUIRED CONFIG)
    set(OPENXR_FOUND TRUE)
endif()
if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Build it with ./scripts/build_linux.sh --apps, or install libopenxr-loader1.")
endif()

# displayxr-common: displayxr::linux_window (the one Linux app window, with the
# phase-snapped drag + the Wayland drag lattice), dxr_weave_snap.h, the shared
# u_wayland_geom.h / u_x11_scale.h copies, stb_image_write. Same pin as the
# cube apps.
include(FetchContent)
set(DISPLAYXR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
get_filename_component(_dxr_ext_includes "${OPENXR_INCLUDE_DIR}" ABSOLUTE)
set(DISPLAYXR_EXTENSIONS_INCLUDE_DIR "${_dxr_ext_includes}" CACHE PATH
    "DisplayXR OpenXR extension headers for displayxr::common")
FetchContent_Declare(
    displayxr_common
    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git
    GIT_TAG v2.22.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(displayxr_common)

# u_x11_scale.h / u_wayland_geom.h: common's copies must match the runtime's.
include(${_dxr_root}/test_apps/common/dxr_linux_window_aux_guard.cmake)

if(NOT TARGET displayxr::linux_window)
    message(FATAL_ERROR "displayxr::linux_window is not available (libx11-dev missing?)")
endif()

add_executable(weave_present_vk_linux
    main.cpp
    wl_geometry_client.cpp
    stb_image_write_impl.cpp
)
target_include_directories(weave_present_vk_linux PRIVATE
    ${OPENXR_INCLUDE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../common
    ${_dxr_root}/test_apps/common   # dxr_weave_snap_grid.h (#1723)
)
target_link_libraries(weave_present_vk_linux PRIVATE
    displayxr::linux_window
    displayxr::common
    OpenXR::openxr_loader
    Vulkan::Vulkan
    X11::X11
)

# libdbus-1: this app's own client of the window-geometry GNOME Shell extension
# (GetWindows for the window's position, MoveWindow for the drop snap). Without
# it a native-Wayland window binds display-scoped. cJSON is the runtime's
# vendored copy.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(WEAVE_PRESENT_DBUS QUIET dbus-1)
endif()
if(WEAVE_PRESENT_DBUS_FOUND)
    target_sources(weave_present_vk_linux PRIVATE "${_dxr_root}/src/external/cjson/cjson/cJSON.c")
    target_include_directories(weave_present_vk_linux PRIVATE
        ${WEAVE_PRESENT_DBUS_INCLUDE_DIRS} "${_dxr_root}/src/external/cjson/cjson")
    target_link_directories(weave_present_vk_linux PRIVATE ${WEAVE_PRESENT_DBUS_LIBRARY_DIRS})
    target_link_libraries(weave_present_vk_linux PRIVATE ${WEAVE_PRESENT_DBUS_LIBRARIES})
    target_compile_definitions(weave_present_vk_linux PRIVATE DXR_PRESENT_HAVE_DBUS)
    message(STATUS "weave_present_vk_linux: Wayland window geometry via libdbus ${WEAVE_PRESENT_DBUS_VERSION}")
else()
    message(STATUS "weave_present_vk_linux: libdbus-1 NOT found — a native-Wayland window binds display-scoped")
endif()

# The workspace manifest + icons beside the executable (INV-9.1).
add_custom_command(TARGET weave_present_vk_linux POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_CURRENT_SOURCE_DIR}/displayxr/weave_present_vk_linux.displayxr.json"
        "${CMAKE_CURRENT_SOURCE_DIR}/displayxr/icon.png"
        "${CMAKE_CURRENT_SOURCE_DIR}/displayxr/icon_sbs.png"
        "$<TARGET_FILE_DIR:weave_present_vk_linux>/"
)

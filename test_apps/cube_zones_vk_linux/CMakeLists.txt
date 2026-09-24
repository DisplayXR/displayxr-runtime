# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Linux handle-class Vulkan cube ZONES app — XR_DXR_display_zones exerciser
# (ADR-027), desktop-Linux Vulkan leg. Port of cube_zones_vk_macos /
# cube_zones_d3d11_win onto the cube_handle_vk_linux scaffold: the app creates
# and owns its X11 window (Xlib) and passes it to the runtime at xrCreateSession;
# the runtime presents into it via XGetXCBConnection + VK_KHR_xcb_surface. The
# zones delta (per-zone swapchains, rig framing, wish mask, frame-end info) is
# layered on top of the base handle app's render path.

cmake_minimum_required(VERSION 3.21)
project(cube_zones_vk_linux LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# Xlib for the app-owned window (libx11-dev). Wayland is optional and wired
# up by dxr_target_add_linux_window() below.
find_package(X11 REQUIRED)

# OpenXR headers from the runtime source tree.
set(OPENXR_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../src/external/openxr_includes")

# Find the OpenXR loader: CMake package → system library → parent build tree.
set(OPENXR_FOUND FALSE)

find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
    message(STATUS "Found OpenXR via find_package")
endif()

if(NOT OPENXR_FOUND)
    find_library(OPENXR_LOADER_LIB NAMES openxr_loader)
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        set_target_properties(OpenXR::openxr_loader PROPERTIES
            IMPORTED_LOCATION "${OPENXR_LOADER_LIB}"
        )
        message(STATUS "Found OpenXR loader: ${OPENXR_LOADER_LIB}")
    endif()
endif()

if(NOT OPENXR_FOUND)
    set(PARENT_BUILD_DIR "${CMAKE_SOURCE_DIR}/../../build")
    if(EXISTS "${PARENT_BUILD_DIR}/src/external/openxr_loader/OpenXRConfig.cmake")
        list(APPEND CMAKE_PREFIX_PATH "${PARENT_BUILD_DIR}/src/external/openxr_loader")
        find_package(OpenXR REQUIRED CONFIG)
        set(OPENXR_FOUND TRUE)
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Install the OpenXR loader (e.g. apt install libopenxr-loader1) or build it from source.")
endif()

# Shared scaffolding (stb_image header + impl TU) via displayxr::common, pinned by
# tag. OpenXR (set up above) is reused, not re-fetched. Also provides
# displayxr::linux_window — THE desktop-Linux app window (X11 or native Wayland
# in one binary, chosen by capability probe; the title bar, input events and
# the phase-snapped drag), which used to live here as test_apps/common/
# dxr_linux_window and is now shared with every demo — hence v2.20.0.
include(FetchContent)
set(DISPLAYXR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
get_filename_component(_dxr_ext_includes
    "${CMAKE_SOURCE_DIR}/../../src/external/openxr_includes" ABSOLUTE)
set(DISPLAYXR_EXTENSIONS_INCLUDE_DIR "${_dxr_ext_includes}" CACHE PATH
    "DisplayXR OpenXR extension headers for displayxr::common")
FetchContent_Declare(
    displayxr_common
    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git
    # v2.21.0 adds the native-Wayland drag-lattice follow-ups (displayxr-common#59,
    # runtime#1697). Linux-only; nothing else changed since the previous pin.
    # v2.22.0: an opaque window's Wayland surface is declared opaque, so the
    # compositor can scan a fullscreen weave out directly (displayxr-common#60,
    # #1698). Linux-only.
    # v2.23.0: the native-Wayland drag stays phase-snapped at ANY output scale
    # (150 %, 166.67 %, ...), not only 100/200 % (displayxr-common#61,
    # runtime#1709).
    # v2.24.0: the Wayland drag move is sent at once and the first lattice
    # table is built on the worker, from the new grid snap provider
    # (set_snap_grid_provider + DxrWeaveSnap::grid_callback, XR_DXR_weave
    # spec >= 11; displayxr-common#62/#63, runtime#1723).
    GIT_TAG v2.24.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(displayxr_common)

# u_x11_scale.h / u_wayland_geom.h: common's copies must match the runtime's.
include(${CMAKE_SOURCE_DIR}/../common/dxr_linux_window_aux_guard.cmake)

# stb_image_impl.cpp supplies the stb_image definition on Linux (displayxr::common
# only ships a macOS-gated impl TU; main.cpp is declarations-only). See that file.
add_executable(cube_zones_vk_linux main.cpp stb_image_impl.cpp)

target_include_directories(cube_zones_vk_linux PRIVATE
    ${OPENXR_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/../common
)

# Dual-backend app-owned window: displayxr::linux_window (X11 always; native
# Wayland when libwayland-client + wayland-scanner were found — see
# displayxr-common's common/linux/dxr_linux_window.cmake).
if(NOT TARGET displayxr::linux_window)
    message(FATAL_ERROR "displayxr::linux_window is not available (libx11-dev missing?)")
endif()
target_link_libraries(cube_zones_vk_linux PRIVATE displayxr::linux_window)

# Copy texture files next to the built executable.
file(GLOB TEXTURE_FILES "${CMAKE_SOURCE_DIR}/../common/textures/*.jpg")
foreach(TEX ${TEXTURE_FILES})
    get_filename_component(TEX_NAME ${TEX} NAME)
    add_custom_command(TARGET cube_zones_vk_linux POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:cube_zones_vk_linux>/textures"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TEX} "$<TARGET_FILE_DIR:cube_zones_vk_linux>/textures/${TEX_NAME}"
    )
endforeach()

target_link_libraries(cube_zones_vk_linux PRIVATE
    displayxr::common
    OpenXR::openxr_loader
    Vulkan::Vulkan
    X11::X11
)

# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Linux handle-class Vulkan cube — the Phase 3 validation vehicle for
# XR_DXR_xlib_window_binding (docs/roadmap/linux-support.md, #660). The app
# creates and owns its X11 window (Xlib) and passes it to the runtime at
# xrCreateSession; the runtime presents into it via XGetXCBConnection +
# VK_KHR_xcb_surface. Adapted from cube_hosted_legacy_vk_linux (same render
# path; the delta is the window + binding).

cmake_minimum_required(VERSION 3.21)
project(cube_handle_vk_linux LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# Xlib for the app-owned window (libx11-dev).
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
# tag. OpenXR (set up above) is reused, not re-fetched.
include(FetchContent)
set(DISPLAYXR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
get_filename_component(_dxr_ext_includes
    "${CMAKE_SOURCE_DIR}/../../src/external/openxr_includes" ABSOLUTE)
set(DISPLAYXR_EXTENSIONS_INCLUDE_DIR "${_dxr_ext_includes}" CACHE PATH
    "DisplayXR OpenXR extension headers for displayxr::common")
FetchContent_Declare(
    displayxr_common
    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git
    GIT_TAG v2.0.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(displayxr_common)

# stb_image_impl.cpp supplies the stb_image definition on Linux (displayxr::common
# only ships a macOS-gated impl TU; main.cpp is declarations-only). See that file.
add_executable(cube_handle_vk_linux main.cpp stb_image_impl.cpp)

target_include_directories(cube_handle_vk_linux PRIVATE
    ${OPENXR_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/../common
)

# Copy texture files next to the built executable.
file(GLOB TEXTURE_FILES "${CMAKE_SOURCE_DIR}/../common/textures/*.jpg")
foreach(TEX ${TEXTURE_FILES})
    get_filename_component(TEX_NAME ${TEX} NAME)
    add_custom_command(TARGET cube_handle_vk_linux POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:cube_handle_vk_linux>/textures"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TEX} "$<TARGET_FILE_DIR:cube_handle_vk_linux>/textures/${TEX_NAME}"
    )
endforeach()

target_link_libraries(cube_handle_vk_linux PRIVATE
    displayxr::common
    OpenXR::openxr_loader
    Vulkan::Vulkan
    X11::X11
)

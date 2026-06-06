# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)
project(cube_handle_vk_macos LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Vulkan SDK
find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR headers from the monado source tree (includes custom extensions)
set(OPENXR_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../src/external/openxr_includes")

# Find OpenXR loader
set(OPENXR_FOUND FALSE)

find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
    message(STATUS "Found OpenXR via find_package")
endif()

# Fallback: try Homebrew or system library
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

# Fallback: parent build directory
if(NOT OPENXR_FOUND)
    set(PARENT_BUILD_DIR "${CMAKE_SOURCE_DIR}/../../build")
    if(EXISTS "${PARENT_BUILD_DIR}/src/external/openxr_loader/OpenXRConfig.cmake")
        list(APPEND CMAKE_PREFIX_PATH "${PARENT_BUILD_DIR}/src/external/openxr_loader")
        find_package(OpenXR REQUIRED CONFIG)
        set(OPENXR_FOUND TRUE)
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Install via: brew install openxr-loader")
endif()

# Off-axis Kooima math (displayxr::math) AND the shared app scaffolding
# (displayxr::common: macOS HUD, window-space-layer UI, atlas-capture helper,
# stb) pinned by tag instead of compiled from ../common (epic #396 W3+W4).
# OpenXR (set up above) is reused, not re-fetched; the DisplayXR extension
# headers come from this repo's source-of-truth copy.
include(FetchContent)
set(DISPLAYXR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
get_filename_component(_dxr_ext_includes
    "${CMAKE_SOURCE_DIR}/../../src/external/openxr_includes" ABSOLUTE)
set(DISPLAYXR_EXTENSIONS_INCLUDE_DIR "${_dxr_ext_includes}" CACHE PATH
    "DisplayXR OpenXR extension headers for displayxr::common")
FetchContent_Declare(
    displayxr_common
    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git
    GIT_TAG v0.3.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(displayxr_common)

add_executable(cube_handle_vk_macos
    main.mm
    # hud_renderer_macos / xr_window_space_hud / atlas_capture_macos come
    # from displayxr::common (#396 W4).
)

target_include_directories(cube_handle_vk_macos PRIVATE
    ${OPENXR_INCLUDE_DIR}
    "${CMAKE_SOURCE_DIR}/../common"
)

target_link_libraries(cube_handle_vk_macos PRIVATE
    displayxr::common
    OpenXR::openxr_loader
    Vulkan::Vulkan
    "-framework Cocoa"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework CoreText"
    "-framework CoreFoundation"
)

# Copy texture files to build output directory
file(GLOB TEXTURE_FILES "${CMAKE_SOURCE_DIR}/../common/textures/*.jpg")
add_custom_command(TARGET cube_handle_vk_macos POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:cube_handle_vk_macos>/textures"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TEXTURE_FILES} "$<TARGET_FILE_DIR:cube_handle_vk_macos>/textures/"
    COMMENT "Copying texture files to build output"
)

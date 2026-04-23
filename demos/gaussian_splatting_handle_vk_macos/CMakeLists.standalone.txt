# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Standalone build — lives inside displayxr-demo-gaussiansplat repo as
# macos/CMakeLists.txt. Assumes sibling dirs: ../common, ../3dgs_common,
# ../openxr_includes. Top-level CMakeLists.txt is expected to have already
# added ../common and ../3dgs_common as subdirectories, exposing the
# `sr_common_base` and `gs_renderer` targets.

cmake_minimum_required(VERSION 3.21)

if(NOT APPLE)
    return()
endif()

find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR loader — system install (brew), find_package, or user-provided root.
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
        set_target_properties(OpenXR::openxr_loader PROPERTIES
            IMPORTED_LOCATION "${OPENXR_LOADER_LIB}"
        )
        message(STATUS "Found OpenXR loader: ${OPENXR_LOADER_LIB}")
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Install via: brew install openxr-loader")
endif()

add_executable(gaussian_splatting_handle_vk_macos
    main.mm
)

target_include_directories(gaussian_splatting_handle_vk_macos PRIVATE
    ${CMAKE_SOURCE_DIR}/openxr_includes
    ${CMAKE_SOURCE_DIR}/common
    ${CMAKE_SOURCE_DIR}/3dgs_common
)

target_link_libraries(gaussian_splatting_handle_vk_macos PRIVATE
    sr_common_base
    gs_renderer
    OpenXR::openxr_loader
    Vulkan::Vulkan
    "-framework Cocoa"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework CoreText"
    "-framework CoreFoundation"
    "-framework UniformTypeIdentifiers"
)

# Copy bundled default scene next to the exe (auto-loaded at startup).
set(BUNDLED_SCENE "${CMAKE_CURRENT_SOURCE_DIR}/assets/butterfly.spz")
if(EXISTS "${BUNDLED_SCENE}")
    add_custom_command(TARGET gaussian_splatting_handle_vk_macos POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BUNDLED_SCENE}" "$<TARGET_FILE_DIR:gaussian_splatting_handle_vk_macos>/butterfly.spz"
        COMMENT "Copying bundled butterfly.spz next to exe"
    )
endif()

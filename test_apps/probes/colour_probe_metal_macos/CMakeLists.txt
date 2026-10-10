# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# ADR-044 colour probe for the macOS Metal compositor: solid CPU-authored
# projection + window-space layers in declared _SRGB / UNORM formats, read back
# numerically from the compositor's /tmp/dxr_atlas.png. No displayxr manifest
# (a verification tool, not a workspace app).

cmake_minimum_required(VERSION 3.21)
project(colour_probe_metal_macos LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(OPENXR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/external/openxr_includes")

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
if(NOT OPENXR_FOUND)
    set(PARENT_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../build")
    if(EXISTS "${PARENT_BUILD_DIR}/src/external/openxr_loader/OpenXRConfig.cmake")
        list(APPEND CMAKE_PREFIX_PATH "${PARENT_BUILD_DIR}/src/external/openxr_loader")
        find_package(OpenXR REQUIRED CONFIG)
        set(OPENXR_FOUND TRUE)
    endif()
endif()
if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Build the parent project first or: brew install openxr-loader")
endif()

add_executable(colour_probe_metal_macos main.mm)
target_include_directories(colour_probe_metal_macos PRIVATE ${OPENXR_INCLUDE_DIR})
target_link_libraries(colour_probe_metal_macos PRIVATE
    OpenXR::openxr_loader
    "-framework Metal"
    "-framework Foundation"
    "-framework Cocoa"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework IOSurface"
)

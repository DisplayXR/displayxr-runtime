# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0
#
# XR_DXR_weave macOS probe (#759) — headless present-owner harness; the macOS
# sibling of probes/weave_rpc_probe_d3d11_win. No window, no displayxr manifest
# (it is a service-contract verification tool, not a workspace app).

cmake_minimum_required(VERSION 3.21)
project(weave_probe_vk_macos LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Vulkan SDK (MoltenVK) — the only IPC-capable graphics binding on macOS.
find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR headers (incl. the DisplayXR extension headers) from the source tree.
set(OPENXR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/external/openxr_includes")

# Find OpenXR loader (same strategy ladder as the other macOS test apps).
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

add_executable(weave_probe_vk_macos main.cpp)

target_include_directories(weave_probe_vk_macos PRIVATE ${OPENXR_INCLUDE_DIR})

target_link_libraries(weave_probe_vk_macos PRIVATE
    OpenXR::openxr_loader
    Vulkan::Vulkan
    "-framework IOSurface"
    "-framework CoreFoundation"
)

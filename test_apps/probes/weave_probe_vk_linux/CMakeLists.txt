# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0
#
# XR_DXR_weave desktop-Linux probe (#1699 R2) — headless present-owner harness;
# the Linux sibling of probes/weave_probe_vk_macos. No window, no swapchain, no
# displayxr manifest (it is a service-contract verification tool, not a
# workspace app). Stage A: the input and the woven output cross the IPC as
# same-driver OPAQUE_FD Vulkan allocations.

cmake_minimum_required(VERSION 3.21)
project(weave_probe_vk_linux LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR headers (incl. the DisplayXR extension headers) from the source tree.
set(OPENXR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/external/openxr_includes")

# Find the OpenXR loader: CMake package → system library → parent build tree
# (same ladder as the other Linux test apps).
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
    message(FATAL_ERROR "OpenXR not found. Build it with ./scripts/build_linux.sh --apps, or install libopenxr-loader1.")
endif()

add_executable(weave_probe_vk_linux main.cpp)

target_include_directories(weave_probe_vk_linux PRIVATE
    ${OPENXR_INCLUDE_DIR}
    # weave_dmabuf_vk.h: the dma-buf transport calls, shared with
    # weave/weave_present_vk_linux.
    ${CMAKE_CURRENT_SOURCE_DIR}/../../weave/common
    ${CMAKE_CURRENT_SOURCE_DIR}/../../common   # dxr_weave_snap_grid.h (#1723)
)

target_link_libraries(weave_probe_vk_linux PRIVATE
    OpenXR::openxr_loader
    Vulkan::Vulkan
)

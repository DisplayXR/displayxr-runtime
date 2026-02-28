# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Parent CMakeLists.txt for test applications
# Build from this directory to build all test apps at once
#
# Apps:
#   sr_cube_native         - Native SR SDK (no OpenXR) - requires LeiaSR SDK
#   sr_cube_openxr         - OpenXR standard mode (Monado creates window)
#   sr_cube_openxr_ext     - OpenXR with XR_EXT_win32_window_binding (app creates window, D3D11)
#   sr_cube_openxr_ext_d3d12 - D3D12 port of sr_cube_openxr_ext
#   sr_cube_openxr_ext_gl  - OpenGL port of sr_cube_openxr_ext
#   sr_cube_openxr_ext_vk  - Vulkan port of sr_cube_openxr_ext (requires Vulkan SDK)
#   sr_3dgs_openxr_ext_vk  - 3D Gaussian Splatting with OpenXR (Vulkan, requires Vulkan SDK)
#   sr_cube_native_vk      - Native SR SDK Vulkan (no OpenXR) - requires LeiaSR SDK + Vulkan SDK
#   spatial_os             - Spatial OS demo (multi-layer war room, D3D11)

cmake_minimum_required(VERSION 3.21)
project(test_apps LANGUAGES CXX)

# Only build on Windows
if(NOT WIN32)
    message(STATUS "Test apps are Windows-only, skipping")
    return()
endif()

# Build sr_cube_native (requires LeiaSR SDK for full functionality)
add_subdirectory(sr_cube_native)

# Build sr_cube_native_vk (requires LeiaSR SDK + Vulkan SDK)
add_subdirectory(sr_cube_native_vk)

# Build OpenXR test apps (require OpenXR loader)
if(OpenXR_FOUND OR DEFINED OpenXR_ROOT)
    add_subdirectory(sr_cube_openxr)
    add_subdirectory(sr_cube_openxr_ext)
    add_subdirectory(sr_cube_openxr_ext_d3d12)
    add_subdirectory(sr_cube_openxr_ext_gl)
    add_subdirectory(sr_cube_openxr_ext_vk)
    add_subdirectory(sr_3dgs_openxr_ext_vk)
    add_subdirectory(spatial_os)
else()
    message(STATUS "OpenXR not found, skipping OpenXR test apps")
    message(STATUS "To build them, set OpenXR_ROOT or install OpenXR SDK")
endif()

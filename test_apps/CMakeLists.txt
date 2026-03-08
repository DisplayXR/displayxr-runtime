# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Parent CMakeLists.txt for test applications
# Build from this directory to build all test apps at once
#
# Apps:
#   cube_d3d11             - OpenXR standard mode (runtime creates window, D3D11)
#   cube_ext_d3d11         - OpenXR with XR_EXT_win32_window_binding (app creates window, D3D11)
#   cube_ext_d3d12         - D3D12 port of cube_ext_d3d11
#   cube_ext_gl            - OpenGL port of cube_ext_d3d11
#   cube_d3d12             - OpenXR standard mode (runtime creates window, D3D12)
#   cube_ext_vk            - Vulkan port of cube_ext_d3d11 (requires Vulkan SDK)
#   cube_shared_d3d11      - Shared texture mode (D3D11 rendering, D3D11 window blit)
#   cube_shared_d3d12      - Shared texture mode (D3D12 rendering, D3D11 window blit)
#   cube_shared_gl         - Shared texture mode (OpenGL rendering, D3D11 window blit)
#   cube_shared_vk         - Shared texture mode (Vulkan rendering, D3D11 window blit)

cmake_minimum_required(VERSION 3.21)
project(test_apps LANGUAGES CXX)

# Only build on Windows
if(NOT WIN32)
    message(STATUS "Test apps are Windows-only, skipping")
    return()
endif()

# Build OpenXR test apps (require OpenXR loader)
if(OpenXR_FOUND OR DEFINED OpenXR_ROOT)
    add_subdirectory(cube_d3d11)
    add_subdirectory(cube_ext_d3d11)
    add_subdirectory(cube_ext_d3d12)
    add_subdirectory(cube_ext_gl)
    add_subdirectory(cube_ext_vk)
    add_subdirectory(cube_shared_d3d11)
    add_subdirectory(cube_d3d12)
    add_subdirectory(cube_shared_d3d12)
    add_subdirectory(cube_shared_gl)
    add_subdirectory(cube_shared_vk)
    add_subdirectory(cube_vk)
    add_subdirectory(cube_gl)
else()
    message(STATUS "OpenXR not found, skipping OpenXR test apps")
    message(STATUS "To build them, set OpenXR_ROOT or install OpenXR SDK")
endif()

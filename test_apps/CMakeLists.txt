# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Parent CMakeLists.txt for test applications
# Build from this directory to build all test apps at once
#
# Apps:
#   cube_hosted_d3d11_win      - Hosted mode (runtime creates window, D3D11)
#   cube_handle_d3d11_win      - Handle mode (app provides window handle, D3D11)
#   cube_handle_d3d12_win      - Handle mode (app provides window handle, D3D12)
#   cube_handle_vk_win         - Handle mode (app provides window handle, Vulkan)
#   cube_handle_gl_win         - Handle mode (app provides window handle, OpenGL)
#   cube_texture_d3d11_win     - Texture mode (D3D11 rendering, shared texture)
#   cube_hosted_legacy_d3d11_win - Legacy hosted mode (no XR_EXT_display_info, D3D11)
#   cube_hosted_legacy_d3d12_win - Legacy hosted mode (no XR_EXT_display_info, D3D12)
#   cube_hosted_legacy_vk_win   - Legacy hosted mode (no XR_EXT_display_info, Vulkan)
#   cube_hosted_legacy_gl_win   - Legacy hosted mode (no XR_EXT_display_info, OpenGL)

cmake_minimum_required(VERSION 3.21)
project(test_apps LANGUAGES CXX)

# Only build on Windows
if(NOT WIN32)
    message(STATUS "Test apps are Windows-only, skipping")
    return()
endif()

# Build OpenXR test apps (require OpenXR loader)
if(OpenXR_FOUND OR DEFINED OpenXR_ROOT)
    add_subdirectory(cube_hosted_d3d11_win)
    add_subdirectory(cube_handle_d3d11_win)
    add_subdirectory(cube_handle_d3d12_win)
    add_subdirectory(cube_handle_vk_win)
    add_subdirectory(cube_handle_gl_win)
    add_subdirectory(cube_texture_d3d11_win)
    add_subdirectory(cube_hosted_legacy_d3d11_win)
    add_subdirectory(cube_hosted_legacy_d3d12_win)
    add_subdirectory(cube_hosted_legacy_vk_win)
    add_subdirectory(cube_hosted_legacy_gl_win)
else()
    message(STATUS "OpenXR not found, skipping OpenXR test apps")
    message(STATUS "To build them, set OpenXR_ROOT or install OpenXR SDK")
endif()

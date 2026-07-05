# Copyright 2025, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Parent CMakeLists.txt for test applications
# Build from this directory to build all test apps at once
#
# Apps:
#   cube_hosted_d3d11_win      - Hosted mode (runtime creates window, D3D11)
#   cube_handle_d3d11_win      - Handle mode (app provides window handle, D3D11)
#   cube_zones_d3d11_win       - XR_EXT_display_zones exerciser (N 3D zones + Local2D strip + wish mask, D3D11)
#   cube_handle_d3d12_win      - Handle mode (app provides window handle, D3D12)
#   cube_zones_d3d12_win       - XR_EXT_display_zones exerciser, ARRAY/SPI stereo layout (repro for #672, D3D12)
#   cube_handle_vk_win         - Handle mode (app provides window handle, Vulkan)
#   cube_zones_vk_win          - XR_EXT_display_zones exerciser, ARRAY/SPI stereo layout (VK leg of #673)
#   cube_handle_gl_win         - Handle mode (app provides window handle, OpenGL)
#   cube_zones_texture_d3d11_win - XR_EXT_display_zones TEXTURE-class parity test (shared texture receives the multi-zone composite, D3D11)
#   cube_zones_texture_d3d12_win - XR_EXT_display_zones TEXTURE-class parity test (native-D3D12 zones texture, #613)
#   cube_zones_texture_gl_win  - XR_EXT_display_zones TEXTURE-class parity test (GL/D3D11 hybrid zones texture, #613)
#   cube_zones_texture_vk_win  - XR_EXT_display_zones TEXTURE-class parity test (VK/D3D11 hybrid zones texture, #613)
#   weave_rpc_probe_d3d11_win   - XR_EXT_weave probe (#625): browser-free present-owner driving the window-bound synchronous weave service (D3D11)
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
    add_subdirectory(cube_zones_d3d11_win)
    add_subdirectory(cube_handle_d3d12_win)
    add_subdirectory(cube_zones_d3d12_win)
    add_subdirectory(cube_handle_vk_win)
    add_subdirectory(cube_zones_vk_win)
    add_subdirectory(cube_handle_gl_win)
    add_subdirectory(cube_zones_gl_win)
    add_subdirectory(cube_zones_texture_d3d11_win)
    add_subdirectory(cube_zones_texture_d3d12_win)
    add_subdirectory(cube_zones_texture_gl_win)
    add_subdirectory(cube_zones_texture_vk_win)
    add_subdirectory(weave_rpc_probe_d3d11_win)
    add_subdirectory(cube_hosted_legacy_d3d11_win)
    add_subdirectory(cube_hosted_legacy_d3d12_win)
    add_subdirectory(cube_hosted_legacy_vk_win)
    add_subdirectory(cube_hosted_legacy_gl_win)
    add_subdirectory(workspace_minimal_d3d11_win)
    # Window-space layer test apps (#389): submit N XrCompositionLayerWindowSpaceEXT
    # layers, no 3D scene, to verify the multi-layer flicker/drop fix per API.
    add_subdirectory(windowspace_handle_d3d11_win)
    add_subdirectory(windowspace_handle_d3d12_win)
    add_subdirectory(windowspace_handle_vk_win)
    add_subdirectory(windowspace_handle_gl_win)
else()
    message(STATUS "OpenXR not found, skipping OpenXR test apps")
    message(STATUS "To build them, set OpenXR_ROOT or install OpenXR SDK")
endif()

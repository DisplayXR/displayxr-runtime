# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Native build for weave_client_vk_android — the reference XR_DXR_weave
# present-owner (#1036). No shaders: the app is clears, copies and blits, so it
# stays about the weave contract rather than about a renderer.

cmake_minimum_required(VERSION 3.22.1)
project(weave_client_vk_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Khronos OpenXR loader from the openxr_loader_for_android .aar (prefab).
find_package(OpenXR REQUIRED CONFIG)

add_library(weave_client_vk_android SHARED main.cpp)

# DisplayXR's in-tree OpenXR headers carry the XR_DXR_* extensions the loader
# AAR's stock headers don't ship — XR_DXR_weave above all. BEFORE the AAR's
# -isystem headers so <openxr/...> resolves here, the same header set the
# runtime is built against.
# Path: src/main/cpp -> repo root is six levels up.
target_include_directories(weave_client_vk_android BEFORE PRIVATE
    "${CMAKE_SOURCE_DIR}/../../../../../../src/external/openxr_includes")

target_link_libraries(weave_client_vk_android
    OpenXR::openxr_loader
    vulkan
    android
    nativewindow
    log)

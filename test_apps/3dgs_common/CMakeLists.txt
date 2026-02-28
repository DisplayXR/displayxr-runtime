# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Shared 3DGS-OpenXR integration library
#
# Phase 1: Placeholder Vulkan renderer (clear operations)
# Phase 2: FetchContent 3DGS.cpp + compute-based Gaussian splatting
#
# The gs_renderer adapter provides a stable API for the test apps.
# Swapping in the real 3DGS compute pipeline only changes this library.

cmake_minimum_required(VERSION 3.21)
project(gs_renderer_lib LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Vulkan REQUIRED)

# ── gs_renderer adapter library ──────────────────────────────────────────
add_library(gs_renderer STATIC
    gs_renderer.h
    gs_renderer.cpp
    gs_scene_loader.h
    gs_scene_loader.cpp
)

target_include_directories(gs_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(gs_renderer PUBLIC
    Vulkan::Vulkan
)

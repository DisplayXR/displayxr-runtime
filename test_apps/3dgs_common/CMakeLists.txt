# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Shared 3DGS-OpenXR integration library
# Fetches 3DGS.cpp via FetchContent, patches for external Vulkan device support,
# and builds the gs_renderer adapter for OpenXR stereo rendering.

cmake_minimum_required(VERSION 3.21)
project(gs_renderer_lib LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Vulkan REQUIRED)

# ── FetchContent: 3DGS.cpp ──────────────────────────────────────────────
include(FetchContent)

FetchContent_Declare(3dgs_cpp
    GIT_REPOSITORY https://github.com/shg8/3DGS.cpp.git
    GIT_TAG        2f78tried  # Pin to known-good commit — update after verifying
    PATCH_COMMAND  ${CMAKE_COMMAND} -E echo "Applying external Vulkan patch..."
          COMMAND  git checkout -- .
          COMMAND  git apply --verbose "${CMAKE_CURRENT_SOURCE_DIR}/patches/external_vulkan.patch"
)

# Disable 3DGS.cpp's own app targets — we only want the library
set(VKGS_BUILD_APPS OFF CACHE BOOL "" FORCE)
set(VKGS_ENABLE_GLFW OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(3dgs_cpp)

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

# 3dgs_cpp target provides vulkan.hpp, glm, spdlog, embedded shaders
target_link_libraries(gs_renderer PUBLIC
    3dgs_cpp
    Vulkan::Vulkan
)

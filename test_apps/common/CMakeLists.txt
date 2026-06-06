# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)
project(sr_common LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static CRT to avoid requiring VC++ redistributable
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Shared off-axis Kooima projection math (display3d_view / camera3d_view /
# projection_depth) now lives in displayxr::math, pinned by tag instead of
# vendored here (epic #396 W3). The OpenXR target each app establishes before
# add_subdirectory(common) is reused — displayxr::math does not re-fetch OpenXR.
include(FetchContent)
set(DISPLAYXR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    displayxr_common
    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git
    GIT_TAG v0.2.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(displayxr_common)

# API-agnostic base library: logging, input, window, xr_session_common
# No D3D11 or other graphics API dependencies
add_library(sr_common_base STATIC
    logging.cpp
    logging.h
    input_handler.cpp
    input_handler.h
    window_manager.cpp
    window_manager.h
    leia_math.h
    xr_session_common.cpp
    xr_session_common.h
    # In-app atlas capture: cross-platform filename helpers + Windows flash
    # overlay + the single STB implementation TU for Windows-linked targets.
    # Per-API readback (atlas_capture_{vk,d3d11,d3d12,gl}.cpp) is added to
    # each app's own source list so this base lib stays free of graphics-
    # API dependencies.
    atlas_capture.h
    atlas_capture.cpp
    # Hybrid-GPU dGPU hint (Windows): NvOptimusEnablement / AmdPowerXpressRequestHighPerformance.
    # Forced into the EXE via the /INCLUDE linker option below so the driver-side
    # GPU selection lines up with the service compositor's dGPU pin.
    optimus_dgpu_hint.c
)

target_include_directories(sr_common_base PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/external/openxr_includes
)

# Shared off-axis Kooima projection math (display3d_view / camera3d_view +
# projection_depth.h) — PUBLIC so the headers propagate to every consumer.
target_link_libraries(sr_common_base PUBLIC displayxr::math)

# Force the linker to pull optimus_dgpu_hint.c into every EXE that links
# sr_common_base, even though nothing references its symbols. /INCLUDE on
# MSVC, --undefined on GNU-style linkers (e.g. clang on Windows).
if(WIN32)
    if(MSVC)
        target_link_options(sr_common_base INTERFACE "/INCLUDE:NvOptimusEnablement")
    else()
        target_link_options(sr_common_base INTERFACE "-Wl,--undefined=NvOptimusEnablement")
    endif()
endif()

# D3D11-specific library: extends base with D3D11 renderer + text overlay + HUD
add_library(sr_common STATIC
    d3d11_renderer.cpp
    d3d11_renderer.h
    text_overlay.cpp
    text_overlay.h
    hud_renderer.cpp
    hud_renderer.h
)

target_link_libraries(sr_common PUBLIC
    sr_common_base
    d3d11
    dxgi
    d3dcompiler
    d2d1
    dwrite
)

# Atlas capture is runtime-owned now (XR_EXT_atlas_capture); the Windows
# cube_handle apps call dxr_capture::RequestRuntimeAtlasCapture (in
# sr_common_base) and the macOS apps call xrCaptureAtlasEXT inline, instead of
# a per-API readback. All per-API readback TUs (atlas_capture_{d3d11,d3d12,gl,
# vk}.cpp + atlas_capture_metal.mm) have been deleted (#396 W6).

# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)
project(sr_common LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static CRT to avoid requiring VC++ redistributable
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

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
)

target_include_directories(sr_common_base PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/external/openxr_includes
)

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

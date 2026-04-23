# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Standalone build (displayxr-demo-* repo). Sibling layout places
# openxr headers at ../openxr_includes. Windows-only parts (D3D11 HUD,
# DirectWrite text overlay, Win32 window manager, xr_session_common)
# are guarded with if(WIN32) so the same library target can link on
# macOS for demos that only need the cross-platform helpers.
# Synced by publish-demo-<name>.yml.

cmake_minimum_required(VERSION 3.21)
project(sr_common LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Cross-platform math helpers — Kooima projection, camera pose, view params.
# These are the only sources that cleanly build on both macOS and Windows.
set(SR_COMMON_BASE_SOURCES
    display3d_view.c
    display3d_view.h
    camera3d_view.c
    camera3d_view.h
    view_params.h
    leia_math.h
)
# Everything else in this library is Windows-only — input_handler.{cpp,h}
# pulls in <windows.h> unconditionally, logging.cpp uses shell32, etc. Gate
# them behind if(WIN32) so macOS demos that only need the math helpers can
# still link sr_common_base.
if(WIN32)
    list(APPEND SR_COMMON_BASE_SOURCES
        input_handler.cpp
        input_handler.h
        logging.cpp
        logging.h
        window_manager.cpp
        window_manager.h
        xr_session_common.cpp
        xr_session_common.h
    )
endif()

add_library(sr_common_base STATIC ${SR_COMMON_BASE_SOURCES})

target_include_directories(sr_common_base PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../openxr_includes
)

# D3D11-specific library (Windows only): extends base with D3D11 renderer,
# text overlay, and HUD. Macs build without this target.
if(WIN32)
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
endif()

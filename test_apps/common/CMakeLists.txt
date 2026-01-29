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

# Create the common library
add_library(sr_common STATIC
    logging.cpp
    logging.h
    input_handler.cpp
    input_handler.h
    d3d11_renderer.cpp
    d3d11_renderer.h
    text_overlay.cpp
    text_overlay.h
    window_manager.cpp
    window_manager.h
    leia_math.h
    xr_session_common.cpp
    xr_session_common.h
)

target_include_directories(sr_common PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/external/openxr_includes
)

target_link_libraries(sr_common PUBLIC
    d3d11
    dxgi
    d3dcompiler
    d2d1
    dwrite
)

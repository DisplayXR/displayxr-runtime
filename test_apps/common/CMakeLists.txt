# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)
project(sr_common LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static CRT to avoid requiring VC++ redistributable. Set BEFORE the
# FetchContent below so the fetched displayxr-common targets inherit it.
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# The shared scaffolding (logging / input / window / xr_session_common / HUD /
# D3D11 reference renderer / window-space-layer UI / atlas-capture helper /
# stb) AND the off-axis Kooima math now live in displayxr-common, pinned by
# tag instead of vendored here (epic #396 W3 math, W4 scaffolding). The OpenXR
# target each app establishes before add_subdirectory(common) is reused —
# displayxr-common does not re-fetch OpenXR. The DisplayXR extension headers
# come from this repo's source-of-truth copy via
# DISPLAYXR_EXTENSIONS_INCLUDE_DIR.
include(FetchContent)
set(DISPLAYXR_COMMON_BUILD_TESTS OFF CACHE BOOL "" FORCE)
get_filename_component(_dxr_ext_includes
    "${CMAKE_CURRENT_SOURCE_DIR}/../../src/external/openxr_includes" ABSOLUTE)
set(DISPLAYXR_EXTENSIONS_INCLUDE_DIR "${_dxr_ext_includes}" CACHE PATH
    "DisplayXR OpenXR extension headers for displayxr::common")
FetchContent_Declare(
    displayxr_common
    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git
    GIT_TAG v0.3.0
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(displayxr_common)

# Thin compatibility shims: every app links sr_common (D3D11 stack) or
# sr_common_base (API-agnostic) — both now map onto displayxr::common, which
# carries the d3d11/d2d1/dwrite link closure and the /INCLUDE dGPU-hint
# INTERFACE option itself. Only leia_math.h (app-local reference math) and
# textures/ remain vendored in this directory.
add_library(sr_common_base INTERFACE)
target_link_libraries(sr_common_base INTERFACE displayxr::common)
target_include_directories(sr_common_base INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../../src/external/openxr_includes
)

add_library(sr_common INTERFACE)
target_link_libraries(sr_common INTERFACE sr_common_base)

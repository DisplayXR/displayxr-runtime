# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Shared 3DGS-OpenXR integration library
#
# Embeds 8 compute shaders for Gaussian splatting, PLY and SPZ parsers,
# and Vulkan utility helpers.  Test apps link this library and call
# GsRenderer::renderEye() with OpenXR swapchain images.

cmake_minimum_required(VERSION 3.21)
project(gs_renderer_lib LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Vulkan REQUIRED)

# ── Niantic SPZ format support (FetchContent) ────────────────────────────
include(FetchContent)

# zlib (required by spz)
find_package(ZLIB QUIET)
if(NOT ZLIB_FOUND)
    message(STATUS "System zlib not found — fetching via FetchContent")
    FetchContent_Declare(zlib
        GIT_REPOSITORY https://github.com/madler/zlib.git
        GIT_TAG        v1.3.1
    )
    set(SKIP_INSTALL_ALL ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(zlib)
    set(SPZ_ZLIB_TARGET zlibstatic)
else()
    set(SPZ_ZLIB_TARGET ZLIB::ZLIB)
endif()

# Download spz source (build manually to avoid find_package(ZLIB) issues)
FetchContent_Declare(spz
    GIT_REPOSITORY https://github.com/nianticlabs/spz.git
    GIT_TAG        v1.1.0
    SOURCE_SUBDIR  _skip  # Skip spz's CMakeLists.txt — we build sources manually below
)
FetchContent_MakeAvailable(spz)

# Patch spz for MSVC compatibility:
# 1. std::sqrt is not constexpr on MSVC → replace constexpr with inline
# 2. 0.0f/0.0f and 1.0f/0.0f cause C2124 → use std::numeric_limits
if(MSVC)
    # Patch splat-types.h: constexpr → inline (MSVC C3615 fix)
    # Also add missing #include <algorithm> (for std::sort) and _USE_MATH_DEFINES (for M_PI)
    set(_SPZ_HEADER "${spz_SOURCE_DIR}/src/cc/splat-types.h")
    file(READ "${_SPZ_HEADER}" _spz_content)
    string(FIND "${_spz_content}" "constexpr" _spz_has_constexpr)
    if(NOT _spz_has_constexpr EQUAL -1)
        string(REPLACE "constexpr " "inline " _spz_content "${_spz_content}")
        file(WRITE "${_SPZ_HEADER}" "${_spz_content}")
        message(STATUS "Patched spz splat-types.h: constexpr -> inline for MSVC")
    endif()
    # Add #include <algorithm> if missing (std::sort used in header)
    string(FIND "${_spz_content}" "#include <algorithm>" _spz_has_algo)
    if(_spz_has_algo EQUAL -1)
        file(READ "${_SPZ_HEADER}" _spz_content)
        string(REPLACE "#include <cmath>" "#define _USE_MATH_DEFINES\n#include <cmath>\n#include <algorithm>" _spz_content "${_spz_content}")
        file(WRITE "${_SPZ_HEADER}" "${_spz_content}")
        message(STATUS "Patched spz splat-types.h: added <algorithm> and _USE_MATH_DEFINES for MSVC")
    endif()

    # Patch splat-types.cc: replace 0/0 NaN/Inf literals with std::numeric_limits (MSVC C2124 fix)
    set(_SPZ_SOURCE "${spz_SOURCE_DIR}/src/cc/splat-types.cc")
    file(READ "${_SPZ_SOURCE}" _spz_src)
    # Always attempt the replacement (string(REPLACE) is a no-op if pattern not found)
    # Add <limits> include after <cmath> (guard against double-insert)
    string(FIND "${_spz_src}" "#include <limits>" _spz_has_limits)
    if(_spz_has_limits EQUAL -1)
        string(REPLACE "#include <cmath>" "#include <cmath>\n#include <limits>" _spz_src "${_spz_src}")
    endif()
    # Replace NaN/Inf construction — try both spaced and unspaced variants
    string(REPLACE "0.0f / 0.0f" "std::numeric_limits<float>::quiet_NaN()" _spz_src "${_spz_src}")
    string(REPLACE "0.0f/0.0f" "std::numeric_limits<float>::quiet_NaN()" _spz_src "${_spz_src}")
    string(REPLACE "1.0f / 0.0f" "std::numeric_limits<float>::infinity()" _spz_src "${_spz_src}")
    string(REPLACE "1.0f/0.0f" "std::numeric_limits<float>::infinity()" _spz_src "${_spz_src}")
    file(WRITE "${_SPZ_SOURCE}" "${_spz_src}")
    message(STATUS "Patched spz splat-types.cc: div-by-zero -> numeric_limits for MSVC")
endif()

add_library(spz_lib STATIC
    ${spz_SOURCE_DIR}/src/cc/load-spz.cc
    ${spz_SOURCE_DIR}/src/cc/splat-c-types.cc
    ${spz_SOURCE_DIR}/src/cc/splat-types.cc
)
target_include_directories(spz_lib PUBLIC ${spz_SOURCE_DIR}/src/cc)
target_link_libraries(spz_lib PRIVATE ${SPZ_ZLIB_TARGET})
set_target_properties(spz_lib PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)
if(MSVC)
    # _USE_MATH_DEFINES: spz uses M_PI which MSVC only defines with this macro
    # /FI<file>: force-include <algorithm> so std::sort is available in spz headers
    # PUBLIC: propagate to consumers (gs_renderer) that include spz headers
    target_compile_definitions(spz_lib PUBLIC _USE_MATH_DEFINES)
    target_compile_options(spz_lib PUBLIC /FIalgorithm)
    set_target_properties(spz_lib PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

# ── SPIR-V shader compilation ────────────────────────────────────────────
# Inline version of cmake/SPIR-V.cmake with --include-dir for common.glsl.

find_program(GLSLANGVALIDATOR_COMMAND glslangValidator)
if(NOT GLSLANGVALIDATOR_COMMAND)
    message(FATAL_ERROR "glslangValidator required - install via Vulkan SDK")
endif()

set(SHADER_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_SOURCES
    shaders/precomp_cov3d.comp
    shaders/preprocess.comp
    shaders/prefix_sum.comp
    shaders/preprocess_sort.comp
    shaders/hist.comp
    shaders/sort.comp
    shaders/tile_boundary.comp
    shaders/render.comp
)

set(SHADER_HEADERS "")
foreach(GLSL_REL ${SHADER_SOURCES})
    # Extract filename without directory: e.g., "precomp_cov3d.comp"
    get_filename_component(GLSL_NAME ${GLSL_REL} NAME)
    # C identifier from filename: e.g., "precomp_cov3d_comp"
    string(MAKE_C_IDENTIFIER ${GLSL_NAME} IDENTIFIER)
    # Variable name with _data suffix: e.g., "precomp_cov3d_comp_data"
    set(VARNAME "${IDENTIFIER}_data")
    # Output header: e.g., "${BINARY_DIR}/precomp_cov3d.comp.h"
    set(HEADER "${CMAKE_CURRENT_BINARY_DIR}/${GLSL_NAME}.h")
    # Full source path
    set(GLSL_SRC "${CMAKE_CURRENT_SOURCE_DIR}/${GLSL_REL}")

    add_custom_command(
        OUTPUT ${HEADER}
        COMMAND ${GLSLANGVALIDATOR_COMMAND}
            -V --target-env vulkan1.2
            -I${SHADER_INCLUDE_DIR}
            ${GLSL_SRC}
            --vn ${VARNAME}
            -o ${HEADER}
        DEPENDS ${GLSL_SRC} ${SHADER_INCLUDE_DIR}/common.glsl
        COMMENT "Compiling SPIR-V: ${GLSL_NAME}"
    )
    list(APPEND SHADER_HEADERS ${HEADER})
endforeach()

# ── gs_renderer adapter library ──────────────────────────────────────────
add_library(gs_renderer STATIC
    gs_renderer.h
    gs_renderer.cpp
    gs_scene_loader.h
    gs_scene_loader.cpp
    gs_spz_loader.h
    gs_spz_loader.cpp
    gs_vulkan_utils.h
    gs_vulkan_utils.cpp
    ${SHADER_HEADERS}
)

target_include_directories(gs_renderer PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_BINARY_DIR}   # For generated SPIR-V headers
)

target_link_libraries(gs_renderer PUBLIC
    Vulkan::Vulkan
    spz_lib
)

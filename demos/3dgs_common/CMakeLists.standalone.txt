# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Standalone build (displayxr-demo-gaussiansplat repo). Identical to the
# in-tree CMakeLists.txt since 3dgs_common has no path references that reach
# outside its own directory. Synced by publish-demo-gaussiansplat.yml.

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

FetchContent_Declare(spz
    GIT_REPOSITORY https://github.com/nianticlabs/spz.git
    GIT_TAG        v1.1.0
    SOURCE_SUBDIR  _skip
)
FetchContent_MakeAvailable(spz)

# MSVC-compat patches for spz (see in-tree CMakeLists.txt for rationale).
if(MSVC)
    set(_SPZ_HEADER "${spz_SOURCE_DIR}/src/cc/splat-types.h")
    file(READ "${_SPZ_HEADER}" _spz_content)
    string(REPLACE "constexpr " "inline " _spz_content "${_spz_content}")
    string(FIND "${_spz_content}" "_USE_MATH_DEFINES" _has_math_def)
    if(_has_math_def EQUAL -1)
        string(REPLACE "#include <cmath>" "#define _USE_MATH_DEFINES\n#include <cmath>\n#include <algorithm>" _spz_content "${_spz_content}")
    endif()
    file(WRITE "${_SPZ_HEADER}" "${_spz_content}")

    set(_SPZ_SOURCE "${spz_SOURCE_DIR}/src/cc/splat-types.cc")
    file(READ "${_SPZ_SOURCE}" _spz_src)
    string(FIND "${_spz_src}" "#include <limits>" _spz_has_limits)
    if(_spz_has_limits EQUAL -1)
        string(REPLACE "#include <cmath>" "#include <cmath>\n#include <limits>" _spz_src "${_spz_src}")
    endif()
    string(REPLACE "0.0f / 0.0f" "std::numeric_limits<float>::quiet_NaN()" _spz_src "${_spz_src}")
    string(REPLACE "0.0f/0.0f" "std::numeric_limits<float>::quiet_NaN()" _spz_src "${_spz_src}")
    string(REPLACE "1.0f / 0.0f" "std::numeric_limits<float>::infinity()" _spz_src "${_spz_src}")
    string(REPLACE "1.0f/0.0f" "std::numeric_limits<float>::infinity()" _spz_src "${_spz_src}")
    file(WRITE "${_SPZ_SOURCE}" "${_spz_src}")
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
    target_compile_definitions(spz_lib PUBLIC _USE_MATH_DEFINES)
    target_compile_options(spz_lib PUBLIC /FIalgorithm)
    set_target_properties(spz_lib PROPERTIES
        MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

# ── SPIR-V shader compilation ────────────────────────────────────────────
find_program(GLSLANGVALIDATOR_COMMAND glslangValidator)
if(NOT GLSLANGVALIDATOR_COMMAND)
    message(FATAL_ERROR "glslangValidator required — install the Vulkan SDK")
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
    get_filename_component(GLSL_NAME ${GLSL_REL} NAME)
    string(MAKE_C_IDENTIFIER ${GLSL_NAME} IDENTIFIER)
    set(VARNAME "${IDENTIFIER}_data")
    set(HEADER "${CMAKE_CURRENT_BINARY_DIR}/${GLSL_NAME}.h")
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
    ${CMAKE_CURRENT_BINARY_DIR}
)

target_link_libraries(gs_renderer PUBLIC
    Vulkan::Vulkan
    spz_lib
)

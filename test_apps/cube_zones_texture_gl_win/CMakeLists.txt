# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0
#
# Cloned from cube_zones_texture_vk_win (texture-class D3D11/DComp machinery) +
# cube_zones_gl_win (GL render + zones logic) for #613.
#
# TEXTURE-class OpenGL display-zones app. Unlike the VK leg this is NOT gated on
# a Vulkan SDK — GL needs no Vulkan. It renders the zones into OpenGL OpenXR
# swapchains (opengl32) and keeps a bespoke D3D11 present/blit/readback path
# (d3d11 / dxgi / dcomp; blit shaders compiled at runtime via d3dcompiler). The
# runtime's GL native compositor bridges the app's D3D11 KMT shared texture into
# a GL texture via WGL_NV_DX_interop2 and composites the zones into it.

cmake_minimum_required(VERSION 3.21)
project(cube_zones_texture_gl_win LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static CRT to avoid requiring VC++ redistributable
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# OpenXR headers location
set(OPENXR_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../src/external/openxr_includes")

# Find OpenXR loader - multiple strategies
set(OPENXR_FOUND FALSE)

find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
    message(STATUS "Found OpenXR via find_package")
endif()

if(NOT OPENXR_FOUND AND DEFINED OpenXR_ROOT)
    find_library(OPENXR_LOADER_LIB
        NAMES openxr_loader openxr_loader-1_0
        PATHS "${OpenXR_ROOT}/x64/lib" "${OpenXR_ROOT}/lib" "${OpenXR_ROOT}/bin"
        NO_DEFAULT_PATH
    )
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        find_file(OPENXR_LOADER_DLL
            NAMES openxr_loader.dll openxr_loader-1_0.dll
            PATHS "${OpenXR_ROOT}/x64/bin" "${OpenXR_ROOT}/bin"
            NO_DEFAULT_PATH
        )
        if(OPENXR_LOADER_DLL)
            set_target_properties(OpenXR::openxr_loader PROPERTIES
                IMPORTED_LOCATION "${OPENXR_LOADER_DLL}"
                IMPORTED_IMPLIB "${OPENXR_LOADER_LIB}"
            )
        else()
            set_target_properties(OpenXR::openxr_loader PROPERTIES
                IMPORTED_IMPLIB "${OPENXR_LOADER_LIB}"
            )
        endif()
    endif()
endif()

if(NOT OPENXR_FOUND)
    set(PARENT_BUILD_DIR "${CMAKE_SOURCE_DIR}/../../build")
    if(EXISTS "${PARENT_BUILD_DIR}/src/external/openxr_loader/OpenXRConfig.cmake")
        list(APPEND CMAKE_PREFIX_PATH "${PARENT_BUILD_DIR}/src/external/openxr_loader")
        find_package(OpenXR REQUIRED CONFIG)
        set(OPENXR_FOUND TRUE)
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found.")
endif()

# Common base library (API-agnostic): provides logging / input / window-space
# HUD / atlas capture / xr_session_common + the DisplayXR extension headers.
# We link sr_common_base (NOT sr_common) — this hybrid renders the cube into the
# OpenXR (OpenGL) swapchains via gl_renderer.cpp, and keeps a bespoke D3D11
# present/blit/readback path (its own device, blit shaders, shared KMT texture)
# that the runtime's GL compositor composites into.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)

add_executable(cube_zones_texture_gl_win WIN32
    main.cpp
    xr_session.cpp
    xr_session.h
    gl_renderer.cpp
    gl_renderer.h
    gl_functions.cpp
    gl_functions.h
    # hud_renderer / text_overlay / atlas capture come from displayxr::common
    # (via sr_common_base) — no direct ../common TU compilation (#396 W4).
    resource.rc
)

target_link_libraries(cube_zones_texture_gl_win PRIVATE
    sr_common_base
    OpenXR::openxr_loader
    opengl32
    d3d11
    dxgi
    dcomp
    d3dcompiler
    d2d1
    dwrite
)

# Embed manifest (copy to build dir; use relative path to avoid spaces-in-path linker issues)
if(MSVC)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/sr_cube_zones_texture_gl.manifest" "${CMAKE_CURRENT_BINARY_DIR}/sr_cube_zones_texture_gl.manifest" COPYONLY)
    set_target_properties(cube_zones_texture_gl_win PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:sr_cube_zones_texture_gl.manifest"
    )
endif()

if(TARGET OpenXR::openxr_loader)
    get_target_property(_loader_location OpenXR::openxr_loader IMPORTED_LOCATION)
    if(_loader_location)
        add_custom_command(TARGET cube_zones_texture_gl_win POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_loader_location}"
                $<TARGET_FILE_DIR:cube_zones_texture_gl_win>
            COMMENT "Copying OpenXR loader DLL"
        )
    endif()
endif()

# Copy texture files to build output directory
file(GLOB TEXTURE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../common/textures/*.jpg")
if(TEXTURE_FILES)
    add_custom_command(TARGET cube_zones_texture_gl_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:cube_zones_texture_gl_win>/textures"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TEXTURE_FILES} "$<TARGET_FILE_DIR:cube_zones_texture_gl_win>/textures/"
        COMMENT "Copying texture files to build output"
    )
endif()

# DisplayXR shell manifest sidecar
displayxr_install_manifest(cube_zones_texture_gl_win "${CMAKE_CURRENT_SOURCE_DIR}/displayxr")

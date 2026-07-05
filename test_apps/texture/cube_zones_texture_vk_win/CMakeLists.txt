# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)
project(cube_zones_texture_vk_win LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static CRT to avoid requiring VC++ redistributable
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Find Vulkan SDK — this is a HYBRID app (VK for XR rendering, D3D11 for the
# shared texture + window present). No Vulkan SDK ⟹ skip, like cube_handle_vk_win.
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    message(STATUS "Vulkan SDK not found - skipping cube_zones_texture_vk_win")
    return()
endif()
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR headers location
set(OPENXR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/external/openxr_includes")

# Find OpenXR loader - multiple strategies
set(OPENXR_FOUND FALSE)

# Strategy 1: find_package
find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
    message(STATUS "Found OpenXR via find_package")
endif()

# Strategy 2: Look in OpenXR_ROOT
if(NOT OPENXR_FOUND AND DEFINED OpenXR_ROOT)
    find_library(OPENXR_LOADER_LIB
        NAMES openxr_loader openxr_loader-1_0
        PATHS
            "${OpenXR_ROOT}/x64/lib"
            "${OpenXR_ROOT}/lib"
            "${OpenXR_ROOT}/bin"
        NO_DEFAULT_PATH
    )
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        find_file(OPENXR_LOADER_DLL
            NAMES openxr_loader.dll openxr_loader-1_0.dll
            PATHS
                "${OpenXR_ROOT}/x64/bin"
                "${OpenXR_ROOT}/bin"
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

# Strategy 3: Look in parent build directory
if(NOT OPENXR_FOUND)
    set(PARENT_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../build")
    if(EXISTS "${PARENT_BUILD_DIR}/src/external/openxr_loader/OpenXRConfig.cmake")
        list(APPEND CMAKE_PREFIX_PATH "${PARENT_BUILD_DIR}/src/external/openxr_loader")
        find_package(OpenXR REQUIRED CONFIG)
        set(OPENXR_FOUND TRUE)
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Options:\n"
        "  1. Install OpenXR SDK\n"
        "  2. Build the parent project first\n"
        "  3. Set OpenXR_ROOT to point to directory containing openxr_loader")
endif()

# Common library.
#
# We link sr_common_base (the API-agnostic stack: logging / input /
# xr_session_common / HUD / text_overlay / atlas_capture / stb), NOT sr_common
# — this hybrid does NOT use the D3D11 reference renderer (D3D11Renderer /
# InitializeD3D11WithLUID / the D3D11 RenderScene). Instead it:
#   - renders the cube into the OpenXR (Vulkan) swapchains via vk_renderer.cpp,
#   - and keeps a *bespoke* D3D11 present/blit/readback path (its own LUID-
#     matched device, blit shaders, shared KMT texture) — the runtime's VK
#     compositor imports that D3D11 KMT texture as its composite target.
# Both sr_common and sr_common_base map onto displayxr::common (the d3d11/
# d2d1/dwrite link closure + the dGPU-hint INTERFACE option), so picking the
# base avoids dragging in the unused D3D11 cube renderer's symbols. The VK leg
# adds Vulkan::Vulkan; the blit shaders compile at runtime via d3dcompiler;
# the transparent present uses dcomp.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../common ${CMAKE_CURRENT_BINARY_DIR}/common)

# Create executable
add_executable(cube_zones_texture_vk_win WIN32
    main.cpp
    xr_session.cpp
    xr_session.h
    vk_renderer.cpp
    vk_renderer.h
    resource.rc
)

# OpenXR include dir is provided by sr_common_base PUBLIC include directories
target_link_libraries(cube_zones_texture_vk_win PRIVATE
    sr_common_base
    OpenXR::openxr_loader
    Vulkan::Vulkan
    d3d11
    dxgi
    dcomp
    d3dcompiler
    d2d1
    dwrite
)

# Embed manifest (copy to build dir; use relative path to avoid spaces-in-path linker issues)
if(MSVC)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/sr_cube_openxr_zones_texture_vk.manifest" "${CMAKE_CURRENT_BINARY_DIR}/sr_cube_openxr_zones_texture_vk.manifest" COPYONLY)
    set_target_properties(cube_zones_texture_vk_win PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:sr_cube_openxr_zones_texture_vk.manifest"
    )
endif()

# Copy texture files to build output directory
file(GLOB TEXTURE_FILES "${CMAKE_CURRENT_SOURCE_DIR}/../../common/textures/*.jpg")
if(TEXTURE_FILES)
    add_custom_command(TARGET cube_zones_texture_vk_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:cube_zones_texture_vk_win>/textures"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TEXTURE_FILES} "$<TARGET_FILE_DIR:cube_zones_texture_vk_win>/textures/"
        COMMENT "Copying texture files to build output"
    )
endif()

# DisplayXR shell manifest sidecar
displayxr_install_manifest(cube_zones_texture_vk_win "${CMAKE_CURRENT_SOURCE_DIR}/displayxr")

# Copy runtime DLLs
if(TARGET OpenXR::openxr_loader)
    get_target_property(_loader_location OpenXR::openxr_loader IMPORTED_LOCATION)
    if(_loader_location)
        add_custom_command(TARGET cube_zones_texture_vk_win POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_loader_location}"
                $<TARGET_FILE_DIR:cube_zones_texture_vk_win>
            COMMENT "Copying OpenXR loader DLL"
        )
    endif()
endif()

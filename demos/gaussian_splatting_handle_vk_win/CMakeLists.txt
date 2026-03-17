# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# 3D Gaussian Splatting with OpenXR (Vulkan)
# Renders 3DGS scenes on tracked 3D displays via OpenXR and XR_EXT_win32_window_binding.

cmake_minimum_required(VERSION 3.21)
project(gaussian_splatting_handle_vk_win LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Find Vulkan SDK
find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    message(STATUS "Vulkan SDK not found - skipping gaussian_splatting_handle_vk_win")
    return()
endif()
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

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

# Common base library (API-agnostic)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../test_apps/common ${CMAKE_CURRENT_BINARY_DIR}/common)

# 3DGS common library (FetchContent + adapter)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../3dgs_common ${CMAKE_CURRENT_BINARY_DIR}/3dgs_common)

add_executable(gaussian_splatting_handle_vk_win WIN32
    main.cpp
    xr_session.cpp
    xr_session.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../test_apps/common/hud_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../test_apps/common/hud_renderer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../../test_apps/common/text_overlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../../test_apps/common/text_overlay.h
    sr_3dgs_openxr_ext_vk.manifest
    resource.rc
)

target_link_libraries(gaussian_splatting_handle_vk_win PRIVATE
    sr_common_base
    gs_renderer
    OpenXR::openxr_loader
    Vulkan::Vulkan
    d3d11
    d2d1
    dwrite
    comdlg32
)

if(MSVC)
    set_target_properties(gaussian_splatting_handle_vk_win PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/sr_3dgs_openxr_ext_vk.manifest"
    )
endif()

if(TARGET OpenXR::openxr_loader)
    get_target_property(_loader_location OpenXR::openxr_loader IMPORTED_LOCATION)
    if(_loader_location)
        add_custom_command(TARGET gaussian_splatting_handle_vk_win POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_loader_location}"
                $<TARGET_FILE_DIR:gaussian_splatting_handle_vk_win>
            COMMENT "Copying OpenXR loader DLL"
        )
    endif()
endif()

# Copy bundled SimulatedRealityVulkanBeta.dll to output directory
set(VK_WEAVER_DLL "${CMAKE_CURRENT_SOURCE_DIR}/../../test_apps/common/SimulatedRealityVulkanBeta.dll")
if(EXISTS "${VK_WEAVER_DLL}")
    add_custom_command(TARGET gaussian_splatting_handle_vk_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VK_WEAVER_DLL}"
            "$<TARGET_FILE_DIR:gaussian_splatting_handle_vk_win>/SimulatedRealityVulkanBeta.dll"
    )
endif()

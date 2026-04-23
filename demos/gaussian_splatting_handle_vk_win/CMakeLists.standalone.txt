# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Standalone build — lives inside displayxr-demo-gaussiansplat repo as
# windows/CMakeLists.txt. Assumes sibling dirs: ../common, ../3dgs_common,
# ../openxr_includes. Top-level CMakeLists.txt is expected to have already
# added ../common and ../3dgs_common as subdirectories, exposing
# `sr_common_base`, `sr_common`, and `gs_renderer` targets.

cmake_minimum_required(VERSION 3.21)

if(NOT WIN32)
    return()
endif()

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    message(STATUS "Vulkan SDK not found — skipping gaussian_splatting_handle_vk_win")
    return()
endif()
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR loader — CONFIG pkg first, then a user-specified OpenXR_ROOT fallback.
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
    message(FATAL_ERROR "OpenXR not found. Provide -DOpenXR_ROOT=<path> or install via vcpkg.")
endif()

add_executable(gaussian_splatting_handle_vk_win WIN32
    main.cpp
    xr_session.cpp
    xr_session.h
    sr_3dgs_openxr_ext_vk.manifest
    resource.rc
)

target_include_directories(gaussian_splatting_handle_vk_win PRIVATE
    ${CMAKE_SOURCE_DIR}/openxr_includes
)

target_link_libraries(gaussian_splatting_handle_vk_win PRIVATE
    sr_common_base
    sr_common
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

# Copy OpenXR loader DLL next to exe.
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

# Copy SimulatedRealityVulkanBeta.dll from ../common if present (vendored by
# the publish step from the original test_apps/common/).
set(VK_WEAVER_DLL "${CMAKE_SOURCE_DIR}/common/SimulatedRealityVulkanBeta.dll")
if(EXISTS "${VK_WEAVER_DLL}")
    add_custom_command(TARGET gaussian_splatting_handle_vk_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${VK_WEAVER_DLL}"
            "$<TARGET_FILE_DIR:gaussian_splatting_handle_vk_win>/SimulatedRealityVulkanBeta.dll"
    )
endif()

# Copy bundled default scene next to the exe.
set(BUNDLED_SCENE "${CMAKE_CURRENT_SOURCE_DIR}/assets/butterfly.spz")
if(EXISTS "${BUNDLED_SCENE}")
    add_custom_command(TARGET gaussian_splatting_handle_vk_win POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${BUNDLED_SCENE}" "$<TARGET_FILE_DIR:gaussian_splatting_handle_vk_win>/butterfly.spz"
        COMMENT "Copying bundled butterfly.spz next to exe"
    )
endif()

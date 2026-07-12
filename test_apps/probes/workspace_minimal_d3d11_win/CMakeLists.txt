# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0
#
# workspace_minimal_d3d11_win — minimal OpenXR client that exercises
# XR_DXR_spatial_workspace end-to-end. No rendering; the test creates an
# instance, resolves the five extension PFNs, and calls activate /
# add-capture / remove-capture / deactivate. Success path requires the test
# to be launched under an authorized workspace orchestrator; standalone
# launch is expected to surface XR_ERROR_FEATURE_UNSUPPORTED at activate
# time (PID-auth deny path) — both outcomes are valid test signals.

cmake_minimum_required(VERSION 3.21)
project(workspace_minimal_d3d11_win LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

set(OPENXR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/external/openxr_includes")

set(OPENXR_FOUND FALSE)

find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
endif()

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

if(NOT OPENXR_FOUND)
    set(PARENT_BUILD_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../build")
    if(EXISTS "${PARENT_BUILD_DIR}/src/external/openxr_loader/OpenXRConfig.cmake")
        list(APPEND CMAKE_PREFIX_PATH "${PARENT_BUILD_DIR}/src/external/openxr_loader")
        find_package(OpenXR REQUIRED CONFIG)
        set(OPENXR_FOUND TRUE)
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found for workspace_minimal_d3d11_win.")
endif()

add_executable(workspace_minimal_d3d11_win main.cpp)

target_include_directories(workspace_minimal_d3d11_win PRIVATE
    "${OPENXR_INCLUDE_DIR}"
)

target_compile_definitions(workspace_minimal_d3d11_win PRIVATE
    XR_USE_GRAPHICS_API_D3D11
    XR_USE_PLATFORM_WIN32
    NOMINMAX
    WIN32_LEAN_AND_MEAN
)

target_link_libraries(workspace_minimal_d3d11_win PRIVATE
    OpenXR::openxr_loader
    d3d11
    dxgi
)

if(TARGET OpenXR::openxr_loader)
    get_target_property(_loader_location OpenXR::openxr_loader IMPORTED_LOCATION)
    if(_loader_location)
        add_custom_command(TARGET workspace_minimal_d3d11_win POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_loader_location}"
                $<TARGET_FILE_DIR:workspace_minimal_d3d11_win>
            COMMENT "Copying OpenXR loader DLL"
        )
    endif()
endif()

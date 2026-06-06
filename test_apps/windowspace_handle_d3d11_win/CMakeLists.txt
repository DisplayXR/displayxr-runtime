# Copyright 2026, DisplayXR contributors
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)
project(windowspace_handle_d3d11_win LANGUAGES CXX)

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

# Common library (D3D11 renderer + API-agnostic base)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)

add_executable(windowspace_handle_d3d11_win WIN32
    main.cpp
    xr_session.cpp
    xr_session.h
    windowspace_layers.h
    # Standalone window-space helpers (4-arg CreateHudSwapchain overload +
    # SubmitWindowSpaceLayersFrame). Not compiled into sr_common on Windows,
    # so each windowspace app adds it directly.
    # xr_window_space_hud comes from displayxr::common (#396 W4).
    resource.rc
)

target_link_libraries(windowspace_handle_d3d11_win PRIVATE
    sr_common
    OpenXR::openxr_loader
)

# Embed manifest (copy to build dir; use relative path to avoid spaces-in-path linker issues)
if(MSVC)
    configure_file("${CMAKE_CURRENT_SOURCE_DIR}/sr_windowspace_ext.manifest" "${CMAKE_CURRENT_BINARY_DIR}/sr_windowspace_ext.manifest" COPYONLY)
    set_target_properties(windowspace_handle_d3d11_win PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:sr_windowspace_ext.manifest"
    )
endif()

if(TARGET OpenXR::openxr_loader)
    get_target_property(_loader_location OpenXR::openxr_loader IMPORTED_LOCATION)
    if(_loader_location)
        add_custom_command(TARGET windowspace_handle_d3d11_win POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_loader_location}"
                $<TARGET_FILE_DIR:windowspace_handle_d3d11_win>
            COMMENT "Copying OpenXR loader DLL"
        )
    endif()
endif()

# DisplayXR shell manifest sidecar
displayxr_install_manifest(windowspace_handle_d3d11_win "${CMAKE_CURRENT_SOURCE_DIR}/displayxr")

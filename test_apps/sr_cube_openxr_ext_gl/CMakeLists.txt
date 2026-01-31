# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)
project(sr_cube_openxr_ext_gl LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# Build number (from CI or default to 0)
if(NOT DEFINED BUILD_NUM)
    set(BUILD_NUM "0")
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

# Common base library (API-agnostic)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../common ${CMAKE_CURRENT_BINARY_DIR}/common)

add_executable(sr_cube_openxr_ext_gl WIN32
    main.cpp
    xr_session.cpp
    xr_session.h
    gl_renderer.cpp
    gl_renderer.h
    gl_functions.cpp
    gl_functions.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/hud_renderer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/hud_renderer.h
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/text_overlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../common/text_overlay.h
    sr_cube_openxr_ext_gl.manifest
    resource.rc
)

set_target_properties(sr_cube_openxr_ext_gl PROPERTIES
    OUTPUT_NAME "sr_cube_openxr_ext_gl.${BUILD_NUM}"
)

target_link_libraries(sr_cube_openxr_ext_gl PRIVATE
    sr_common_base
    OpenXR::openxr_loader
    opengl32
    d3d11
    d2d1
    dwrite
)

if(MSVC)
    set_target_properties(sr_cube_openxr_ext_gl PROPERTIES
        LINK_FLAGS "/MANIFEST:EMBED /MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/sr_cube_openxr_ext_gl.manifest"
    )
endif()

if(TARGET OpenXR::openxr_loader)
    get_target_property(_loader_location OpenXR::openxr_loader IMPORTED_LOCATION)
    if(_loader_location)
        add_custom_command(TARGET sr_cube_openxr_ext_gl POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_loader_location}"
                $<TARGET_FILE_DIR:sr_cube_openxr_ext_gl>
            COMMENT "Copying OpenXR loader DLL"
        )
    endif()
endif()

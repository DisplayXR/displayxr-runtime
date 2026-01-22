# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.16)
project(session_target_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OpenXR headers are always in the parent project
set(OPENXR_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../src/external/openxr_includes")

# Find OpenXR loader - multiple strategies
set(OPENXR_FOUND FALSE)

# Strategy 1: find_package (works with installed SDK or proper CMake config)
find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
    message(STATUS "Found OpenXR via find_package")
endif()

# Strategy 2: Look in OpenXR_ROOT for the loader library (CI scenario)
if(NOT OPENXR_FOUND AND DEFINED OpenXR_ROOT)
    # Debug: show what we're looking for
    message(STATUS "Looking for OpenXR loader in OpenXR_ROOT: ${OpenXR_ROOT}")
    file(GLOB_RECURSE _all_files "${OpenXR_ROOT}/*")
    message(STATUS "Files found under OpenXR_ROOT:")
    foreach(_f ${_all_files})
        message(STATUS "  ${_f}")
    endforeach()

    # Look for the loader library in various locations
    # Note: OpenXR loader may have versioned names like openxr_loader-1_0
    # The Khronos SDK uses x64/lib and x64/bin structure
    find_library(OPENXR_LOADER_LIB
        NAMES openxr_loader openxr_loader-1_0
        PATHS
            "${OpenXR_ROOT}/x64/lib"
            "${OpenXR_ROOT}/lib"
            "${OpenXR_ROOT}/bin"
            "${OpenXR_ROOT}"
        NO_DEFAULT_PATH
    )
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        message(STATUS "Found OpenXR loader at: ${OPENXR_LOADER_LIB}")

        # Create imported target
        add_library(OpenXR::openxr_loader SHARED IMPORTED)

        # Find the DLL for runtime
        # The Khronos SDK uses x64/bin for DLLs
        find_file(OPENXR_LOADER_DLL
            NAMES openxr_loader.dll openxr_loader-1_0.dll
            PATHS
                "${OpenXR_ROOT}/x64/bin"
                "${OpenXR_ROOT}/bin"
                "${OpenXR_ROOT}/lib"
                "${OpenXR_ROOT}"
            NO_DEFAULT_PATH
        )

        if(OPENXR_LOADER_DLL)
            set_target_properties(OpenXR::openxr_loader PROPERTIES
                IMPORTED_LOCATION "${OPENXR_LOADER_DLL}"
                IMPORTED_IMPLIB "${OPENXR_LOADER_LIB}"
            )
            message(STATUS "Found OpenXR loader DLL at: ${OPENXR_LOADER_DLL}")
        else()
            # On Windows, .lib might be the import lib alongside .dll
            set_target_properties(OpenXR::openxr_loader PROPERTIES
                IMPORTED_IMPLIB "${OPENXR_LOADER_LIB}"
            )
            message(STATUS "OpenXR loader DLL not found, using .lib only")
        endif()
    endif()
endif()

# Strategy 3: Look in parent build directory (local dev)
if(NOT OPENXR_FOUND)
    set(PARENT_BUILD_DIR "${CMAKE_SOURCE_DIR}/../../build")
    if(EXISTS "${PARENT_BUILD_DIR}/src/external/openxr_loader/OpenXRConfig.cmake")
        list(APPEND CMAKE_PREFIX_PATH "${PARENT_BUILD_DIR}/src/external/openxr_loader")
        find_package(OpenXR REQUIRED CONFIG)
        set(OPENXR_FOUND TRUE)
        message(STATUS "Found OpenXR in parent build directory")
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Options:\n"
        "  1. Install OpenXR SDK\n"
        "  2. Build the parent project first\n"
        "  3. Set OpenXR_ROOT to point to directory containing openxr_loader")
endif()

add_executable(session_target_test WIN32
    main.cpp
    xr_session.cpp
    xr_session.h
    d3d11_renderer.cpp
    d3d11_renderer.h
    input_state.cpp
    input_state.h
    text_overlay.cpp
    text_overlay.h
    logging.cpp
    logging.h
)

target_include_directories(session_target_test PRIVATE
    ${OPENXR_INCLUDE_DIR}
)

target_link_libraries(session_target_test PRIVATE
    OpenXR::openxr_loader
    d3d11
    dxgi
    d3dcompiler
    d2d1
    dwrite
)

# For development: create a batch file to run with local runtime
file(WRITE ${CMAKE_BINARY_DIR}/run_local.bat
    "@echo off\r\n"
    "set XR_RUNTIME_JSON=${CMAKE_SOURCE_DIR}/../../build/openxr_monado-dev.json\r\n"
    "session_target_test.exe\r\n"
    "pause\r\n"
)

# Copy runtime DLLs to output directory for standalone use
if(TARGET OpenXR::openxr_loader)
    get_target_property(_loader_location OpenXR::openxr_loader IMPORTED_LOCATION)
    if(_loader_location)
        add_custom_command(TARGET session_target_test POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_loader_location}"
                $<TARGET_FILE_DIR:session_target_test>
            COMMENT "Copying OpenXR loader DLL"
        )
    endif()
endif()

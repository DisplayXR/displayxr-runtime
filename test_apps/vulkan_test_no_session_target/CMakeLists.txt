# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)
project(vulkan_test_no_session_target LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Use static CRT to avoid requiring VC++ redistributable on target machines
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# OpenXR headers are always in the parent project
set(OPENXR_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../src/external/openxr_includes")

# Find Vulkan SDK
find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_INCLUDE_DIRS}")

# Find glslc compiler for shader compilation
find_program(GLSLC_EXECUTABLE glslc
    HINTS
        "$ENV{VULKAN_SDK}/Bin"
        "$ENV{VULKAN_SDK}/bin"
        "/usr/bin"
)
if(NOT GLSLC_EXECUTABLE)
    message(FATAL_ERROR "glslc not found. Please install Vulkan SDK.")
endif()
message(STATUS "Found glslc: ${GLSLC_EXECUTABLE}")

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

# Compile GLSL shaders to SPIR-V
set(SHADER_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders")
set(SHADER_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/shaders")
file(MAKE_DIRECTORY ${SHADER_BINARY_DIR})

set(SHADER_SOURCES
    ${SHADER_SOURCE_DIR}/cube.vert
    ${SHADER_SOURCE_DIR}/cube.frag
    ${SHADER_SOURCE_DIR}/grid.vert
    ${SHADER_SOURCE_DIR}/grid.frag
)

set(SHADER_OUTPUTS)
foreach(SHADER ${SHADER_SOURCES})
    get_filename_component(SHADER_NAME ${SHADER} NAME)
    set(SHADER_OUTPUT "${SHADER_BINARY_DIR}/${SHADER_NAME}.spv")
    add_custom_command(
        OUTPUT ${SHADER_OUTPUT}
        COMMAND ${GLSLC_EXECUTABLE} -o ${SHADER_OUTPUT} ${SHADER}
        DEPENDS ${SHADER}
        COMMENT "Compiling shader ${SHADER_NAME}"
    )
    list(APPEND SHADER_OUTPUTS ${SHADER_OUTPUT})
endforeach()

add_custom_target(shaders_vulkan DEPENDS ${SHADER_OUTPUTS})

add_executable(vulkan_test_no_session_target WIN32
    main.cpp
    xr_session.cpp
    xr_session.h
    vulkan_renderer.cpp
    vulkan_renderer.h
    input_state.cpp
    input_state.h
    logging.cpp
    logging.h
)

add_dependencies(vulkan_test_no_session_target shaders_vulkan)

target_include_directories(vulkan_test_no_session_target PRIVATE
    ${OPENXR_INCLUDE_DIR}
    ${Vulkan_INCLUDE_DIRS}
)

target_compile_definitions(vulkan_test_no_session_target PRIVATE
    VK_USE_PLATFORM_WIN32_KHR
)

target_link_libraries(vulkan_test_no_session_target PRIVATE
    OpenXR::openxr_loader
    Vulkan::Vulkan
)

# Pass shader directory to source code
target_compile_definitions(vulkan_test_no_session_target PRIVATE
    SHADER_DIR="${SHADER_BINARY_DIR}"
)

# For development: create a batch file to run with local runtime
file(WRITE ${CMAKE_BINARY_DIR}/run_local_vulkan.bat
    "@echo off\r\n"
    "set XR_RUNTIME_JSON=${CMAKE_SOURCE_DIR}/../../build/openxr_monado-dev.json\r\n"
    "vulkan_test_no_session_target.exe\r\n"
    "pause\r\n"
)

# Copy runtime DLLs to output directory for standalone use
if(TARGET OpenXR::openxr_loader)
    get_target_property(_loader_location OpenXR::openxr_loader IMPORTED_LOCATION)
    if(_loader_location)
        add_custom_command(TARGET vulkan_test_no_session_target POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${_loader_location}"
                $<TARGET_FILE_DIR:vulkan_test_no_session_target>
            COMMENT "Copying OpenXR loader DLL"
        )
    endif()
endif()

# Use CMake 3.21+ TARGET_RUNTIME_DLLS to automatically copy all required runtime DLLs
add_custom_command(TARGET vulkan_test_no_session_target POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        $<TARGET_RUNTIME_DLLS:vulkan_test_no_session_target>
        $<TARGET_FILE_DIR:vulkan_test_no_session_target>
    COMMAND_EXPAND_LISTS
    COMMENT "Copying runtime DLLs"
)

# Copy compiled shaders to output directory
add_custom_command(TARGET vulkan_test_no_session_target POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:vulkan_test_no_session_target>/shaders
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${SHADER_OUTPUTS}
        $<TARGET_FILE_DIR:vulkan_test_no_session_target>/shaders/
    COMMENT "Copying compiled shaders"
)

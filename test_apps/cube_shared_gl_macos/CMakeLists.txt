# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)
project(cube_shared_gl_macos LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OpenXR headers from the monado source tree
set(OPENXR_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/../../src/external/openxr_includes")

# Find OpenXR loader
set(OPENXR_FOUND FALSE)

find_package(OpenXR QUIET CONFIG)
if(OpenXR_FOUND)
    set(OPENXR_FOUND TRUE)
    message(STATUS "Found OpenXR via find_package")
endif()

# Fallback: try Homebrew or system library
if(NOT OPENXR_FOUND)
    find_library(OPENXR_LOADER_LIB NAMES openxr_loader)
    if(OPENXR_LOADER_LIB)
        set(OPENXR_FOUND TRUE)
        add_library(OpenXR::openxr_loader SHARED IMPORTED)
        set_target_properties(OpenXR::openxr_loader PROPERTIES
            IMPORTED_LOCATION "${OPENXR_LOADER_LIB}"
        )
        message(STATUS "Found OpenXR loader: ${OPENXR_LOADER_LIB}")
    endif()
endif()

# Fallback: parent build directory
if(NOT OPENXR_FOUND)
    set(PARENT_BUILD_DIR "${CMAKE_SOURCE_DIR}/../../build")
    if(EXISTS "${PARENT_BUILD_DIR}/src/external/openxr_loader/OpenXRConfig.cmake")
        list(APPEND CMAKE_PREFIX_PATH "${PARENT_BUILD_DIR}/src/external/openxr_loader")
        find_package(OpenXR REQUIRED CONFIG)
        set(OPENXR_FOUND TRUE)
    endif()
endif()

if(NOT OPENXR_FOUND)
    message(FATAL_ERROR "OpenXR not found. Install via: brew install openxr-loader")
endif()

add_executable(cube_shared_gl_macos
    main.mm
    "${CMAKE_SOURCE_DIR}/../common/display3d_view.c"
    "${CMAKE_SOURCE_DIR}/../common/camera3d_view.c"
    "${CMAKE_SOURCE_DIR}/../common/hud_overlay_macos.m"
)

target_compile_definitions(cube_shared_gl_macos PRIVATE
    GL_SILENCE_DEPRECATION
)

target_include_directories(cube_shared_gl_macos PRIVATE
    ${OPENXR_INCLUDE_DIR}
    ${CMAKE_SOURCE_DIR}/../common
)

# Copy texture files next to the built executable
file(GLOB TEXTURE_FILES "${CMAKE_SOURCE_DIR}/../common/textures/*.jpg")
foreach(TEX ${TEXTURE_FILES})
    get_filename_component(TEX_NAME ${TEX} NAME)
    add_custom_command(TARGET cube_shared_gl_macos POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:cube_shared_gl_macos>/textures"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TEX} "$<TARGET_FILE_DIR:cube_shared_gl_macos>/textures/${TEX_NAME}"
    )
endforeach()

target_link_libraries(cube_shared_gl_macos PRIVATE
    OpenXR::openxr_loader
    "-framework OpenGL"
    "-framework Metal"
    "-framework Foundation"
    "-framework Cocoa"
    "-framework QuartzCore"
    "-framework IOSurface"
)

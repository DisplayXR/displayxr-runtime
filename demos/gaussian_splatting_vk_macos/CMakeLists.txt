# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# 3D Gaussian Splatting with OpenXR (Vulkan/MoltenVK, macOS)
# Renders 3DGS scenes on tracked 3D displays via OpenXR and XR_EXT_cocoa_window_binding.

cmake_minimum_required(VERSION 3.21)
project(gaussian_splatting_vk_macos LANGUAGES C CXX OBJCXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Vulkan SDK
find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# OpenXR headers from the monado source tree (includes custom extensions)
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

# 3DGS common library (FetchContent + adapter)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../3dgs_common ${CMAKE_CURRENT_BINARY_DIR}/3dgs_common)

add_executable(gaussian_splatting_vk_macos
    main.mm
    "${CMAKE_SOURCE_DIR}/../../test_apps/common/display3d_view.c"
    "${CMAKE_SOURCE_DIR}/../../test_apps/common/camera3d_view.c"
)

target_include_directories(gaussian_splatting_vk_macos PRIVATE
    ${OPENXR_INCLUDE_DIR}
    "${CMAKE_SOURCE_DIR}/../../test_apps/common"
    "${CMAKE_SOURCE_DIR}/../3dgs_common"
)

target_link_libraries(gaussian_splatting_vk_macos PRIVATE
    gs_renderer
    OpenXR::openxr_loader
    Vulkan::Vulkan
    "-framework Cocoa"
    "-framework QuartzCore"
    "-framework CoreGraphics"
    "-framework CoreText"
    "-framework CoreFoundation"
    "-framework UniformTypeIdentifiers"
)

# Copy texture files to build output directory
file(GLOB TEXTURE_FILES "${CMAKE_SOURCE_DIR}/../../test_apps/common/textures/*.jpg")
add_custom_command(TARGET gaussian_splatting_vk_macos POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:gaussian_splatting_vk_macos>/textures"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different ${TEXTURE_FILES} "$<TARGET_FILE_DIR:gaussian_splatting_vk_macos>/textures/"
    COMMENT "Copying texture files to build output"
)

# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.21)
project(openxr_bridge_host LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find Vulkan SDK
find_package(Vulkan REQUIRED)
message(STATUS "Found Vulkan: ${Vulkan_LIBRARY}")

# Find libwebsockets
find_package(libwebsockets QUIET CONFIG)
if(NOT libwebsockets_FOUND)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LWS IMPORTED_TARGET libwebsockets)
    endif()
endif()

if(NOT libwebsockets_FOUND AND NOT LWS_FOUND)
    message(FATAL_ERROR "libwebsockets not found. Install via: brew install libwebsockets")
endif()

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

add_executable(openxr_bridge_host openxr_bridge_host.cpp)

target_include_directories(openxr_bridge_host PRIVATE ${OPENXR_INCLUDE_DIR})

target_link_libraries(openxr_bridge_host PRIVATE
    OpenXR::openxr_loader
    Vulkan::Vulkan
)

# Link libwebsockets
if(libwebsockets_FOUND)
    target_link_libraries(openxr_bridge_host PRIVATE websockets)
elseif(LWS_FOUND)
    target_link_libraries(openxr_bridge_host PRIVATE PkgConfig::LWS)
endif()

# pthreads
find_package(Threads REQUIRED)
target_link_libraries(openxr_bridge_host PRIVATE Threads::Threads)

# macOS: hidden window for XR_EXT_macos_window_binding (suppresses Monado popup)
if(APPLE)
    enable_language(OBJC OBJCXX)
    target_sources(openxr_bridge_host PRIVATE bridge_window_macos.mm)
    set_source_files_properties(bridge_window_macos.mm PROPERTIES
        COMPILE_FLAGS "-fobjc-arc"
    )
    target_link_libraries(openxr_bridge_host PRIVATE
        "-framework AppKit"
        "-framework QuartzCore"
    )
endif()

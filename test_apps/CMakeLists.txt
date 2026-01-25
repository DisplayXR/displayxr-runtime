# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Parent CMakeLists.txt for test applications
# Build from this directory to build all test apps at once

cmake_minimum_required(VERSION 3.21)
project(test_apps LANGUAGES CXX)

# Only build on Windows
if(NOT WIN32)
    message(STATUS "Test apps are Windows-only, skipping")
    return()
endif()

# Build standalone Vulkan cube test (no OpenXR dependency)
add_subdirectory(vulkan_cube_standalone)

# Build D3D11 cube test (no OpenXR dependency)
add_subdirectory(d3d11_cube_test)

# Build OpenXR session_target tests (require OpenXR loader)
# These can be built standalone or as part of this project
if(OpenXR_FOUND OR DEFINED OpenXR_ROOT)
    add_subdirectory(session_target_test)
    add_subdirectory(session_target_test_vulkan)
else()
    message(STATUS "OpenXR not found, skipping session_target tests")
    message(STATUS "To build them, set OpenXR_ROOT or install OpenXR SDK")
endif()

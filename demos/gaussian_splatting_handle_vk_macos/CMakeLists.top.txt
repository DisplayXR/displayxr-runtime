# Copyright 2025, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Top-level CMakeLists.txt for the displayxr-demo-gaussiansplat standalone
# repo. Orchestrates the shared `common` and `3dgs_common` libraries, then
# dispatches to macos/ or windows/ for the platform-specific executable.
#
# Authored in displayxr-runtime-pvt as CMakeLists.top.txt; the publish
# workflow renames it to CMakeLists.txt when syncing to the public repo.

cmake_minimum_required(VERSION 3.21)
project(displayxr-demo-gaussiansplat LANGUAGES C CXX)

if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Shared libraries (built once, linked by the platform executable).
add_subdirectory(common)
add_subdirectory(3dgs_common)

# Platform-specific executable.
if(APPLE)
    add_subdirectory(macos)
elseif(WIN32)
    add_subdirectory(windows)
else()
    message(FATAL_ERROR "displayxr-demo-gaussiansplat supports macOS and Windows only.")
endif()

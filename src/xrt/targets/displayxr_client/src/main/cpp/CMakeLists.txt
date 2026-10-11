# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: Apache-2.0

# Native build of the DisplayXR client AAR (#1919): the queue-lock Vulkan
# layer and NOTHING else.
#
# The AAR ships libVkLayer_DXR_queue_lock.so under jni/arm64-v8a/ so it lands
# in every consuming app's own lib dir, which Android's Vulkan loader searches
# for libVkLayer*.so in any app, debuggable or not (#1905/#1915). The runtime's
# injection in oxr_vulkan.c then enables it by name at xrCreateVulkanInstanceKHR.
#
# Why a standalone project instead of the repo-root CMakeLists.txt (which the
# runtime APK uses): configuring the root needs Eigen, Python, the FetchContent'd
# displayxr-mcp / displayxr-common (network) and the whole xrt tree, even when
# cmake.targets names one target. The layer is deliberately dependency-free
# (Vulkan headers from the NDK sysroot, liblog, libc), so this project reuses
# the SAME source directory (src/xrt/targets/vk_layer) and the SAME global
# compiler flags (CompilerFlags.cmake, C11, PIC) as the root build. CI compares
# the result against the copy inside the runtime APK built in the same run.

cmake_minimum_required(VERSION 3.22)
project(displayxr_client_native LANGUAGES C)

if(NOT ANDROID)
	message(FATAL_ERROR "displayxr_client native build is Android-only")
endif()

get_filename_component(DXR_REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../../../.." ABSOLUTE)
if(NOT EXISTS "${DXR_REPO_ROOT}/src/xrt/targets/vk_layer/queue_lock_layer.c")
	message(FATAL_ERROR "displayxr_client: repo root not found at ${DXR_REPO_ROOT}")
endif()

# Mirror the root CMakeLists.txt's global settings that reach this target.
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
include("${DXR_REPO_ROOT}/CompilerFlags.cmake")

add_subdirectory("${DXR_REPO_ROOT}/src/xrt/targets/vk_layer" vk_layer)

# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Native build for cube_handle_vk_android. Links the Khronos OpenXR
# loader (sourced from openxr_loader_for_android .aar via prefab) so
# the test app can call xrCreateInstance against our runtime APK.

cmake_minimum_required(VERSION 3.22.1)
project(cube_handle_vk_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

# Pick up the project's SPIR-V.cmake (host glslangValidator → SPIR-V
# header arrays). Relative path: test_apps/cube_handle_vk_android/src/
# main/cpp/CMakeLists.txt → repo-root/cmake/. The host must have
# glslangValidator on PATH (typically from the Vulkan SDK).
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/../../../../../cmake")
include(SPIR-V)

spirv_shaders(
    TRIANGLE_SHADER_HEADERS
    SPIRV_VERSION 1.0
    SOURCES
        shaders/triangle.vert
        shaders/triangle.frag
)

# NativeActivity glue ships with the NDK.
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

# Khronos OpenXR loader, pulled as a prefab module from the
# openxr_loader_for_android .aar declared in build.gradle. Defines the
# OpenXR::openxr_loader IMPORTED target with its headers + libopenxr_loader.so.
find_package(OpenXR REQUIRED CONFIG)

add_library(cube_handle_vk_android SHARED main.cpp ${TRIANGLE_SHADER_HEADERS})
target_include_directories(cube_handle_vk_android PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# -u ANativeActivity_onCreate forces the linker to keep the symbol the
# Android runtime resolves dynamically on Activity launch — without it,
# the shared lib loads but exposes nothing.
set_target_properties(cube_handle_vk_android PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")

target_link_libraries(cube_handle_vk_android
    native_app_glue
    OpenXR::openxr_loader
    vulkan
    android
    log)

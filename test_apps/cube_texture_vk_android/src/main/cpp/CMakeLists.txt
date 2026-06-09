# Copyright 2026, Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Native build for cube_texture_vk_android — Android port of the
# cube_texture_d3d11_win texture-class demo (app provides a shared VkImage,
# the runtime weaves 3D into a canvas sub-rect, the app draws the 2D surround
# and presents on its own swapchain).

cmake_minimum_required(VERSION 3.22.1)
project(cube_texture_vk_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/../../../../../cmake")
include(SPIR-V)

spirv_shaders(
    CUBE_SHADER_HEADERS
    SPIRV_VERSION 1.0
    SOURCES
        shaders/cube.vert
        shaders/cube.frag
        shaders/surround.vert
        shaders/surround.frag
)

add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

find_package(OpenXR REQUIRED CONFIG)

add_library(cube_texture_vk_android SHARED main.cpp ${CUBE_SHADER_HEADERS})
target_include_directories(cube_texture_vk_android PRIVATE ${CMAKE_CURRENT_BINARY_DIR})

# The Khronos AAR ships the base OpenXR headers; the DisplayXR custom extension
# headers (XR_EXT_android_surface_binding.h) live in the repo.
target_include_directories(cube_texture_vk_android PRIVATE
    ${CMAKE_SOURCE_DIR}/../../../../../src/external/openxr_includes)

set_target_properties(cube_texture_vk_android PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")

target_link_libraries(cube_texture_vk_android
    native_app_glue
    OpenXR::openxr_loader
    vulkan
    android
    log)

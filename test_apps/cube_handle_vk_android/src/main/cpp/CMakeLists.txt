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

# NativeActivity glue ships with the NDK.
add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

# Khronos OpenXR loader, pulled as a prefab module from the
# openxr_loader_for_android .aar declared in build.gradle. Defines the
# OpenXR::openxr_loader IMPORTED target with its headers + libopenxr_loader.so.
find_package(OpenXR REQUIRED CONFIG)

add_library(cube_handle_vk_android SHARED main.cpp)

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

# Copyright 2026, The DisplayXR Project and its contributors
# SPDX-License-Identifier: BSL-1.0

cmake_minimum_required(VERSION 3.22.1)
project(workspace_controller_android C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

add_library(native_app_glue STATIC
    ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC
    ${ANDROID_NDK}/sources/android/native_app_glue)

find_package(OpenXR REQUIRED CONFIG)

add_library(workspace_controller_android SHARED main.cpp)

# DisplayXR's in-tree OpenXR headers (XR_DXR_spatial_workspace, XR_DXR_display_info),
# ahead of the loader AAR's stock headers. src/main/cpp -> repo root is five up.
target_include_directories(workspace_controller_android BEFORE PRIVATE
    "${CMAKE_SOURCE_DIR}/../../../../../../src/external/openxr_includes")

set_target_properties(workspace_controller_android PROPERTIES
    LINK_FLAGS "-u ANativeActivity_onCreate")

target_link_libraries(workspace_controller_android
    native_app_glue
    OpenXR::openxr_loader
    android
    log)

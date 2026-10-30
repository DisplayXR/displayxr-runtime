# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
cmake_minimum_required(VERSION 3.22.1)
project(camera_probe_android C CXX)
set(CMAKE_CXX_STANDARD 17)

# libopenxr_loader.so + openxr headers from the openxr_loader_for_android AAR (prefab).
find_package(OpenXR REQUIRED CONFIG)

add_library(camera_probe_android SHARED camera_probe.cpp)
# The DisplayXR extension headers (XR_DXR_stereo_camera.h) win over the AAR's.
target_include_directories(camera_probe_android BEFORE PRIVATE
    "${CMAKE_SOURCE_DIR}/../../../../../../src/external/openxr_includes")
target_link_libraries(camera_probe_android OpenXR::openxr_loader log android)

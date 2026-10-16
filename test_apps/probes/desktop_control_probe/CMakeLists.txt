# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# XR_DXR_desktop_control probe (ADR-056) - a headless (XR_MND_headless) OpenXR
# CLI exposing every desktop-control call, for bring-up and E2E runs. No
# graphics API, no window, no displayxr manifest: it builds on Windows, macOS
# and desktop Linux alike (the `_probe` suffix gate in ../../apps.cmake).
#
# Standalone:  cmake -S test_apps/probes/desktop_control_probe -B <dir> \
#                    -DCMAKE_PREFIX_PATH=<openxr install>   (or -DOpenXR_ROOT=...)

cmake_minimum_required(VERSION 3.21)
project(desktop_control_probe LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# OpenXR headers (incl. the DisplayXR extension headers) from the source tree.
set(OPENXR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../../src/external/openxr_includes")

if(NOT TARGET OpenXR::openxr_loader)
	find_package(OpenXR QUIET CONFIG)
endif()
if(NOT TARGET OpenXR::openxr_loader)
	find_library(OPENXR_LOADER_LIB NAMES openxr_loader)
	if(OPENXR_LOADER_LIB)
		add_library(OpenXR::openxr_loader UNKNOWN IMPORTED)
		set_target_properties(OpenXR::openxr_loader PROPERTIES IMPORTED_LOCATION "${OPENXR_LOADER_LIB}")
	endif()
endif()
if(NOT TARGET OpenXR::openxr_loader)
	message(WARNING "desktop_control_probe: OpenXR loader not found - skipped")
	return()
endif()

add_executable(desktop_control_probe main.cpp)
target_include_directories(desktop_control_probe PRIVATE ${OPENXR_INCLUDE_DIR})
target_link_libraries(desktop_control_probe PRIVATE OpenXR::openxr_loader)

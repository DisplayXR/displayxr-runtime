# Copyright 2025, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# Platform-aware aggregator for the DisplayXR test apps.
#
# Build every test app for the current platform at once:
#   cmake -S test_apps -B test_apps/build -G Ninja -DOpenXR_ROOT=<sdk>
#   cmake --build test_apps/build
# All app executables land in test_apps/build/bin/.
#
# The app list is DISCOVERED, not hardcoded: this file globs every
# <class>/<app>/CMakeLists.txt and gates each on its dir-name suffix
# (_win→WIN32, _macos→APPLE, _android→ANDROID, _linux→Linux) via
# dxr_testapp_platform_ok() from apps.cmake. That is what makes the "app on disk
# that no build list references" failure mode (#692 item 1) structurally
# impossible — there is no list to drift out of sync. Layout (#692 item 3):
#
#   test_apps/
#     apps.cmake                      # classification + gating + run-env rule
#     common/                         # shared scaffolding (added once, below)
#     handle/   cube_handle_*  cube_zones_*            # zones = FEATURE modifier
#     hosted/   cube_hosted_*
#     texture/  cube_zones_texture_*
#     legacy/   cube_hosted_legacy_*
#     probes/   windowspace_* workspace_* weave_*
#
# Leaf dir names are unchanged from the flat layout, so run-script names,
# _package\run_*.bat muscle memory, and doc links all survive.

cmake_minimum_required(VERSION 3.21)
project(displayxr_test_apps LANGUAGES C CXX)

# Each app find_package(OpenXR)s for itself; the aggregate just needs a hint that
# the SDK is locatable. Windows/CI pass -DOpenXR_ROOT; macOS passes
# -DCMAKE_PREFIX_PATH at the OpenXR install. Accept either (or an already-found
# package) so the aggregate works uniformly on both build scripts.
if(NOT (OpenXR_FOUND OR DEFINED OpenXR_ROOT OR DEFINED CMAKE_PREFIX_PATH))
    message(STATUS "OpenXR not found — pass -DOpenXR_ROOT or -DCMAKE_PREFIX_PATH. Skipping test apps.")
    return()
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/apps.cmake)

# Shared scaffolding — added ONCE at the aggregate root. Each app also does its
# own add_subdirectory(<...>/common ...) for the standalone per-app build; under
# this aggregate that re-entry is a guarded no-op (common/CMakeLists.txt).
add_subdirectory(common)

# Uniform output dir so every app exe (and the loader the build script drops
# beside them) share one location the run-scripts can point at.
set(_dxr_testapps_bin "${CMAKE_BINARY_DIR}/bin")

file(GLOB _dxr_app_cmakelists RELATIVE "${CMAKE_CURRENT_SOURCE_DIR}"
     "${CMAKE_CURRENT_SOURCE_DIR}/*/*/CMakeLists.txt")

set(_dxr_built "")
set(_dxr_skipped "")
foreach(_rel ${_dxr_app_cmakelists})
    get_filename_component(_appdir "${_rel}" DIRECTORY)   # <class>/<app>
    get_filename_component(_app "${_appdir}" NAME)        # <app>
    dxr_testapp_platform_ok("${_app}" _ok)
    if(NOT _ok)
        list(APPEND _dxr_skipped "${_app}")
        continue()
    endif()
    add_subdirectory("${_appdir}")
    if(TARGET ${_app})
        set_target_properties(${_app} PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${_dxr_testapps_bin}")
        # Each app embeds its manifest with a RELATIVE /MANIFESTINPUT: (the
        # spaces-in-path mt.exe workaround), which the linker resolves against
        # its cwd — the app's own binary dir in a STANDALONE build, but the
        # aggregate BUILD ROOT here. Re-anchor it to the app's binary dir
        # (build/<class>/<app>, i.e. _appdir relative to the build root) so the
        # copied manifest is found. Generic — no per-app CMakeLists change, and
        # standalone builds never run this loop so they are unaffected.
        get_target_property(_lf ${_app} LINK_FLAGS)
        if(_lf AND _lf MATCHES "/MANIFESTINPUT:")
            string(REPLACE "/MANIFESTINPUT:" "/MANIFESTINPUT:${_appdir}/" _lf "${_lf}")
            set_target_properties(${_app} PROPERTIES LINK_FLAGS "${_lf}")
        endif()
        list(APPEND _dxr_built "${_app}")
    endif()
endforeach()

list(LENGTH _dxr_built _dxr_n)
message(STATUS "DisplayXR test apps: ${_dxr_n} app(s) for this platform → ${_dxr_testapps_bin}")
foreach(_a ${_dxr_built})
    message(STATUS "  + ${_a}")
endforeach()

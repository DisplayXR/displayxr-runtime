# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Cross-compile toolchain for compile-checking Windows code from macOS / Linux
# using MinGW-w64. **This is a compile-only sanity check** — the canonical
# Windows binary still comes from the MSVC build in CI. Mirrors the pattern
# DisplayXR/displayxr-unity uses for its native plugin (native~/toolchain-mingw.cmake).
#
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=cmake/toolchain-mingw-w64.cmake -B build-mingw -G Ninja
#   cmake --build build-mingw --target aux_util mcp_adapter
#
# The wrapper script scripts/build-mingw-check.sh selects a curated target
# subset; many MSVC-only paths (WIL, vcpkg-provided libs) will not build.

set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(CMAKE_C_COMPILER x86_64-w64-mingw32-gcc)
set(CMAKE_CXX_COMPILER x86_64-w64-mingw32-g++)
set(CMAKE_RC_COMPILER x86_64-w64-mingw32-windres)

set(CMAKE_FIND_ROOT_PATH /opt/homebrew/x86_64-w64-mingw32 /usr/x86_64-w64-mingw32)
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Do NOT restrict PACKAGE discovery to the find-root (i.e. no
# CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY): the build does
# `find_package(Eigen3 REQUIRED NO_MODULE)`, and Eigen is a header-only host
# package (e.g. Homebrew) we legitimately want to find for the cross-compile.
# Restricting PACKAGE mode would make that REQUIRED find fail.
#
# But that openness lets CONFIG-mode `find_package(cJSON)` (via the project's
# cmake/FindcJSON.cmake) pick up a *host* cJSON config — e.g. Homebrew's
# /opt/homebrew/.../lib/cmake/cJSON, which ships only a Release-config imported
# target. A Debug cross-build then dies at generate with "IMPORTED_IMPLIB not
# set for imported target cjson configuration Debug". The cross-check never
# wants system cJSON anyway (it builds the bundled / displayxr_mcp-vendored
# cJSON, and build-mingw-check.sh forces XRT_HAVE_SYSTEM_CJSON=OFF), so disable
# cJSON package discovery outright. find_package(cJSON MODULE) is not REQUIRED,
# so this cleanly yields cJSON_FOUND=FALSE → bundled cJSON path.
set(CMAKE_DISABLE_FIND_PACKAGE_cJSON ON)

# Mark MinGW-w64 cross builds so CMakeLists can guard out incompatible
# pieces (WIL, vcpkg-only deps) without affecting native MSVC builds.
set(XRT_CROSS_MINGW_CHECK ON CACHE BOOL "Cross-compile compile-check via MinGW-w64" FORCE)

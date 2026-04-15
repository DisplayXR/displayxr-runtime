#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Compile-only Windows check from macOS / Linux via MinGW-w64. Catches
# Win32-API typos, missing #ifdef guards, wrong-platform symbols (e.g.
# CLOCK_REALTIME) before pushing to CI. Not a substitute for the MSVC
# CI build — many MSVC-only headers (WIL, vcpkg deps) won't compile
# under MinGW. Builds a curated subset of targets known to be portable.
#
# Usage:  scripts/build-mingw-check.sh [target...]
# Default targets: aux_util mcp_adapter

set -u
set -o pipefail

cd "$(dirname "$0")/.."
ROOT="$(pwd)"
BUILD="$ROOT/build-mingw"

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
	echo "FAIL: x86_64-w64-mingw32-gcc not found. Install with:" >&2
	echo "      brew install mingw-w64" >&2
	exit 1
fi

TARGETS=("$@")
if [[ ${#TARGETS[@]} -eq 0 ]]; then
	TARGETS=(aux_util mcp_adapter)
fi

echo "=== MinGW-w64 compile check ==="
echo "    targets: ${TARGETS[*]}"
echo ""

if [[ ! -f "$BUILD/CMakeCache.txt" ]]; then
	# Minimal config — we only need the targets to *compile*, not link.
	# OpenXR / compositors / IPC require Vulkan + vcpkg deps that aren't
	# available under MinGW; turn them off and let CMake skip those
	# subdirectories. Anything we left on (aux_util, mcp_adapter, comp_d3d11)
	# either has no external deps or is what we want to verify.
	cmake -B "$BUILD" -G Ninja \
	    -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/toolchain-mingw-w64.cmake" \
	    -DCMAKE_BUILD_TYPE=Debug \
	    -DXRT_FEATURE_OPENXR=OFF \
	    -DXRT_FEATURE_SERVICE=OFF \
	    -DXRT_FEATURE_TRACING=OFF \
	    -DXRT_FEATURE_DEBUG_GUI=OFF \
	    -DBUILD_TESTING=OFF \
	    -DXRT_HAVE_LIBUSB=OFF \
	    -DXRT_HAVE_OPENGL=OFF \
	    -DXRT_HAVE_VULKAN=OFF \
	    -DXRT_HAVE_LEIA_SR=OFF \
	    -DXRT_HAVE_SDL2=OFF \
	    -DXRT_MODULE_AUX_VK=OFF \
	    -DXRT_MODULE_COMPOSITOR=OFF \
	    -DXRT_MODULE_IPC=OFF \
	    -DXRT_MODULE_OPENXR_STATE_TRACKER=OFF \
	    -DXRT_HAVE_SYSTEM_CJSON=OFF \
	    "$ROOT" || {
		echo "FAIL: cmake configure failed" >&2
		exit 1
	}
fi

cmake --build "$BUILD" --target "${TARGETS[@]}" 2>&1 | tee "$BUILD/build.log"
RC=${PIPESTATUS[0]}

echo ""
if [[ $RC -eq 0 ]]; then
	echo "=== MinGW compile check PASSED for: ${TARGETS[*]} ==="
	echo "    NOTE: This is compile-only validation. CI MSVC build is canonical."
else
	echo "=== MinGW compile check FAILED ==="
	echo "    See $BUILD/build.log for details."
	exit $RC
fi

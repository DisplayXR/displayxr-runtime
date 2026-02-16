#!/bin/bash
# Build the SRMonado runtime and sim_cube_openxr test app on macOS
#
# Prerequisites: brew install cmake ninja eigen vulkan-sdk
#
# Usage:
#   ./scripts/build_macos.sh
#
# Then run:
#   XR_RUNTIME_JSON=./build/openxr_monado-dev.json \
#   DYLD_LIBRARY_PATH=/tmp/openxr-install/lib \
#   SIM_DISPLAY_ENABLE=1 SIM_DISPLAY_OUTPUT=anaglyph \
#   ./test_apps/sim_cube_openxr/build/sim_cube_openxr

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build"
OPENXR_DIR="/tmp/openxr-install"
OPENXR_VERSION="1.1.43"

# Step 1: Build the runtime
echo "=== Building SRMonado runtime ==="
cmake -B "$BUILD_DIR" -S "$ROOT" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DXRT_FEATURE_SERVICE=OFF
cmake --build "$BUILD_DIR"

# Step 2: Build OpenXR loader (if not already cached)
if [ ! -f "$OPENXR_DIR/lib/libopenxr_loader.dylib" ]; then
  echo "=== Building OpenXR loader ==="
  rm -rf /tmp/openxr-sdk
  git clone --depth 1 --branch "release-$OPENXR_VERSION" \
    https://github.com/KhronosGroup/OpenXR-SDK-Source.git /tmp/openxr-sdk
  cmake -B /tmp/openxr-sdk/build -S /tmp/openxr-sdk -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$OPENXR_DIR" \
    -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF
  cmake --build /tmp/openxr-sdk/build
  cmake --install /tmp/openxr-sdk/build
else
  echo "=== OpenXR loader already built at $OPENXR_DIR ==="
fi

# Step 3: Build test app
echo "=== Building sim_cube_openxr ==="
cmake -B "$ROOT/test_apps/sim_cube_openxr/build" \
  -S "$ROOT/test_apps/sim_cube_openxr" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/test_apps/sim_cube_openxr/build"

echo ""
echo "=== Build complete! ==="
echo "Run with:"
echo "  XR_RUNTIME_JSON=$BUILD_DIR/openxr_monado-dev.json \\"
echo "  DYLD_LIBRARY_PATH=$OPENXR_DIR/lib \\"
echo "  SIM_DISPLAY_ENABLE=1 SIM_DISPLAY_OUTPUT=anaglyph \\"
echo "  $ROOT/test_apps/sim_cube_openxr/build/sim_cube_openxr"

#!/bin/bash
# Build the SRMonado runtime and sim_cube_openxr test app on macOS
#
# Prerequisites: brew install cmake ninja eigen vulkan-sdk
#
# Usage:
#   ./scripts/build_macos.sh             # In-process mode (default)
#   ./scripts/build_macos.sh --service   # IPC service mode (monado-service + client)
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

# Parse arguments
SERVICE_MODE=OFF
for arg in "$@"; do
  case "$arg" in
    --service) SERVICE_MODE=ON ;;
  esac
done

# Detect macOS SDK (CMake may pick a stale sysroot otherwise)
MACOS_SDK="$(xcrun --show-sdk-path 2>/dev/null)"

# Step 1: Build the runtime
echo "=== Building SRMonado runtime (SERVICE=$SERVICE_MODE) ==="
cmake -B "$BUILD_DIR" -S "$ROOT" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DXRT_FEATURE_SERVICE=$SERVICE_MODE \
  -DXRT_BUILD_DRIVER_QWERTY=ON \
  -DXRT_FEATURE_DEBUG_GUI=OFF \
  -DXRT_FEATURE_WINDOW_PEEK=OFF \
  -DXRT_HAVE_OPENCV=OFF \
  -DXRT_BUILD_DRIVER_EUROC=OFF \
  ${MACOS_SDK:+-DCMAKE_OSX_SYSROOT="$MACOS_SDK"}
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

# Step 4: Package artifacts (mirrors CI workflow)
echo "=== Packaging artifacts ==="
PKG_DIR="$ROOT/_package/SRMonado-macOS"
rm -rf "$ROOT/_package" 2>/dev/null || true
mkdir -p "$PKG_DIR/lib"
mkdir -p "$PKG_DIR/share/vulkan/icd.d"
mkdir -p "$PKG_DIR/bin"

# Find and copy runtime
RUNTIME_LIB=$(find "$BUILD_DIR/src/xrt/targets/openxr" -name "libopenxr_monado*" -type f | head -1)
RUNTIME_BASENAME=$(basename "$RUNTIME_LIB")
cp "$RUNTIME_LIB" "$PKG_DIR/lib/"

# Copy test app binary
cp "$ROOT/test_apps/sim_cube_openxr/build/sim_cube_openxr" "$PKG_DIR/bin/"

# Copy OpenXR loader
cp "$OPENXR_DIR"/lib/libopenxr_loader*.dylib "$PKG_DIR/lib/"

# Bundle Vulkan loader and MoltenVK from Homebrew
BREW_PREFIX="$(brew --prefix)"
for lib in libvulkan.1.dylib libMoltenVK.dylib; do
  if [ -f "$BREW_PREFIX/lib/$lib" ]; then
    cp -L "$BREW_PREFIX/lib/$lib" "$PKG_DIR/lib/"
  else
    FOUND=$(find "$BREW_PREFIX/Cellar" -name "$lib" 2>/dev/null | head -1)
    if [ -n "$FOUND" ]; then
      cp -L "$FOUND" "$PKG_DIR/lib/"
    else
      echo "Warning: $lib not found, skipping"
    fi
  fi
done

# Fix rpaths
install_name_tool -add_rpath @loader_path "$PKG_DIR/lib/$RUNTIME_BASENAME" 2>/dev/null || true
install_name_tool -add_rpath @executable_path/../lib "$PKG_DIR/bin/sim_cube_openxr" 2>/dev/null || true
install_name_tool -add_rpath @loader_path "$PKG_DIR"/lib/libopenxr_loader*.dylib 2>/dev/null || true

# Create MoltenVK ICD manifest
cat > "$PKG_DIR/share/vulkan/icd.d/MoltenVK_icd.json" <<EOF
{
    "file_format_version": "1.0.0",
    "ICD": {
        "library_path": "../../../lib/libMoltenVK.dylib",
        "api_version": "1.2.0",
        "is_portability_driver": true
    }
}
EOF

# Create runtime manifest
cat > "$PKG_DIR/openxr_monado.json" <<EOF
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "Monado (SRMonado macOS)",
        "library_path": "lib/$RUNTIME_BASENAME"
    }
}
EOF

# Create run script
cat > "$PKG_DIR/run_sim_cube.sh" <<'SCRIPT'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_monado.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export VK_ICD_FILENAMES="$DIR/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_DRIVER_FILES="$DIR/share/vulkan/icd.d/MoltenVK_icd.json"
export SIM_DISPLAY_ENABLE=1
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
echo "Starting sim_cube_openxr with $SIM_DISPLAY_OUTPUT output..."
exec "$DIR/bin/sim_cube_openxr" "$@"
SCRIPT
chmod +x "$PKG_DIR/run_sim_cube.sh"

# Step 5: Build .app bundle and .pkg installer
echo "=== Building .app bundle ==="
"$ROOT/installer/macos/create_app_bundle.sh" "$PKG_DIR" "$ROOT/_package/SimCubeOpenXR.app"

echo "=== Building .pkg installer ==="
"$ROOT/installer/macos/build_installer.sh" "$PKG_DIR" "$ROOT/_package/SRMonado-Installer.pkg"

echo ""
echo "=== Build complete! ==="
echo ""
echo "Artifacts in _package/:"
ls -lh "$ROOT/_package/" 2>/dev/null
echo ""
echo "Run the .app bundle:"
echo "  open $ROOT/_package/SimCubeOpenXR.app"
echo ""
echo "Or run directly:"
echo "  $PKG_DIR/run_sim_cube.sh"
echo ""
echo "Or run manually:"
echo "  XR_RUNTIME_JSON=$BUILD_DIR/openxr_monado-dev.json \\"
echo "  DYLD_LIBRARY_PATH=$OPENXR_DIR/lib \\"
echo "  VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \\"
echo "  VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \\"
echo "  SIM_DISPLAY_ENABLE=1 SIM_DISPLAY_OUTPUT=anaglyph \\"
echo "  $ROOT/test_apps/sim_cube_openxr/build/sim_cube_openxr"

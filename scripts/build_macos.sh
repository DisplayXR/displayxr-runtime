#!/bin/bash
# Build the SRMonado runtime and test apps on macOS
#
# Prerequisites: brew install cmake ninja eigen vulkan-sdk
#
# Usage:
#   ./scripts/build_macos.sh             # In-process mode (default)
#   ./scripts/build_macos.sh --service   # IPC service mode (monado-service + client)
#   ./scripts/build_macos.sh --hybrid    # Hybrid mode (in-process + IPC auto-switching)
#
# Then run:
#   XR_RUNTIME_JSON=./build/openxr_monado-dev.json \
#   DYLD_LIBRARY_PATH=/tmp/openxr-install/lib \
#   SIM_DISPLAY_ENABLE=1 SIM_DISPLAY_OUTPUT=anaglyph \
#   ./test_apps/cube_vk_macos/build/cube_vk_macos

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build"
OPENXR_DIR="/tmp/openxr-install"
OPENXR_VERSION="1.1.43"

# Parse arguments
SERVICE_MODE=OFF
HYBRID_MODE=OFF
for arg in "$@"; do
  case "$arg" in
    --service) SERVICE_MODE=ON ;;
    --hybrid) SERVICE_MODE=ON; HYBRID_MODE=ON ;;
  esac
done

# Detect macOS SDK (CMake may pick a stale sysroot otherwise)
MACOS_SDK="$(xcrun --show-sdk-path 2>/dev/null)"

# Step 1: Build the runtime
echo "=== Building SRMonado runtime (SERVICE=$SERVICE_MODE, HYBRID=$HYBRID_MODE) ==="
cmake -B "$BUILD_DIR" -S "$ROOT" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DXRT_FEATURE_SERVICE=$SERVICE_MODE \
  -DXRT_FEATURE_HYBRID_MODE=$HYBRID_MODE \
  -DXRT_BUILD_DRIVER_QWERTY=ON \
  -DXRT_FEATURE_DEBUG_GUI=OFF \
  -DXRT_FEATURE_WINDOW_PEEK=OFF \
  -DXRT_HAVE_SDL2=OFF \
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
    -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
    -DCMAKE_MAP_IMPORTED_CONFIG_RELEASE="Release;None;"
  cmake --build /tmp/openxr-sdk/build
  cmake --install /tmp/openxr-sdk/build
else
  echo "=== OpenXR loader already built at $OPENXR_DIR ==="
fi

# Step 3: Build test app
echo "=== Building cube_vk_macos ==="
cmake -B "$ROOT/test_apps/cube_vk_macos/build" \
  -S "$ROOT/test_apps/cube_vk_macos" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/test_apps/cube_vk_macos/build"

# Step 3b: Build external window test app
echo "=== Building cube_ext_vk_macos ==="
cmake -B "$ROOT/test_apps/cube_ext_vk_macos/build" \
  -S "$ROOT/test_apps/cube_ext_vk_macos" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/test_apps/cube_ext_vk_macos/build"

# Step 3c: Build Metal cube test app
echo "=== Building cube_metal_macos ==="
cmake -B "$ROOT/test_apps/cube_metal_macos/build" \
  -S "$ROOT/test_apps/cube_metal_macos" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/test_apps/cube_metal_macos/build"

# Step 3e: Build Metal external window cube test app
echo "=== Building cube_ext_metal_macos ==="
cmake -B "$ROOT/test_apps/cube_ext_metal_macos/build" \
  -S "$ROOT/test_apps/cube_ext_metal_macos" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/test_apps/cube_ext_metal_macos/build"

# Step 3f: Build 3DGS demo app
echo "=== Building gaussian_splatting_vk_macos ==="
cmake -B "$ROOT/demos/gaussian_splatting_vk_macos/build" \
  -S "$ROOT/demos/gaussian_splatting_vk_macos" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/demos/gaussian_splatting_vk_macos/build"

# Step 3d: Build WebXR bridge host
echo "=== Building openxr_bridge_host ==="
cmake -B "$ROOT/webxr-bridge/native-host/build" \
  -S "$ROOT/webxr-bridge/native-host" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/webxr-bridge/native-host/build"

# Step 4: Package artifacts (mirrors CI workflow)
echo "=== Packaging artifacts ==="
PKG_DIR="$ROOT/_package/SRMonado-macOS"
# Clean managed directories only (preserve user-added files like run_bridge_host.sh)
rm -rf "$PKG_DIR/lib" "$PKG_DIR/bin" "$PKG_DIR/share" 2>/dev/null || true
rm -f "$PKG_DIR/openxr_monado.json" "$PKG_DIR/run_cube_vk.sh" "$PKG_DIR/run_cube_ext_vk.sh" "$PKG_DIR/run_cube_metal.sh" "$PKG_DIR/run_cube_ext_metal.sh" "$PKG_DIR/run_cube_metal_ext.sh" "$PKG_DIR/run_gaussian_splatting.sh" "$PKG_DIR/run_sim_cube.sh" "$PKG_DIR/run_sim_cube_ext.sh" "$PKG_DIR/run_sim_3dgs_ext.sh" 2>/dev/null || true
mkdir -p "$PKG_DIR/lib"
mkdir -p "$PKG_DIR/share/vulkan/icd.d"
mkdir -p "$PKG_DIR/bin"

# Find and copy runtime
RUNTIME_LIB=$(find "$BUILD_DIR/src/xrt/targets/openxr" -name "libopenxr_monado*" -type f | head -1)
RUNTIME_BASENAME=$(basename "$RUNTIME_LIB")
cp "$RUNTIME_LIB" "$PKG_DIR/lib/"

# Copy test app binaries
cp "$ROOT/test_apps/cube_vk_macos/build/cube_vk_macos" "$PKG_DIR/bin/"
cp "$ROOT/test_apps/cube_ext_vk_macos/build/cube_ext_vk_macos" "$PKG_DIR/bin/"
cp "$ROOT/test_apps/cube_metal_macos/build/cube_metal_macos" "$PKG_DIR/bin/" 2>/dev/null || true
cp "$ROOT/test_apps/cube_ext_metal_macos/build/cube_ext_metal_macos" "$PKG_DIR/bin/" 2>/dev/null || true
cp "$ROOT/demos/gaussian_splatting_vk_macos/build/gaussian_splatting_vk_macos" "$PKG_DIR/bin/" 2>/dev/null || true

# Copy texture files for ext app
mkdir -p "$PKG_DIR/bin/textures"
cp "$ROOT/test_apps/common/textures/"*.jpg "$PKG_DIR/bin/textures/" 2>/dev/null || true

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
install_name_tool -add_rpath @executable_path/../lib "$PKG_DIR/bin/cube_vk_macos" 2>/dev/null || true
install_name_tool -add_rpath @executable_path/../lib "$PKG_DIR/bin/cube_ext_vk_macos" 2>/dev/null || true
install_name_tool -add_rpath @executable_path/../lib "$PKG_DIR/bin/cube_metal_macos" 2>/dev/null || true
install_name_tool -add_rpath @executable_path/../lib "$PKG_DIR/bin/cube_ext_metal_macos" 2>/dev/null || true
install_name_tool -add_rpath @executable_path/../lib "$PKG_DIR/bin/gaussian_splatting_vk_macos" 2>/dev/null || true
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
cat > "$PKG_DIR/run_cube_vk.sh" <<'SCRIPT'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_monado.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export VK_ICD_FILENAMES="$DIR/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_DRIVER_FILES="$DIR/share/vulkan/icd.d/MoltenVK_icd.json"
export SIM_DISPLAY_ENABLE=1
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
echo "Starting cube_vk_macos with $SIM_DISPLAY_OUTPUT output..."
exec "$DIR/bin/cube_vk_macos" "$@"
SCRIPT
chmod +x "$PKG_DIR/run_cube_vk.sh"

# Create run script for external window test app
cat > "$PKG_DIR/run_cube_ext_vk.sh" <<'SCRIPT'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_monado.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export VK_ICD_FILENAMES="$DIR/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_DRIVER_FILES="$DIR/share/vulkan/icd.d/MoltenVK_icd.json"
export SIM_DISPLAY_ENABLE=1
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
echo "Starting cube_ext_vk_macos (external window) with $SIM_DISPLAY_OUTPUT output..."
exec "$DIR/bin/cube_ext_vk_macos" "$@"
SCRIPT
chmod +x "$PKG_DIR/run_cube_ext_vk.sh"

# Create run script for Metal cube test app (simpler — no Vulkan env vars needed)
cat > "$PKG_DIR/run_cube_metal.sh" <<'SCRIPT'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_monado.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export SIM_DISPLAY_ENABLE=1
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
echo "Starting cube_metal_macos (Metal, no Vulkan) with $SIM_DISPLAY_OUTPUT output..."
exec "$DIR/bin/cube_metal_macos" "$@"
SCRIPT
chmod +x "$PKG_DIR/run_cube_metal.sh"

# Create run script for Metal external window cube test app (no Vulkan env vars needed)
cat > "$PKG_DIR/run_cube_ext_metal.sh" <<'SCRIPT'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_monado.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export SIM_DISPLAY_ENABLE=1
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
echo "Starting cube_ext_metal_macos (Metal, external window) with $SIM_DISPLAY_OUTPUT output..."
exec "$DIR/bin/cube_ext_metal_macos" "$@"
SCRIPT
chmod +x "$PKG_DIR/run_cube_ext_metal.sh"

# Create run script for 3DGS demo app
cat > "$PKG_DIR/run_gaussian_splatting.sh" <<'SCRIPT'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_monado.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export VK_ICD_FILENAMES="$DIR/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_DRIVER_FILES="$DIR/share/vulkan/icd.d/MoltenVK_icd.json"
export SIM_DISPLAY_ENABLE=1
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
echo "Starting gaussian_splatting_vk_macos (3D Gaussian Splatting) with $SIM_DISPLAY_OUTPUT output..."
exec "$DIR/bin/gaussian_splatting_vk_macos" "$@"
SCRIPT
chmod +x "$PKG_DIR/run_gaussian_splatting.sh"

# Step 5: Build .app bundles and .pkg installer (commented out for dev -- uncomment when ready)
# NOTE: if _package has root-owned files from a previous sudo run, clean up first:
#   sudo rm -rf /Users/david.fattal/Documents/GitHub/CNSDK-OpenXR/_package
# echo "=== Building .app bundles ==="
# "$ROOT/installer/macos/create_app_bundle.sh" "$PKG_DIR" "$ROOT/_package/CubeVKMacOS.app" cube_vk_macos
# "$ROOT/installer/macos/create_app_bundle.sh" "$PKG_DIR" "$ROOT/_package/CubeExtVKMacOS.app" cube_ext_vk_macos
#
# echo "=== Building .pkg installer ==="
# "$ROOT/installer/macos/build_installer.sh" "$PKG_DIR" "$ROOT/_package/SRMonado-Installer.pkg"

echo ""
echo "=== Build complete! ==="
echo ""
echo "Artifacts in _package/:"
ls -lh "$ROOT/_package/" 2>/dev/null
echo ""
echo "Run directly:"
echo "  $PKG_DIR/run_cube_vk.sh"
echo "  $PKG_DIR/run_cube_ext_vk.sh"
echo "  $PKG_DIR/run_cube_metal.sh"
echo "  $PKG_DIR/run_cube_ext_metal.sh"
echo "  $PKG_DIR/run_gaussian_splatting.sh"
echo ""
echo "Or run manually:"
echo "  XR_RUNTIME_JSON=$BUILD_DIR/openxr_monado-dev.json \\"
echo "  DYLD_LIBRARY_PATH=$OPENXR_DIR/lib \\"
echo "  VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \\"
echo "  VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \\"
echo "  SIM_DISPLAY_ENABLE=1 SIM_DISPLAY_OUTPUT=anaglyph \\"
echo "  $ROOT/test_apps/cube_vk_macos/build/cube_vk_macos"

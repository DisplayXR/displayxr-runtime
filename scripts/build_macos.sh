#!/bin/bash
# Build the DisplayXR runtime and test apps on macOS
#
# Prerequisites: brew install cmake ninja eigen vulkan-sdk
#
# Usage:
#   ./scripts/build_macos.sh             # In-process mode (default)
#   ./scripts/build_macos.sh --service   # IPC service mode (displayxr-service + client)
#   ./scripts/build_macos.sh --hybrid    # Hybrid mode (in-process + IPC auto-switching)
#   ./scripts/build_macos.sh --installer # Also build .pkg installer
#
# Then run:
#   XR_RUNTIME_JSON=./build/openxr_displayxr-dev.json \
#   DYLD_LIBRARY_PATH=/tmp/openxr-install/lib \
#   SIM_DISPLAY_OUTPUT=anaglyph \
#   ./test_apps/cube_handle_vk_macos/build/cube_handle_vk_macos

set -e

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build"
OPENXR_DIR="/tmp/openxr-install"
OPENXR_VERSION="1.1.43"

# Parse arguments
SERVICE_MODE=OFF
HYBRID_MODE=OFF
BUILD_INSTALLER=OFF
for arg in "$@"; do
  case "$arg" in
    --service) SERVICE_MODE=ON ;;
    --hybrid) SERVICE_MODE=ON; HYBRID_MODE=ON ;;
    --installer) BUILD_INSTALLER=ON ;;
  esac
done

# Detect macOS SDK (CMake may pick a stale sysroot otherwise)
MACOS_SDK="$(xcrun --show-sdk-path 2>/dev/null)"

# Step 1: Build the runtime
echo "=== Building DisplayXR runtime (SERVICE=$SERVICE_MODE, HYBRID=$HYBRID_MODE) ==="
cmake -B "$BUILD_DIR" -S "$ROOT" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DXRT_FEATURE_SERVICE=$SERVICE_MODE \
  -DXRT_FEATURE_HYBRID_MODE=$HYBRID_MODE \
  -DXRT_BUILD_DRIVER_QWERTY=ON \
  -DXRT_FEATURE_DEBUG_GUI=OFF \
  -DXRT_FEATURE_WINDOW_PEEK=OFF \
  -DXRT_HAVE_SDL2=OFF \
  -DXRT_HAVE_OPENCV=OFF \
  -DXRT_HAVE_LIBUSB=OFF \
  -DXRT_BUILD_DRIVER_EUROC=OFF \
  ${MACOS_SDK:+-DCMAKE_OSX_SYSROOT="$MACOS_SDK"}
cmake --build "$BUILD_DIR"

# Step 2: Build OpenXR loader (if not already cached)
#
# BUILD_WITH_SYSTEM_JSONCPP=OFF forces the OpenXR loader to compile its
# vendored jsoncpp sources (src/external/jsoncpp/) directly into
# libopenxr_loader.dylib. Otherwise the SDK's CMake auto-detects Homebrew
# jsoncpp via find_package and produces a dylib with a hardcoded
# /opt/homebrew/opt/jsoncpp/lib/libjsoncpp.26.dylib dependency, which
# breaks redistribution (Unity/Unreal apps that bundle the loader fail
# on end-user machines without Homebrew jsoncpp). See issue #205.
if [ ! -f "$OPENXR_DIR/lib/libopenxr_loader.dylib" ]; then
  echo "=== Building OpenXR loader ==="
  rm -rf /tmp/openxr-sdk
  git clone --depth 1 --branch "release-$OPENXR_VERSION" \
    https://github.com/KhronosGroup/OpenXR-SDK-Source.git /tmp/openxr-sdk

  # Patch the loader's CMakeLists.txt so the vendored jsoncpp include
  # path is prepended (BEFORE) instead of appended. Without this, the
  # Vulkan include path Vulkan_INCLUDE_DIR=/opt/homebrew/include is
  # listed first and the vendored jsoncpp .cpp files end up including
  # Homebrew's /opt/homebrew/include/json/*.h (different ABI), which
  # fails to compile.
  /usr/bin/sed -i '' \
    's|PRIVATE "${PROJECT_SOURCE_DIR}/src/external/jsoncpp/include"|BEFORE PRIVATE "${PROJECT_SOURCE_DIR}/src/external/jsoncpp/include"|' \
    /tmp/openxr-sdk/src/loader/CMakeLists.txt

  cmake -B /tmp/openxr-sdk/build -S /tmp/openxr-sdk -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="$OPENXR_DIR" \
    -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
    -DBUILD_WITH_SYSTEM_JSONCPP=OFF \
    -DCMAKE_MAP_IMPORTED_CONFIG_RELEASE="Release;None;"
  cmake --build /tmp/openxr-sdk/build
  cmake --install /tmp/openxr-sdk/build
else
  echo "=== OpenXR loader already built at $OPENXR_DIR ==="
fi

# Step 3: Build handle (window-handle) test apps
echo "=== Building cube_handle_vk_macos ==="
cmake -B "$ROOT/test_apps/cube_handle_vk_macos/build" \
  -S "$ROOT/test_apps/cube_handle_vk_macos" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/test_apps/cube_handle_vk_macos/build"

echo "=== Building cube_handle_metal_macos ==="
cmake -B "$ROOT/test_apps/cube_handle_metal_macos/build" \
  -S "$ROOT/test_apps/cube_handle_metal_macos" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/test_apps/cube_handle_metal_macos/build"

echo "=== Building cube_handle_gl_macos ==="
cmake -B "$ROOT/test_apps/cube_handle_gl_macos/build" \
  -S "$ROOT/test_apps/cube_handle_gl_macos" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/test_apps/cube_handle_gl_macos/build"

# Step 3b: Build texture (shared-texture) test apps
echo "=== Building cube_texture_metal_macos ==="
cmake -B "$ROOT/test_apps/cube_texture_metal_macos/build" \
  -S "$ROOT/test_apps/cube_texture_metal_macos" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/test_apps/cube_texture_metal_macos/build"

# Step 3c: Build hosted (runtime-managed) test apps
echo "=== Building cube_hosted_metal_macos ==="
cmake -B "$ROOT/test_apps/cube_hosted_metal_macos/build" \
  -S "$ROOT/test_apps/cube_hosted_metal_macos" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/test_apps/cube_hosted_metal_macos/build"

# Step 3c2: Build legacy hosted test apps
echo "=== Building cube_hosted_legacy_metal_macos ==="
cmake -B "$ROOT/test_apps/cube_hosted_legacy_metal_macos/build" \
  -S "$ROOT/test_apps/cube_hosted_legacy_metal_macos" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/test_apps/cube_hosted_legacy_metal_macos/build"

echo "=== Building cube_hosted_legacy_gl_macos ==="
cmake -B "$ROOT/test_apps/cube_hosted_legacy_gl_macos/build" \
  -S "$ROOT/test_apps/cube_hosted_legacy_gl_macos" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/test_apps/cube_hosted_legacy_gl_macos/build"

echo "=== Building cube_hosted_legacy_vk_macos ==="
cmake -B "$ROOT/test_apps/cube_hosted_legacy_vk_macos/build" \
  -S "$ROOT/test_apps/cube_hosted_legacy_vk_macos" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
cmake --build "$ROOT/test_apps/cube_hosted_legacy_vk_macos/build"

# 3DGS demo source moved to DisplayXR/displayxr-demo-gaussiansplat
# (master plan Step 2, 2026-05). Build it from that repo separately.

# Step 4: Package artifacts (mirrors CI workflow)
echo "=== Packaging artifacts ==="
PKG_DIR="$ROOT/_package/DisplayXR-macOS"
# Clean managed directories only (preserve user-added files like run_bridge_host.sh)
rm -rf "$PKG_DIR/lib" "$PKG_DIR/bin" "$PKG_DIR/share" 2>/dev/null || true
rm -f "$PKG_DIR/openxr_displayxr.json" "$PKG_DIR/run_cube_handle_vk.sh" "$PKG_DIR/run_cube_handle_metal.sh" "$PKG_DIR/run_cube_handle_gl.sh" "$PKG_DIR/run_cube_texture_metal.sh" "$PKG_DIR/run_cube_hosted_metal.sh" "$PKG_DIR/run_gaussian_splatting_handle_vk.sh" 2>/dev/null || true
# Also clean up old-named scripts
rm -f "$PKG_DIR/run_cube_rt_vk.sh" "$PKG_DIR/run_cube_ext_vk.sh" "$PKG_DIR/run_cube_rt_metal.sh" "$PKG_DIR/run_cube_ext_metal.sh" "$PKG_DIR/run_cube_rt_gl.sh" "$PKG_DIR/run_cube_ext_gl.sh" "$PKG_DIR/run_cube_shared_metal.sh" "$PKG_DIR/run_cube_shared_gl.sh" "$PKG_DIR/run_cube_shared_vk.sh" "$PKG_DIR/run_cube_metal_ext.sh" "$PKG_DIR/run_gaussian_splatting_ext_vk.sh" "$PKG_DIR/run_sim_cube.sh" "$PKG_DIR/run_sim_cube_ext.sh" "$PKG_DIR/run_sim_3dgs_ext.sh" 2>/dev/null || true
mkdir -p "$PKG_DIR/lib"
mkdir -p "$PKG_DIR/share/vulkan/icd.d"
mkdir -p "$PKG_DIR/bin"

# Find and copy runtime.
# The runtime is a SHARED dylib with PREFIX="" (issue #256 / ADR-019):
# CMake produces `openxr_displayxr.dylib`, not `libopenxr_displayxr.*`.
# Match both the new dylib name and legacy `.so`/`lib*` shapes in case an
# in-flight build leaves one of those behind during transition.
RUNTIME_LIB=$(find "$BUILD_DIR/src/xrt/targets/openxr" -maxdepth 1 \
  \( -name "openxr_displayxr.dylib" -o -name "openxr_displayxr.so" -o -name "libopenxr_displayxr*" \) \
  -type f | head -1)
if [ -z "$RUNTIME_LIB" ] || [ ! -f "$RUNTIME_LIB" ]; then
  echo "ERROR: runtime dylib not found in $BUILD_DIR/src/xrt/targets/openxr/" >&2
  exit 1
fi
RUNTIME_BASENAME=$(basename "$RUNTIME_LIB")
cp "$RUNTIME_LIB" "$PKG_DIR/lib/"

# Ship the vendor plug-in dylibs (issue #267 / ADR-019). The runtime
# dylib no longer link-includes drv_sim_display; everything routes
# through DisplayXR-SimDisplay.dylib, discovered via the JSON manifest
# placed alongside it. Run scripts point XRT_PLUGIN_SEARCH_PATH at this
# dir so dev iteration doesn't require touching
# ~/Library/Application Support/.
mkdir -p "$PKG_DIR/lib/displayxr/plugins"
SIMDISPLAY_PLUGIN=$(find "$BUILD_DIR/src/xrt/drivers" -name "DisplayXR-SimDisplay.dylib" -type f | head -1)
if [ -n "$SIMDISPLAY_PLUGIN" ] && [ -f "$SIMDISPLAY_PLUGIN" ]; then
  cp "$SIMDISPLAY_PLUGIN" "$PKG_DIR/lib/displayxr/plugins/"
  cat > "$PKG_DIR/lib/displayxr/plugins/200-sim-display.json" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "sim-display",
        "display_name": "DisplayXR Sim Display",
        "vendor":       "DisplayXR",
        "version":      "dev",
        "binary_path":  "$PKG_DIR/lib/displayxr/plugins/DisplayXR-SimDisplay.dylib",
        "probe_order":  200
    }
}
EOF
  # Plug-in resolves aux symbols from the runtime dylib next to it via
  # the @loader_path-relative rpath baked in at link time. Add an rpath
  # to be safe for the dev tree layout (../.. from plugins/ → lib/).
  install_name_tool -add_rpath @loader_path/../.. "$PKG_DIR/lib/displayxr/plugins/DisplayXR-SimDisplay.dylib" 2>/dev/null || true
else
  echo "Warning: DisplayXR-SimDisplay.dylib not found in build tree — runtime will have no display processor!"
fi

# Copy test app binaries
cp "$ROOT/test_apps/cube_handle_vk_macos/build/cube_handle_vk_macos" "$PKG_DIR/bin/"
cp "$ROOT/test_apps/cube_handle_metal_macos/build/cube_handle_metal_macos" "$PKG_DIR/bin/" 2>/dev/null || true
cp "$ROOT/test_apps/cube_handle_gl_macos/build/cube_handle_gl_macos" "$PKG_DIR/bin/" 2>/dev/null || true
cp "$ROOT/test_apps/cube_texture_metal_macos/build/cube_texture_metal_macos" "$PKG_DIR/bin/" 2>/dev/null || true
cp "$ROOT/test_apps/cube_hosted_metal_macos/build/cube_hosted_metal_macos" "$PKG_DIR/bin/" 2>/dev/null || true
cp "$ROOT/test_apps/cube_hosted_legacy_metal_macos/build/cube_hosted_legacy_metal_macos" "$PKG_DIR/bin/" 2>/dev/null || true
cp "$ROOT/test_apps/cube_hosted_legacy_gl_macos/build/cube_hosted_legacy_gl_macos" "$PKG_DIR/bin/" 2>/dev/null || true
cp "$ROOT/test_apps/cube_hosted_legacy_vk_macos/build/cube_hosted_legacy_vk_macos" "$PKG_DIR/bin/" 2>/dev/null || true
# Copy texture files for handle apps
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

# Rewrite the bundled Vulkan/MoltenVK install_names to @rpath. As shipped
# by Homebrew, both carry hardcoded LC_ID_DYLIB of
# `/opt/homebrew/opt/{vulkan-loader,molten-vk}/lib/...`, which means
# anything linking them inherits the absolute Homebrew path as a direct
# dependency — so the .pkg runtime would only load on machines that
# happen to have Homebrew installed at that exact prefix. Retarget to
# @rpath so dyld finds the bundled copy next to the runtime.
#
# Every install_name_tool invocation invalidates the dylib's code
# signature; on modern macOS the loader SIGKILLs processes that try
# to load a dylib with an invalidated signature. Re-sign ad-hoc with
# `codesign --force --sign -` after each modification.
chmod u+w "$PKG_DIR/lib/libvulkan.1.dylib" 2>/dev/null || true
install_name_tool -id @rpath/libvulkan.1.dylib "$PKG_DIR/lib/libvulkan.1.dylib" 2>/dev/null || true
codesign --force --sign - "$PKG_DIR/lib/libvulkan.1.dylib" 2>/dev/null || true

chmod u+w "$PKG_DIR/lib/libMoltenVK.dylib" 2>/dev/null || true
install_name_tool -id @rpath/libMoltenVK.dylib "$PKG_DIR/lib/libMoltenVK.dylib" 2>/dev/null || true
codesign --force --sign - "$PKG_DIR/lib/libMoltenVK.dylib" 2>/dev/null || true

# Retarget the runtime + sim-display plug-in's Vulkan dependency from
# the Homebrew absolute path to @rpath/libvulkan.1.dylib. The @rpath
# entries below give dyld the right hint:
#   runtime  → @loader_path        (libvulkan.1.dylib lives next to it)
#   plug-in  → @loader_path/../..  (libvulkan.1.dylib is two levels up)
chmod u+w "$PKG_DIR/lib/$RUNTIME_BASENAME" 2>/dev/null || true
install_name_tool -change /opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib \
                          @rpath/libvulkan.1.dylib \
                          "$PKG_DIR/lib/$RUNTIME_BASENAME" 2>/dev/null || true
codesign --force --sign - "$PKG_DIR/lib/$RUNTIME_BASENAME" 2>/dev/null || true

if [ -f "$PKG_DIR/lib/displayxr/plugins/DisplayXR-SimDisplay.dylib" ]; then
  chmod u+w "$PKG_DIR/lib/displayxr/plugins/DisplayXR-SimDisplay.dylib"
  install_name_tool -change /opt/homebrew/opt/vulkan-loader/lib/libvulkan.1.dylib \
                            @rpath/libvulkan.1.dylib \
                            "$PKG_DIR/lib/displayxr/plugins/DisplayXR-SimDisplay.dylib" 2>/dev/null || true
  codesign --force --sign - "$PKG_DIR/lib/displayxr/plugins/DisplayXR-SimDisplay.dylib" 2>/dev/null || true
fi

# Fix rpaths
install_name_tool -add_rpath @loader_path "$PKG_DIR/lib/$RUNTIME_BASENAME" 2>/dev/null || true
install_name_tool -add_rpath @executable_path/../lib "$PKG_DIR/bin/cube_handle_vk_macos" 2>/dev/null || true
install_name_tool -add_rpath @executable_path/../lib "$PKG_DIR/bin/cube_handle_metal_macos" 2>/dev/null || true
install_name_tool -add_rpath @executable_path/../lib "$PKG_DIR/bin/cube_handle_gl_macos" 2>/dev/null || true
install_name_tool -add_rpath @executable_path/../lib "$PKG_DIR/bin/cube_texture_metal_macos" 2>/dev/null || true
install_name_tool -add_rpath @executable_path/../lib "$PKG_DIR/bin/cube_hosted_metal_macos" 2>/dev/null || true
install_name_tool -add_rpath @executable_path/../lib "$PKG_DIR/bin/cube_hosted_legacy_metal_macos" 2>/dev/null || true
install_name_tool -add_rpath @executable_path/../lib "$PKG_DIR/bin/cube_hosted_legacy_gl_macos" 2>/dev/null || true
install_name_tool -add_rpath @executable_path/../lib "$PKG_DIR/bin/cube_hosted_legacy_vk_macos" 2>/dev/null || true
install_name_tool -add_rpath @loader_path "$PKG_DIR"/lib/libopenxr_loader*.dylib 2>/dev/null || true

# Re-sign every artifact we touched with install_name_tool. The
# add_rpath calls above invalidate signatures the same way -id and
# -change do; without re-signing, modern macOS SIGKILLs the process
# at dlopen with "Code Signature Invalid" before any of our logging
# gets a chance to fire.
codesign --force --sign - "$PKG_DIR/lib/$RUNTIME_BASENAME" 2>/dev/null || true
codesign --force --sign - "$PKG_DIR"/lib/libopenxr_loader*.dylib 2>/dev/null || true
for app in cube_handle_vk_macos cube_handle_metal_macos cube_handle_gl_macos \
           cube_texture_metal_macos cube_hosted_metal_macos \
           cube_hosted_legacy_metal_macos cube_hosted_legacy_gl_macos \
           cube_hosted_legacy_vk_macos; do
  [ -f "$PKG_DIR/bin/$app" ] && codesign --force --sign - "$PKG_DIR/bin/$app" 2>/dev/null || true
done

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
cat > "$PKG_DIR/openxr_displayxr.json" <<EOF
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "DisplayXR Runtime",
        "library_path": "lib/$RUNTIME_BASENAME"
    }
}
EOF

# Create run script for Vulkan handle test app
cat > "$PKG_DIR/run_cube_handle_vk.sh" <<'SCRIPT'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_displayxr.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export XRT_PLUGIN_SEARCH_PATH="$DIR/lib/displayxr/plugins"
export VK_ICD_FILENAMES="$DIR/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_DRIVER_FILES="$DIR/share/vulkan/icd.d/MoltenVK_icd.json"
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
echo "Starting cube_handle_vk_macos (Vulkan, window handle) with $SIM_DISPLAY_OUTPUT output..."
exec "$DIR/bin/cube_handle_vk_macos" "$@"
SCRIPT
chmod +x "$PKG_DIR/run_cube_handle_vk.sh"

# Create run script for Metal handle test app (no Vulkan env vars needed)
cat > "$PKG_DIR/run_cube_handle_metal.sh" <<'SCRIPT'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_displayxr.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export XRT_PLUGIN_SEARCH_PATH="$DIR/lib/displayxr/plugins"
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
echo "Starting cube_handle_metal_macos (Metal, window handle) with $SIM_DISPLAY_OUTPUT output..."
exec "$DIR/bin/cube_handle_metal_macos" "$@"
SCRIPT
chmod +x "$PKG_DIR/run_cube_handle_metal.sh"

# Create run script for OpenGL handle test app (no Vulkan env vars needed)
cat > "$PKG_DIR/run_cube_handle_gl.sh" <<'SCRIPT'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_displayxr.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export XRT_PLUGIN_SEARCH_PATH="$DIR/lib/displayxr/plugins"
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
echo "Starting cube_handle_gl_macos (OpenGL, window handle) with $SIM_DISPLAY_OUTPUT output..."
exec "$DIR/bin/cube_handle_gl_macos" "$@"
SCRIPT
chmod +x "$PKG_DIR/run_cube_handle_gl.sh"

# Create run script for Metal texture test app
cat > "$PKG_DIR/run_cube_texture_metal.sh" <<'SCRIPT'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_displayxr.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export XRT_PLUGIN_SEARCH_PATH="$DIR/lib/displayxr/plugins"
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
echo "Starting cube_texture_metal_macos (Metal, real view + shared IOSurface + 2D surround) with $SIM_DISPLAY_OUTPUT output..."
exec "$DIR/bin/cube_texture_metal_macos" "$@"
SCRIPT
chmod +x "$PKG_DIR/run_cube_texture_metal.sh"

# Create run script for Metal hosted test app
cat > "$PKG_DIR/run_cube_hosted_metal.sh" <<'SCRIPT'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_displayxr.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export XRT_PLUGIN_SEARCH_PATH="$DIR/lib/displayxr/plugins"
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
echo "Starting cube_hosted_metal_macos (Metal, hosted) with $SIM_DISPLAY_OUTPUT output..."
exec "$DIR/bin/cube_hosted_metal_macos" "$@"
SCRIPT
chmod +x "$PKG_DIR/run_cube_hosted_metal.sh"

# Create run script for Metal legacy hosted test app
cat > "$PKG_DIR/run_cube_hosted_legacy_metal.sh" <<'SCRIPT'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_displayxr.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export XRT_PLUGIN_SEARCH_PATH="$DIR/lib/displayxr/plugins"
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
echo "Starting cube_hosted_legacy_metal_macos (Metal, legacy hosted) with $SIM_DISPLAY_OUTPUT output..."
exec "$DIR/bin/cube_hosted_legacy_metal_macos" "$@"
SCRIPT
chmod +x "$PKG_DIR/run_cube_hosted_legacy_metal.sh"

# Create run script for OpenGL legacy hosted test app
cat > "$PKG_DIR/run_cube_hosted_legacy_gl.sh" <<'SCRIPT'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_displayxr.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export XRT_PLUGIN_SEARCH_PATH="$DIR/lib/displayxr/plugins"
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
echo "Starting cube_hosted_legacy_gl_macos (OpenGL, legacy hosted) with $SIM_DISPLAY_OUTPUT output..."
exec "$DIR/bin/cube_hosted_legacy_gl_macos" "$@"
SCRIPT
chmod +x "$PKG_DIR/run_cube_hosted_legacy_gl.sh"

# Create run script for Vulkan legacy hosted test app
cat > "$PKG_DIR/run_cube_hosted_legacy_vk.sh" <<'SCRIPT'
#!/bin/bash
DIR="$(cd "$(dirname "$0")" && pwd)"
export XR_RUNTIME_JSON="$DIR/openxr_displayxr.json"
export DYLD_LIBRARY_PATH="$DIR/lib:${DYLD_LIBRARY_PATH:-}"
export XRT_PLUGIN_SEARCH_PATH="$DIR/lib/displayxr/plugins"
export VK_ICD_FILENAMES="$DIR/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_DRIVER_FILES="$DIR/share/vulkan/icd.d/MoltenVK_icd.json"
export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
echo "Starting cube_hosted_legacy_vk_macos (Vulkan, legacy hosted) with $SIM_DISPLAY_OUTPUT output..."
exec "$DIR/bin/cube_hosted_legacy_vk_macos" "$@"
SCRIPT
chmod +x "$PKG_DIR/run_cube_hosted_legacy_vk.sh"


# Step 5: Build .pkg installer (optional). Versioned filename mirrors
# Windows' `DisplayXRSetup-X.Y.Z.NNN.exe`; DISPLAYXR_VERSION is also
# baked into the .pkg's internal version + the sim-display manifest,
# so it's the single source of truth across the artifact.
#
# Falls back to a sha-suffixed dev string for local builds with no
# explicit version set — matches build-macos.yml's BuildInstaller job.
if [ "$BUILD_INSTALLER" = "ON" ]; then
  if [ -z "${DISPLAYXR_VERSION:-}" ]; then
    GIT_SHA=$(git -C "$ROOT" rev-parse --short=8 HEAD 2>/dev/null || echo "local")
    INSTALLER_VERSION="0.0.0-dev.$GIT_SHA"
  else
    INSTALLER_VERSION="$DISPLAYXR_VERSION"
  fi
  INSTALLER_PKG="$ROOT/_package/DisplayXR-Installer-$INSTALLER_VERSION.pkg"
  echo "=== Building .pkg installer (VERSION=$INSTALLER_VERSION) ==="
  DISPLAYXR_VERSION="$INSTALLER_VERSION" \
    "$ROOT/installer/macos/build_installer.sh" "$PKG_DIR" "$INSTALLER_PKG"
fi

echo ""
echo "=== Build complete! ==="
echo ""
if [ "$BUILD_INSTALLER" = "ON" ]; then
  echo "Installer: $INSTALLER_PKG"
  echo ""
fi
echo "Artifacts in _package/:"
ls -lh "$ROOT/_package/" 2>/dev/null
echo ""
echo "Run directly:"
echo "  $PKG_DIR/run_cube_handle_vk.sh"
echo "  $PKG_DIR/run_cube_handle_metal.sh"
echo "  $PKG_DIR/run_cube_handle_gl.sh"
echo "  $PKG_DIR/run_cube_texture_metal.sh"
echo "  $PKG_DIR/run_cube_hosted_metal.sh"
echo "  $PKG_DIR/run_cube_hosted_legacy_metal.sh"
echo "  $PKG_DIR/run_cube_hosted_legacy_gl.sh"
echo "  $PKG_DIR/run_cube_hosted_legacy_vk.sh"
echo "  $PKG_DIR/run_gaussian_splatting_handle_vk.sh"
echo ""
echo "Or run manually:"
echo "  XR_RUNTIME_JSON=$BUILD_DIR/openxr_displayxr-dev.json \\"
echo "  DYLD_LIBRARY_PATH=$OPENXR_DIR/lib \\"
echo "  VK_ICD_FILENAMES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \\"
echo "  VK_DRIVER_FILES=/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json \\"
echo "  SIM_DISPLAY_OUTPUT=anaglyph \\"
echo "  $ROOT/test_apps/cube_handle_vk_macos/build/cube_handle_vk_macos"

#!/bin/bash
# Build the DisplayXR runtime on Linux — PHASE 0 (headless bring-up).
#
# This is the Linux mirror of build_macos.sh / build_windows.bat, but scoped to
# Phase 0 of the Linux port (docs/roadmap/linux-support.md): get the runtime to
# CONFIGURE, BUILD, and pass `displayxr-cli selftest` — the hardware-free gate
# that exercises plug-in discovery + the display-processor path WITHOUT a GPU,
# window, or OpenXR loader. There is no native presentation compositor on Linux
# yet (Phase 1), so the test apps are intentionally not built here.
#
# What this proves: OS detection, aux/os (threading/time), the POSIX plug-in
# loader (JSON manifest → dlopen), the sim_display display processor, and that
# the runtime starts and reports valid display info on Linux.
#
# Prerequisites (Debian/Ubuntu):
#   sudo apt-get install -y build-essential cmake ninja-build pkg-config \
#       libvulkan-dev vulkan-validationlayers glslang-tools \
#       libeigen3-dev libcjson-dev
#   # glslang-tools provides glslangValidator, required at configure time to
#   # compile the null compositor's SPIR-V (cmake/SPIR-V.cmake).
#   # optional (enables the legacy udev VR prober — NOT needed for selftest):
#   sudo apt-get install -y libudev-dev
#
# Usage:
#   ./scripts/build_linux.sh             # in-process headless build + selftest
#   ./scripts/build_linux.sh --service   # also build displayxr-service (IPC)
#   ./scripts/build_linux.sh --no-test   # build only, skip the selftest run

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT/build"

SERVICE_MODE=OFF
RUN_TEST=ON
for arg in "$@"; do
  case "$arg" in
    --service) SERVICE_MODE=ON ;;
    --no-test) RUN_TEST=OFF ;;
    *) echo "Unknown arg: $arg" >&2; exit 2 ;;
  esac
done

# Step 1: Configure + build the runtime, the CLI, and the sim_display plug-in.
#
# displayxr-cli links the no-compositor instance directly (target_instance_no_comp),
# so Phase 0 needs neither a native compositor nor the OpenXR loader — only the
# runtime libs, the CLI, and the discoverable sim_display plug-in .so.
echo "=== Configuring DisplayXR runtime (Linux, SERVICE=$SERVICE_MODE) ==="
cmake -B "$BUILD_DIR" -S "$ROOT" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DXRT_FEATURE_SERVICE=$SERVICE_MODE \
  -DXRT_MODULE_CLI=ON \
  -DXRT_BUILD_DRIVER_QWERTY=OFF \
  -DXRT_FEATURE_DEBUG_GUI=OFF \
  -DXRT_FEATURE_WINDOW_PEEK=OFF \
  -DXRT_HAVE_SDL2=OFF \
  -DXRT_HAVE_OPENCV=OFF \
  -DXRT_HAVE_LIBUSB=OFF \
  -DXRT_BUILD_DRIVER_EUROC=OFF

echo "=== Building runtime + displayxr-cli + sim_display plug-in ==="
cmake --build "$BUILD_DIR"

# Step 2: Locate the built artifacts.
CLI_BIN="$(find "$BUILD_DIR/src/xrt/targets/cli" -maxdepth 1 -name displayxr-cli -type f | head -1)"
if [ -z "$CLI_BIN" ]; then
  echo "ERROR: displayxr-cli not found under $BUILD_DIR/src/xrt/targets/cli/" >&2
  exit 1
fi

# On Linux the plug-in target is OUTPUT_NAME "DisplayXR-SimDisplay" PREFIX ""
# → DisplayXR-SimDisplay.so (src/xrt/drivers/CMakeLists.txt:264-266).
SIMDISPLAY_PLUGIN="$(find "$BUILD_DIR/src/xrt/drivers" -name "DisplayXR-SimDisplay.so" -type f | head -1)"
if [ -z "$SIMDISPLAY_PLUGIN" ]; then
  echo "ERROR: DisplayXR-SimDisplay.so not found — runtime would have no display processor." >&2
  exit 1
fi

# Step 3: Stage the plug-in + a JSON discovery manifest into a search dir.
#
# Linux plug-in discovery is JSON-manifest driven (target_plugin_loader.c POSIX
# path). For dev we point XRT_PLUGIN_SEARCH_PATH at a staging dir rather than
# installing into the XDG / /usr/share roots — mirrors the macOS run_*.sh model.
PLUGIN_DIR="$BUILD_DIR/_plugins"
mkdir -p "$PLUGIN_DIR"
cp "$SIMDISPLAY_PLUGIN" "$PLUGIN_DIR/"
cat > "$PLUGIN_DIR/200-sim-display.json" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "sim-display",
        "display_name": "DisplayXR Sim Display",
        "vendor":       "DisplayXR",
        "version":      "dev",
        "binary_path":  "$PLUGIN_DIR/DisplayXR-SimDisplay.so",
        "probe_order":  200
    }
}
EOF

export XRT_PLUGIN_SEARCH_PATH="$PLUGIN_DIR"

echo ""
echo "=== Build complete! ==="
echo "  displayxr-cli:        $CLI_BIN"
echo "  sim_display plug-in:  $PLUGIN_DIR/DisplayXR-SimDisplay.so"
echo "  XRT_PLUGIN_SEARCH_PATH=$PLUGIN_DIR"
echo ""

# Step 4: Run the hardware-free selftest gate (the CI gate, headless).
if [ "$RUN_TEST" = "ON" ]; then
  echo "=== displayxr-cli info ==="
  "$CLI_BIN" info || true
  echo ""
  echo "=== displayxr-cli selftest ==="
  "$CLI_BIN" selftest
  echo ""
  echo "Phase 0 selftest PASSED — plug-in discovery + display-processor path work on Linux."
else
  echo "Run the headless gate manually:"
  echo "  XRT_PLUGIN_SEARCH_PATH=$PLUGIN_DIR $CLI_BIN selftest"
fi

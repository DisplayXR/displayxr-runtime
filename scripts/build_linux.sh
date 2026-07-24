#!/bin/bash
# Build the DisplayXR runtime on Linux (docs/roadmap/linux-support.md, #660).
#
# This is the Linux mirror of build_macos.sh / build_windows.bat. The default
# invocation is the Phase 0 headless gate: CONFIGURE, BUILD, and pass
# `displayxr-cli selftest` — hardware-free, exercising plug-in discovery + the
# display-processor path WITHOUT a GPU, window, or OpenXR loader. --apps adds
# the Phase 1 on-screen bring-up vehicle; --service adds the Phase 2 service /
# IPC build (displayxr-service + the IPC-client runtime — build-green only on a
# headless box; the IPC round-trip needs a display, Phase 1b).
#
# What this proves: OS detection, aux/os (threading/time), the POSIX plug-in
# loader (JSON manifest → dlopen), the sim_display display processor, and that
# the runtime starts and reports valid display info on Linux.
#
# Prerequisites (Debian/Ubuntu):
#   sudo apt-get install -y build-essential cmake ninja-build pkg-config \
#       libvulkan-dev vulkan-validationlayers glslang-tools \
#       libeigen3-dev libcjson-dev \
#       libxcb1-dev libxcb-randr0-dev libx11-dev libx11-xcb-dev libxrandr-dev
#   # glslang-tools provides glslangValidator, required at configure time to
#   # compile the null compositor's SPIR-V (cmake/SPIR-V.cmake).
#   # libxcb*-dev enables XRT_HAVE_XCB → the native Vulkan compositor builds on
#   # Linux with the VK_KHR_xcb_surface present path (Phase 1). Without it the
#   # runtime still builds headless (Phase 0) but has no on-screen compositor.
#   # libx11-dev + libx11-xcb-dev supply Xlib + XGetXCBConnection — the runtime
#   # converts an app-provided Xlib window (XR_DXR_xlib_window_binding, Phase 3)
#   # to its XCB connection, and the handle-class test app opens an X11 window.
#   # libx11-dev + libxrandr-dev also enable XRT_HAVE_XLIB_XRANDR → the optional
#   # direct-scanout present path (DXR_LINUX_DIRECT_SCANOUT=1) that acquires the
#   # 3D-panel connector as a VkDisplayKHR and bypasses Xorg/compositor (ST-5539).
#   # optional (enables the legacy udev VR prober — NOT needed for selftest):
#   sudo apt-get install -y libudev-dev
#
# Usage:
#   ./scripts/build_linux.sh             # in-process headless build + selftest
#   ./scripts/build_linux.sh --service   # also build displayxr-service (IPC)
#   ./scripts/build_linux.sh --no-test   # build only, skip the selftest run
#   ./scripts/build_linux.sh --apps      # also build the OpenXR loader + the
#                                        # test apps: cube_hosted_legacy_vk_linux
#                                        # (hosted, Phase 1b) and cube_handle_vk_linux
#                                        # (handle, XR_DXR_xlib_window_binding,
#                                        # Phase 3); running them needs a GPU + X
#                                        # server

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# BUILD_DIR is overridable so the tree can be built out-of-source — e.g. the
# .deb Docker test (scripts/test_deb_linux.sh) builds into a container-local
# dir to avoid colliding with a host build/ cache on the bind-mounted repo.
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"

SERVICE_MODE=OFF
RUN_TEST=ON
BUILD_APPS=OFF
for arg in "$@"; do
  case "$arg" in
    --service) SERVICE_MODE=ON ;;
    --no-test) RUN_TEST=OFF ;;
    --apps) BUILD_APPS=ON ;;
    *) echo "Unknown arg: $arg" >&2; exit 2 ;;
  esac
done

OPENXR_DIR="$BUILD_DIR/_openxr"
OPENXR_VERSION="1.1.51"

# Step 1: Configure + build the runtime, the CLI, and the sim_display plug-in.
#
# displayxr-cli links the no-compositor instance directly (target_instance_no_comp),
# so Phase 0 needs neither a native compositor nor the OpenXR loader — only the
# runtime libs, the CLI, and the discoverable sim_display plug-in .so.
# CMAKE_BUILD_TYPE is overridable (default Debug for dev). The .deb packager
# sets it to Release so the shipped runtime matches the Release-built Leia
# plug-in — a Debug/Release skew across the plug-in↔runtime struct boundary
# (NDEBUG-conditional layout) is a candidate cause of the VK-DP-factory
# null-dispatch crash seen with a Debug .deb runtime (cube-hw finding C), and a
# Debug runtime is the wrong (36 MB, unoptimized) release artifact regardless.
echo "=== Configuring DisplayXR runtime (Linux, SERVICE=$SERVICE_MODE, TYPE=${CMAKE_BUILD_TYPE:-Debug}) ==="
cmake -B "$BUILD_DIR" -S "$ROOT" -G Ninja \
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}" \
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

# Phase 2 (#660): in a --service build, assert the service executable and the
# IPC-client runtime actually linked. CI has no display, so build-green IS the
# Phase 2 gate — the IPC round-trip is validated on hardware (Phase 1b).
SERVICE_BIN=""
if [ "$SERVICE_MODE" = "ON" ]; then
  SERVICE_BIN="$(find "$BUILD_DIR/src/xrt/targets/service" -maxdepth 1 -name displayxr-service -type f | head -1)"
  if [ -z "$SERVICE_BIN" ]; then
    echo "ERROR: displayxr-service not found under $BUILD_DIR/src/xrt/targets/service/" >&2
    exit 1
  fi
  RUNTIME_SO="$(find "$BUILD_DIR/src/xrt/targets/openxr" -name "openxr_displayxr.so*" -type f | head -1)"
  if [ -z "$RUNTIME_SO" ]; then
    echo "ERROR: IPC-client runtime .so not found under $BUILD_DIR/src/xrt/targets/openxr/" >&2
    exit 1
  fi
  echo "Service build artifacts OK:"
  echo "  displayxr-service:    $SERVICE_BIN"
  echo "  IPC-client runtime:   $RUNTIME_SO"
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

# Step 5: optionally build the OpenXR loader + the hosted Vulkan cube test app.
# Unlike the headless cli (which links the no-comp instance directly), a test app
# is a real OpenXR client and links the loader. This is the Phase 1b on-screen
# bring-up vehicle — it needs a GPU + running X server to actually present.
if [ "$BUILD_APPS" = "ON" ]; then
  # Step 5a: OpenXR loader (built from source, cached in $OPENXR_DIR).
  if [ ! -f "$OPENXR_DIR/lib/libopenxr_loader.so" ] || \
     [ ! -f "$OPENXR_DIR/lib/cmake/openxr/OpenXRConfig.cmake" ]; then
    echo "=== Building OpenXR loader $OPENXR_VERSION ==="
    rm -rf /tmp/openxr-sdk-linux "$OPENXR_DIR"
    git clone --depth 1 --branch "release-$OPENXR_VERSION" \
      https://github.com/KhronosGroup/OpenXR-SDK-Source.git /tmp/openxr-sdk-linux
    cmake -B /tmp/openxr-sdk-linux/build -S /tmp/openxr-sdk-linux -G Ninja \
      -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_INSTALL_PREFIX="$OPENXR_DIR" \
      -DBUILD_TESTS=OFF -DBUILD_CONFORMANCE_TESTS=OFF \
      -DBUILD_WITH_SYSTEM_JSONCPP=OFF
    cmake --build /tmp/openxr-sdk-linux/build
    cmake --install /tmp/openxr-sdk-linux/build
  else
    echo "=== OpenXR loader already built at $OPENXR_DIR ==="
  fi

  # Step 5b: build the Vulkan cube test apps + per-app run scripts.
  #   cube_hosted_legacy_vk_linux — hosted: runtime self-creates the XCB window.
  #   cube_handle_vk_linux        — handle: app creates its own X11 window and
  #                                 passes it via XR_DXR_xlib_window_binding.
  for APP in cube_hosted_legacy_vk_linux cube_handle_vk_linux; do
    APP_DIR="$ROOT/test_apps/$APP"
    # CANDIDATE PATCH (#706 Linux validation): the apps aren't all flat under
    # test_apps/ — cube_hosted_legacy_vk_linux lives in test_apps/legacy/. Fall
    # back to a nested lookup when the flat path is absent.
    if [ ! -d "$APP_DIR" ]; then
      APP_DIR="$(find "$ROOT/test_apps" -type d -name "$APP" | head -1)"
    fi
    if [ -z "$APP_DIR" ] || [ ! -f "$APP_DIR/CMakeLists.txt" ]; then
      echo "ERROR: test app source for '$APP' not found under $ROOT/test_apps" >&2; exit 1
    fi
    echo "=== Building $APP (src: $APP_DIR) ==="
    cmake -B "$APP_DIR/build" -S "$APP_DIR" -G Ninja \
      -DCMAKE_BUILD_TYPE=Debug \
      -DCMAKE_PREFIX_PATH="$OPENXR_DIR"
    cmake --build "$APP_DIR/build"

    # Run script — dev runtime manifest + sim-display plug-in + loader.
    RUN="$BUILD_DIR/run_${APP}.sh"
    cat > "$RUN" <<EOF
#!/bin/bash
# Run $APP against the dev runtime build. Needs a running X server (DISPLAY
# set) + a Vulkan GPU.
# OXR_ENABLE_VK_NATIVE_COMPOSITOR=1 selects the native Vulkan compositor path;
# SIM_DISPLAY_OUTPUT picks the sim-display weave (anaglyph/sbs/...).
export XR_RUNTIME_JSON="$BUILD_DIR/openxr_displayxr-dev.json"
export LD_LIBRARY_PATH="$OPENXR_DIR/lib:\${LD_LIBRARY_PATH:-}"
export XRT_PLUGIN_SEARCH_PATH="$PLUGIN_DIR"
export OXR_ENABLE_VK_NATIVE_COMPOSITOR="\${OXR_ENABLE_VK_NATIVE_COMPOSITOR:-1}"
export SIM_DISPLAY_OUTPUT="\${SIM_DISPLAY_OUTPUT:-anaglyph}"
exec "$APP_DIR/build/$APP" "\$@"
EOF
    chmod +x "$RUN"
    echo ""
    echo "Built $APP. Run on a Linux box with a GPU + display:"
    echo "  $RUN"
  done
fi

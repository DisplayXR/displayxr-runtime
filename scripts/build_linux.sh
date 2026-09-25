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
#       libxcb1-dev libxcb-randr0-dev libx11-dev libx11-xcb-dev libxrandr-dev \
#       libwayland-dev libdbus-1-dev
#   # libwayland-dev enables the XR_DXR_wayland_surface_binding present path;
#   # libdbus-1-dev adds the Wayland window-geometry provider (#817) so
#   # windowed weaving works under Wayland (GNOME extension in contrib/).
#   # Both optional — absent, the build degrades exactly as before.
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
#   # CI PARITY — install these too if you want a local build to PREDICT CI.
#   sudo apt-get install -y libgl-dev libegl-dev libglvnd-dev \
#       libxxf86vm-dev libxcb-glx0-dev libxrandr-dev
#   # The Selftest/Service jobs in .github/workflows/build-linux.yml install
#   # GL/EGL on purpose: they flip XRT_HAVE_OPENGL/XRT_HAVE_EGL ON so CI
#   # configures like a real desktop (aux_ogl + the GL client bindings compile)
#   # and keeps the comp_gl platform gate honest (#709), and once GL is found
#   # the --apps OpenXR-loader build additionally needs Xxf86vm/Xrandr
#   # (gfxwrapper) and xcb/glx.h. Without them a local build compiles a
#   # strictly SMALLER set of targets than CI does, so green here does not mean
#   # green there. (The .deb deliberately ships without GL/EGL — see the Deb
#   # job's comment.)
#
#   NOTE: install the dependencies BEFORE the first configure. CMake caches a
#   failed feature probe permanently, so a package installed afterwards is not
#   picked up — and the mismatch can wedge the tree outright (see --clean).
#
# Usage:
#   ./scripts/build_linux.sh             # in-process headless build + selftest
#   ./scripts/build_linux.sh --service   # also build displayxr-service (IPC)
#   ./scripts/build_linux.sh --no-test   # build only, skip the selftest run
#   ./scripts/build_linux.sh --clean     # drop the CMake cache first — REQUIRED
#                                        # after installing a dependency into a
#                                        # tree that was already configured
#   ./scripts/build_linux.sh --qwerty    # build the qwerty keyboard/mouse driver
#                                        # (#1727): the self-created X11 window
#                                        # then drives the qwerty HMD + hand
#                                        # controllers. REQUIRED for the
#                                        # interactive CTS categories (actions,
#                                        # scenario need controllers and a select
#                                        # click). Off by default: the shipped
#                                        # runtime has no qwerty on Linux.
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
CLEAN=OFF
QWERTY=OFF
for arg in "$@"; do
  case "$arg" in
    --service) SERVICE_MODE=ON ;;
    --no-test) RUN_TEST=OFF ;;
    --apps) BUILD_APPS=ON ;;
    --clean) CLEAN=ON ;;
    --qwerty) QWERTY=ON ;;
    *) echo "Unknown arg: $arg" >&2; exit 2 ;;
  esac
done

# --clean: drop the CMake cache so feature detection re-runs from scratch.
#
# This exists because a stale cache does not merely miss a new dependency, it
# can WEDGE the tree. Feature probes made with check_*_source_compiles cache
# their result permanently — a probe that failed because its -dev package was
# absent stays failed after you install it, since CMake never retries. Install
# libgles-dev into a tree first configured without libegl-dev and you get a
# hard configure error (`XRT_HAVE_OPENGLES requires XRT_HAVE_EGL`) describing a
# situation that is no longer true: EGL is present, only the cached probe says
# otherwise. The build is then unfixable by installing anything.
#
# Deleting CMakeCache.txt + CMakeFiles/ (rather than the whole build dir) keeps
# the expensive artifacts — the provisioned OpenXR loader under
# _openxr-$OPENXR_VERSION and the _plugins staging dir — so a clean reconfigure
# costs a rebuild, not a re-download. Mirrors build_windows.bat's --clean.
if [ "$CLEAN" = "ON" ]; then
  echo "=== --clean: removing CMake cache in $BUILD_DIR (keeping _openxr-* and _plugins) ==="
  rm -rf "$BUILD_DIR/CMakeCache.txt" "$BUILD_DIR/CMakeFiles"
fi

OPENXR_VERSION="1.1.63"
# Versioned cache (#1487): the existence gate below is inherently
# version-correct, so bumping OPENXR_VERSION always re-builds the loader
# instead of silently reusing the previously cached one.
OPENXR_DIR="$BUILD_DIR/_openxr-$OPENXR_VERSION"

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
echo "=== Configuring DisplayXR runtime (Linux, SERVICE=$SERVICE_MODE, QWERTY=$QWERTY, TYPE=${CMAKE_BUILD_TYPE:-Debug}) ==="
cmake -B "$BUILD_DIR" -S "$ROOT" -G Ninja \
  -DCMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Debug}" \
  -DXRT_FEATURE_SERVICE=$SERVICE_MODE \
  -DXRT_MODULE_CLI=ON \
  -DXRT_BUILD_DRIVER_QWERTY=$QWERTY \
  -DXRT_FEATURE_DEBUG_GUI=OFF \
  -DXRT_FEATURE_WINDOW_PEEK=OFF \
  -DXRT_HAVE_SDL2=OFF \
  -DXRT_HAVE_OPENCV=OFF \
  -DXRT_HAVE_LIBUSB=OFF

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

# Input-provider plug-in (ADR-034 / #823): the simulated motion
# controllers. Same search dir; the `-input-provider.json` suffix routes
# the manifest to the input loader instead of the DP loader.
#
# Staging the manifest is safe on its own: sim-input's probe() declines
# unless DXR_SIM_INPUT is set in the environment (ADR-034 Amendment 1), so
# a dev tree does NOT get synthetic controllers by default — qwerty keeps
# the hand roles until you ask for the simulator.
SIMINPUT_PLUGIN="$(find "$BUILD_DIR/src/xrt/drivers" -name "DisplayXR-SimInput.so" -type f | head -1)"
if [ -n "$SIMINPUT_PLUGIN" ]; then
  cp "$SIMINPUT_PLUGIN" "$PLUGIN_DIR/"
  cat > "$PLUGIN_DIR/200-sim-input-input-provider.json" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "sim-input",
        "display_name": "DisplayXR Sim Input",
        "vendor":       "DisplayXR",
        "version":      "dev",
        "binary_path":  "$PLUGIN_DIR/DisplayXR-SimInput.so",
        "probe_order":  200
    }
}
EOF
else
  echo "Note: DisplayXR-SimInput.so not found — no input provider staged (qwerty keeps the hand roles)."
fi

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
  #   cube_handle_vk_linux        — handle: app creates its own window and passes
  #                                 it to the runtime. ONE binary, TWO backends,
  #                                 picked at runtime with --platform=x11|wayland|auto
  #                                 (or DXR_WINDOW_BACKEND): X11 via
  #                                 XR_DXR_xlib_window_binding, native Wayland via
  #                                 XR_DXR_wayland_surface_binding (fullscreen-only —
  #                                 docs/specs/extensions/XR_DXR_wayland_surface_binding.md).
  #                                 Default auto: native Wayland when ready, else X11.
  #   cube_zones_vk_linux         — handle + XR_DXR_display_zones (ADR-027):
  #                                 2 clear-based 3D zones + a Local2D strip. Same
  #                                 --platform / DXR_WINDOW_BACKEND selection.
  #   weave_probe_vk_linux        — headless XR_DXR_weave present-owner probe
  #                                 (#1699 R2): no window; needs a running
  #                                 displayxr-service (its run script forces IPC).
  #   weave_present_vk_linux      — XR_DXR_weave present-owner WITH a window
  #                                 (#1699): renders SBS into a dma-buf, the
  #                                 service weaves it, the app presents the woven
  #                                 dma-buf in its own swapchain. Same service
  #                                 requirement; --headless N self-checks offscreen.
  for APP in cube_hosted_legacy_vk_linux cube_handle_vk_linux cube_zones_vk_linux weave_probe_vk_linux \
             weave_present_vk_linux; do
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
    # The weave probe is a present-owner: it only works on the IPC path, against
    # a displayxr-service started separately (see the probe's file header).
    FORCE_IPC=""
    PLUGIN_LINE="export XRT_PLUGIN_SEARCH_PATH=\"$PLUGIN_DIR\""
    case "$APP" in
      weave_probe_*) FORCE_IPC='export XRT_FORCE_MODE="${XRT_FORCE_MODE:-ipc}"' ;;
      weave_present_*)
        FORCE_IPC='export XRT_FORCE_MODE="${XRT_FORCE_MODE:-ipc}"'
        # The display processor lives in the SERVICE, which reads its own
        # environment: the client never loads a plug-in, so the run script
        # passes XRT_PLUGIN_SEARCH_PATH through only when the caller set it.
        PLUGIN_LINE='# XRT_PLUGIN_SEARCH_PATH: inherited only (the service, not this client, loads the display processor)'
        ;;
    esac
    cat > "$RUN" <<EOF
#!/bin/bash
# Run $APP against the dev runtime build. Needs a Vulkan GPU and either an X
# server (DISPLAY) or a Wayland compositor (WAYLAND_DISPLAY).
#
# The cube_handle / cube_zones apps take --platform=x11|wayland|auto (default
# auto; DXR_WINDOW_BACKEND sets the same thing, --platform wins; --backend= is
# the older spelling) and pass every argument straight through, e.g.
#   \$0 --platform=wayland
# auto probes connections (never session env vars): native Wayland when the
# compositor is ready (fractional-scale + viewporter + the window-geometry
# extension), else X11 (XWayland counts). --help lists the rest.
# OXR_ENABLE_VK_NATIVE_COMPOSITOR=1 selects the native Vulkan compositor path;
# SIM_DISPLAY_OUTPUT picks the sim-display weave (anaglyph/sbs/...).
export XR_RUNTIME_JSON="$BUILD_DIR/openxr_displayxr-dev.json"
export LD_LIBRARY_PATH="$OPENXR_DIR/lib:\${LD_LIBRARY_PATH:-}"
$PLUGIN_LINE
# #902: dev-tree manifest for VK_LAYER_DXR_queue_lock, so the runtime-injected
# queue-serialization layer is discoverable (shared-queue late-weave repaint on
# single-graphics-queue GPUs). Installed builds use the system manifest path.
export VK_LAYER_PATH="$BUILD_DIR/src/xrt/targets/vk_layer\${VK_LAYER_PATH:+:\$VK_LAYER_PATH}"
export OXR_ENABLE_VK_NATIVE_COMPOSITOR="\${OXR_ENABLE_VK_NATIVE_COMPOSITOR:-1}"
export SIM_DISPLAY_OUTPUT="\${SIM_DISPLAY_OUTPUT:-anaglyph}"
$FORCE_IPC
exec "$APP_DIR/build/$APP" "\$@"
EOF
    chmod +x "$RUN"
    echo ""
    echo "Built $APP. Run on a Linux box with a GPU + display:"
    echo "  $RUN"
  done
fi

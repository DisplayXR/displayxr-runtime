#!/usr/bin/env bash
# =============================================================================
# DisplayXR — fetch + build the Khronos OpenXR-CTS (Linux/x86_64)
#
# Linux port of scripts/fetch_build_cts.bat (#1527 / #1523). Builds
# conformance_cli + conformance_test out-of-tree under build-cts/ (gitignored),
# together with the two CTS API layers. NOT wired into build_linux.sh — the CTS
# is a developer/CI harness, not a runtime artifact.
#
# Usage:
#   ./scripts/fetch_build_cts.sh             # clone (if needed) + configure + build
#   ./scripts/fetch_build_cts.sh --apt       # apt-get the build deps first (sudo)
#   ./scripts/fetch_build_cts.sh --clean     # drop build-cts/build before configuring
#
# THE PIN IS SHARED WITH WINDOWS ON PURPOSE. CTS_TAG below must equal the one in
# fetch_build_cts.bat: a submission package that mixes CTS revisions across the
# arms is not one result set, and a Linux-only bump would silently change which
# tests exist on one half of the matrix. The rationale for openxr-cts-1.1.63.0
# (why >= 1.1.60.0 is the floor, why there is no 1.1.62.0, what the 1.1.61 ->
# 1.1.63 delta is) is written out once, in the .bat — read it there.
#
# WHAT THIS SCRIPT DELIBERATELY DOES NOT DO, and why (the .bat does both):
#
#   * No DPI manifest embed. That is a Win32 concern — a DPI-unaware process on
#     a scaled Windows display reads virtualised window geometry and every
#     geometric CTS measurement comes back wrong by the scale factor (#1506).
#     X11 has no equivalent per-process geometry virtualisation; the compositor
#     reads real root-window pixels through xcb_get_geometry /
#     xcb_translate_coordinates. Nothing to embed, nothing to verify.
#
#   * No Vulkan-loader staging next to the exe. On Windows the CTS links a
#     vulkan-1.dll that a bare runner does not have in System32, so the process
#     dies at load with 0xC0000135 before any test runs (#1525). On Linux
#     libvulkan.so.1 comes from libvulkan1, a hard dependency of every ICD
#     package, and ld.so resolves it from the normal search path.
#
# Output: build-cts/build/**/conformance_cli (path echoed at the end).
# =============================================================================

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CTS_TAG="${CTS_TAG:-openxr-cts-1.1.63.0}"
CTS_ROOT="$ROOT/build-cts"
CTS_SRC="$CTS_ROOT/OpenXR-CTS"
CTS_BUILD="$CTS_ROOT/build"
CTS_CONFIG="${CTS_CONFIG:-RelWithDebInfo}"

APT=OFF
CLEAN=OFF
for arg in "$@"; do
  case "$arg" in
    --apt)   APT=ON ;;
    --clean) CLEAN=ON ;;
    -h|--help)
      sed -n '2,40p' "$0"
      exit 0
      ;;
    *) echo "Unknown arg: $arg" >&2; exit 2 ;;
  esac
done

# -----------------------------------------------------------------------------
# 1. Dependencies.
#
# Superset of scripts/build_linux.sh's header list plus what the CTS itself
# needs on top of the runtime: the CTS is a real OpenXR *client*, so it links
# the loader's X11/XCB/Wayland platform bits and (for its opengl plugin, which
# it builds whenever GL is found) the gfxwrapper's Xrandr/Xxf86vm. python3 is
# required at configure time — the CTS generates sources.
# -----------------------------------------------------------------------------
APT_PACKAGES=(
  build-essential cmake ninja-build pkg-config git python3
  libvulkan-dev glslang-tools
  libx11-dev libx11-xcb-dev libxcb1-dev libxcb-randr0-dev libxcb-glx0-dev
  libxrandr-dev libxxf86vm-dev libwayland-dev libxkbcommon-dev
  libgl1-mesa-dev libegl1-mesa-dev
)

if [ "$APT" = "ON" ]; then
  SUDO=""
  [ "$(id -u)" != 0 ] && SUDO=sudo
  echo "=== apt-get install (CTS build deps) ==="
  $SUDO apt-get update
  $SUDO apt-get install -y "${APT_PACKAGES[@]}"
fi

for tool in cmake ninja git python3; do
  command -v "$tool" >/dev/null 2>&1 || {
    echo "ERROR: $tool not found. Re-run with --apt, or install: ${APT_PACKAGES[*]}" >&2
    exit 1
  }
done

# -----------------------------------------------------------------------------
# 2. Fetch OpenXR-CTS at the pinned tag (out-of-tree).
# -----------------------------------------------------------------------------
mkdir -p "$CTS_ROOT"

if [ ! -d "$CTS_SRC/.git" ]; then
  echo "=== Cloning OpenXR-CTS @ $CTS_TAG ==="
  git clone --depth 1 --branch "$CTS_TAG" https://github.com/KhronosGroup/OpenXR-CTS.git "$CTS_SRC"
else
  echo "=== OpenXR-CTS present; ensuring tag $CTS_TAG ==="
  git -C "$CTS_SRC" fetch --depth 1 origin "tag" "$CTS_TAG" >/dev/null 2>&1 || true
  git -C "$CTS_SRC" checkout -q "$CTS_TAG"
fi

if [ "$CLEAN" = "ON" ]; then
  echo "=== --clean: removing $CTS_BUILD ==="
  rm -rf "$CTS_BUILD"
fi

# -----------------------------------------------------------------------------
# 3. Configure (Ninja Multi-Config, same generator as Windows so the output
#    layout — <target>/<config>/<binary> — and therefore run_cts.sh's discovery
#    match across both platforms). The CTS vendors its own deps (Catch2,
#    jsoncpp, tinygltf, ...) in src/external: no vcpkg, no submodules. The
#    OpenXR loader links statically by default; conformance_cli finds the
#    runtime through the loader's normal discovery, which on Linux honours
#    XR_RUNTIME_JSON (no elevation caveat — see run_cts.sh).
# -----------------------------------------------------------------------------
echo "=== CMake configure (CTS) ==="
cmake -S "$CTS_SRC" -B "$CTS_BUILD" -G "Ninja Multi-Config" \
  -DCMAKE_CROSS_CONFIGS="$CTS_CONFIG" \
  -DCMAKE_DEFAULT_BUILD_TYPE="$CTS_CONFIG"

echo "=== Building conformance_cli + conformance_test ($CTS_CONFIG) ==="
cmake --build "$CTS_BUILD" --config "$CTS_CONFIG" --target conformance_cli conformance_test

# -----------------------------------------------------------------------------
# 4. Report + assert. Finding the binary here (rather than only in run_cts.sh)
#    turns a CTS output-layout change into a build failure with a clear message
#    instead of a "missing:" error three steps later in a CI arm.
# -----------------------------------------------------------------------------
CTS_EXE="$(find "$CTS_BUILD" -type f -name conformance_cli -perm -u+x | head -1)"
if [ -z "$CTS_EXE" ]; then
  echo "ERROR: conformance_cli not found under $CTS_BUILD after build." >&2
  exit 1
fi

# The vulkan / vulkan2 plugins exist only if the CTS's find_package(Vulkan)
# succeeded — the Linux matrix is Vulkan-only (no D3D, no Metal, and the
# runtime has no Linux comp_gl branch), so a Vulkan-less CTS means the whole
# lane has nothing to run. Assert it at BUILD time against the CACHE, which is
# what find_package(Vulkan) actually writes: "-- Could NOT find Vulkan" is 40
# lines deep in configure output and cost a CI round on Windows (#1525).
VK_INC_CACHE="$(sed -n 's/^Vulkan_INCLUDE_DIR:PATH=//p' "$CTS_BUILD/CMakeCache.txt" 2>/dev/null || true)"
if [ -z "$VK_INC_CACHE" ] || [ "${VK_INC_CACHE%NOTFOUND}" != "$VK_INC_CACHE" ]; then
  echo "ERROR: conformance_cli was built WITHOUT the vulkan/vulkan2 graphics plugins." >&2
  echo "       find_package(Vulkan) failed (Vulkan_INCLUDE_DIR unset/NOTFOUND in $CTS_BUILD/CMakeCache.txt)." >&2
  echo "       Install libvulkan-dev, or re-run with --apt." >&2
  exit 1
fi

echo ""
echo "=== CTS build complete ==="
echo "  conformance_cli:  $CTS_EXE"
find "$CTS_BUILD" -type f -name 'libconformance_test.so' -o -type f -name 'conformance_test.so' 2>/dev/null |
  head -1 | sed 's/^/  conformance_test: /'
find "$CTS_BUILD" -type f -name 'XrApiLayer_*.json' | sed 's/^/  layer manifest:   /'
echo ""
echo "=== DONE ==="

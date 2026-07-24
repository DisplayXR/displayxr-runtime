#!/bin/bash
# Acceptance test for the Linux runtime .deb (issue #781, Phase 1).
#
# Runs entirely in Docker, so it works from a macOS/Linux dev box that has no
# dpkg toolchain of its own. Two stages:
#
#   1. BUILD  — an ubuntu:24.04 builder image (build deps cached) builds the
#               in-process runtime and packages it via package_deb_linux.sh.
#   2. VERIFY — a PRISTINE ubuntu:24.04 container `apt-get install`s the .deb
#               (deps resolved from the archive) and, with NO DisplayXR env
#               vars set, runs `displayxr-cli selftest` (must PASS on
#               sim-display) and `displayxr-cli info` (must show the sim modes).
#
# This is the CI-adoptable gate: a green run proves an end user gets a working
# runtime from the .deb alone — zero configuration.
#
#   ./scripts/test_deb_linux.sh                 # build + verify
#   ./scripts/test_deb_linux.sh --verify-only   # reuse dist/*.deb, just verify
#   ./scripts/test_deb_linux.sh --rebuild-image # force-rebuild the builder image

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="displayxr-deb-builder:ubuntu2404"

VERIFY_ONLY=0
REBUILD_IMAGE=0
for arg in "$@"; do
    case "$arg" in
    --verify-only) VERIFY_ONLY=1 ;;
    --rebuild-image) REBUILD_IMAGE=1 ;;
    *) echo "Unknown option: $arg" >&2; exit 2 ;;
    esac
done

command -v docker >/dev/null 2>&1 || { echo "error: docker not found" >&2; exit 1; }

# --- 1. Builder image (deps cached across runs) ----------------------------
if [ "$REBUILD_IMAGE" = 1 ] || ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "==> Building builder image $IMAGE"
    docker build -t "$IMAGE" -f - "$ROOT" <<'DOCKERFILE'
FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates \
        binutils dpkg-dev fakeroot \
        libvulkan-dev glslang-tools libeigen3-dev libcjson-dev \
        libxcb1-dev libxcb-randr0-dev libx11-dev libx11-xcb-dev \
    && rm -rf /var/lib/apt/lists/*
DOCKERFILE
fi

# --- 2. Build the .deb (repo bind-mounted; artifacts land in dist/) --------
if [ "$VERIFY_ONLY" = 0 ]; then
    echo "==> Building .deb inside $IMAGE"
    # BUILD_DIR points at a container-local dir (NOT under the /src bind mount),
    # so a stale host build/ cache can't collide with the container's /src path
    # and the build runs on the fast container fs. DIST_DIR defaults to /src/dist
    # (mounted) so the .deb lands back on the host.
    docker run --rm -v "$ROOT":/src -w /src -e BUILD_DIR=/root/dxr-build "$IMAGE" bash -c '
        set -e
        git config --global --add safe.directory /src
        ./scripts/package_deb_linux.sh'
fi

DEB="$(ls -t "$ROOT"/dist/displayxr-runtime_*_*.deb 2>/dev/null | head -1 || true)"
[ -n "$DEB" ] || { echo "error: no dist/displayxr-runtime_*_*.deb found" >&2; exit 1; }
echo "==> Testing $(basename "$DEB")"

# --- 3. Clean-install + env-free acceptance run ----------------------------
# A pristine ubuntu:24.04 (NOT the builder) proves the Depends are complete and
# nothing leaks in from the build environment.
docker run --rm -v "$ROOT/dist":/deb:ro ubuntu:24.04 bash -c '
    set -e
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    echo "=== apt-get install ./'"$(basename "$DEB")"' ==="
    apt-get install -y -qq "/deb/'"$(basename "$DEB")"'"

    echo "=== assert ZERO DisplayXR env vars are set ==="
    if env | grep -E "^(XR_RUNTIME_JSON|XRT_PLUGIN_SEARCH_PATH)=" ; then
        echo "FAIL: a DisplayXR env var is set — the test would not prove the env-free path"; exit 1
    fi
    echo "ok — no XR_RUNTIME_JSON / XRT_PLUGIN_SEARCH_PATH"

    echo "=== installed files of interest ==="
    ls -l /etc/xdg/openxr/1/active_runtime.json /usr/lib/displayxr/plugins/
    echo "--- active_runtime.json ---"; cat /etc/xdg/openxr/1/active_runtime.json

    echo "=== displayxr-cli info (must show sim-display + its modes) ==="
    displayxr-cli info | tee /tmp/info.txt

    echo "=== displayxr-cli selftest (must PASS on sim-display) ==="
    displayxr-cli selftest

    # Content assertion: info must name the sim-display plug-in.
    grep -qi "sim-display\|Sim Display" /tmp/info.txt || {
        echo "FAIL: displayxr-cli info did not mention sim-display"; exit 1; }

    echo ""
    echo "ACCEPTANCE PASS — env-free .deb install runs the runtime on sim-display."
'

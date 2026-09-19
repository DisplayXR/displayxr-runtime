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
#
# WHY THE BUILDER IMAGE IS PINNED, AND TO WHAT.
# package_deb_linux.sh's compute_depends() derives the .deb's `Depends:` from
# the BUILD host's dpkg file database (objdump NEEDED sonames -> owning
# packages). So the builder image is not a free choice: build on a different
# release than CI and this test validates a dependency set CI never ships.
# The image therefore tracks the `Deb` job in .github/workflows/build-linux.yml,
# which is `runs-on: ubuntu-latest` — resolved to the **ubuntu-24.04** runner
# image as of 2026-09-19 (read off a live run's "Operating System / Image:"
# line, not assumed). NB the `Package` job's `container: ubuntu:26.04` is a
# DIFFERENT artifact (the tarball, which has no derived Depends at all) and is
# not what this script mirrors.
# ==> WHEN GITHUB MOVES ubuntu-latest TO 26.04, MOVE $IMAGE WITH IT. <==
# The verify stage stays on the OLDEST supported LTS on purpose: that is a
# genuine "the shipped .deb still installs on the oldest LTS we claim" check,
# and it must NOT be re-pinned in lockstep with the builder.
#
# The apt line below must also stay in lockstep with that Deb job's: anything
# it installs that this image lacks silently produces a differently-configured
# runtime here (libwayland-dev/libdbus-1-dev gate the Wayland present path +
# the #817 geometry provider; libxrandr-dev gates XRT_HAVE_XLIB_XRANDR, the
# direct-scanout present path).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Builder: must match the CI `Deb` job's runner image (see header). Bump the
# tag alongside the FROM line so a stale cached layer can't masquerade as the
# new image.
IMAGE="displayxr-deb-builder:ubuntu2404"
# Verify: the oldest LTS the .deb claims to install on — deliberately NOT
# bumped in lockstep with the builder.
VERIFY_IMAGE="ubuntu:24.04"

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
# libwayland-dev + libdbus-1-dev + libxrandr-dev must match the Deb job in
# build-linux.yml — without them this image would build a Wayland-less /
# Xrandr-less .deb and the acceptance test would not be testing the artifact
# CI ships (different feature set AND different derived Depends).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates \
        binutils dpkg-dev fakeroot \
        libvulkan-dev glslang-tools libeigen3-dev libcjson-dev \
        libxcb1-dev libxcb-randr0-dev libx11-dev libx11-xcb-dev \
        libwayland-dev libdbus-1-dev libxrandr-dev \
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
# A pristine $VERIFY_IMAGE (NOT the builder) proves the Depends are complete
# and nothing leaks in from the build environment.
echo "==> Verifying in $VERIFY_IMAGE"
docker run --rm -v "$ROOT/dist":/deb:ro "$VERIFY_IMAGE" bash -c '
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

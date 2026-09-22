#!/bin/bash
# Acceptance test for the Linux runtime .deb (issue #781, Phase 1).
#
# Runs entirely in Docker, so it works from a macOS/Linux dev box that has no
# dpkg toolchain of its own. Two stages:
#
#   1. BUILD  — an ubuntu:22.04 builder image (build deps cached) builds the
#               in-process runtime and packages it via package_deb_linux.sh.
#   2. VERIFY — a PRISTINE container of EVERY supported release (22.04, 24.04,
#               26.04) `apt-get install`s the .deb (deps resolved from that
#               release's archive) and, with NO DisplayXR env vars set, runs
#               `displayxr-cli info` + `selftest` (must PASS on sim-display).
#
# This is the CI-adoptable gate: a green run proves an end user gets a working
# runtime from the .deb alone — zero configuration.
#
#   ./scripts/test_deb_linux.sh                 # build + verify
#   ./scripts/test_deb_linux.sh --verify-only   # reuse dist/*.deb, just verify
#   ./scripts/test_deb_linux.sh --rebuild-image # force-rebuild the builder image
#
# WHY THE BUILDER IMAGE IS PINNED, AND TO WHAT (#1656).
# The .deb's glibc / libstdc++ floor is whatever the BUILD host has, and
# package_deb_linux.sh derives versioned Depends from that host with
# dpkg-shlibdeps. So the builder image is not a free choice: it must be the
# OLDEST release the package claims to install on, which is what the CI `Deb`
# job uses (`container: ubuntu:22.04`). Building on a newer one produces a
# package that apt correctly REFUSES on 22.04 — which is the v2.19.1 bug
# (GLIBC_2.38 binaries, unversioned `libc6`) turned into a hard failure, not a
# passing test. The DXR_DEB_MAX_GLIBC below makes the mismatch fail at package
# time instead.
# ==> KEEP $IMAGE EQUAL TO THE Deb JOB'S CONTAINER. <==
# The verify stage runs on EVERY supported release, oldest first, via
# scripts/verify_deb_install_linux.sh — the same script CI's DebInstall matrix
# runs. NB the `Newest` job's ubuntu:26.04 build is a different thing (compile
# coverage, no packaging) and is not what this script mirrors.
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
IMAGE="displayxr-deb-builder:ubuntu2204"
# Verify: every release the .deb claims to install on.
VERIFY_IMAGES=("ubuntu:22.04" "ubuntu:24.04" "ubuntu:26.04")

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
FROM ubuntu:22.04
ENV DEBIAN_FRONTEND=noninteractive
# libwayland-dev + libdbus-1-dev + libxrandr-dev must match the Deb job in
# build-linux.yml — without them this image would build a Wayland-less /
# Xrandr-less .deb and the acceptance test would not be testing the artifact
# CI ships (different feature set AND different derived Depends).
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build pkg-config git ca-certificates python3 \
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
    docker run --rm -v "$ROOT":/src -w /src -e BUILD_DIR=/root/dxr-build \
        -e DXR_DEB_MAX_GLIBC=2.35 "$IMAGE" bash -c '
        set -e
        git config --global --add safe.directory /src
        ./scripts/package_deb_linux.sh'
fi

DEB="$(ls -t "$ROOT"/dist/displayxr-runtime_*_*.deb 2>/dev/null | head -1 || true)"
[ -n "$DEB" ] || { echo "error: no dist/displayxr-runtime_*_*.deb found" >&2; exit 1; }
echo "==> Testing $(basename "$DEB")"

# --- 3. Clean-install + env-free acceptance run, on every supported release -
# A pristine image (NOT the builder) proves the Depends are complete, resolvable
# on that release, and that nothing leaks in from the build environment.
# scripts/verify_deb_install_linux.sh is the same script CI's DebInstall job
# runs, so a green run here predicts CI.
for img in "${VERIFY_IMAGES[@]}"; do
    echo ""
    echo "==> Verifying in $img"
    docker run --rm -v "$ROOT":/w:ro -w /w "$img" \
        ./scripts/verify_deb_install_linux.sh "dist/$(basename "$DEB")"
done

echo ""
echo "ACCEPTANCE PASS — the .deb installs and runs env-free on ${VERIFY_IMAGES[*]}."

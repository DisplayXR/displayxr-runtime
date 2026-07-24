#!/bin/bash
# Package the Linux runtime build as a Debian package (.deb) — issue #781, Phase 1.
#
#   ./scripts/package_deb_linux.sh            # build (if needed) + stage + dpkg-deb
#   ./scripts/package_deb_linux.sh --no-build # fail instead of building
#
# Output: dist/displayxr-runtime_<ver>_amd64.deb
#
# Payload (installed layout — mirrors the CI-proven tarball's bin/ + sibling
# lib/ so displayxr-cli's `$ORIGIN/../lib` RUNPATH resolves exactly as it does
# in package_linux.sh):
#   /usr/lib/displayxr/bin/displayxr-cli
#   /usr/lib/displayxr/lib/openxr_displayxr.so           (the in-process runtime)
#   /usr/lib/displayxr/plugins/DisplayXR-SimDisplay.so   (built-in fallback DP)
#   /usr/lib/displayxr/plugins/200-sim-display.json      (its discovery manifest)
#   /usr/bin/displayxr-cli -> ../lib/displayxr/bin/displayxr-cli  (PATH symlink;
#       exec'd via the symlink, ld.so still takes $ORIGIN from the real target)
#
#   postinst writes /etc/xdg/openxr/1/active_runtime.json (the Khronos loader
#   well-known path — the Linux ActiveRuntime equivalent) pointing at the
#   installed runtime .so; postrm removes it (and restores any pre-existing
#   runtime it backed up).
#
# Net effect (design in #781): an installed box needs ZERO env vars —
#   * XR_RUNTIME_JSON        -> active_runtime.json (postinst)
#   * XRT_PLUGIN_SEARCH_PATH -> the runtime's built-in default plug-in dir
#                               /usr/lib/displayxr/plugins (loader searches it
#                               when the env is unset; target_plugin_loader.c)
#
# Phase 1 scope: the IN-PROCESS runtime only. displayxr-service is intentionally
# NOT packaged — Linux out-of-process service render is not ready (#710). So the
# build is done WITHOUT --service. The vendor (Leia SR) plug-in ships its own
# .deb in a later phase; this package stays vendor-free (sim-display fallback).

set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Both overridable (inherited by the build_linux.sh call below): the .deb Docker
# test points BUILD_DIR at a container-local dir to avoid a stale host build/
# cache on the bind-mounted repo, while DIST_DIR stays on the mount so the .deb
# lands back on the host.
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
DIST_DIR="${DIST_DIR:-$ROOT/dist}"
# Ship a RELEASE runtime: matches the Release-built Leia plug-in (avoids a
# Debug/Release struct-layout skew across the plug-in↔runtime boundary — cube-hw
# finding C) and is the correct optimized artifact (Debug was 36 MB). Overridable.
export CMAKE_BUILD_TYPE="${CMAKE_BUILD_TYPE:-Release}"
export BUILD_DIR   # so the build_linux.sh child below uses the same tree + type

NO_BUILD=0
for arg in "$@"; do
    case "$arg" in
    --no-build) NO_BUILD=1 ;;
    *) echo "Unknown option: $arg (supported: --no-build)" >&2; exit 2 ;;
    esac
done

command -v dpkg-deb >/dev/null 2>&1 || {
    echo "error: dpkg-deb not found — run this on a Debian/Ubuntu host or in the" >&2
    echo "       ubuntu:24.04 container (see scripts/test_deb_linux.sh)." >&2
    exit 1
}

find_runtime() { find "$BUILD_DIR/src/xrt/targets/openxr" -maxdepth 1 -name "openxr_displayxr.so" -type f 2>/dev/null | head -1; }

if [ "$NO_BUILD" = 1 ]; then
    # Reuse an existing build — the caller guarantees it matches the current
    # checkout (e.g. a CI step that built immediately before). Fail if none.
    [ -n "$(find_runtime)" ] || {
        echo "error: --no-build set but no build under $BUILD_DIR (run scripts/build_linux.sh --no-test first)" >&2
        exit 1
    }
    echo "==> --no-build: reusing existing build under $BUILD_DIR (caller-guaranteed current)"
else
    # ALWAYS a clean build so the .deb bits match HEAD. Reusing a stale build/
    # from an earlier checkout would ship wrong code under a fresh `git describe`
    # version string — the silent correctness bug from the Suzhou Odyssey Track B
    # run (cube-hw finding A: 58039d4 bits shipped as 6afba6a → zero-config
    # discovery broke). Phase 1: IN-PROCESS runtime, no service (#710).
    echo "==> Clean build via build_linux.sh --no-test (in-process, no service)"
    rm -rf "$BUILD_DIR"
    "$ROOT/scripts/build_linux.sh" --no-test
fi

RUNTIME_SO="$(find_runtime)"
CLI_BIN="$(find "$BUILD_DIR/src/xrt/targets/cli" -maxdepth 1 -name displayxr-cli -type f | head -1)"
PLUGIN_SO="$(find "$BUILD_DIR/src/xrt/drivers" -name "DisplayXR-SimDisplay.so" -type f | head -1)"

for f in "$RUNTIME_SO" "$CLI_BIN" "$PLUGIN_SO"; do
    [ -n "$f" ] || { echo "error: missing build artifact (runtime/cli/plugin)" >&2; exit 1; }
done

# --- Version: turn `git describe` into a Debian-legal upstream version. -----
# v2.1.0 -> 2.1.0 ; v2.1.0-3-gabc123 -> 2.1.0+3.gabc123 ; dirty -> +dirty
RAW="$(git -C "$ROOT" describe --tags --always --dirty 2>/dev/null || echo 0.0.0)"
VERSION="$(echo "$RAW" | sed -e 's/^v//' -e 's/-dirty$/+dirty/' -e 's/-\([0-9]\+\)-g/+\1.g/')"
# A Debian upstream version must start with a digit; fall back if we only had a hash.
case "$VERSION" in
    [0-9]*) : ;;
    *) VERSION="0.0.0+g$VERSION" ;;
esac
# Derive the Debian arch from the build host so the label matches the binaries:
# x86 CI / release boxes -> amd64; an arm64 dev box (e.g. Apple-silicon colima)
# -> arm64. The runtime .deb targets whatever the build produced.
ARCH="$(dpkg --print-architecture 2>/dev/null || echo amd64)"
PKG="displayxr-runtime"

STAGE="$DIST_DIR/${PKG}_${VERSION}_${ARCH}"
echo "==> Staging $STAGE"
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN" \
         "$STAGE/usr/bin" \
         "$STAGE/usr/lib/displayxr/bin" \
         "$STAGE/usr/lib/displayxr/lib" \
         "$STAGE/usr/lib/displayxr/plugins"

install -m 0755 "$CLI_BIN"     "$STAGE/usr/lib/displayxr/bin/displayxr-cli"
install -m 0644 "$RUNTIME_SO"  "$STAGE/usr/lib/displayxr/lib/openxr_displayxr.so"
install -m 0644 "$PLUGIN_SO"   "$STAGE/usr/lib/displayxr/plugins/DisplayXR-SimDisplay.so"
# PATH entry — relative symlink so it stays valid regardless of install root.
ln -s ../lib/displayxr/bin/displayxr-cli "$STAGE/usr/bin/displayxr-cli"

# --- Packaged display-processor discovery manifest -------------------------
# binary_path is fixed at the installed location, so we ship the manifest as a
# real dpkg-tracked file (removed on purge) rather than generating it in postinst.
cat > "$STAGE/usr/lib/displayxr/plugins/200-sim-display.json" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "sim-display",
        "display_name": "DisplayXR Sim Display",
        "vendor":       "DisplayXR",
        "version":      "$VERSION",
        "binary_path":  "/usr/lib/displayxr/plugins/DisplayXR-SimDisplay.so",
        "probe_order":  200
    }
}
EOF
chmod 0644 "$STAGE/usr/lib/displayxr/plugins/200-sim-display.json"

# --- Depends: resolve the shared libs the binaries actually NEED to the ----
# Debian packages that provide them (works because this runs in the same
# ubuntu:24.04 base the .deb is installed into). Falls back to a conservative
# hardcoded set if objdump/dpkg aren't available.
compute_depends() {
    local sonames pkgs="" so pkg
    sonames="$(objdump -p "$RUNTIME_SO" "$CLI_BIN" "$PLUGIN_SO" 2>/dev/null \
                | awk '/NEEDED/{print $2}' | sort -u)"
    [ -n "$sonames" ] || { echo "libc6, libcjson1, libvulkan1, libx11-6, libx11-xcb1, libxcb1, libxcb-randr0"; return; }
    for so in $sonames; do
        # Resolve the soname to its owning package via dpkg's file DB. Search by
        # BARE soname (no leading '/': a leading slash makes dpkg-query treat it
        # as an absolute path and miss) — a substring match that is immune to the
        # /lib-vs-/usr/lib usr-merge split that broke `dpkg -S <ldconfig-path>`
        # (which had dropped libcjson1/libvulkan1). Keep only the line whose file
        # basename is EXACTLY the soname (so libfoo.so.1.2.3 / dev symlinks don't
        # match), then strip dpkg's ':arch' qualifier off the package name.
        # Only accept a match whose file lives in a SYSTEM linker dir (/lib,
        # /usr/lib, /lib64, ...), and never the vendor SR runtime. The Leia SR
        # runtime bundles copies of common .so's under /opt/leiasr/lib, so a bare
        # `dpkg -S <soname>` can otherwise attribute a system lib to
        # `leiasr-runtime` — the runtime .deb must NEVER Depend on the commercial
        # SR package (cube-hw finding B).
        pkg="$(dpkg -S "$so" 2>/dev/null \
               | awk -F': ' -v s="$so" '$2 ~ /^\/(usr\/)?lib(32|64)?\// {n=split($2,a,"/"); if (a[n]==s){p=$1; sub(/:.*/,"",p); if (p!="leiasr-runtime"){print p; exit}}}')"
        [ -n "$pkg" ] && pkgs="$pkgs $pkg"
    done
    # Always include libc6; dedupe; comma-join.
    echo "libc6 $pkgs" | tr ' ' '\n' | sed '/^$/d' | sort -u | paste -sd, - | sed 's/,/, /g'
}
DEPENDS="$(compute_depends)"
echo "==> Depends: $DEPENDS"

INSTALLED_KB="$(du -sk "$STAGE/usr" | cut -f1)"

# --- control ---------------------------------------------------------------
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Section: libs
Priority: optional
Architecture: $ARCH
Depends: $DEPENDS
Installed-Size: $INSTALLED_KB
Maintainer: The DisplayXR Project <noreply@displayxr.dev>
Homepage: https://github.com/DisplayXR/displayxr-runtime
Description: DisplayXR OpenXR runtime for 3D displays (in-process, sim-display)
 Lightweight standalone OpenXR runtime purpose-built for 3D displays. This
 package ships the in-process runtime, the displayxr-cli diagnostic tool, and
 the vendor-neutral sim-display display processor as the built-in fallback.
 .
 After install the box needs no environment variables: the OpenXR ActiveRuntime
 is registered at /etc/xdg/openxr/1/active_runtime.json and the runtime's
 built-in plug-in dir (/usr/lib/displayxr/plugins) is searched automatically.
 .
 A vendor display plug-in (e.g. Leia SR) installs alongside with a lower
 probe_order and claims the display automatically when present; otherwise
 sim-display drives apps. The out-of-process service is not included on Linux
 yet (see issue #710).
EOF

# --- maintainer scripts ----------------------------------------------------
cat > "$STAGE/DEBIAN/postinst" <<'EOF'
#!/bin/sh
# Register DisplayXR as the OpenXR ActiveRuntime (the XR_RUNTIME_JSON dev env's
# installed replacement). Well-known Khronos loader path on Linux.
set -e

CONF_DIR="/etc/xdg/openxr/1"
ACTIVE="$CONF_DIR/active_runtime.json"
BACKUP="$ACTIVE.pre-displayxr.bak"
LIB="/usr/lib/displayxr/lib/openxr_displayxr.so"

case "$1" in
configure)
    mkdir -p "$CONF_DIR"
    # Preserve a pre-existing non-DisplayXR runtime so uninstall can restore it.
    if [ -f "$ACTIVE" ] && ! grep -q "openxr_displayxr.so" "$ACTIVE" 2>/dev/null; then
        cp -a "$ACTIVE" "$BACKUP" || true
        echo "displayxr-runtime: backed up existing OpenXR active runtime to $BACKUP"
    fi
    cat > "$ACTIVE" <<JSON
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "DisplayXR",
        "library_path": "$LIB"
    }
}
JSON
    echo "displayxr-runtime: OpenXR ActiveRuntime -> $ACTIVE"
    echo "displayxr-runtime: verify with  displayxr-cli selftest"
    ;;
esac

exit 0
EOF

cat > "$STAGE/DEBIAN/postrm" <<'EOF'
#!/bin/sh
# Undo the ActiveRuntime registration our postinst wrote (only if it still
# points at us), restoring any runtime we backed up.
set -e

CONF_DIR="/etc/xdg/openxr/1"
ACTIVE="$CONF_DIR/active_runtime.json"
BACKUP="$ACTIVE.pre-displayxr.bak"

case "$1" in
remove|purge|upgrade|deconfigure)
    if [ -f "$ACTIVE" ] && grep -q "openxr_displayxr.so" "$ACTIVE" 2>/dev/null; then
        rm -f "$ACTIVE"
        if [ -f "$BACKUP" ]; then
            mv "$BACKUP" "$ACTIVE"
            echo "displayxr-runtime: restored previous OpenXR active runtime"
        fi
    fi
    ;;
esac

exit 0
EOF

chmod 0755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm"

# --- build the .deb --------------------------------------------------------
mkdir -p "$DIST_DIR"
DEB="$DIST_DIR/${PKG}_${VERSION}_${ARCH}.deb"
# fakeroot so the payload is owned root:root inside the archive even when built
# unprivileged (dpkg-deb warns otherwise); fall back to plain build if absent.
if command -v fakeroot >/dev/null 2>&1; then
    fakeroot dpkg-deb --build --root-owner-group "$STAGE" "$DEB"
else
    dpkg-deb --build --root-owner-group "$STAGE" "$DEB"
fi

echo ""
echo "==> $DEB"
dpkg-deb --info "$DEB" | sed 's/^/    /'
echo "    --- contents ---"
dpkg-deb --contents "$DEB" | sed 's/^/    /'

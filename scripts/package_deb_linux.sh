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
#   /usr/lib/displayxr/bin/displayxr-service             (out-of-process compositor, #1744)
#   /usr/lib/displayxr/lib/openxr_displayxr.so           (the HYBRID runtime: in-process
#       for ordinary apps, IPC to displayxr-service for XR_DXR_weave present-owners
#       and workspace controllers — the Windows/macOS shape)
#   /usr/lib/displayxr/plugins/DisplayXR-SimDisplay.so   (built-in fallback DP)
#   /usr/lib/displayxr/plugins/200-sim-display.json      (its discovery manifest)
#   /usr/bin/displayxr-cli -> ../lib/displayxr/bin/displayxr-cli  (PATH symlink;
#       exec'd via the symlink, ld.so still takes $ORIGIN from the real target)
#   /usr/bin/displayxr-service -> ../lib/displayxr/bin/displayxr-service
#   /usr/lib/systemd/user/displayxr.socket + displayxr.service
#       systemd USER units (scripts/linux/systemd/): the socket is enabled for
#       every user (postinst: systemctl --global enable) and socket-activates
#       the service on the first client that dials it, so a user who never runs
#       a present-owner / workspace client never starts it; the service exits
#       30 s after its last client leaves (IPC_EXIT_WHEN_IDLE).
#   /usr/share/gnome-shell/extensions/window-geometry@displayxr.org/
#       the GNOME Shell extension from contrib/gnome-shell/ (window geometry for
#       windowed Wayland weaving; capture exclusion for transparency)
#   /usr/lib/displayxr/bin/displayxr-gnome-extension-enable
#   /etc/xdg/autostart/displayxr-gnome-extension-enable.desktop
#       enable the extension ONCE per user at their GNOME login, never over an
#       explicit opt-out (the script's header has the rules, and why a dconf
#       default alone would not reach most users)
#
#   Provides/Conflicts/Replaces the virtual package
#   displayxr-window-geometry-publisher: exactly one installed package may own
#   the extension's D-Bus name (docs/specs/runtime/wayland-window-geometry.md §4).
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
# Build: `build_linux.sh --hybrid` (#1744). Until then the package shipped the
# in-process-only runtime and no service, so every XR_DXR_weave call on an
# installed box returned XR_ERROR_FEATURE_UNSUPPORTED ("the weave service is
# only available on the out-of-process (service) path") — the DisplayXR browser
# could not weave. The vendor (Leia SR) plug-in ships its own .deb; this package
# stays vendor-free (sim-display fallback).

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
    echo "       ubuntu:22.04 container (see scripts/test_deb_linux.sh)." >&2
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
    # discovery broke). --hybrid: the service + the hybrid runtime (#1744).
    echo "==> Clean build via build_linux.sh --hybrid --no-test (hybrid runtime + displayxr-service)"
    rm -rf "$BUILD_DIR"
    "$ROOT/scripts/build_linux.sh" --hybrid --no-test
fi

RUNTIME_SO="$(find_runtime)"
CLI_BIN="$(find "$BUILD_DIR/src/xrt/targets/cli" -maxdepth 1 -name displayxr-cli -type f | head -1)"
PLUGIN_SO="$(find "$BUILD_DIR/src/xrt/drivers" -name "DisplayXR-SimDisplay.so" -type f | head -1)"
# `|| true`: no targets/service dir at all in a non-service build; the check
# below turns that into the real error message.
SERVICE_BIN="$(find "$BUILD_DIR/src/xrt/targets/service" -maxdepth 1 -name displayxr-service -type f 2>/dev/null | head -1 || true)"

for f in "$RUNTIME_SO" "$CLI_BIN" "$PLUGIN_SO"; do
    [ -n "$f" ] || { echo "error: missing build artifact (runtime/cli/plugin)" >&2; exit 1; }
done
[ -n "$SERVICE_BIN" ] || {
    echo "error: displayxr-service not built — the .deb ships it (#1744). Build with" >&2
    echo "       scripts/build_linux.sh --hybrid (--no-build reuses whatever is in $BUILD_DIR)." >&2
    exit 1
}

# The runtime must be the HYBRID one (#1744). An in-process-only .so has no IPC
# client, so present-owners silently lose XR_DXR_weave; an IPC-only one (plain
# --service) would push EVERY app through the service. The hybrid router's
# lifecycle log line is the marker that it is compiled in. (A here-string,
# not `echo | grep -q`: grep -q exits at the first match, the echo takes
# SIGPIPE, and under pipefail the pipeline then reports failure.)
RUNTIME_STRINGS="$(strings -a "$RUNTIME_SO" 2>/dev/null || true)"
if ! grep -q 'Hybrid mode: XR_DXR_weave present-owner' <<<"$RUNTIME_STRINGS"; then
    echo "error: $RUNTIME_SO is not the hybrid runtime (no present-owner IPC routing)." >&2
    echo "       Build with scripts/build_linux.sh --hybrid." >&2
    exit 1
fi

SYSTEMD_SRC="$ROOT/scripts/linux/systemd"
for f in displayxr.socket displayxr.service.in; do
    [ -f "$SYSTEMD_SRC/$f" ] || { echo "error: missing $SYSTEMD_SRC/$f (systemd user units)" >&2; exit 1; }
done

# The GNOME Shell extension is source, not a build artifact: ship it from the
# checkout. docs/specs/runtime/wayland-window-geometry.md §4 is the packaging
# contract this follows (system path, virtual-package trio, login notice).
EXT_UUID="window-geometry@displayxr.org"
EXT_SRC="$ROOT/contrib/gnome-shell/$EXT_UUID"
EXT_ENABLE="$ROOT/scripts/linux/displayxr-gnome-extension-enable"
# Both entry-point forms ship: extension.js is the GNOME 45+ ES module (the
# slot GNOME reads), extension-gnome42.js the GNOME 40-44 legacy one, lib.js
# the logic they share. The per-user login script materialises the legacy form
# on an older shell -- Ubuntu 22.04, which this .deb installs on, is GNOME 42.
EXT_FILES="extension.js extension-gnome42.js lib.js metadata.json"
for f in $EXT_FILES; do
    [ -f "$EXT_SRC/$f" ] || { echo "error: missing $EXT_SRC/$f (GNOME Shell extension payload)" >&2; exit 1; }
done
[ -f "$EXT_ENABLE" ] || { echo "error: missing $EXT_ENABLE" >&2; exit 1; }

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
         "$STAGE/usr/lib/displayxr/plugins" \
         "$STAGE/usr/share/gnome-shell/extensions/$EXT_UUID" \
         "$STAGE/usr/lib/systemd/user" \
         "$STAGE/etc/xdg/autostart"

install -m 0755 "$CLI_BIN"     "$STAGE/usr/lib/displayxr/bin/displayxr-cli"
install -m 0755 "$SERVICE_BIN" "$STAGE/usr/lib/displayxr/bin/displayxr-service"
install -m 0644 "$RUNTIME_SO"  "$STAGE/usr/lib/displayxr/lib/openxr_displayxr.so"
install -m 0644 "$PLUGIN_SO"   "$STAGE/usr/lib/displayxr/plugins/DisplayXR-SimDisplay.so"
# PATH entry — relative symlink so it stays valid regardless of install root.
ln -s ../lib/displayxr/bin/displayxr-cli "$STAGE/usr/bin/displayxr-cli"
ln -s ../lib/displayxr/bin/displayxr-service "$STAGE/usr/bin/displayxr-service"

# --- systemd user units: socket-activated service (#1744) -------------------
# Why socket activation (vs. an always-on user service or an XDG autostart
# entry): the service owns a Vulkan device and the display processor, and most
# users never run a client that needs it. With the socket it costs nothing
# until a present-owner / workspace client dials
# $XDG_RUNTIME_DIR/displayxr_comp_ipc, and it exits when idle. It needs no
# terminal and no libsystemd: the service reads the LISTEN_FDS hand-off itself.
install -m 0644 "$SYSTEMD_SRC/displayxr.socket" "$STAGE/usr/lib/systemd/user/displayxr.socket"
sed 's|@SERVICE_BIN@|/usr/lib/displayxr/bin/displayxr-service|g' "$SYSTEMD_SRC/displayxr.service.in" \
    >"$STAGE/usr/lib/systemd/user/displayxr.service"
chmod 0644 "$STAGE/usr/lib/systemd/user/displayxr.service"

# --- GNOME Shell extension + the per-user enable at login -------------------
for f in $EXT_FILES; do
    install -m 0644 "$EXT_SRC/$f" "$STAGE/usr/share/gnome-shell/extensions/$EXT_UUID/"
done
install -m 0755 "$EXT_ENABLE" "$STAGE/usr/lib/displayxr/bin/displayxr-gnome-extension-enable"
# Deliberately NOT a conffile (not in DEBIAN/conffiles): `apt remove` deletes it
# with the script it runs instead of leaving an entry behind (TryExec also
# guards a dangling one).
cat > "$STAGE/etc/xdg/autostart/displayxr-gnome-extension-enable.desktop" <<DESKTOP
[Desktop Entry]
Type=Application
Name=DisplayXR GNOME Shell extension
Comment=Enables the DisplayXR window-geometry extension once per user, never over an opt-out
Exec=/usr/lib/displayxr/bin/displayxr-gnome-extension-enable
TryExec=/usr/lib/displayxr/bin/displayxr-gnome-extension-enable
OnlyShowIn=GNOME;
NoDisplay=true
X-GNOME-Autostart-enabled=true
DESKTOP
chmod 0644 "$STAGE/etc/xdg/autostart/displayxr-gnome-extension-enable.desktop"

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

# --- Depends (#1656) ---------------------------------------------------------
# ONE .deb for Ubuntu 22.04, 24.04 and 26.04. Two things make that true, and
# both are checked here rather than trusted to the build host:
#
#   1. The glibc / libstdc++ floor is the BUILD host's. The release .deb is
#      built on the OLDEST supported release (the CI Deb job runs in an
#      ubuntu:22.04 container), and dpkg-shlibdeps turns the symbol versions
#      the binaries actually reference into versioned Depends — so a package
#      built on a newer host (a dev box, or a runner image bump) says
#      `libc6 (>= 2.38)` and apt REFUSES it on 22.04 instead of installing a
#      runtime that then fails at dlopen() (v2.19.1 shipped unversioned
#      `libc6` with a GLIBC_2.38 / GLIBCXX_3.4.31 floor).
#      DXR_DEB_MAX_GLIBC (CI sets 2.35 = Ubuntu 22.04) turns a floor above the
#      oldest release into a hard error at package time.
#   2. Every DT_NEEDED soname must be on STABLE_SONAMES: libraries whose
#      package name is the same on all three releases. A new system library
#      fails the build here — its package may be renamed on another release
#      (t64 transition, soname bumps), which would make the .deb uninstallable
#      there. Adding one is a claim about all three releases; CI's DebInstall
#      matrix (scripts/verify_deb_install_linux.sh) proves it.
#
# (libdbus-1-3: the #817 Wayland window-geometry provider. NOT
# libwayland-client0 — no wl_* symbol is referenced, so --as-needed drops
# -lwayland-client and libwayland-dev is a build-time-only dependency.)
STABLE_SONAMES=(
    libc.so.6 libm.so.6 libdl.so.2 libpthread.so.0 librt.so.1 ld-linux-x86-64.so.2
    libstdc++.so.6 libgcc_s.so.1
    libvulkan.so.1                  # libvulkan1
    libcjson.so.1                   # libcjson1
    libdbus-1.so.3                  # libdbus-1-3
    libudev.so.1                    # libudev1
    libX11.so.6 libX11-xcb.so.1     # libx11-6, libx11-xcb1
    libxcb.so.1 libxcb-randr.so.0   # libxcb1, libxcb-randr0
    libXrandr.so.2                  # libxrandr2
)
ELF_FILES=("$RUNTIME_SO" "$CLI_BIN" "$SERVICE_BIN" "$PLUGIN_SO")

command -v dpkg-shlibdeps >/dev/null 2>&1 || {
    echo "error: dpkg-shlibdeps not found — install dpkg-dev." >&2
    exit 1
}

bad=""
for so in $(objdump -p "${ELF_FILES[@]}" | awk '/NEEDED/{print $2}' | sort -u); do
    ok=0
    for s in "${STABLE_SONAMES[@]}"; do [ "$so" = "$s" ] && ok=1 && break; done
    [ "$ok" = 1 ] || bad="$bad $so"
done
if [ -n "$bad" ]; then
    echo "error: DT_NEEDED on system libraries not known to share a package name across" >&2
    echo "       Ubuntu 22.04/24.04/26.04:$bad" >&2
    echo "       Drop the dependency, link it statically, or add it to STABLE_SONAMES once" >&2
    echo "       CI's DebInstall matrix proves the package exists under that name on all three." >&2
    exit 1
fi

# dpkg-shlibdeps wants a debian/control to read; give it a throwaway one. It
# resolves each soname through the linker search path and the owning package's
# shlibs/symbols files, so the result carries real version floors.
SHLIBS_TMP="$(mktemp -d)"
mkdir -p "$SHLIBS_TMP/debian"
printf 'Source: %s\n\nPackage: %s\nArchitecture: any\n' "$PKG" "$PKG" >"$SHLIBS_TMP/debian/control"
DEPENDS="$(cd "$SHLIBS_TMP" && dpkg-shlibdeps -O "${ELF_FILES[@]}" | sed -n 's/^shlibs:Depends=//p')"
rm -rf "$SHLIBS_TMP"
[ -n "$DEPENDS" ] || { echo "error: dpkg-shlibdeps produced no Depends." >&2; exit 1; }
# The Leia SR runtime bundles copies of common .so's under /opt/leiasr/lib; on
# an SR box whose linker path reaches them, the owner would come out as
# leiasr-runtime. The runtime .deb must NEVER Depend on the commercial SR
# package (cube-hw finding B).
if echo "$DEPENDS" | grep -q leiasr; then
    echo "error: Depends names the vendor SR package: $DEPENDS" >&2
    echo "       (an SR runtime's bundled lib shadowed a system one on this host's linker path)" >&2
    exit 1
fi
echo "==> Depends: $DEPENDS"

GLIBC_FLOOR="$(objdump -T "${ELF_FILES[@]}" | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)"
GLIBCXX_FLOOR="$(objdump -T "${ELF_FILES[@]}" | grep -o 'GLIBCXX_[0-9.]*' | sed 's/GLIBCXX_//' | sort -uV | tail -1)"
echo "==> glibc floor: GLIBC_$GLIBC_FLOOR, GLIBCXX_$GLIBCXX_FLOOR"
if [ -n "${DXR_DEB_MAX_GLIBC:-}" ] &&
    [ "$(printf '%s\n%s\n' "$GLIBC_FLOOR" "$DXR_DEB_MAX_GLIBC" | sort -V | tail -1)" != "$DXR_DEB_MAX_GLIBC" ]; then
    echo "error: the binaries need GLIBC_$GLIBC_FLOOR, above DXR_DEB_MAX_GLIBC=$DXR_DEB_MAX_GLIBC" >&2
    echo "       (the oldest supported release). Build the .deb on that release." >&2
    exit 1
fi

# The runtime must link libdbus-1: it is the Wayland window-geometry provider
# (#817) that consumes the GNOME Shell extension this package ships. It is an
# OPTIONAL CMake dependency, so a build host without libdbus-1-dev silently
# drops the provider and libdbus-1-3 with it — v2.17.1 shipped exactly that
# (Depends had no libdbus-1-3; fixed in CI by #1565). Refuse such a package here
# rather than rely on every build host's apt line. (PipeWire is deliberately
# absent: the desktop capture lives in the vendor plug-in, not the runtime.)
# (Captured first: `objdump | grep -q` under pipefail can fail on SIGPIPE.)
RUNTIME_NEEDED="$(objdump -p "$RUNTIME_SO" 2>/dev/null | awk '/NEEDED/{print $2}')"
if ! echo "$RUNTIME_NEEDED" | grep -qx 'libdbus-1.so.3'; then
    echo "error: $RUNTIME_SO does not link libdbus-1 — the Wayland window-geometry provider" >&2
    echo "       was compiled out. Install libdbus-1-dev and rebuild." >&2
    exit 1
fi

# The runtime must carry the native-Wayland present path. Unlike libdbus this
# cannot be checked with NEEDED: the path makes no libwayland-client call (the
# app owns the Wayland connection; the runtime only calls
# vkCreateWaylandSurfaceKHR), so the linker rightly drops that library and the
# package correctly has no libwayland Depends. What proves the path is compiled
# in is the extension it advertises and the VkInstance extension it requests.
"$ROOT/scripts/check_linux_runtime_wayland.sh" "$RUNTIME_SO" || exit 1

INSTALLED_KB="$(du -sk "$STAGE/usr" | cut -f1)"

# --- control ---------------------------------------------------------------
cat > "$STAGE/DEBIAN/control" <<EOF
Package: $PKG
Version: $VERSION
Section: libs
Priority: optional
Architecture: $ARCH
Depends: $DEPENDS
Provides: displayxr-window-geometry-publisher
Conflicts: displayxr-window-geometry-publisher
Replaces: displayxr-window-geometry-publisher
Installed-Size: $INSTALLED_KB
Maintainer: The DisplayXR Project <noreply@displayxr.dev>
Homepage: https://github.com/DisplayXR/displayxr-runtime
Description: DisplayXR OpenXR runtime for 3D displays (sim-display)
 Lightweight standalone OpenXR runtime purpose-built for 3D displays. This
 package ships the runtime, the displayxr-service out-of-process compositor,
 the displayxr-cli diagnostic tool, and the vendor-neutral sim-display display
 processor as the built-in fallback.
 .
 After install the box needs no environment variables: the OpenXR ActiveRuntime
 is registered at /etc/xdg/openxr/1/active_runtime.json and the runtime's
 built-in plug-in dir (/usr/lib/displayxr/plugins) is searched automatically.
 .
 A vendor display plug-in (e.g. Leia SR) installs alongside with a lower
 probe_order and claims the display automatically when present; otherwise
 sim-display drives apps.
 .
 Ordinary apps run in-process. Apps that need the out-of-process path (the
 XR_DXR_weave present-owners such as the DisplayXR browser, and workspace
 controllers) connect to displayxr-service, which a systemd user socket
 (displayxr.socket, enabled for every user) starts on demand; it exits when
 its last client leaves.
 .
 It also installs the GNOME Shell extension window-geometry@displayxr.org,
 which windowed weaving under Wayland and transparent apps on a 3D panel both
 need. It is enabled for each user at their next GNOME login, except for a
 user who has disabled it, and takes effect from that login.
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

# Run `systemctl --user <args>` in every user manager running right now
# (--machine=USER@ needs systemd >= 248; Ubuntu 22.04 has 249). Never fails.
dxr_each_user_manager() {
    command -v loginctl >/dev/null 2>&1 || return 0
    for dxr_user in $(loginctl list-users --no-legend 2>/dev/null | awk '{print $2}'); do
        dxr_uid="$(id -u "$dxr_user" 2>/dev/null)" || continue
        [ -d "/run/user/$dxr_uid/systemd" ] || continue
        timeout 10 systemctl --user --machine="$dxr_user@" "$@" >/dev/null 2>&1 || true
    done
}

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

    # displayxr-service (#1744): enable the socket for every user (a symlink
    # under /etc/systemd/user — works offline), then start it in the user
    # managers already running so nobody has to log out first. Best effort
    # throughout: a box without systemd (a container) just has no socket, and
    # clients then stay in-process.
    if command -v systemctl >/dev/null 2>&1; then
        systemctl --global enable displayxr.socket >/dev/null 2>&1 || true
        dxr_each_user_manager daemon-reload
        dxr_each_user_manager start displayxr.socket
        echo "displayxr-runtime: displayxr-service is socket-activated per user (displayxr.socket)"
        echo "  check with  systemctl --user status displayxr.socket"
    fi
    # Required notice (wayland-window-geometry.md §4): until the user logs in
    # again the extension is absent and every consumer silently falls back
    # (display-scoped weaving, silhouette transparency).
    echo ""
    echo "displayxr-runtime: GNOME Shell extension window-geometry@displayxr.org installed"
    echo "  in /usr/share/gnome-shell/extensions/. It is enabled for every user at their"
    echo "  next GNOME login, except a user who has disabled it. It takes effect only"
    echo "  after you LOG OUT AND BACK IN: a Wayland session cannot reload GNOME Shell,"
    echo "  and the same applies when this package updates the extension."
    echo "  To turn it back on after disabling it:"
    echo "      gnome-extensions enable window-geometry@displayxr.org"
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

cat > "$STAGE/DEBIAN/prerm" <<'EOF'
#!/bin/sh
# Before the unit files go away: stop displayxr-service in running user
# sessions and drop the global enablement (#1744). NOT on upgrade: a live
# service keeps serving its clients with the old binary and exits when idle,
# and the next connect starts the new one — stopping it here would cut off a
# running browser mid-session.
set -e

# Run `systemctl --user <args>` in every user manager running right now
# (--machine=USER@ needs systemd >= 248; Ubuntu 22.04 has 249). Never fails.
dxr_each_user_manager() {
    command -v loginctl >/dev/null 2>&1 || return 0
    for dxr_user in $(loginctl list-users --no-legend 2>/dev/null | awk '{print $2}'); do
        dxr_uid="$(id -u "$dxr_user" 2>/dev/null)" || continue
        [ -d "/run/user/$dxr_uid/systemd" ] || continue
        timeout 10 systemctl --user --machine="$dxr_user@" "$@" >/dev/null 2>&1 || true
    done
}

case "$1" in
remove|deconfigure)
    if command -v systemctl >/dev/null 2>&1; then
        dxr_each_user_manager stop displayxr.socket displayxr.service
        systemctl --global disable displayxr.socket >/dev/null 2>&1 || true
    fi
    ;;
esac

exit 0
EOF

chmod 0755 "$STAGE/DEBIAN/postinst" "$STAGE/DEBIAN/postrm" "$STAGE/DEBIAN/prerm"

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

#!/usr/bin/env bash
# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0
#
# Install-verify the runtime .deb on a CLEAN Ubuntu release (#1656). Run it as
# root inside a pristine ubuntu:<release> container — CI's DebInstall job does
# this for 22.04, 24.04 and 26.04; locally:
#
#   docker run --rm -v "$PWD:/w" -w /w ubuntu:22.04 \
#     ./scripts/verify_deb_install_linux.sh dist/displayxr-runtime_*_amd64.deb \
#                                           [dist/displayxr-runtime-linux-*.tar.gz]
#
# Fails if:
#   * apt cannot resolve the package's Depends from that release's archive
#     (installed with --no-install-recommends: Depends alone must be enough —
#     an unknown package name or an unsatisfiable floor such as
#     `libc6 (>= 2.38)` on 22.04 fails here);
#   * a Depends / Recommends / Suggests name does not exist on that release;
#   * `ldd -r` on any installed ELF reports a missing library, symbol or symbol
#     version (a glibc / libstdc++ floor above the release);
#   * `displayxr-cli selftest` fails with NO DisplayXR env vars set: it loads
#     the installed runtime's in-process instance + the packaged sim-display
#     plug-in through the default discovery path — no GPU, window or display;
#   * displayxr-service (#1744) or its systemd user units are missing, or the
#     installed service fails the headless smoke (scripts/smoke_service_linux.sh):
#     detached start with no terminal, an IPC handshake, clean SIGTERM,
#     stale-socket recovery, and socket activation + exit-when-idle.
#   * the DisplayXR Dashboard is missing (binary, PATH symlink, menu entry,
#     icon) or `displayxr-dashboard --version` fails: the self-contained .NET
#     host must start on the release with no .NET runtime, no ICU and no display
#     server (`--version` exits before any window is created).
#
# With the optional tarball (scripts/package_linux.sh), its ELFs are also checked with
# `ldd -r`, but only for symbol-VERSION errors (the glibc-floor class): the
# tarball has no dependency metadata, so a library its user has not installed
# is not a packaging defect.
set -euo pipefail

[ "$#" -ge 1 ] || { echo "usage: $0 <displayxr-runtime.deb> [displayxr-runtime-linux.tar.gz]" >&2; exit 2; }
[ "$(id -u)" = 0 ] || { echo "error: run as root in a throwaway container." >&2; exit 2; }

DEB="$(readlink -f "$1")"
TARBALL="${2:+$(readlink -f "$2")}"
. /etc/os-release
echo "==> $PRETTY_NAME: installing ${DEB##*/}"

export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends "$DEB"

PKG=displayxr-runtime
dpkg-query -W -f='==> installed ${Package} ${Version}\n    Depends: ${Depends}\n' "$PKG"

fail=0

# Every Depends / Recommends / Suggests alternative must name a real package
# here (apt already proved Depends; this also covers the optional fields).
for field in Depends Recommends Suggests; do
    for p in $(dpkg-query -W -f="\${$field}" "$PKG" | tr ',|' '\n\n' | sed 's/(.*)//; s/:any//; s/ //g' | sed '/^$/d'); do
        if apt-cache show "$p" >/dev/null 2>&1; then
            echo "    $field $p: available"
        else
            echo "error: $field '$p' does not exist on $PRETTY_NAME." >&2
            fail=1
        fi
    done
done

# ldd -r on every ELF the package installed (found by content, so a newly
# shipped binary is covered without editing this list).
elf_files() {
    local f
    for f in "$@"; do
        [ -f "$f" ] && [ ! -L "$f" ] || continue
        [ "$(head -c 4 "$f" | od -An -c | tr -d ' ')" = '177ELF' ] && echo "$f"
    done
}
mapfile -t ELFS < <(elf_files $(dpkg -L "$PKG"))
[ "${#ELFS[@]}" -ge 3 ] || { echo "error: expected >= 3 ELF files in $PKG, found ${#ELFS[@]}." >&2; exit 1; }
for elf in "${ELFS[@]}"; do
    out="$(ldd -r "$elf" 2>&1)" || true
    if grep -E 'not found|undefined symbol' <<<"$out"; then
        echo "error: unresolved dependency in $elf" >&2
        fail=1
    else
        echo "    ldd -r $elf: ok"
    fi
done

echo "=== displayxr-cli selftest (env-free, sim-display) ==="
unset XR_RUNTIME_JSON XRT_PLUGIN_SEARCH_PATH
displayxr-cli info || fail=1
displayxr-cli selftest || { echo "error: displayxr-cli selftest failed on $PRETTY_NAME." >&2; fail=1; }

# --- displayxr-service (#1744) ----------------------------------------------
# Shipped since the .deb stopped being in-process-only: without it an
# XR_DXR_weave present-owner (the DisplayXR browser) cannot weave at all.
echo "=== displayxr-service: payload + systemd user units"
SVC=/usr/lib/displayxr/bin/displayxr-service
UNIT_DIR=/usr/lib/systemd/user
[ -x "$SVC" ] || { echo "error: $SVC not installed" >&2; fail=1; }
[ "$(readlink -f /usr/bin/displayxr-service 2>/dev/null)" = "$SVC" ] ||
    { echo "error: /usr/bin/displayxr-service does not resolve to $SVC" >&2; fail=1; }
for u in displayxr.socket displayxr.service; do
    [ -f "$UNIT_DIR/$u" ] || { echo "error: $UNIT_DIR/$u not installed" >&2; fail=1; }
done
# The unit must start the binary the package installed, on the path clients
# dial ($XDG_RUNTIME_DIR/displayxr_comp_ipc = XRT_IPC_MSG_SOCK_FILENAME).
grep -qx "ExecStart=$SVC" "$UNIT_DIR/displayxr.service" 2>/dev/null ||
    { echo "error: displayxr.service ExecStart is not $SVC" >&2; fail=1; }
grep -qx 'ListenStream=%t/displayxr_comp_ipc' "$UNIT_DIR/displayxr.socket" 2>/dev/null ||
    { echo "error: displayxr.socket does not listen on %t/displayxr_comp_ipc" >&2; fail=1; }
[ "$fail" = 0 ] && echo "    service + units installed"

echo "=== displayxr-service: headless smoke (installed binaries, sim-display)"
"$(dirname "$(readlink -f "$0")")/smoke_service_linux.sh" ||
    { echo "error: displayxr-service smoke failed on $PRETTY_NAME." >&2; fail=1; }

# --- DisplayXR Dashboard ------------------------------------------------------
echo "=== displayxr-dashboard: payload + --version (no display server)"
DASH=/usr/lib/displayxr/bin/displayxr-dashboard
[ -x "$DASH" ] || { echo "error: $DASH not installed" >&2; fail=1; }
[ "$(readlink -f /usr/bin/displayxr-dashboard 2>/dev/null)" = "$DASH" ] ||
    { echo "error: /usr/bin/displayxr-dashboard does not resolve to $DASH" >&2; fail=1; }
grep -qx "Exec=$DASH" /usr/share/applications/displayxr-dashboard.desktop 2>/dev/null ||
    { echo "error: /usr/share/applications/displayxr-dashboard.desktop missing or not Exec=$DASH" >&2; fail=1; }
[ -f /usr/share/icons/hicolor/256x256/apps/displayxr-dashboard.png ] ||
    { echo "error: the dashboard's 256 px icon is not installed" >&2; fail=1; }
# Unset DISPLAY / WAYLAND_DISPLAY so a stray one cannot mask a --version that
# reaches for a window. The bundle extracts its natives under $HOME/.net.
if out="$(env -u DISPLAY -u WAYLAND_DISPLAY HOME="${HOME:-/root}" displayxr-dashboard --version 2>&1)" &&
    grep -q '^displayxr-dashboard ' <<<"$out"; then
    echo "    $out"
else
    echo "$out" >&2
    echo "error: displayxr-dashboard --version failed on $PRETTY_NAME." >&2
    fail=1
fi

if [ -n "$TARBALL" ]; then
    echo "=== tarball ${TARBALL##*/}: glibc / libstdc++ floor ==="
    T="$(mktemp -d)"
    tar -C "$T" -xzf "$TARBALL"
    mapfile -t TELFS < <(elf_files $(find "$T" -type f))
    [ "${#TELFS[@]}" -ge 3 ] || { echo "error: expected >= 3 ELF files in the tarball, found ${#TELFS[@]}." >&2; exit 1; }
    for elf in "${TELFS[@]}"; do
        out="$(ldd -r "$elf" 2>&1)" || true
        if grep -E "version .* not found" <<<"$out"; then
            echo "error: ${elf#"$T"/} needs a newer glibc / libstdc++ than $PRETTY_NAME has" >&2
            fail=1
        else
            echo "    ${elf#"$T"/}: no missing symbol versions"
        fi
    done
    # The tarball's dashboard must start here too (the tarball has no
    # Depends, but its .NET host needs nothing beyond libc / libstdc++).
    TDASH="$(find "$T" -path '*/bin/displayxr-dashboard' -type f | head -1)"
    if [ -z "$TDASH" ]; then
        echo "error: the tarball has no bin/displayxr-dashboard" >&2
        fail=1
    elif out="$(env -u DISPLAY -u WAYLAND_DISPLAY "$TDASH" --version 2>&1)"; then
        echo "    tarball dashboard: $out"
    else
        echo "$out" >&2
        echo "error: the tarball's displayxr-dashboard --version failed on $PRETTY_NAME." >&2
        fail=1
    fi
    rm -rf "$T"
fi

[ "$fail" = 0 ] || { echo "==> FAIL on $PRETTY_NAME" >&2; exit 1; }
echo "==> PASS on $PRETTY_NAME"

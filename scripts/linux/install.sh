#!/bin/bash
# DisplayXR runtime — Linux installer (#705, v1: runtime + sim_display, user-level).
#
# Run from the unpacked tarball root. Default is a USER-LEVEL install — no root:
#   ./install.sh
#     runtime + plug-in     -> $XDG_DATA_HOME/displayxr            (~/.local/share/displayxr)
#     OpenXR ActiveRuntime  -> $XDG_CONFIG_HOME/openxr/1/active_runtime.json
#     DP discovery manifest -> $XDG_DATA_HOME/DisplayXR/DisplayProcessors/200-sim-display.json
#     systemd --user unit   -> $XDG_CONFIG_HOME/systemd/user/displayxr.service (if service present)
#
#   sudo ./install.sh --system
#     runtime + plug-in     -> /usr/local/{bin,lib}
#     OpenXR ActiveRuntime  -> /etc/xdg/openxr/1/active_runtime.json
#     DP discovery manifest -> /usr/local/share/displayxr/DisplayProcessors/200-sim-display.json
#     (no systemd unit in system mode, v1 — start displayxr-service per user)
#
# The DisplayProcessors directory is a SHARED discovery root: a vendor plug-in
# installer (e.g. Leia SR) drops its own .so + <probe-order>-<id>.json next to
# the sim-display one; lower probe_order wins. Nothing here assumes sim-display
# is the only display processor.
#
# Flags: --system  system-wide (needs root)
#        --no-service  skip the systemd --user unit

set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"

SYSTEM=0
NO_SERVICE=0
for arg in "$@"; do
    case "$arg" in
    --system) SYSTEM=1 ;;
    --no-service) NO_SERVICE=1 ;;
    *) echo "Unknown option: $arg (supported: --system --no-service)" >&2; exit 2 ;;
    esac
done

[ -f "$HERE/lib/openxr_displayxr.so" ] || {
    echo "error: run install.sh from the unpacked tarball root (lib/openxr_displayxr.so not found)" >&2
    exit 1
}

if [ "$SYSTEM" = 1 ]; then
    [ "$(id -u)" = 0 ] || { echo "error: --system needs root (use sudo)" >&2; exit 1; }
    PREFIX=/usr/local
    OPENXR_CONF_DIR=/etc/xdg/openxr/1
    DP_ROOT=/usr/local/share/displayxr/DisplayProcessors
else
    DATA_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}"
    CONFIG_ROOT="${XDG_CONFIG_HOME:-$HOME/.config}"
    PREFIX="$DATA_ROOT/displayxr"
    OPENXR_CONF_DIR="$CONFIG_ROOT/openxr/1"
    DP_ROOT="$DATA_ROOT/DisplayXR/DisplayProcessors"
fi

echo "==> Installing DisplayXR runtime to $PREFIX"
mkdir -p "$PREFIX"
cp -R "$HERE/bin" "$PREFIX/"
cp -R "$HERE/lib" "$PREFIX/"

# --- OpenXR ActiveRuntime -------------------------------------------------
mkdir -p "$OPENXR_CONF_DIR"
ACTIVE="$OPENXR_CONF_DIR/active_runtime.json"
if [ -f "$ACTIVE" ] && ! grep -q "openxr_displayxr.so" "$ACTIVE"; then
    echo "    (existing non-DisplayXR active runtime backed up to active_runtime.json.bak)"
    cp "$ACTIVE" "$ACTIVE.bak"
fi
cat > "$ACTIVE" <<EOF
{
    "file_format_version": "1.0.0",
    "runtime": {
        "name": "DisplayXR",
        "library_path": "$PREFIX/lib/openxr_displayxr.so"
    }
}
EOF
echo "==> OpenXR ActiveRuntime: $ACTIVE"

# --- Display-processor discovery manifest ----------------------------------
mkdir -p "$DP_ROOT"
cat > "$DP_ROOT/200-sim-display.json" <<EOF
{
    "file_format_version": "1.0",
    "plugin": {
        "id":           "sim-display",
        "display_name": "DisplayXR Sim Display",
        "vendor":       "DisplayXR",
        "version":      "$(cat "$HERE/VERSION" 2>/dev/null || echo unknown)",
        "binary_path":  "$PREFIX/lib/displayxr/plugins/DisplayXR-SimDisplay.so",
        "probe_order":  200
    }
}
EOF
echo "==> Display processor manifest: $DP_ROOT/200-sim-display.json"

# --- systemd --user unit (user-level installs with the service binary) -----
if [ "$SYSTEM" = 0 ] && [ "$NO_SERVICE" = 0 ] && [ -x "$PREFIX/bin/displayxr-service" ]; then
    UNIT_DIR="$CONFIG_ROOT/systemd/user"
    mkdir -p "$UNIT_DIR"
    cat > "$UNIT_DIR/displayxr.service" <<EOF
[Unit]
Description=DisplayXR runtime service (out-of-process compositor)

[Service]
ExecStart=$PREFIX/bin/displayxr-service
Restart=no

[Install]
WantedBy=default.target
EOF
    echo "==> systemd --user unit: $UNIT_DIR/displayxr.service"
    # Containers / non-systemd sessions have no user bus — skip gracefully.
    if [ -d /run/systemd/system ] && systemctl --user daemon-reload 2>/dev/null; then
        systemctl --user enable displayxr.service >/dev/null 2>&1 || true
        echo "    enabled (start now: systemctl --user start displayxr)"
    else
        echo "    (no systemd user session detected — enable manually later:"
        echo "     systemctl --user daemon-reload && systemctl --user enable displayxr)"
    fi
fi

echo ""
echo "DisplayXR installed. Sanity check:"
echo "    $PREFIX/bin/displayxr-cli selftest"

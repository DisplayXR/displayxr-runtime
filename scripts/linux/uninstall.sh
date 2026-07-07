#!/bin/bash
# DisplayXR runtime — Linux uninstaller (mirror of install.sh, #705).
# User-level by default; `sudo ./uninstall.sh --system` for a --system install.

set -euo pipefail

SYSTEM=0
for arg in "$@"; do
    case "$arg" in
    --system) SYSTEM=1 ;;
    *) echo "Unknown option: $arg (supported: --system)" >&2; exit 2 ;;
    esac
done

if [ "$SYSTEM" = 1 ]; then
    [ "$(id -u)" = 0 ] || { echo "error: --system needs root (use sudo)" >&2; exit 1; }
    PREFIX=/usr/local
    OPENXR_CONF_DIR=/etc/xdg/openxr/1
    DP_ROOT=/usr/local/share/displayxr/DisplayProcessors
    UNIT=""
else
    DATA_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}"
    CONFIG_ROOT="${XDG_CONFIG_HOME:-$HOME/.config}"
    PREFIX="$DATA_ROOT/displayxr"
    OPENXR_CONF_DIR="$CONFIG_ROOT/openxr/1"
    DP_ROOT="$DATA_ROOT/DisplayXR/DisplayProcessors"
    UNIT="$CONFIG_ROOT/systemd/user/displayxr.service"
fi

if [ -n "$UNIT" ] && [ -f "$UNIT" ]; then
    systemctl --user disable --now displayxr.service >/dev/null 2>&1 || true
    rm -f "$UNIT"
    systemctl --user daemon-reload 2>/dev/null || true
    echo "==> Removed systemd --user unit"
fi

# Only unset the ActiveRuntime if it is ours; restore a backup if present.
ACTIVE="$OPENXR_CONF_DIR/active_runtime.json"
if [ -f "$ACTIVE" ] && grep -q "openxr_displayxr.so" "$ACTIVE"; then
    rm -f "$ACTIVE"
    [ -f "$ACTIVE.bak" ] && mv "$ACTIVE.bak" "$ACTIVE" && echo "==> Restored previous active runtime"
    echo "==> Removed OpenXR ActiveRuntime"
fi

rm -f "$DP_ROOT/200-sim-display.json"
# Leave other vendors' manifests + the shared root in place; prune if empty.
rmdir "$DP_ROOT" 2>/dev/null || true

if [ "$SYSTEM" = 1 ]; then
    # /usr/local is shared — remove only what install.sh placed.
    rm -f "$PREFIX/bin/displayxr-cli" "$PREFIX/bin/displayxr-service" \
        "$PREFIX/lib/openxr_displayxr.so" "$PREFIX/lib/displayxr/plugins/DisplayXR-SimDisplay.so"
    rmdir -p "$PREFIX/lib/displayxr/plugins" 2>/dev/null || true
else
    rm -rf "$PREFIX"
fi
echo "==> DisplayXR uninstalled"

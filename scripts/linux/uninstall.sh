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
    EXT_ROOT=/usr/local/share/gnome-shell/extensions
    UNIT_DIR=/usr/local/lib/systemd/user
else
    DATA_ROOT="${XDG_DATA_HOME:-$HOME/.local/share}"
    CONFIG_ROOT="${XDG_CONFIG_HOME:-$HOME/.config}"
    PREFIX="$DATA_ROOT/displayxr"
    OPENXR_CONF_DIR="$CONFIG_ROOT/openxr/1"
    DP_ROOT="$DATA_ROOT/DisplayXR/DisplayProcessors"
    EXT_ROOT="$DATA_ROOT/gnome-shell/extensions"
    UNIT_DIR="$CONFIG_ROOT/systemd/user"
fi

# displayxr-service units (#1744): the socket-activated pair, and v1's plain
# displayxr.service if an older install left it.
if [ -f "$UNIT_DIR/displayxr.socket" ] || [ -f "$UNIT_DIR/displayxr.service" ]; then
    if [ "$SYSTEM" = 1 ]; then
        systemctl --global disable displayxr.socket >/dev/null 2>&1 || true
    else
        systemctl --user disable --now displayxr.socket displayxr.service >/dev/null 2>&1 || true
        rm -f "$UNIT_DIR/default.target.wants/displayxr.service" "$UNIT_DIR/sockets.target.wants/displayxr.socket"
    fi
    rm -f "$UNIT_DIR/displayxr.socket" "$UNIT_DIR/displayxr.service"
    [ "$SYSTEM" = 1 ] || systemctl --user daemon-reload 2>/dev/null || true
    echo "==> Removed displayxr-service systemd user units"
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

# GNOME Shell extension: remove the files install.sh placed. Users' own
# enabled-extensions lists are left alone — a UUID with no files is inert, and
# another package (e.g. the .deb) may still provide the extension.
rm -rf "$EXT_ROOT/window-geometry@displayxr.org"
# (An `if` rather than `[ ... ] && ...`: under `set -e` a false test as a
# whole statement aborts the script, which used to cut a user-level uninstall
# short right here, before the binaries were removed.)
if [ "$SYSTEM" = 1 ]; then
    rm -f /etc/xdg/autostart/displayxr-gnome-extension-enable.desktop
    # On a pre-45 GNOME the login script materialises a per-user copy of the
    # legacy entry point that shadows the system one. Remove the invoking
    # user's (it is entirely ours -- hence the marker it carries); another
    # user's goes at their next login, when the script finds no system install
    # to refresh it from.
    SHADOW_HOME="$HOME"
    if [ -n "${SUDO_USER:-}" ]; then
        SHADOW_HOME="$(getent passwd "$SUDO_USER" 2>/dev/null | cut -d: -f6 || true)"
        [ -n "$SHADOW_HOME" ] || SHADOW_HOME="$HOME"
    fi
    SHADOW="$SHADOW_HOME/.local/share/gnome-shell/extensions/window-geometry@displayxr.org"
    if [ -f "$SHADOW/.displayxr-shadow" ]; then
        rm -rf "$SHADOW"
        echo "==> Removed the per-user GNOME 40-44 copy in $SHADOW"
    fi
fi

if [ "$SYSTEM" = 1 ]; then
    # /usr/local is shared — remove only what install.sh placed.
    rm -f "$PREFIX/bin/displayxr-cli" "$PREFIX/bin/displayxr-service" \
        "$PREFIX/bin/displayxr-gnome-extension-enable" \
        "$PREFIX/lib/openxr_displayxr.so" "$PREFIX/lib/displayxr/plugins/DisplayXR-SimDisplay.so"
    rmdir -p "$PREFIX/lib/displayxr/plugins" 2>/dev/null || true
else
    rm -rf "$PREFIX"
fi
echo "==> DisplayXR uninstalled"

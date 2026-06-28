#!/bin/bash
# Uninstall DisplayXR OpenXR runtime from macOS
set -e

echo "=== DisplayXR Uninstaller ==="

# Stop + remove the service LaunchAgent (macOS spatial shell, #61) before
# deleting its binary, so launchd isn't left pointing at a missing program.
AGENT="/Library/LaunchAgents/com.displayxr.service.plist"
if [ -f "$AGENT" ]; then
    echo "Stopping + removing DisplayXR service LaunchAgent..."
    CONSOLE_USER="$(stat -f%Su /dev/console 2>/dev/null || true)"
    if [ -n "$CONSOLE_USER" ] && [ "$CONSOLE_USER" != "root" ]; then
        CONSOLE_UID="$(id -u "$CONSOLE_USER" 2>/dev/null || true)"
        [ -n "$CONSOLE_UID" ] && launchctl bootout "gui/$CONSOLE_UID" "$AGENT" 2>/dev/null || true
    fi
    sudo rm -f "$AGENT"
fi
pkill -x displayxr-service 2>/dev/null || true

echo "Removing DisplayXR runtime..."
# This recursive delete covers the runtime dylib, the
# DisplayXR-SimDisplay.dylib plug-in under lib/displayxr/plugins/,
# and the 200-sim-display.json manifest under DisplayProcessors/
# (issue #274). Per-user manifests at ~/Library/Application
# Support/DisplayXR/DisplayProcessors/ are intentionally left alone —
# those are user-owned dev-iteration state.
sudo rm -rf "/Library/Application Support/DisplayXR"

echo "Removing OpenXR runtime registration..."
# The postinstall registers active_runtime.json in both the XDG config dir and
# the loader datadir (#385). Remove each only when it points at our install, so
# a third-party runtime registered there isn't clobbered. (readlink returns the
# stored link text even after the target dir above is deleted.)
for rt in /etc/xdg/openxr/1/active_runtime.json /usr/local/share/openxr/1/active_runtime.json; do
    target="$(readlink "$rt" 2>/dev/null || true)"
    case "$target" in
        "/Library/Application Support/DisplayXR/"*) sudo rm -f "$rt" ;;
    esac
done

echo "Removing test app..."
rm -rf "/Applications/DisplayXRCube.app"

echo "Forgetting installer receipts..."
# Identifiers must match the --identifier values in build_installer.sh
# (com.displayxr.runtime, com.displayxr.testapp). Earlier revisions of
# this script had `com.displayxr.displayxr.*` here — a double-prefix
# typo that silently swallowed via `|| true`, leaving orphan receipts
# in `pkgutil --pkgs` after every uninstall (#274).
sudo pkgutil --forget com.displayxr.runtime 2>/dev/null || true
sudo pkgutil --forget com.displayxr.testapp 2>/dev/null || true

echo "=== DisplayXR uninstalled ==="

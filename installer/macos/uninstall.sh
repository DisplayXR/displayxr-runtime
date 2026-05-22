#!/bin/bash
# Uninstall DisplayXR OpenXR runtime from macOS
set -e

echo "=== DisplayXR Uninstaller ==="

echo "Removing DisplayXR runtime..."
# This recursive delete covers the runtime dylib, the
# DisplayXR-SimDisplay.dylib plug-in under lib/displayxr/plugins/,
# and the 200-sim-display.json manifest under DisplayProcessors/
# (issue #274). Per-user manifests at ~/Library/Application
# Support/DisplayXR/DisplayProcessors/ are intentionally left alone —
# those are user-owned dev-iteration state.
sudo rm -rf "/Library/Application Support/DisplayXR"

echo "Removing OpenXR runtime registration..."
sudo rm -f /etc/xdg/openxr/1/active_runtime.json

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

#!/bin/bash
# Register / unregister the dev build's displayxr-desktop-capture helper (#2264)
# as the per-user LaunchAgent that serves com.displayxr.desktop-capture, so
# in-process Metal compositors of a dev runtime can reach it.
#
#   ./scripts/macos_desktop_capture_dev.sh install     # bootstraps build/…/DisplayXRDesktopCapture.app
#   ./scripts/macos_desktop_capture_dev.sh uninstall
#   ./scripts/macos_desktop_capture_dev.sh status
#
# The helper only runs while a transparent session on a weaving DP wants the
# desktop.
#
# SIGNING MATTERS FOR THE GRANT. macOS keys an ad-hoc-signed binary's Screen
# Recording grant to its cdhash, so EVERY rebuild silently loses it (measured,
# #2264). A real identity keys it to bundle id + team instead, which survives
# rebuilds. `install` therefore re-signs the bundle with
# $DXR_CODESIGN_IDENTITY, else the first "Developer ID Application" /
# "Apple Development" identity in the keychain, else leaves it ad-hoc (and
# says so). Re-run `install` after every rebuild of the helper.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
LABEL=com.displayxr.desktop-capture
SRC="$ROOT/build/src/xrt/targets/desktop_capture_macos/com.displayxr.desktop-capture.dev.plist"
DST="$HOME/Library/LaunchAgents/$LABEL.plist"
DOMAIN="gui/$(id -u)"

case "${1:-status}" in
install)
	[ -f "$SRC" ] || { echo "build first (./scripts/build_macos.sh): $SRC missing" >&2; exit 1; }
	if [ -f "/Library/LaunchAgents/$LABEL.plist" ]; then
		echo "note: an installed helper agent exists in /Library/LaunchAgents; booting it out of $DOMAIN for this session"
		launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
	fi
	APP="$(dirname "$(dirname "$(dirname "$(/usr/libexec/PlistBuddy -c 'Print :Program' "$SRC")")")")"
	IDENT="${DXR_CODESIGN_IDENTITY:-}"
	if [ -z "$IDENT" ]; then
		IDENT="$(security find-identity -v -p codesigning 2>/dev/null |
			grep -oE '"(Developer ID Application|Apple Development): [^"]+"' | head -1 | tr -d '"')"
	fi
	if [ -n "$IDENT" ]; then
		codesign --force --options runtime --timestamp=none -s "$IDENT" "$APP"
		echo "signed $APP with '$IDENT' (the Screen Recording grant survives rebuilds)"
	else
		echo "warning: no codesigning identity — the helper stays ad-hoc and loses its Screen Recording grant on every rebuild" >&2
	fi
	mkdir -p "$HOME/Library/LaunchAgents"
	cp "$SRC" "$DST"
	launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
	launchctl bootstrap "$DOMAIN" "$DST"
	echo "registered $LABEL → $(/usr/libexec/PlistBuddy -c 'Print :Program' "$DST")"
	;;
uninstall)
	launchctl bootout "$DOMAIN/$LABEL" 2>/dev/null || true
	rm -f "$DST"
	echo "unregistered $LABEL"
	;;
status)
	launchctl print "$DOMAIN/$LABEL" 2>/dev/null | grep -E "state|program|pid" || echo "$LABEL not registered"
	;;
*) echo "usage: $0 install|uninstall|status" >&2; exit 2 ;;
esac

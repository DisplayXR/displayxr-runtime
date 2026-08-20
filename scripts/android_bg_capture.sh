#!/usr/bin/env bash
# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0
#
# Start the #1073 T2 background-capture producer on a connected Android device
# and stream real screen pixels into the runtime's compose-under seam.
#
# The producer lives in the vendor display-config APK
# (CNSDK, branch dxr/background-capture-service) and is launched here with
# app_process at shell/root uid. That indirection is not laziness: screen
# capture needs READ_FRAME_BUFFER (signature|recents), CAPTURE_VIDEO_OUTPUT or
# ACCESS_SURFACE_FLINGER (both plain signature). None is signature|privileged,
# so no system-app placement or privapp-permissions allowlist can grant them to
# a vendor-signed APK -- only the platform signature can. Until the platform
# signs the service, a permitted uid is the only way to exercise the path.
#
# Usage:
#   scripts/android_bg_capture.sh [--mode uids|once|all|uid] [--uid N[,N...]]
#                                 [--width N] [--rate HZ] [--delay-ms N]
#                                 [--serial S]
#
# Typical validation run (avatar over the launcher):
#   adb shell setprop debug.dxr.bg2d capture     # arm the runtime consumer
#   scripts/android_bg_capture.sh                # start the producer
#   ...then launch the transparent app.
#
# `uids` is requested by default because where it works it is the only mode
# that survives a DEVICE ROTATION. A rotation invalidates a held capture twice
# over -- the frame's aspect no longer matches the panel, and the launcher
# behind it has re-laid out. `uids` captures each listed uid separately and
# composites them bottom-up, so the consumer's own uid is absent by
# construction; being feedback-free it can run continuously, and a rotation
# then needs no trigger at all.
#
# WHETHER IT WORKS IS A PER-BUILD PROPERTY, and the daemon decides, not this
# script. The union is well defined only if a uid-filtered captureDisplay
# leaves the layers it skipped TRANSPARENT. SurfaceFlinger composites a display
# screenshot over a fill layer whose alpha is RenderArea::CaptureFill, and
# DisplayRenderArea uses OPAQUE -- on such a build every per-uid capture is
# opaque black outside that uid's layers and the union collapses to the LAST
# uid. Measured on the NP02J: the launcher's black fill erased the wallpaper
# captured under it, leaving icons and the dock floating on black, which reads
# as a working background until you notice the wallpaper is gone. CNSDK#718
# probes for this at startup and falls back to `once`, printing
#
#   uid-filtered captureDisplay fills OPAQUE on this build, ... falling back to --mode=once
#
# so `--mode uids` on such a device is a REQUEST, not a guarantee. The fallback
# is not a downgrade in content -- `once` is complete by construction -- and it
# keeps most of the rotation-follow by re-capturing in the gap between consumer
# sessions, which is where a rotation lands for an orientation-locked app.
#
# With no --uid this resolves the wallpaper host and the current home launcher
# itself. Pass --uid to override (bottom layer first).

set -euo pipefail

MODE=uids
UID_ARG=""
WIDTH=512
RATE=2
DELAY=0
SERIAL=""
SOCKET=displayxr.bg2d
PKG=com.leialoft.display.config

while [ $# -gt 0 ]; do
	case "$1" in
	--mode) MODE="$2"; shift 2 ;;
	--uid) UID_ARG="--uid=$2"; shift 2 ;;
	--width) WIDTH="$2"; shift 2 ;;
	--rate) RATE="$2"; shift 2 ;;
	--delay-ms) DELAY="$2"; shift 2 ;;
	--socket) SOCKET="$2"; shift 2 ;;
	--serial) SERIAL="-s $2"; shift 2 ;;
	-h | --help) sed -n '2,45p' "$0"; exit 0 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

# shellcheck disable=SC2086
ADB="adb $SERIAL"

APK=$($ADB shell pm path "$PKG" | head -1 | tr -d '\r' | cut -d: -f2)
if [ -z "$APK" ]; then
	echo "error: $PKG is not installed. Build and install the CNSDK branch" >&2
	echo "       dxr/background-capture-service first (adb install -r)." >&2
	exit 1
fi

# Resolve the default uid list. The wallpaper belongs to the wallpaper host
# (SystemUI here) and the icons to the home launcher, and they are DIFFERENT
# uids -- which is exactly why a single --uid drops the wallpaper and why this
# mode exists. Bottom layer first.
if [ "$MODE" = "uids" ] && [ -z "$UID_ARG" ]; then
	uid_of() {
		# shellcheck disable=SC2086
		$ADB shell dumpsys package "$1" 2>/dev/null | sed -n 's/.*userId=\([0-9]*\).*/\1/p' | head -1 | tr -d '\r'
	}
	# shellcheck disable=SC2086
	WALLPAPER_PKG=$($ADB shell dumpsys wallpaper 2>/dev/null |
		sed -n 's/.*mWallpaperComponent=ComponentInfo{\([^\/]*\)\/.*/\1/p' | head -1 | tr -d '\r')
	# shellcheck disable=SC2086
	LAUNCHER_PKG=$($ADB shell cmd package resolve-activity -a android.intent.action.MAIN \
		-c android.intent.category.HOME 2>/dev/null |
		sed -n 's/^ *packageName=\(.*\)/\1/p' | head -1 | tr -d '\r')
	UIDS=""
	for pkg in "$WALLPAPER_PKG" "$LAUNCHER_PKG"; do
		[ -n "$pkg" ] || continue
		u=$(uid_of "$pkg")
		[ -n "$u" ] || continue
		case ",$UIDS," in *",$u,"*) continue ;; esac
		UIDS="${UIDS:+$UIDS,}$u"
		echo "  contributor: $pkg -> uid $u"
	done
	if [ -z "$UIDS" ]; then
		echo "error: could not resolve the wallpaper host / home launcher uids." >&2
		echo "       Pass them explicitly: --uid <wallpaper_uid>,<launcher_uid>" >&2
		exit 1
	fi
	UID_ARG="--uid=$UIDS"
fi

echo "producer APK: $APK"
echo "mode=$MODE width=$WIDTH rate=${RATE}Hz socket=@$SOCKET ${UID_ARG}"
echo
echo "The runtime must already be listening: adb shell setprop debug.dxr.bg2d capture"
echo "Ctrl-C releases the background (the runtime keeps the last frame)."
echo
echo "If the connect fails with EACCES, SELinux is refusing the cross-domain"
echo "abstract-socket connect: 'adb shell setenforce 0' on an engineering unit."
echo

# app_process needs the dex on its classpath; /system/bin is the nominal cwd
# argument the runtime expects, not a path we use.
# shellcheck disable=SC2086
exec $ADB shell "CLASSPATH=$APK app_process /system/bin \
	com.leialoft.display.config.capture.CaptureDaemonMain \
	--mode=$MODE --width=$WIDTH --rate=$RATE --delay-ms=$DELAY --socket=$SOCKET $UID_ARG"

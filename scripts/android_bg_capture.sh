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
#   scripts/android_bg_capture.sh [--mode once|all|uid] [--uid N] [--width N]
#                                 [--rate HZ] [--delay-ms N] [--serial S]
#
# Typical validation run (avatar over the launcher):
#   adb shell setprop debug.dxr.bg2d capture     # arm the runtime consumer
#   scripts/android_bg_capture.sh --mode once    # capture the launcher, hold it
#   ...then launch the transparent app.
#
# `once` is the default because it is the only mode that is both complete
# (wallpaper included) and feedback-free: it captures before the overlay exists.

set -euo pipefail

MODE=once
UID_ARG=""
WIDTH=512
RATE=5
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
	-h | --help) sed -n '2,32p' "$0"; exit 0 ;;
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

echo "producer APK: $APK"
echo "mode=$MODE width=$WIDTH rate=${RATE}Hz socket=@$SOCKET"
echo
echo "The runtime must already be listening: adb shell setprop debug.dxr.bg2d capture"
echo "Ctrl-C releases the background (the runtime keeps the last frame)."
echo

# app_process needs the dex on its classpath; /system/bin is the nominal cwd
# argument the runtime expects, not a path we use.
# shellcheck disable=SC2086
exec $ADB shell "CLASSPATH=$APK app_process /system/bin \
	com.leialoft.display.config.capture.CaptureDaemonMain \
	--mode=$MODE --width=$WIDTH --rate=$RATE --delay-ms=$DELAY --socket=$SOCKET $UID_ARG"

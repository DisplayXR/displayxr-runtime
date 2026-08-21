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
#                                 [--rotation-watch HZ] [--detach] [--serial S]
#   scripts/android_bg_capture.sh --status | --stop
#
# Typical validation run (avatar over the launcher):
#   adb shell setprop debug.dxr.bg2d capture              # arm the runtime consumer
#   scripts/android_bg_capture.sh --detach --mode once    # start the producer
#   ...then launch the transparent app.
#
# PREFER --detach. Without it the daemon is a child of this adb shell and dies
# with it -- on a restage, a cable bump, or simply closing the terminal. Nothing
# reports that: the runtime consumer goes on listening, `comp_bg2d_ensure`
# returns no backdrop, and the only symptom is that the transparent edges fringe
# again exactly as they did before T2 landed. (That is what happened on
# 2026-08-21.) `--detach` reparents it to init so it survives everything short of
# a reboot, and `--status` answers "is the background actually alive?" in one
# command. The permanent fix is the vendor service auto-starting it, CNSDK#719.
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
ROTATION_WATCH=""
DETACH=0
DAEMON_LOG=/data/local/tmp/bg2d_daemon.log

while [ $# -gt 0 ]; do
	case "$1" in
	--mode) MODE="$2"; shift 2 ;;
	--uid) UID_ARG="--uid=$2"; shift 2 ;;
	--width) WIDTH="$2"; shift 2 ;;
	--rate) RATE="$2"; shift 2 ;;
	--delay-ms) DELAY="$2"; shift 2 ;;
	--socket) SOCKET="$2"; shift 2 ;;
	--rotation-watch) ROTATION_WATCH="--rotation-watch=$2"; shift 2 ;;
	--detach) DETACH=1; shift ;;
	--stop) STOP=1; shift ;;
	--status) STATUS=1; shift ;;
	--serial) SERIAL="-s $2"; shift 2 ;;
	-h | --help) sed -n '2,45p' "$0"; exit 0 ;;
	*) echo "unknown argument: $1" >&2; exit 2 ;;
	esac
done

# shellcheck disable=SC2086
ADB="adb $SERIAL"

# --status / --stop act on whatever is already running and need none of the
# resolution below. `pgrep -f` is unavailable on this shell; match the class.
daemon_pids() {
	# shellcheck disable=SC2086
	$ADB shell "ps -A -o PID,ARGS | grep CaptureDaemonMain | grep -v grep" 2>/dev/null |
		awk '{print $1}' | tr -d '\r'
}

if [ "${STATUS:-0}" = 1 ]; then
	pids=$(daemon_pids)
	if [ -z "$pids" ]; then
		echo "capture daemon: NOT RUNNING (no backdrop -> transparent edges will fringe)"
		exit 1
	fi
	echo "capture daemon: running (pid $(echo "$pids" | tr '\n' ' '))"
	# shellcheck disable=SC2086
	$ADB shell "tail -6 $DAEMON_LOG" 2>/dev/null
	exit 0
fi

if [ "${STOP:-0}" = 1 ]; then
	pids=$(daemon_pids)
	[ -n "$pids" ] || { echo "capture daemon: not running"; exit 0; }
	for p in $pids; do
		# shellcheck disable=SC2086
		$ADB shell "kill $p" >/dev/null 2>&1
	done
	echo "capture daemon: stopped ($(echo "$pids" | tr '\n' ' '))"
	exit 0
fi

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
#
# CLASSPATH is kept OUT of $DAEMON_CMD deliberately: `nohup VAR=v cmd` treats
# `VAR=v` as the program name and fails with a bare ENOENT, so the assignment
# has to sit in front of `nohup`, not behind it.
DAEMON_CMD="app_process /system/bin \
	com.leialoft.display.config.capture.CaptureDaemonMain \
	--mode=$MODE --width=$WIDTH --rate=$RATE --delay-ms=$DELAY --socket=$SOCKET \
	$ROTATION_WATCH $UID_ARG"

if [ "$DETACH" = 0 ]; then
	# shellcheck disable=SC2086
	exec $ADB shell "CLASSPATH=$APK $DAEMON_CMD"
fi

# --detach: survive this shell. In the foreground the daemon dies with the adb
# connection, which is how a restage or a cable bump silently removes the
# backdrop and brings the fringes back with no error anywhere -- the runtime
# just listens forever. Reparenting to init keeps it up across all of that
# (though NOT across a reboot; the permanent home is the vendor service
# auto-starting it, CNSDK#719).
# shellcheck disable=SC2086
$ADB shell "kill \$(ps -A -o PID,ARGS | grep CaptureDaemonMain | grep -v grep | awk '{print \$1}') " \
	>/dev/null 2>&1 || true
# shellcheck disable=SC2086
$ADB shell "rm -f $DAEMON_LOG; CLASSPATH=$APK nohup $DAEMON_CMD >$DAEMON_LOG 2>&1 </dev/null &" \
	>/dev/null 2>&1

i=0
while [ $i -lt 20 ]; do
	if [ -n "$(daemon_pids)" ]; then
		echo "capture daemon detached (pid $(daemon_pids | tr '\n' ' ')), log $DAEMON_LOG"
		echo "  stop it with: $0 --stop      check it with: $0 --status"
		exit 0
	fi
	sleep 0.5
	i=$((i + 1))
done
echo "error: the daemon did not come up; see $DAEMON_LOG" >&2
# shellcheck disable=SC2086
$ADB shell "cat $DAEMON_LOG" 2>/dev/null >&2
exit 1

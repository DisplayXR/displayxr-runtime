#!/usr/bin/env bash
# Build (and optionally install) the DisplayXR Android runtime APK.
#
# Wraps the openxr_android Gradle build with the hard-won flags a fresh dev box
# otherwise trips over — chiefly -PdxrForceVendoredCjson (issue #496): on a host
# with a system cJSON (macOS `brew install cjson`, Linux `libcjson-dev`) the host
# dylib/.so leaks into the aarch64 link and ld.lld dies with "unknown file type".
#
# Usage:
#   ./scripts/build-android.sh                      # build debug
#   ./scripts/build-android.sh install              # build + adb install debug
#   ./scripts/build-android.sh build release
#
# Args (any order after the verb):
#   verb     : build (default) | install | clean
#   variant  : debug (default) | release
#
# #1031: there is ONE hybrid runtime APK — in-process by default, IPC per app.
# The old inprocess/outofprocess flavor words are accepted and ignored so old
# muscle memory and scripts don't break.
#
# Env overrides:
#   ANDROID_HOME   — Android SDK (else read from local.properties sdk.dir)
#   DXR_DEVICE     — adb serial for `install` (else the only attached device)
#
# Notes:
#   * The runtime owns no shell binary; this builds the runtime APK only.
#   * Leia 3D needs gitignored extras hand-dropped into src/main/jniLibs/arm64-v8a/
#     (libdxrp050_leia_cnsdk.so + libleiaCore-loader.so + libleiaSDK-jni.so, from
#     the plugin repo's `scripts/build-android.sh install-runtime-jnilibs`) AND
#     cnsdk.dir=<cnsdk> in local.properties. Without them the service silently
#     falls back to sim_display passthrough (un-weaved SBS). This script warns.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ANDROID_MODULE=":src:xrt:targets:openxr_android"
ANDROID_DIR="$REPO_ROOT/src/xrt/targets/openxr_android"

# ---- parse args (positional, order-insensitive after the verb) --------------
VERB="build"
VARIANT="debug"
for arg in "$@"; do
    case "$arg" in
        build|install|clean)        VERB="$arg" ;;
        # Accepted-and-ignored: the flavors were merged in #1031.
        inprocess|inProcess|in|outofprocess|outOfProcess|out|oop)
            echo "NOTE: '$arg' ignored — there is one hybrid runtime APK since #1031." >&2 ;;
        debug|release)              VARIANT="$arg" ;;
        *) echo "ERROR: unknown arg '$arg'" >&2; exit 2 ;;
    esac
done

VARIANT_CAP="$(tr '[:lower:]' '[:upper:]' <<< "${VARIANT:0:1}")${VARIANT:1}"
APK="$ANDROID_DIR/build/outputs/apk/$VARIANT/openxr_android-$VARIANT.apk"
PKG="org.freedesktop.monado.openxr_runtime.out_of_process"

# ---- locate the SDK ---------------------------------------------------------
if [ -z "${ANDROID_HOME:-}" ]; then
    if [ -f "$REPO_ROOT/local.properties" ]; then
        ANDROID_HOME="$(grep -E '^sdk\.dir=' "$REPO_ROOT/local.properties" | head -1 | cut -d= -f2-)"
    fi
fi
if [ -z "${ANDROID_HOME:-}" ] || [ ! -d "$ANDROID_HOME" ]; then
    echo "ERROR: Android SDK not found. Set ANDROID_HOME or sdk.dir in local.properties." >&2
    exit 1
fi
export ANDROID_HOME
ADB="$ANDROID_HOME/platform-tools/adb"

cd "$REPO_ROOT"

if [ "$VERB" = "clean" ]; then
    echo ">> ./gradlew $ANDROID_MODULE:clean"
    ./gradlew "$ANDROID_MODULE:clean"
    exit 0
fi

# ---- preflight: warn about the Leia jniLibs / cnsdk.dir footgun -------------
JNILIBS="$ANDROID_DIR/src/main/jniLibs/arm64-v8a"
for so in libdxrp050_leia_cnsdk.so libleiaCore-loader.so libleiaSDK-jni.so; do
    if [ ! -f "$JNILIBS/$so" ]; then
        echo "WARN: $JNILIBS/$so missing — Leia DP won't load; service falls back to sim_display (un-weaved SBS)." >&2
        echo "      Build it from the plugin repo: scripts/build-android.sh install-runtime-jnilibs" >&2
    fi
done
if [ -f "$REPO_ROOT/local.properties" ] && ! grep -qE '^cnsdk\.dir=' "$REPO_ROOT/local.properties"; then
    echo "WARN: cnsdk.dir not set in local.properties — CNSDK Java glue AAR absent; Leia service-load fails (black)." >&2
fi

# ---- build ------------------------------------------------------------------
# -PdxrForceVendoredCjson is the #496 fix; CI hosts have no system cJSON so it's
# a no-op there, but every local macOS/Linux dev box needs it.
TASK="$ANDROID_MODULE:assemble${VARIANT_CAP}"
echo ">> ./gradlew $TASK -PdxrForceVendoredCjson  (ANDROID_HOME=$ANDROID_HOME)"
./gradlew "$TASK" -PdxrForceVendoredCjson

if [ ! -f "$APK" ]; then
    echo "ERROR: build reported success but APK not found at:" >&2
    echo "       $APK" >&2
    exit 1
fi
echo "APK: $APK"

[ "$VERB" = "build" ] && exit 0

# ---- install ----------------------------------------------------------------
DEVICE_ARGS=()
if [ -n "${DXR_DEVICE:-}" ]; then
    DEVICE_ARGS=(-s "$DXR_DEVICE")
else
    n="$("$ADB" devices | grep -cE '\sdevice$' || true)"
    if [ "$n" -eq 0 ]; then echo "ERROR: no adb device attached." >&2; exit 1; fi
    if [ "$n" -gt 1 ]; then
        echo "ERROR: $n devices attached — set DXR_DEVICE=<serial>." >&2
        "$ADB" devices >&2
        exit 1
    fi
fi

# -r reinstall keeping data; -d allow version downgrade — a plain `-r` SILENTLY
# fails on a version downgrade (prints only "Performing Streamed Install").
echo ">> adb ${DEVICE_ARGS[*]} install -r -d <apk>"
"$ADB" "${DEVICE_ARGS[@]}" install -r -d "$APK"
echo "Installed $VARIANT on device."

# ---- re-grant the overlay permission ----------------------------------------
# SYSTEM_ALERT_WINDOW ("display over other apps") is a special app-op permission:
# it is NEVER granted at install time, and an uninstall+install DROPS a previous
# grant. The runtime needs it for overlay mode (#558) — the service-owned
# TYPE_APPLICATION_OVERLAY that see-through apps (demo-avatar) weave into.
# Without it MonadoImpl.canDrawOverOtherApps() returns false, the client falls
# back to publishing its own opaque surface, and the app renders on a BLACK
# background with no other symptom (3D/weave keeps working). Re-grant on every
# install so a reinstall can't silently regress transparency.
# ---- clear FLAG_STOPPED so the runtime broker resolves ----------------------
# Uninstalling the runtime deregisters its OpenXRRuntimeBroker ContentProvider,
# and until the app is launched at least once Android's FLAG_STOPPED keeps that
# provider unresolvable by OTHER packages. Every OpenXR app then dies at
# instance creation with "Failed to find provider info for
# org.khronos.openxr.runtime_broker" / XR_ERROR_RUNTIME_UNAVAILABLE, which reads
# as a broken runtime rather than "nobody opened it". There is no
# BOOT_COMPLETED receiver to clear the flag, so a silent `adb install` leaves
# the device in a state where everything fails for a reason nothing explains.
echo ">> adb shell am start (launch once to register the runtime broker)"
if ! "$ADB" "${DEVICE_ARGS[@]}" shell am start \
        -n "$PKG/org.freedesktop.monado.openxr_runtime.DashboardActivity" >/dev/null 2>&1; then
    echo "WARN: could not launch $PKG — open the DisplayXR app on the device once," >&2
    echo "      or every OpenXR app will fail with XR_ERROR_RUNTIME_UNAVAILABLE." >&2
fi

echo ">> adb shell appops set $PKG SYSTEM_ALERT_WINDOW allow"
if "$ADB" "${DEVICE_ARGS[@]}" shell appops set "$PKG" SYSTEM_ALERT_WINDOW allow; then
    echo "Overlay permission (SYSTEM_ALERT_WINDOW) granted to $PKG."
else
    echo "WARN: could not grant SYSTEM_ALERT_WINDOW to $PKG — overlay mode (#558)" >&2
    echo "      will be OFF, so see-through apps (demo-avatar) render on black." >&2
    echo "      Grant it by hand: adb shell appops set $PKG SYSTEM_ALERT_WINDOW allow" >&2
fi

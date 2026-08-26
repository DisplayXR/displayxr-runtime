#!/usr/bin/env bash
# ============================================================
# DisplayXR — Android install from RELEASED artifacts
# ============================================================
# The Android analogue of the Windows NSIS installer and the Linux .deb.
# There is no Android equivalent of either, so this is the documented,
# scriptable install path (#1212).
#
# Usage:
#   ./scripts/install-android.sh <runtime.apk> [app.apk ...]
#   ./scripts/install-android.sh --uninstall
#
# Example, entirely from GitHub release assets:
#   ./scripts/install-android.sh \
#       DisplayXR-Runtime-Leia-2.13.5-android-arm64.apk \
#       DisplayXRModelViewer-0.24.2.apk
#
# Env:
#   DXR_DEVICE  — adb serial (else the single attached device)
#   ADB         — adb binary (else PATH, else $ANDROID_HOME/platform-tools/adb)
#
# WHY THIS IS A SCRIPT AND NOT `adb install`
#
# Two device-state requirements are invisible, are dropped by every
# reinstall, and both fail with symptoms that point somewhere else:
#
#   1. The runtime app MUST BE LAUNCHED ONCE after install. Uninstalling
#      the runtime deregisters its OpenXRRuntimeBroker ContentProvider, and
#      until the app is launched at least once Android's FLAG_STOPPED keeps
#      that provider unresolvable by other packages. Every OpenXR app then
#      dies at instance creation with
#          Failed to find provider info for org.khronos.openxr.runtime_broker
#          xrCreateInstance -> XR_ERROR_RUNTIME_UNAVAILABLE
#      which reads as "the runtime is broken", not "nobody opened it". There
#      is no BOOT_COMPLETED receiver to clear the flag. A silent `adb
#      install` therefore leaves the device in a state where everything
#      fails for a reason nothing on screen explains.
#
#   2. SYSTEM_ALERT_WINDOW is an app-op, never granted at install, and
#      dropped by uninstall+install. Without it the runtime's overlay path
#      is off and see-through apps render on a BLACK background while 3D and
#      weaving keep working — so it looks like a content bug, not a
#      permissions one.
#
# Order matters: runtime first, then apps. An app installed first simply
# finds no runtime.
# ============================================================

set -euo pipefail

PKG="org.freedesktop.monado.openxr_runtime.out_of_process"
LAUNCH_ACTIVITY="${PKG}/org.freedesktop.monado.openxr_runtime.DashboardActivity"

# ---- adb --------------------------------------------------------------------
if [ -z "${ADB:-}" ]; then
    if command -v adb >/dev/null 2>&1; then
        ADB="$(command -v adb)"
    elif [ -n "${ANDROID_HOME:-}" ] && [ -x "${ANDROID_HOME}/platform-tools/adb" ]; then
        ADB="${ANDROID_HOME}/platform-tools/adb"
    elif [ -n "${ANDROID_SDK_ROOT:-}" ] && [ -x "${ANDROID_SDK_ROOT}/platform-tools/adb" ]; then
        ADB="${ANDROID_SDK_ROOT}/platform-tools/adb"
    else
        echo "ERROR: adb not found. Set ADB=/path/to/adb or put it on PATH." >&2
        exit 1
    fi
fi

DEVICE_ARGS=()
if [ -n "${DXR_DEVICE:-}" ]; then
    DEVICE_ARGS=(-s "$DXR_DEVICE")
else
    n="$("$ADB" devices | grep -cE '\sdevice$' || true)"
    if [ "$n" -eq 0 ]; then
        echo "ERROR: no adb device attached." >&2
        exit 1
    fi
    if [ "$n" -gt 1 ]; then
        echo "ERROR: $n devices attached — set DXR_DEVICE=<serial>." >&2
        "$ADB" devices >&2
        exit 1
    fi
fi

adbsh() { "$ADB" "${DEVICE_ARGS[@]}" "$@"; }

# ---- uninstall --------------------------------------------------------------
if [ "${1:-}" = "--uninstall" ]; then
    echo ">> uninstalling $PKG"
    adbsh uninstall "$PKG" || true
    echo "Done. NOTE: any DisplayXR app left installed will now fail at"
    echo "xrCreateInstance with XR_ERROR_RUNTIME_UNAVAILABLE — that is expected"
    echo "with no runtime present, not a fault in the app."
    exit 0
fi

if [ $# -lt 1 ]; then
    echo "usage: $(basename "$0") <runtime.apk> [app.apk ...]" >&2
    echo "       $(basename "$0") --uninstall" >&2
    exit 2
fi

RUNTIME_APK="$1"; shift
[ -f "$RUNTIME_APK" ] || { echo "ERROR: $RUNTIME_APK not found." >&2; exit 1; }

case "$(basename "$RUNTIME_APK")" in
    *Runtime*) ;;
    *) echo "WARN: '$(basename "$RUNTIME_APK")' does not look like a runtime APK." >&2
       echo "      The RUNTIME must be the first argument; apps follow it." >&2 ;;
esac

# Flag the vendor-neutral variant early. It installs and self-tests fine, it
# simply cannot weave — which on a vendor display looks exactly like a bug.
case "$(basename "$RUNTIME_APK")" in
    *Runtime-Leia*) : ;;
    *) echo
       echo "NOTE: this looks like the VENDOR-NEUTRAL runtime APK (sim-display only)."
       echo "      On a Leia device it will install, pass most checks and render"
       echo "      nothing weaveable. Use DisplayXR-Runtime-Leia-*.apk there." ;;
esac

# ---- 1. runtime -------------------------------------------------------------
# -r keeps data; -d allows a version downgrade, which a plain -r fails at
# SILENTLY (it prints only "Performing Streamed Install").
echo
echo ">> [1/4] installing runtime: $(basename "$RUNTIME_APK")"
adbsh install -r -d "$RUNTIME_APK"

# ---- 2. clear FLAG_STOPPED so the broker resolves ---------------------------
echo
echo ">> [2/4] launching the runtime once (registers OpenXRRuntimeBroker)"
adbsh shell am start -n "$LAUNCH_ACTIVITY" >/dev/null 2>&1 || \
    adbsh shell monkey -p "$PKG" -c android.intent.category.LAUNCHER 1 >/dev/null 2>&1 || {
        echo "WARN: could not launch $PKG automatically." >&2
        echo "      OPEN THE 'DisplayXR' APP ON THE DEVICE ONCE before running any" >&2
        echo "      OpenXR app, or every one of them will fail at xrCreateInstance" >&2
        echo "      with XR_ERROR_RUNTIME_UNAVAILABLE." >&2
    }
# Give the activity a moment to come up before we start querying it.
sleep 2

# ---- 3. overlay app-op ------------------------------------------------------
echo
echo ">> [3/4] granting SYSTEM_ALERT_WINDOW to $PKG"
if adbsh shell appops set "$PKG" SYSTEM_ALERT_WINDOW allow; then
    echo "     granted."
else
    echo "WARN: could not grant SYSTEM_ALERT_WINDOW." >&2
    echo "      Overlay mode will be OFF: see-through apps render on BLACK while" >&2
    echo "      3D and weaving keep working, so it looks like a content bug." >&2
    echo "      Grant by hand: adb shell appops set $PKG SYSTEM_ALERT_WINDOW allow" >&2
fi

# ---- 4. apps ----------------------------------------------------------------
echo
if [ $# -eq 0 ]; then
    echo ">> [4/4] no app APKs given — runtime only."
else
    echo ">> [4/4] installing $# app APK(s)"
    for apk in "$@"; do
        [ -f "$apk" ] || { echo "ERROR: $apk not found." >&2; exit 1; }
        echo "   - $(basename "$apk")"
        adbsh install -r -d "$apk"
    done
fi

# ---- verdict ----------------------------------------------------------------
# The on-device dashboard runs the same checks as `displayxr-cli selftest`,
# including the vendor_dp check (#1212) that fails when a better-ranked plug-in
# was present and failed to load — i.e. when the runtime silently fell back to
# sim-display. Read it back rather than declaring success from here.
echo
echo ">> installed. Verify on the device:"
echo "   1. Open the 'DisplayXR' app — the dashboard runs the self-test."
echo "   2. It must report PASS, and the active plug-in must NOT be 'sim-display'"
echo "      on a vendor display. A failing 'vendor_dp' check means a vendor"
echo "      plug-in was present but could not be loaded (usually an ABI"
echo "      mismatch — reinstall a runtime APK matching your plug-in)."
echo "   3. Then launch an app APK."
echo
echo "   Logs:  adb ${DEVICE_ARGS[*]} logcat -s monado DisplayXR"

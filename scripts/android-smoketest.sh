#!/usr/bin/env bash
#
# Android emulator smoke test: end-to-end validation that the runtime
# + leia-plugin chain reaches xrCreateInstance success on a real
# Android device or emulator. Catches regressions like PR #343's
# preload break, which compiled fine but broke at load time.
#
# Run from the runtime repo root:
#
#   scripts/android-smoketest.sh
#
# Optional env vars:
#   PLUGIN_DIR        Plug-in repo path (default: ../displayxr-leia-plugin)
#   PLUGIN_BRANCH     Branch to check out in the plug-in repo (default: don't touch)
#   CNSDK_ROOT        CNSDK extracted dir (default: ./cnsdk)
#   AVD               AVD name (default: Medium_Phone_API_36)
#   ANDROID_SDK_ROOT  Android SDK (default: $LOCALAPPDATA/Android/Sdk on Windows
#                                          or $HOME/Android/Sdk on Linux/macOS)
#   SKIP_EMULATOR     1 to use an already-attached device, skip emulator boot
#   SMOKE_TIMEOUT     seconds to wait for the sentinel (default: 60)
#
# Exit codes:
#   0 — sentinel `ANDROID_POC_SENTINEL xrCreateInstance=XR_SUCCESS` seen
#   1 — hard failure (FATAL / XR_ERROR_INITIALIZATION_FAILED / RUNTIME_UNAVAILABLE)
#   2 — sentinel not seen within timeout
#   3 — prerequisite missing (plug-in repo, CNSDK, adb, etc.)

set -euo pipefail

# ----- defaults + path resolution ---------------------------------------------

RUNTIME_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$RUNTIME_ROOT"

PLUGIN_DIR="${PLUGIN_DIR:-$RUNTIME_ROOT/../displayxr-leia-plugin}"
CNSDK_ROOT="${CNSDK_ROOT:-$RUNTIME_ROOT/cnsdk}"
AVD="${AVD:-Medium_Phone_API_36}"
SMOKE_TIMEOUT="${SMOKE_TIMEOUT:-60}"

if [[ -z "${ANDROID_SDK_ROOT:-}" ]]; then
    if [[ -n "${LOCALAPPDATA:-}" && -d "$LOCALAPPDATA/Android/Sdk" ]]; then
        ANDROID_SDK_ROOT="$LOCALAPPDATA/Android/Sdk"
    elif [[ -d "$HOME/Android/Sdk" ]]; then
        ANDROID_SDK_ROOT="$HOME/Android/Sdk"
    elif [[ -d "$HOME/Library/Android/sdk" ]]; then
        ANDROID_SDK_ROOT="$HOME/Library/Android/sdk"
    else
        echo "ERROR: ANDROID_SDK_ROOT unset and no SDK found at common paths." >&2
        exit 3
    fi
fi

# Pick adb / emulator binary (Windows .exe vs Linux/macOS)
if [[ -x "$ANDROID_SDK_ROOT/platform-tools/adb.exe" ]]; then
    ADB="$ANDROID_SDK_ROOT/platform-tools/adb.exe"
elif [[ -x "$ANDROID_SDK_ROOT/platform-tools/adb" ]]; then
    ADB="$ANDROID_SDK_ROOT/platform-tools/adb"
else
    echo "ERROR: adb not found under $ANDROID_SDK_ROOT/platform-tools/" >&2
    exit 3
fi

if [[ -x "$ANDROID_SDK_ROOT/emulator/emulator.exe" ]]; then
    EMULATOR="$ANDROID_SDK_ROOT/emulator/emulator.exe"
elif [[ -x "$ANDROID_SDK_ROOT/emulator/emulator" ]]; then
    EMULATOR="$ANDROID_SDK_ROOT/emulator/emulator"
else
    EMULATOR=""
fi

# ----- prereq checks ----------------------------------------------------------

if [[ ! -d "$PLUGIN_DIR/scripts" || ! -f "$PLUGIN_DIR/scripts/build-android.sh" ]]; then
    echo "ERROR: plug-in repo not found at $PLUGIN_DIR" >&2
    echo "  clone DisplayXR/displayxr-leia-plugin and set PLUGIN_DIR env var" >&2
    exit 3
fi

if [[ ! -d "$CNSDK_ROOT/include" ]]; then
    echo "ERROR: CNSDK not extracted at $CNSDK_ROOT (no include/ dir)" >&2
    echo "  download cnsdk-android-0.7.28.zip and extract to $CNSDK_ROOT" >&2
    exit 3
fi

# ----- emulator boot ----------------------------------------------------------

ensure_device() {
    if [[ "${SKIP_EMULATOR:-0}" == "1" ]]; then
        return
    fi
    local attached
    attached=$("$ADB" devices | awk 'NR>1 && $2=="device"{print $1}' | head -1 || true)
    if [[ -n "$attached" ]]; then
        echo "[smoketest] device already attached: $attached"
        return
    fi
    if [[ -z "$EMULATOR" ]]; then
        echo "ERROR: no device attached and emulator binary not found." >&2
        exit 3
    fi
    echo "[smoketest] launching emulator $AVD"
    "$EMULATOR" -avd "$AVD" -no-snapshot-load -no-audio \
        -gpu swiftshader_indirect > /tmp/smoketest-emulator.log 2>&1 &
    # Bound the boot wait with our own deadline — bare `adb wait-for-device`
    # and the boot_completed poll can hang forever if the emulator never comes
    # up, stalling the whole run until a job-level timeout.
    local boot_deadline=$(( $(date +%s) + ${EMULATOR_BOOT_TIMEOUT:-300} ))
    until [[ "$("$ADB" get-state 2>/dev/null | tr -d '\r')" == "device" ]]; do
        (( $(date +%s) < boot_deadline )) || {
            echo "ERROR: emulator did not attach within ${EMULATOR_BOOT_TIMEOUT:-300}s (see /tmp/smoketest-emulator.log)" >&2
            exit 3
        }
        sleep 2
    done
    until [[ "$("$ADB" shell getprop sys.boot_completed 2>/dev/null | tr -d '\r')" == "1" ]]; do
        (( $(date +%s) < boot_deadline )) || {
            echo "ERROR: emulator attached but sys.boot_completed != 1 within deadline" >&2
            exit 3
        }
        sleep 2
    done
    echo "[smoketest] emulator booted"
}

# Fail loud on an ABI mismatch (arm64-only APK on an x86_64 emulator yields a
# confusing INSTALL_FAILED_NO_MATCHING_ABIS) before spending minutes building.
assert_abi() {
    local abilist
    abilist=$("$ADB" shell getprop ro.product.cpu.abilist 2>/dev/null | tr -d '\r')
    if [[ "$abilist" != *arm64-v8a* ]]; then
        echo "ERROR: device ABI list '$abilist' has no arm64-v8a — the test app is" >&2
        echo "  built arm64-v8a only (test_apps/.../build.gradle). Use an arm64 AVD" >&2
        echo "  or a Lume Pad. Aborting before the build." >&2
        exit 3
    fi
    echo "[smoketest] device ABI list: $abilist (arm64-v8a present)"
}

# ----- build chain ------------------------------------------------------------

build_plugin_and_install_jnilibs() {
    echo "[smoketest] building plug-in + installing transitive jniLibs"
    (
        cd "$PLUGIN_DIR"
        # Honor PLUGIN_BRANCH (documented in the header): check it out before
        # building so the smoke test builds a known plug-in revision. Requires a
        # clean plug-in working tree; abort loudly rather than build a stale tip.
        if [[ -n "${PLUGIN_BRANCH:-}" ]]; then
            if [[ -n "$(git status --porcelain 2>/dev/null)" ]]; then
                echo "ERROR: PLUGIN_BRANCH set but $PLUGIN_DIR has uncommitted changes" >&2
                exit 3
            fi
            echo "[smoketest] checking out plug-in branch: $PLUGIN_BRANCH"
            git checkout "$PLUGIN_BRANCH" >/dev/null 2>&1 || {
                echo "ERROR: failed to check out plug-in branch '$PLUGIN_BRANCH'" >&2
                exit 3
            }
        fi
        CNSDK_ROOT="$CNSDK_ROOT" \
        DXR_RUNTIME_SOURCE_DIR="$RUNTIME_ROOT" \
        bash scripts/build-android.sh install-runtime-jnilibs
    )
    local jni_dir="$RUNTIME_ROOT/src/xrt/targets/openxr_android/src/main/jniLibs/arm64-v8a"
    local count
    count=$(ls "$jni_dir" 2>/dev/null | wc -l)
    if (( count < 5 )); then
        echo "ERROR: expected >=5 transitive .so in $jni_dir, got $count" >&2
        exit 3
    fi
    echo "[smoketest] $count .so files in jniLibs/arm64-v8a/"
}

GRADLEW="./gradlew"
if [[ -x "./gradlew.bat" ]] && command -v cmd.exe >/dev/null 2>&1; then
    GRADLEW="./gradlew.bat"
fi

build_apks() {
    echo "[smoketest] building runtime APK + test app APK"
    "$GRADLEW" :src:xrt:targets:openxr_android:assembleInProcessDebug --console=plain --rerun-tasks
    "$GRADLEW" :test_apps:cube_handle_vk_android:assembleDebug --console=plain
}

# ----- install + launch + grep -----------------------------------------------

RUNTIME_APK="$RUNTIME_ROOT/src/xrt/targets/openxr_android/build/outputs/apk/inProcess/debug/openxr_android-inProcess-debug.apk"
TEST_APK="$RUNTIME_ROOT/test_apps/cube_handle_vk_android/build/outputs/apk/debug/cube_handle_vk_android-debug.apk"
TEST_PKG="com.displayxr.cube_handle_vk_android"
SENTINEL="ANDROID_POC_SENTINEL xrCreateInstance=XR_SUCCESS"

smoke_test() {
    [[ -f "$RUNTIME_APK" ]] || { echo "ERROR: $RUNTIME_APK not built" >&2; exit 3; }
    [[ -f "$TEST_APK"    ]] || { echo "ERROR: $TEST_APK not built"    >&2; exit 3; }

    echo "[smoketest] installing runtime APK"
    "$ADB" install -r "$RUNTIME_APK" > /dev/null
    echo "[smoketest] installing test app APK"
    "$ADB" install -r "$TEST_APK" > /dev/null

    "$ADB" shell setprop debug.dxr.hw.verbose 1 > /dev/null
    "$ADB" shell am force-stop "$TEST_PKG" > /dev/null
    "$ADB" logcat -c
    # Launch via the LAUNCHER intent rather than a hard-coded activity class:
    # the test app's entry point is a Kotlin .MainActivity wrapper (it requests
    # CAMERA before handing off to NativeActivity), and the class name has moved
    # before. Resolving the launcher activity keeps this rename-proof.
    "$ADB" shell monkey -p "$TEST_PKG" -c android.intent.category.LAUNCHER 1 > /dev/null

    # monkey returns 0 even if the activity never resolved or the app crashed on
    # resume. Confirm the process actually came up so an instant crash fails fast
    # (return 1) instead of masquerading as a sentinel timeout (return 2).
    local proc_deadline=$(( $(date +%s) + 15 ))
    until "$ADB" shell pidof "$TEST_PKG" >/dev/null 2>&1; do
        (( $(date +%s) < proc_deadline )) || {
            echo "[smoketest] FAIL: $TEST_PKG never started (no process within 15s — bad launch / instant crash)"
            "$ADB" logcat -d 2>/dev/null | grep -E "AndroidRuntime|$TEST_PKG|libc :" | tail -20
            return 1
        }
        sleep 1
    done

    echo "[smoketest] watching logcat for sentinel (timeout ${SMOKE_TIMEOUT}s)"
    local end=$(( $(date +%s) + SMOKE_TIMEOUT ))
    while (( $(date +%s) < end )); do
        # One capture per iteration: cheaper than 2-3 logcat calls, and lets us
        # distinguish "adb/emulator died" (return 3) from "not seen yet".
        local snap
        if ! snap=$("$ADB" logcat -d 2>/dev/null); then
            echo "[smoketest] FAIL: 'adb logcat' failed mid-run — device disconnected / emulator died" >&2
            return 3
        fi
        if grep -q "$SENTINEL" <<<"$snap"; then
            echo "[smoketest] PASS: sentinel found"
            grep "$SENTINEL" <<<"$snap" | head -1
            return 0
        fi
        if grep -qE 'AndroidRuntime: FATAL|XR_ERROR_INITIALIZATION_FAILED|XR_ERROR_RUNTIME_UNAVAILABLE' <<<"$snap"; then
            echo "[smoketest] FAIL: hit a hard error before sentinel"
            grep -E 'AndroidRuntime: FATAL|XR_ERROR_|monado\.' <<<"$snap" | tail -20
            return 1
        fi
        # Process vanished after starting → crashed before reaching the sentinel.
        if ! "$ADB" shell pidof "$TEST_PKG" >/dev/null 2>&1; then
            echo "[smoketest] FAIL: $TEST_PKG exited before sentinel (crash mid-bring-up)"
            grep -E "$TEST_PKG|AndroidRuntime|libc :|DEBUG|XR_ERROR_" <<<"$snap" | tail -20
            return 1
        fi
        sleep 2
    done
    echo "[smoketest] FAIL: sentinel not seen within ${SMOKE_TIMEOUT}s"
    "$ADB" logcat -d 2>/dev/null | tail -30
    return 2
}

# ----- main -------------------------------------------------------------------

ensure_device
assert_abi
build_plugin_and_install_jnilibs
build_apks
smoke_test

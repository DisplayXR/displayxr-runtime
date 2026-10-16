#!/usr/bin/env bash
# XR_DXR_desktop_control end-to-end on the REAL macOS desktop (ADR-056).
#
# Opens two scratch TextEdit documents it owns (a target and a decoy), drives them
# through the full OpenXR stack with desktop_control_probe, and checks the result
# ON DISK: the typed text must land in the target, the decoy must be untouched.
# Acts on no other window. Needs Accessibility for the launching app (Terminal /
# your IDE) and an unlocked screen; the runtime refuses everything while locked.
#
#   test_apps/probes/desktop_control_probe/e2e_real_macos.sh <runtime build dir> <probe binary> [repeat N]
#
# With a repeat count the idle check runs once, before the first pass: the test's own
# keystrokes reset the OS idle timer, so a per-pass check would refuse pass 2 onwards.
#
# Exit 0 = every expectation held and both files read back as expected.
set -euo pipefail
BUILD=$(cd "${1:?runtime build dir}" && pwd)
PROBE=${2:?desktop_control_probe binary}
REPEAT=${4:-1}
SMOKE=$BUILD/src/xrt/auxiliary/desktop/desktop_backend_smoke
CLI=$BUILD/src/xrt/targets/cli/displayxr-cli
# Only on an idle machine: this test takes focus and types. A person using the Mac
# would have their keystrokes land in the scratch documents, and their focus changes
# make the safety layer (correctly) refuse. DXR_E2E_FORCE=1 skips the check.
idle=$("$SMOKE" info | sed -nE 's/.*idle=([0-9]+) ms.*/\1/p')
if [ "${DXR_E2E_FORCE:-0}" != 1 ] && [ "${idle:-0}" -lt 60000 ]; then
	echo "refusing: someone is using this Mac (idle ${idle:-?} ms < 60 s). Run it unattended."
	exit 3
fi
if [ "$REPEAT" -gt 1 ]; then
	fails=0
	for n in $(seq 1 "$REPEAT"); do
		if DXR_E2E_FORCE=1 "$0" "$1" "$2"; then echo "pass $n/$REPEAT: clean"; else fails=$((fails+1)); echo "pass $n/$REPEAT: FAILED"; fi
	done
	echo "repeat summary: $((REPEAT-fails))/$REPEAT clean"
	exit $fails
fi
WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

text="hello from DisplayXR 你好 😀 "
printf 'START\n' > "$WORK/dxr-e2e-target.txt"
printf 'DECOY\n' > "$WORK/dxr-e2e-decoy.txt"
had_textedit=0; pgrep -x TextEdit >/dev/null && had_textedit=1
open -a TextEdit "$WORK/dxr-e2e-target.txt" "$WORK/dxr-e2e-decoy.txt"
sleep 3

id_of() { "$SMOKE" list | grep "title=\"$1\"" | sed -E 's/^ *id=([0-9]+) .*/\1/' | head -1; }
T=$(id_of dxr-e2e-target.txt); D=$(id_of dxr-e2e-decoy.txt)
[ -n "$T" ] && [ -n "$D" ] || { echo "could not find the scratch windows (Accessibility granted?)"; exit 2; }

cat > "$WORK/script.txt" <<SCRIPT
request e2e
expect-state GRANTED
act activate target=$D
expect XR_SUCCESS
act type target=$T text="MUST NOT LAND"
expect XR_ERROR_DESKTOP_CONTROL_REFUSED_DXR
act activate target=$T
expect XR_SUCCESS
act type target=$T text="$text"
expect XR_SUCCESS
act key target=$T keys=cmd+s
expect XR_SUCCESS
act move target=$T rect=241,142,0,0
expect XR_SUCCESS
act minimize target=$T
expect XR_SUCCESS
act restore target=$T
expect XR_SUCCESS
act maximize target=$T
expect XR_SUCCESS
act restore target=$T
expect XR_SUCCESS
run $CLI desktop stop
act key target=$T keys=cmd+s
expect XR_ERROR_DESKTOP_CONTROL_NOT_GRANTED_DXR
expect-state SUSPENDED
run $CLI desktop resume
expect-state GRANTED
act close target=$T
expect XR_SUCCESS
# CLOSE is a polite, asynchronous request: let the app finish closing
sleep 2000
act activate target=$T
expect XR_ERROR_DESKTOP_WINDOW_LOST_DXR
act close target=$D
expect XR_SUCCESS
SCRIPT

rc=0
DXR_DESKTOP_CONTROL_DIR="$WORK/store" XR_RUNTIME_JSON="$BUILD/openxr_displayxr-dev.json" \
	XRT_PLUGIN_SEARCH_PATH="$BUILD/src/xrt/drivers" "$PROBE" script "$WORK/script.txt" > "$WORK/log" 2>&1 || rc=$?
grep -E ": (PASS|FAIL)" "$WORK/log" || true

target=$(cat "$WORK/dxr-e2e-target.txt"); decoy=$(cat "$WORK/dxr-e2e-decoy.txt")
case "$target" in *"$text"*) echo "on disk: target has the typed text: PASS" ;; *) echo "on disk: target = '$target': FAIL"; rc=1 ;; esac
[ "$decoy" = "DECOY" ] && echo "on disk: decoy untouched: PASS" || { echo "on disk: decoy = '$decoy': FAIL"; rc=1; }

if [ $rc != 0 ]; then
	echo "--- action outcomes:"
	grep -E "^> act|^event: action" "$WORK/log" || true
fi
[ $had_textedit = 0 ] && pkill -x TextEdit || true
exit $rc

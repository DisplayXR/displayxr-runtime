#!/bin/bash
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
#
# End-to-end check of displayxr-hypr-geometry-bridge against fake_hyprland.py,
# on a private session bus. Run it under dbus-run-session:
#
#   dbus-run-session -- tests/hyprland/test_hypr_bridge.sh build/.../displayxr-hypr-geometry-bridge
#
# Checks: the snapshot it serves, that a movewindowv2 event moves the window in
# GetWindows and in a WindowsChanged signal, that it serves no windows while
# Hyprland is gone and recovers when it is back, and that it refuses to take
# the bus name outside a Hyprland session.

set -euo pipefail

BRIDGE="$(realpath "$1")"
HERE="$(cd "$(dirname "$0")" && pwd)"
FIX="$HERE/../data/hyprland"

W="$(mktemp -d)"
SIG="fake_0123456789_1732500000"
D="$W/hypr/$SIG"
mkdir -p "$D"
FAKE=""
BRIDGE_PID=""
MON=""
cleanup() {
    for p in $BRIDGE_PID $FAKE $MON; do kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
    if [ -n "${KEEP:-}" ]; then echo "logs kept in $W"; else rm -rf "$W"; fi
}
trap cleanup EXIT

fail() {
    echo "FAIL: $*" >&2
    echo "--- bridge log" >&2
    cat "$W/bridge.log" >&2 || true
    echo "--- last GetWindows" >&2
    get_windows >&2 || true
    exit 1
}

get_windows() {
    dbus-send --session --print-reply=literal --dest=org.displayxr.WindowGeometry \
        /org/displayxr/WindowGeometry org.displayxr.WindowGeometry1.GetWindows 2>/dev/null
}

# wait_for <seconds> <fixed string> [absent]
wait_for() {
    local secs="$1" needle="$2" absent="${3:-}" i
    for ((i = 0; i < secs * 20; i++)); do
        local out
        out="$(get_windows || true)"
        if [ -z "$absent" ] && grep -qF -- "$needle" <<<"$out"; then return 0; fi
        if [ -n "$absent" ] && [ -n "$out" ] && ! grep -qF -- "$needle" <<<"$out"; then return 0; fi
        sleep 0.05
    done
    return 1
}

start_fake() {
    python3 "$HERE/fake_hyprland.py" --dir "$D" --clients "$W/clients.json" \
        --monitors "$FIX/monitors_two.json" --monitors-text "$FIX/monitors.txt" >"$W/fake.log" 2>&1 &
    FAKE=$!
    for _ in $(seq 100); do
        [ -S "$D/.socket2.sock" ] && [ -p "$D/events.fifo" ] && return 0
        sleep 0.05
    done
    fail "fake Hyprland did not start"
}

export XDG_RUNTIME_DIR="$W"
cp "$FIX/clients_two.json" "$W/clients.json"

echo "== gating: not Hyprland, no live socket -> exits 0 and leaves the name alone"
env -u HYPRLAND_INSTANCE_SIGNATURE XDG_CURRENT_DESKTOP=GNOME XDG_RUNTIME_DIR="$W" timeout 5 "$BRIDGE" \
    2>"$W/gate.log" || fail "bridge did not exit 0 outside Hyprland"
grep -q "not a Hyprland session" "$W/gate.log" || fail "no refusal message"
if get_windows >/dev/null; then fail "the name was taken outside Hyprland"; fi

echo "== start fake Hyprland and the bridge"
start_fake
export HYPRLAND_INSTANCE_SIGNATURE="$SIG" XDG_CURRENT_DESKTOP=Hyprland
dbus-monitor --session "type='signal',interface='org.displayxr.WindowGeometry1'" >"$W/signals.log" 2>&1 &
MON=$!
"$BRIDGE" 2>"$W/bridge.log" &
BRIDGE_PID=$!

echo "== initial snapshot"
wait_for 5 '"pid":4242' || fail "window 4242 never appeared"
OUT="$(get_windows)"
grep -qF '"frame":[100,200,1280,720]' <<<"$OUT" || fail "wrong frame for 4242"
grep -qF '"buffer_device":[60,60,2700,1650]' <<<"$OUT" || fail "wrong buffer_device on the 1.5x output"
grep -qF '"layout_mode":"logical"' <<<"$OUT" || fail "layout_mode"
if grep -qF 'title' <<<"$OUT"; then fail "a title was published"; fi

echo "== movewindowv2 -> moved within 1 s, and signalled"
cp "$FIX/clients_two_moved.json" "$W/clients.json"
echo "movewindowv2>>5f3a1c0a2b10,1,1" >"$D/events.fifo"
wait_for 1 '"frame":[640,400,1280,720]' || fail "move not picked up within 1 s"
sleep 0.3
grep -qF '640,400,1280,720' "$W/signals.log" || fail "no WindowsChanged with the new rect"

echo "== no event, window moved (a mouse drag of a floating window) -> polled"
sed 's/"at": \[640, 400\]/"at": [700, 420]/' "$FIX/clients_two_moved.json" >"$W/clients.json"
wait_for 1 '"frame":[700,420,1280,720]' || fail "floating window not polled within 1 s"

echo "== Hyprland goes away -> no windows; back -> recovers"
kill "$FAKE"; wait "$FAKE" 2>/dev/null || true; FAKE=""
wait_for 3 '"windows":[]' || fail "stale windows served while Hyprland is gone"
start_fake
echo "openwindow>>1,1,x,y" >"$D/events.fifo" &
wait_for 6 '"pid":4242' || fail "did not reconnect"

echo "== still owns the name, exits 0 on SIGTERM"
kill -TERM "$BRIDGE_PID"
wait "$BRIDGE_PID" || fail "bridge exit status $?"
BRIDGE_PID=""

echo "PASS: displayxr-hypr-geometry-bridge"

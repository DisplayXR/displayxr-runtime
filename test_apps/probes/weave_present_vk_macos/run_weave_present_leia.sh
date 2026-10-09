#!/bin/bash
# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0
#
# Start a dev displayxr-service + the weave_present_vk_macos presenter, so the
# macOS weave service (#759, Metal backend) can be eyeballed on a panel without
# the browser. Kills ONLY what it started.
#
#   run_weave_present_leia.sh [--sim] [--display N] [--secs N] [-- <presenter args>]
#
#   default  Leia SR: the Leia macOS plug-in (DXR_PLUGIN_EXCLUSIVE=leia-sr), the
#            Metal weave backend (DXR_WEAVE_MAC_BACKEND=auto picks metal), the SR
#            dev env from ~/.leiasr-fpc/env.local. Needs SRService running.
#   --sim    sim_display anaglyph (DXR_PLUGIN_EXCLUSIVE=sim-display) — local A/B;
#            auto picks vk there (set DXR_WEAVE_MAC_BACKEND=metal to force metal).
#
# Env overrides: LEIA_PLUGIN_DIR (dir with DisplayXR-LeiaSR.dylib + 050-leia-sr.json),
# LEIASR_MACOS_DIR, DXR_WEAVE_MAC_BACKEND, LOG (service log path).
# Presenter args (after --): --size WxH  --sbs-only  --ring  (see main.mm).
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
PKG="$ROOT/_package/DisplayXR-macOS"
GH="$(cd "$ROOT/.." && pwd)"

MODE=leia
DISPLAY_ID=2
SECS=0
EXTRA=()
while [ $# -gt 0 ]; do
    case "$1" in
    --sim) MODE=sim ;;
    --display) DISPLAY_ID="$2"; shift ;;
    --secs) SECS="$2"; shift ;;
    --) shift; EXTRA=("$@"); break ;;
    *) echo "usage: $0 [--sim] [--display N] [--secs N] [-- presenter args]" >&2; exit 2 ;;
    esac
    shift
done

SERVICE="$PKG/bin/displayxr-service"
PRESENT="$PKG/bin/weave_present_vk_macos"
[ -x "$PRESENT" ] || PRESENT="$ROOT/test_apps/build/bin/weave_present_vk_macos"
for f in "$SERVICE" "$PRESENT" "$PKG/openxr_displayxr.json"; do
    [ -e "$f" ] || { echo "error: missing $f — run scripts/build_macos.sh --service first" >&2; exit 1; }
done

if pgrep -x displayxr-service >/dev/null; then
    echo "error: a displayxr-service is already running (pid $(pgrep -x displayxr-service | tr '\n' ' '))" >&2
    echo "       — not ours, not touching it. Stop it yourself and re-run." >&2
    exit 1
fi

export DYLD_LIBRARY_PATH="$PKG/lib${DYLD_LIBRARY_PATH:+:$DYLD_LIBRARY_PATH}"
export VK_ICD_FILENAMES="$PKG/share/vulkan/icd.d/MoltenVK_icd.json"
export VK_DRIVER_FILES="$VK_ICD_FILENAMES"
export DXR_WEAVE_MAC_BACKEND="${DXR_WEAVE_MAC_BACKEND:-auto}"

if [ "$MODE" = sim ]; then
    export XRT_PLUGIN_SEARCH_PATH="$PKG/lib/displayxr/plugins"
    export DXR_PLUGIN_EXCLUSIVE=sim-display
    export SIM_DISPLAY_OUTPUT="${SIM_DISPLAY_OUTPUT:-anaglyph}"
else
    LEIA_PLUGIN_DIR="${LEIA_PLUGIN_DIR:-$GH/displayxr-leia-plugin.wt-macos/build-macos-weave2/_plugins}"
    LEIASR_MACOS_DIR="${LEIASR_MACOS_DIR:-$GH/LeiaSR-macos}"
    [ -f "$LEIA_PLUGIN_DIR/050-leia-sr.json" ] || { echo "error: no Leia plug-in at $LEIA_PLUGIN_DIR" >&2; exit 1; }
    pgrep -x SRService >/dev/null || echo "warning: SRService is not running — the Leia DP will have no tracker" >&2
    export XRT_PLUGIN_SEARCH_PATH="$LEIA_PLUGIN_DIR"
    export DXR_PLUGIN_EXCLUSIVE=leia-sr
    # Dev SR tree: LEIASR_* must match the running SRService (port rendezvous).
    if [ -f "$HOME/.leiasr-fpc/env.local" ]; then
        # shellcheck disable=SC1091
        . "$HOME/.leiasr-fpc/env.local"
        export LEIASR_RUNTIME_DIR
        export DYLD_LIBRARY_PATH="$DYLD_LIBRARY_PATH:${LEIASR_DYLD:-}"
    fi
    export SR_RUNTIME_PATH="${SR_RUNTIME_PATH_OVERRIDE:-$LEIASR_MACOS_DIR/build/macos/install/lib/libLeiaSR_runtime.dylib}"
    export LEIASR_DATA_DIR="$LEIASR_MACOS_DIR/build/macos/sr"
fi

LOG="${LOG:-/tmp/weave_present_${MODE}_service.log}"
SVC_PID=""
PRES_PID=""
cleanup() {
    [ -n "$PRES_PID" ] && kill "$PRES_PID" 2>/dev/null && wait "$PRES_PID" 2>/dev/null
    if [ -n "$SVC_PID" ] && kill -0 "$SVC_PID" 2>/dev/null; then
        kill "$SVC_PID" 2>/dev/null
        for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$SVC_PID" 2>/dev/null || break; sleep 0.3; done
        kill -9 "$SVC_PID" 2>/dev/null
    fi
    echo "==> service log: $LOG"
    grep -E "plugin loader|weave\(#759\)|leia_mac|SR error|ERROR|\[E\]" "$LOG" 2>/dev/null | head -40
}
trap cleanup EXIT
trap 'exit 130' INT TERM

echo "==> mode=$MODE backend=$DXR_WEAVE_MAC_BACKEND plugins=$XRT_PLUGIN_SEARCH_PATH display=$DISPLAY_ID"
"$SERVICE" >"$LOG" 2>&1 &
SVC_PID=$!
# Ready = its IPC socket is listening (lsof on our own pid), give up after 10 s.
for _ in $(seq 1 50); do
    kill -0 "$SVC_PID" 2>/dev/null || { echo "error: service exited early" >&2; exit 1; }
    lsof -a -p "$SVC_PID" -U 2>/dev/null | grep -q displayxr_comp_ipc && break
    sleep 0.2
done
echo "==> service pid $SVC_PID up; launching presenter"

ARGS=(--display "$DISPLAY_ID")
[ "$SECS" != 0 ] && ARGS+=(--secs "$SECS")
XRT_FORCE_MODE=ipc XR_RUNTIME_JSON="$PKG/openxr_displayxr.json" "$PRESENT" "${ARGS[@]}" "${EXTRA[@]+"${EXTRA[@]}"}" &
PRES_PID=$!
wait "$PRES_PID"
RC=$?
PRES_PID=""
exit $RC

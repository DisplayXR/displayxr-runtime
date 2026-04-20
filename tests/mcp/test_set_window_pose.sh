#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Slice 3 (Phase B) — get/set_window_pose round-trip.
#
# Requires displayxr-service + at least one shell-mode handle app running.
# Grabs the first window id, reads its pose, sets a new pose with a known
# delta, reads it back, and asserts the delta landed within a small
# tolerance. On success, restores the original pose.

set -eu
set -o pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

ADAPTER=""
for cand in \
    "$ROOT/_package/DisplayXR/bin/displayxr-mcp.exe" \
    "$ROOT/_package/bin/displayxr-mcp.exe" \
    "$ROOT/build/src/xrt/targets/mcp_adapter/displayxr-mcp" \
    "$ROOT/build/src/xrt/targets/mcp_adapter/displayxr-mcp.exe"; do
    [[ -x "$cand" ]] && ADAPTER="$cand" && break
done
[[ -n "$ADAPTER" ]] || { echo "FAIL: displayxr-mcp not found" >&2; exit 1; }

echo "=== Slice 3 set_window_pose round-trip ==="
echo "    adapter: $ADAPTER"

"$ADAPTER" --target service </dev/null >/dev/null 2>&1 || {
    echo "FAIL: service MCP endpoint not reachable" >&2; exit 1
}

python3 tests/mcp/_set_window_pose_helper.py "$ADAPTER"

#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Slice 4 (Phase B) — set_focus + apply_layout_preset.
#
# Requires displayxr-service + shell-mode handle app running.

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

"$ADAPTER" --target service </dev/null >/dev/null 2>&1 || {
    echo "FAIL: service MCP endpoint not reachable" >&2; exit 1
}

python3 tests/mcp/_focus_preset_helper.py "$ADAPTER"

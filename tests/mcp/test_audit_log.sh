#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Slice 6 (Phase B) — audit log and allowlist smoke-check.
#
# - Triggers a write tool (apply_layout_preset) and asserts a new line
#   appears in the audit log.
# - Separate optional run with DISPLAYXR_MCP_ALLOW set to a bogus id
#   must cause set_window_pose to fail. That branch requires restarting
#   the service, so the test just documents it; actual verification
#   happens in the Windows validation pass.

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

python3 tests/mcp/_audit_log_helper.py "$ADAPTER"

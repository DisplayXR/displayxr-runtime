#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# MCP Phase A slice 2 test: list_sessions / get_display_info /
# get_runtime_metrics round-trip via the stdio adapter.

set -u
set -o pipefail

if [[ "$(uname)" != "Darwin" ]]; then
	echo "skip: mac-only test" >&2
	exit 0
fi

cd "$(dirname "$0")/../.."
ROOT="$(pwd)"

ADAPTER="$ROOT/build/src/xrt/targets/mcp_adapter/displayxr-mcp"
APP="$ROOT/test_apps/cube_handle_metal_macos/build/cube_handle_metal_macos"
RUNTIME_JSON="$ROOT/build/openxr_displayxr-dev.json"

for f in "$ADAPTER" "$APP" "$RUNTIME_JSON"; do
	if [[ ! -e "$f" ]]; then
		echo "FAIL: missing $f — run ./scripts/build_macos.sh first" >&2
		exit 1
	fi
done

export DISPLAYXR_MCP=1
export XR_RUNTIME_JSON="$RUNTIME_JSON"

"$APP" >/tmp/mcp_core_app.log 2>&1 &
APP_PID=$!
trap 'kill $APP_PID 2>/dev/null; wait $APP_PID 2>/dev/null' EXIT

SOCK="/tmp/displayxr-mcp-${APP_PID}.sock"
for _ in $(seq 1 50); do
	[[ -S "$SOCK" ]] && break
	sleep 0.1
done
[[ -S "$SOCK" ]] || { echo "FAIL: socket never appeared"; cat /tmp/mcp_core_app.log; exit 1; }

# Give the app a moment to create its XrSession so the tool handlers
# see an attached session — list_sessions returns empty pre-session.
sleep 1

python3 - "$ADAPTER" "$APP_PID" <<'PY'
import json, subprocess, sys

adapter, pid = sys.argv[1], sys.argv[2]

def frame(obj):
    body = json.dumps(obj).encode()
    return b"Content-Length: %d\r\n\r\n" % len(body) + body

proc = subprocess.Popen(
    [adapter, "--pid", pid],
    stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
)

def read_frame(p):
    hdr = b""
    while b"\r\n\r\n" not in hdr and b"\n\n" not in hdr:
        c = p.stdout.read(1)
        if not c:
            raise RuntimeError("EOF: " + p.stderr.read().decode("utf-8", "replace"))
        hdr += c
    clen = next(int(l.split(b":",1)[1]) for l in hdr.splitlines() if l.lower().startswith(b"content-length:"))
    return json.loads(p.stdout.read(clen))

def call(i, method, params=None):
    msg = {"jsonrpc":"2.0","id":i,"method":method}
    if params is not None: msg["params"] = params
    proc.stdin.write(frame(msg)); proc.stdin.flush()
    return read_frame(proc)

try:
    r = call(1, "initialize", {"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"test","version":"0"}})
    assert r["result"]["serverInfo"]["name"] == "displayxr-mcp", r

    r = call(2, "tools/list")
    tools = {t["name"] for t in r["result"]["tools"]}
    for needed in ("echo","list_sessions","get_display_info","get_runtime_metrics"):
        assert needed in tools, f"missing tool {needed} in {tools}"

    r = call(3, "tools/call", {"name":"list_sessions","arguments":{}})
    sessions = r["result"]["structured"]["sessions"]
    assert len(sessions) == 1, f"expected 1 session, got {sessions}"
    assert sessions[0]["api"] == "metal", sessions
    assert sessions[0]["view_count"] >= 1, sessions

    r = call(4, "tools/call", {"name":"get_display_info","arguments":{}})
    di = r["result"]["structured"]
    assert "physical_size" in di, di
    assert "panel" in di, di
    assert "views" in di and len(di["views"]) >= 1, di
    assert di["view_count"] == len(di["views"]), di

    r = call(5, "tools/call", {"name":"get_runtime_metrics","arguments":{}})
    m = r["result"]["structured"]
    assert m["gfx_api"] == "metal", m
    assert "frames_begun" in m, m

    print("PASS")
finally:
    try: proc.stdin.close()
    except: pass
    proc.wait(timeout=3)
PY
RC=$?
[[ $RC -eq 0 ]] && echo "test_core_tools.sh: OK" || { echo "test_core_tools.sh: FAIL"; cat /tmp/mcp_core_app.log; }
exit $RC

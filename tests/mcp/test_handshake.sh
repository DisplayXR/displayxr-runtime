#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# MCP Phase A slice 1 handshake test.
#
# Launches cube_handle_metal_macos with DISPLAYXR_MCP=1 and verifies that
# the runtime's MCP server accepts an `initialize` + `echo` round-trip via
# the displayxr-mcp stdio adapter.
#
# Exit 0 = pass, nonzero = fail.

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

# Launch the handle app detached.
"$APP" >/tmp/mcp_handshake_app.log 2>&1 &
APP_PID=$!

cleanup() {
	kill "$APP_PID" 2>/dev/null || true
	wait "$APP_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for the MCP socket to appear.
SOCK="/tmp/displayxr-mcp-${APP_PID}.sock"
for _ in $(seq 1 50); do
	[[ -S "$SOCK" ]] && break
	sleep 0.1
done
if [[ ! -S "$SOCK" ]]; then
	echo "FAIL: socket $SOCK never appeared (app log:)" >&2
	cat /tmp/mcp_handshake_app.log >&2 || true
	exit 1
fi

# Drive an initialize + tools/call echo round-trip through the adapter.
python3 - "$ADAPTER" "$APP_PID" <<'PY'
import json, os, subprocess, sys

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
            raise RuntimeError("EOF reading header; stderr=%s" % p.stderr.read().decode("utf-8", "replace"))
        hdr += c
    clen = None
    for line in hdr.splitlines():
        if line.lower().startswith(b"content-length:"):
            clen = int(line.split(b":", 1)[1].strip())
    if clen is None:
        raise RuntimeError("no Content-Length in %r" % hdr)
    return json.loads(p.stdout.read(clen))

try:
    proc.stdin.write(frame({
        "jsonrpc": "2.0", "id": 1, "method": "initialize",
        "params": {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}},
    }))
    proc.stdin.flush()
    r = read_frame(proc)
    assert r.get("id") == 1, r
    assert r.get("result", {}).get("serverInfo", {}).get("name") == "displayxr-mcp", r

    proc.stdin.write(frame({
        "jsonrpc": "2.0", "id": 2, "method": "tools/call",
        "params": {"name": "echo", "arguments": {"hello": "mcp"}},
    }))
    proc.stdin.flush()
    r = read_frame(proc)
    assert r.get("id") == 2, r
    structured = r["result"]["structured"]
    assert structured["echo"] == {"hello": "mcp"}, r
    print("PASS")
finally:
    try:
        proc.stdin.close()
    except Exception:
        pass
    proc.wait(timeout=3)
PY
RC=$?

if [[ $RC -eq 0 ]]; then
	echo "test_handshake.sh: OK"
else
	echo "test_handshake.sh: FAIL (rc=$RC)" >&2
	echo "--- app log ---" >&2
	cat /tmp/mcp_handshake_app.log >&2 || true
fi
exit $RC

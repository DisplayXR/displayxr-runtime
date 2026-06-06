#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# XR_EXT_mcp_tools e2e (#447) — the full app-defined-tool round-trip.
#
# Launches cube_handle_metal_macos (the reference XR_EXT_mcp_tools
# adopter) with DISPLAYXR_MCP=1 and verifies, via the displayxr-mcp
# stdio adapter on the per-PID endpoint:
#
#   1. tools/list contains the app tools (set_spin, get_status) tagged
#      _meta {"displayxr/group":"app"} and the result-level
#      _meta {"displayxr/appId":"cube-metal"}.
#   2. tools/call set_spin round-trips through the OpenXR event queue
#      (trampoline -> XrEventDataMCPToolCallEXT -> app frame loop ->
#      xrSubmitMCPToolResultEXT) and the new value reads back through
#      get_status.
#   3. A bad call (missing required arg) surfaces as a JSON-RPC error,
#      not a hang.
#
# Unlike test_handshake.sh this needs a *session* (tools register after
# xrCreateSession), so the sim-display plug-in must be discoverable —
# we point XRT_PLUGIN_SEARCH_PATH at the dev package tree.
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

ADAPTER="$ROOT/build/_deps/displayxr_mcp-build/displayxr-mcp"
APP="$ROOT/test_apps/cube_handle_metal_macos/build/cube_handle_metal_macos"
RUNTIME_JSON="$ROOT/build/openxr_displayxr-dev.json"
PLUGINS="$ROOT/_package/DisplayXR-macOS/lib/displayxr/plugins"

for f in "$ADAPTER" "$APP" "$RUNTIME_JSON" "$PLUGINS"; do
	if [[ ! -e "$f" ]]; then
		echo "FAIL: missing $f — run ./scripts/build_macos.sh first" >&2
		exit 1
	fi
done

export DISPLAYXR_MCP=1
export XR_RUNTIME_JSON="$RUNTIME_JSON"
export XRT_PLUGIN_SEARCH_PATH="$PLUGINS"

"$APP" >/tmp/mcp_app_tools_app.log 2>&1 &
APP_PID=$!

cleanup() {
	kill "$APP_PID" 2>/dev/null || true
	wait "$APP_PID" 2>/dev/null || true
}
trap cleanup EXIT

# Wait for the MCP socket, then for tool registration (post-session).
SOCK="/tmp/displayxr-mcp-${APP_PID}.sock"
for _ in $(seq 1 50); do
	[[ -S "$SOCK" ]] && break
	sleep 0.1
done
if [[ ! -S "$SOCK" ]]; then
	echo "FAIL: socket $SOCK never appeared (app log:)" >&2
	cat /tmp/mcp_app_tools_app.log >&2 || true
	exit 1
fi
for _ in $(seq 1 50); do
	grep -q "XR_EXT_mcp_tools: appId=0" /tmp/mcp_app_tools_app.log && break
	sleep 0.1
done
# get_status asserts session_running — wait for the session to begin.
for _ in $(seq 1 100); do
	grep -q "Session started" /tmp/mcp_app_tools_app.log && break
	sleep 0.1
done

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
            raise RuntimeError("EOF reading header; stderr=%s" % p.stderr.read().decode("utf-8", "replace"))
        hdr += c
    clen = None
    for line in hdr.splitlines():
        if line.lower().startswith(b"content-length:"):
            clen = int(line.split(b":", 1)[1].strip())
    if clen is None:
        raise RuntimeError("no Content-Length in %r" % hdr)
    return json.loads(p.stdout.read(clen))

def request(p, rid, method, params=None):
    msg = {"jsonrpc": "2.0", "id": rid, "method": method}
    if params is not None:
        msg["params"] = params
    p.stdin.write(frame(msg))
    p.stdin.flush()
    # Skip interleaved notifications (tools/list_changed is expected —
    # the app registers tools while agents may already be connected).
    while True:
        r = read_frame(p)
        if r.get("id") == rid:
            return r

try:
    r = request(proc, 1, "initialize", {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}})
    assert r["result"]["capabilities"]["tools"]["listChanged"] is True, r
    assert r["result"]["serverInfo"].get("appId") == "cube-metal", r

    # 1. App tools present, tagged, and namespaceable.
    r = request(proc, 2, "tools/list")
    tools = {t["name"]: t for t in r["result"]["tools"]}
    assert r["result"].get("_meta", {}).get("displayxr/appId") == "cube-metal", r
    for name in ("set_spin", "get_status"):
        assert name in tools, sorted(tools)
        assert tools[name]["_meta"]["displayxr/group"] == "app", tools[name]
    assert tools["capture_frame"]["_meta"]["displayxr/group"] == "capture", tools["capture_frame"]

    # 2. set_spin round-trips the event queue; get_status reads it back.
    r = request(proc, 3, "tools/call", {"name": "set_spin", "arguments": {"speed_rad_per_sec": 2.25}})
    structured = r["result"]["structured"]
    assert abs(structured["spin_speed_rad_per_sec"] - 2.25) < 1e-3, r

    r = request(proc, 4, "tools/call", {"name": "get_status", "arguments": {}})
    structured = r["result"]["structured"]
    assert abs(structured["spin_speed_rad_per_sec"] - 2.25) < 1e-3, r
    assert structured["session_running"] is True, r

    # 3. Bad call errors cleanly (app answers success=false -> tool error).
    r = request(proc, 5, "tools/call", {"name": "set_spin", "arguments": {}})
    assert "error" in r, r

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
	echo "test_app_tools.sh: OK"
else
	echo "test_app_tools.sh: FAIL (rc=$RC)" >&2
	echo "--- app log ---" >&2
	cat /tmp/mcp_app_tools_app.log >&2 || true
fi
exit $RC

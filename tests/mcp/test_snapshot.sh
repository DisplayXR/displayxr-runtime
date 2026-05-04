#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# MCP Phase A slice 3 test: get_kooima_params / get_submitted_projection
# return matching-shape per-view data from a running handle app.

set -u
set -o pipefail

if [[ "$(uname)" != "Darwin" ]]; then exit 0; fi

cd "$(dirname "$0")/../.."
ROOT="$(pwd)"

ADAPTER="$ROOT/build/_deps/displayxr_mcp-build/displayxr-mcp"
APP="$ROOT/test_apps/cube_handle_metal_macos/build/cube_handle_metal_macos"
RUNTIME_JSON="$ROOT/build/openxr_displayxr-dev.json"

for f in "$ADAPTER" "$APP" "$RUNTIME_JSON"; do
	[[ -e "$f" ]] || { echo "FAIL: missing $f"; exit 1; }
done

export DISPLAYXR_MCP=1 XR_RUNTIME_JSON="$RUNTIME_JSON"
"$APP" >/tmp/mcp_snap_app.log 2>&1 &
APP_PID=$!
trap 'kill $APP_PID 2>/dev/null; wait $APP_PID 2>/dev/null' EXIT

SOCK="/tmp/displayxr-mcp-${APP_PID}.sock"
for _ in $(seq 1 50); do [[ -S "$SOCK" ]] && break; sleep 0.1; done
[[ -S "$SOCK" ]] || { echo "FAIL: no socket"; cat /tmp/mcp_snap_app.log; exit 1; }

# Give the app time to run xrLocateViews + xrEndFrame at least once.
sleep 2

python3 - "$ADAPTER" "$APP_PID" <<'PY'
import json, subprocess, sys
adapter, pid = sys.argv[1], sys.argv[2]
def frame(o):
    b = json.dumps(o).encode()
    return b"Content-Length: %d\r\n\r\n" % len(b) + b
p = subprocess.Popen([adapter,"--pid",pid], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
def read():
    h = b""
    while b"\r\n\r\n" not in h and b"\n\n" not in h:
        c = p.stdout.read(1)
        if not c: raise RuntimeError("EOF: "+p.stderr.read().decode("utf-8","replace"))
        h += c
    n = next(int(l.split(b":",1)[1]) for l in h.splitlines() if l.lower().startswith(b"content-length:"))
    return json.loads(p.stdout.read(n))
def call(i,m,par=None):
    msg={"jsonrpc":"2.0","id":i,"method":m}
    if par is not None: msg["params"]=par
    p.stdin.write(frame(msg)); p.stdin.flush()
    return read()
try:
    call(1,"initialize",{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"0"}})

    r = call(2,"tools/call",{"name":"get_kooima_params","arguments":{}})
    k = r["result"]["structured"]
    assert "error" not in k, k
    assert k["view_count"] >= 1, k
    assert len(k["views"]) == k["view_count"], k
    assert "recommended_pose" in k["views"][0] and "recommended_fov" in k["views"][0], k
    assert "display" in k, k

    r = call(3,"tools/call",{"name":"get_submitted_projection","arguments":{}})
    s = r["result"]["structured"]
    assert "error" not in s, s
    # declared view_count can be < recommended (active mode vs. system max).
    assert 1 <= s["view_count"] <= k["view_count"], (s, k)
    assert "declared_pose" in s["views"][0] and "declared_fov" in s["views"][0] and "subimage" in s["views"][0], s
    print("PASS")
finally:
    try: p.stdin.close()
    except: pass
    p.wait(timeout=3)
PY
RC=$?
[[ $RC -eq 0 ]] && echo "test_snapshot.sh: OK" || { echo "test_snapshot.sh: FAIL"; cat /tmp/mcp_snap_app.log; }
exit $RC

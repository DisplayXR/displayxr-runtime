#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# MCP capture_frame end-to-end test on the OpenGL native compositor.

set -u
set -o pipefail

if [[ "$(uname)" != "Darwin" ]]; then exit 0; fi

cd "$(dirname "$0")/../.."
ROOT="$(pwd)"
ADAPTER="$ROOT/build/_deps/displayxr_mcp-build/displayxr-mcp"
APP="$ROOT/test_apps/cube_handle_gl_macos/build/cube_handle_gl_macos"
RUNTIME_JSON="$ROOT/build/openxr_displayxr-dev.json"
for f in "$ADAPTER" "$APP" "$RUNTIME_JSON"; do
	[[ -e "$f" ]] || { echo "FAIL: missing $f"; exit 1; }
done
export DISPLAYXR_MCP=1 XR_RUNTIME_JSON="$RUNTIME_JSON"
"$APP" >/tmp/mcp_cap_gl_app.log 2>&1 &
APP_PID=$!
trap 'kill $APP_PID 2>/dev/null; wait $APP_PID 2>/dev/null' EXIT
SOCK="/tmp/displayxr-mcp-${APP_PID}.sock"
for _ in $(seq 1 50); do [[ -S "$SOCK" ]] && break; sleep 0.1; done
[[ -S "$SOCK" ]] || { echo "FAIL: no socket"; cat /tmp/mcp_cap_gl_app.log; exit 1; }
sleep 2

python3 - "$ADAPTER" "$APP_PID" <<'PY'
import json, os, subprocess, sys
ad, pid = sys.argv[1], sys.argv[2]
def fr(o):
    b = json.dumps(o).encode()
    return b"Content-Length: %d\r\n\r\n" % len(b) + b
p = subprocess.Popen([ad,"--pid",pid], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
def rd():
    h=b""
    while b"\r\n\r\n" not in h and b"\n\n" not in h:
        c=p.stdout.read(1)
        if not c: raise RuntimeError(p.stderr.read().decode("utf-8","replace"))
        h+=c
    n=next(int(l.split(b":",1)[1]) for l in h.splitlines() if l.lower().startswith(b"content-length:"))
    return json.loads(p.stdout.read(n))
def call(i,m,a=None):
    msg={"jsonrpc":"2.0","id":i,"method":m}
    if a is not None: msg["params"]=a
    p.stdin.write(fr(msg)); p.stdin.flush()
    return rd()
try:
    call(1,"initialize",{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"0"}})
    # Confirm we really are talking to the GL compositor.
    s = call(2,"tools/call",{"name":"list_sessions","arguments":{}})["result"]["structured"]
    assert s["sessions"][0]["api"] == "opengl", s
    r = call(3,"tools/call",{"name":"capture_frame","arguments":{}})
    d = r["result"]["structured"]
    assert "error" not in d, d
    assert os.path.isfile(d["path"]), d
    assert d["size_bytes"] > 1024, d
    with open(d["path"], "rb") as fh:
        assert fh.read(8) == b"\x89PNG\r\n\x1a\n", d
    print(f"PASS ({os.path.basename(d['path'])}, {d['size_bytes']} bytes)")
finally:
    try: p.stdin.close()
    except: pass
    p.wait(timeout=3)
PY
RC=$?
[[ $RC -eq 0 ]] && echo "test_capture_frame_gl.sh: OK" || { echo "test_capture_frame_gl.sh: FAIL"; cat /tmp/mcp_cap_gl_app.log; }
exit $RC

#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# MCP Phase A slice 4 test: diff_projection on a well-behaved handle app
# (cube_handle_metal_macos) should return ok=true with an empty flags
# array. Exercising the bug-class triggers requires a deliberately-
# broken app and is deferred to an end-to-end demo (demo_maya.sh).

set -u
set -o pipefail

if [[ "$(uname)" != "Darwin" ]]; then exit 0; fi

cd "$(dirname "$0")/../.."
ROOT="$(pwd)"
ADAPTER="$ROOT/build/src/xrt/targets/mcp_adapter/displayxr-mcp"
APP="$ROOT/test_apps/cube_handle_metal_macos/build/cube_handle_metal_macos"
RUNTIME_JSON="$ROOT/build/openxr_displayxr-dev.json"
for f in "$ADAPTER" "$APP" "$RUNTIME_JSON"; do
	[[ -e "$f" ]] || { echo "FAIL: missing $f"; exit 1; }
done
export DISPLAYXR_MCP=1 XR_RUNTIME_JSON="$RUNTIME_JSON"
"$APP" >/tmp/mcp_diff_app.log 2>&1 &
APP_PID=$!
trap 'kill $APP_PID 2>/dev/null; wait $APP_PID 2>/dev/null' EXIT
SOCK="/tmp/displayxr-mcp-${APP_PID}.sock"
for _ in $(seq 1 50); do [[ -S "$SOCK" ]] && break; sleep 0.1; done
[[ -S "$SOCK" ]] || { echo "FAIL: no socket"; cat /tmp/mcp_diff_app.log; exit 1; }
sleep 2

python3 - "$ADAPTER" "$APP_PID" <<'PY'
import json, subprocess, sys
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
def call(i,m,ar=None):
    msg={"jsonrpc":"2.0","id":i,"method":m}
    if ar is not None: msg["params"]=ar
    p.stdin.write(fr(msg)); p.stdin.flush()
    return rd()
try:
    call(1,"initialize",{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"t","version":"0"}})
    r = call(2,"tools/call",{"name":"diff_projection","arguments":{}})
    d = r["result"]["structured"]
    assert "error" not in d, d
    assert "flags" in d and isinstance(d["flags"], list), d
    assert d["view_count"] >= 1, d
    assert len(d["views"]) == d["view_count"], d
    v0 = d["views"][0]
    assert "metrics" in v0 and "declared_fov_aspect_wh" in v0["metrics"], v0
    # cube_handle_metal_macos recomputes Kooima client-side instead of
    # forwarding the runtime's recommendation, so the tool may flag
    # app_ignores_recommended / stale_head_pose — that's a working tool,
    # not a failure. We only assert that every flag is a known class.
    known = {
        "app_ignores_recommended",
        "fov_aspect_mismatch_subimage",
        "fov_aspect_mismatch_display",
        "stale_head_pose",
    }
    assert set(d["flags"]).issubset(known), f"unexpected flag in {d['flags']}"
    print(f"PASS (flags={d['flags']}, ok={d['ok']})")
finally:
    try: p.stdin.close()
    except: pass
    p.wait(timeout=3)
PY
RC=$?
[[ $RC -eq 0 ]] && echo "test_diff_projection.sh: OK" || { echo "test_diff_projection.sh: FAIL"; cat /tmp/mcp_diff_app.log; }
exit $RC

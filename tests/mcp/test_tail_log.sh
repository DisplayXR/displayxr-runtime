#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# MCP Phase A slice 5 test: tail_log returns buffered U_LOG lines with
# monotonic seq, and cursor advances across calls.

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
export DISPLAYXR_MCP=1 XR_RUNTIME_JSON="$RUNTIME_JSON" XRT_LOG=info
"$APP" >/tmp/mcp_log_app.log 2>&1 &
APP_PID=$!
trap 'kill $APP_PID 2>/dev/null; wait $APP_PID 2>/dev/null' EXIT
SOCK="/tmp/displayxr-mcp-${APP_PID}.sock"
for _ in $(seq 1 50); do [[ -S "$SOCK" ]] && break; sleep 0.1; done
[[ -S "$SOCK" ]] || { echo "FAIL: no socket"; cat /tmp/mcp_log_app.log; exit 1; }
sleep 1

python3 - "$ADAPTER" "$APP_PID" <<'PY'
import json, subprocess, sys, time
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
    r = call(2,"tools/call",{"name":"tail_log","arguments":{"since":0,"max":16}})
    d = r["result"]["structured"]
    assert "entries" in d and "cursor" in d, d
    assert isinstance(d["entries"], list), d
    # We should have buffered at least a few INFO/WARN lines from startup.
    assert len(d["entries"]) > 0, d
    for e in d["entries"]:
        assert "seq" in e and "text" in e and "level" in e, e
    first_cursor = d["cursor"]

    # Second call advances cursor to the latest.
    time.sleep(0.5)
    r2 = call(3,"tools/call",{"name":"tail_log","arguments":{"since":first_cursor,"max":16}})
    d2 = r2["result"]["structured"]
    assert d2["cursor"] >= first_cursor, (first_cursor, d2)
    # Every returned entry must have seq > first_cursor.
    for e in d2["entries"]:
        assert e["seq"] > first_cursor, (first_cursor, e)
    print(f"PASS (first={len(d['entries'])} second={len(d2['entries'])} cursor {first_cursor}->{d2['cursor']})")
finally:
    try: p.stdin.close()
    except: pass
    p.wait(timeout=3)
PY
RC=$?
[[ $RC -eq 0 ]] && echo "test_tail_log.sh: OK" || { echo "test_tail_log.sh: FAIL"; cat /tmp/mcp_log_app.log; }
exit $RC

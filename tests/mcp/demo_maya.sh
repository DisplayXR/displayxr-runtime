#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# MCP Phase A end-to-end "Maya stereo-debug" demo.
#
# Drives the tool sequence from docs/roadmap/mcp-phase-a-agent-prompt.md
# against a running cube_handle_metal_macos via displayxr-mcp, and prints
# a pass/fail summary. This is the slice-6-complete checkpoint the plan
# calls out.

set -u
set -o pipefail

cd "$(dirname "$0")/../.."
ROOT="$(pwd)"

if [[ "$(uname)" != "Darwin" ]]; then
	echo "demo_maya.sh: skip — mac-only demo (Windows adapter runs the same tools over named pipes)"
	exit 0
fi

ADAPTER="$ROOT/build/src/xrt/targets/mcp_adapter/displayxr-mcp"
APP="$ROOT/test_apps/cube_handle_metal_macos/build/cube_handle_metal_macos"
RUNTIME_JSON="$ROOT/build/openxr_displayxr-dev.json"
for f in "$ADAPTER" "$APP" "$RUNTIME_JSON"; do
	[[ -e "$f" ]] || { echo "FAIL: missing $f — run ./scripts/build_macos.sh first"; exit 1; }
done

export DISPLAYXR_MCP=1 XR_RUNTIME_JSON="$RUNTIME_JSON"
"$APP" >/tmp/mcp_demo_app.log 2>&1 &
APP_PID=$!
trap 'kill $APP_PID 2>/dev/null; wait $APP_PID 2>/dev/null' EXIT
SOCK="/tmp/displayxr-mcp-${APP_PID}.sock"
for _ in $(seq 1 50); do [[ -S "$SOCK" ]] && break; sleep 0.1; done
[[ -S "$SOCK" ]] || { echo "FAIL: no socket"; cat /tmp/mcp_demo_app.log; exit 1; }
sleep 2

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
def structured(r):
    return r["result"]["structured"]
passed = []
failed = []
def check(name, cond, note=""):
    (passed if cond else failed).append(f"{name}" + (f" — {note}" if note else ""))
try:
    r = call(1,"initialize",{"protocolVersion":"2024-11-05","capabilities":{},"clientInfo":{"name":"demo_maya","version":"0"}})
    check("initialize", r["result"]["serverInfo"]["name"] == "displayxr-mcp")

    s = structured(call(2,"tools/call",{"name":"list_sessions","arguments":{}}))
    check("list_sessions", len(s["sessions"]) == 1 and s["sessions"][0]["api"] == "metal",
          f"api={s['sessions'][0]['api']}")

    k = structured(call(3,"tools/call",{"name":"get_kooima_params","arguments":{}}))
    check("get_kooima_params", k["view_count"] >= 1 and len(k["views"]) == k["view_count"],
          f"view_count={k['view_count']}")

    sub = structured(call(4,"tools/call",{"name":"get_submitted_projection","arguments":{}}))
    # declared view_count may be less than kooima's (system max vs. active
    # mode count) — e.g. 2 in Anaglyph mode, 4 from xrLocateViews.
    check("get_submitted_projection",
          sub["view_count"] >= 1 and sub["view_count"] <= k["view_count"],
          f"declared={sub['view_count']} recommended={k['view_count']}")

    d = structured(call(5,"tools/call",{"name":"diff_projection","arguments":{}}))
    known = {"app_not_forwarding_locate_views_fov","app_not_forwarding_locate_views_pose",
            "fov_aspect_mismatch_subimage","fov_aspect_mismatch_display"}
    forwards = not any(f.startswith("app_not_forwarding_locate_views") for f in d["flags"])
    fwd_note = "forwards xrLocateViews" if forwards else "uses own projection math (not forwarding xrLocateViews)"
    check("diff_projection", set(d["flags"]).issubset(known),
          f"pipeline_consistent={d['pipeline_consistent']}, {fwd_note}")

    cap = structured(call(6,"tools/call",{"name":"capture_frame","arguments":{}}))
    import os
    path = cap.get("path", "")
    ok = path and os.path.isfile(path) and cap.get("size_bytes", 0) > 1024
    check("capture_frame", ok, f"{os.path.basename(path)} ({cap.get('size_bytes', 0)} bytes)")

    tl = structured(call(7,"tools/call",{"name":"tail_log","arguments":{"since":0,"max":8}}))
    check("tail_log", len(tl["entries"]) > 0, f"got {len(tl['entries'])} entries")
finally:
    try: p.stdin.close()
    except: pass
    p.wait(timeout=5)

print()
print("=== Maya stereo-debug demo ===")
for n in passed:
    print(f"  PASS  {n}")
for n in failed:
    print(f"  FAIL  {n}")
sys.exit(1 if failed else 0)
PY
RC=$?
exit $RC

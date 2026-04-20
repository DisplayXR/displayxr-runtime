#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Slice 2 (Phase B) — list_windows against a running service.
#
# Requires displayxr-service + two handle apps running. On a Windows Leia
# SR machine this is the canonical flow:
#
#   set DISPLAYXR_MCP=1
#   _package\bin\displayxr-shell.exe \
#       test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe \
#       test_apps\cube_handle_d3d11_win\build\cube_handle_d3d11_win.exe
#
# Then run this test in a second terminal. It connects via --target service
# and asserts that list_windows reports >= the expected number of clients
# (default 2; override with EXPECTED_WINDOWS env var).

set -eu
set -o pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

EXPECTED_WINDOWS="${EXPECTED_WINDOWS:-2}"

ADAPTER=""
for cand in \
    "$ROOT/_package/DisplayXR/bin/displayxr-mcp.exe" \
    "$ROOT/_package/bin/displayxr-mcp.exe" \
    "$ROOT/build/src/xrt/targets/mcp_adapter/displayxr-mcp" \
    "$ROOT/build/src/xrt/targets/mcp_adapter/displayxr-mcp.exe"; do
    if [[ -x "$cand" ]]; then
        ADAPTER="$cand"
        break
    fi
done
[[ -n "$ADAPTER" ]] || { echo "FAIL: displayxr-mcp not found" >&2; exit 1; }

echo "=== Slice 2 list_windows ==="
echo "    adapter:  $ADAPTER"
echo "    expected: >= $EXPECTED_WINDOWS windows"

# Probe that the service endpoint is reachable before running python.
if ! "$ADAPTER" --target service </dev/null >/dev/null 2>&1; then
    echo "FAIL: displayxr-service MCP endpoint not reachable" >&2
    echo "      start the service with DISPLAYXR_MCP=1 and at least $EXPECTED_WINDOWS app(s)" >&2
    exit 1
fi

python3 - "$ADAPTER" "$EXPECTED_WINDOWS" <<'PY'
import json, subprocess, sys
adapter, expected = sys.argv[1], int(sys.argv[2])

p = subprocess.Popen([adapter, "--target", "service"],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE)

def send(obj):
    b = json.dumps(obj).encode()
    p.stdin.write(f"Content-Length: {len(b)}\r\n\r\n".encode() + b)
    p.stdin.flush()

def recv():
    hdr = b""
    while not (hdr.endswith(b"\r\n\r\n") or hdr.endswith(b"\n\n")):
        c = p.stdout.read(1)
        if not c: raise RuntimeError("eof")
        hdr += c
    n = 0
    for line in hdr.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            n = int(line.split(b":",1)[1].strip())
    return json.loads(p.stdout.read(n))

send({"jsonrpc":"2.0","id":1,"method":"initialize",
      "params":{"protocolVersion":"2024-11-05","capabilities":{},
                "clientInfo":{"name":"test","version":"1"}}})
recv()

send({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{}})
r = recv()
names = {t["name"] for t in r["result"]["tools"]}
assert "list_windows" in names, f"list_windows not registered: {sorted(names)}"
print("  PASS  tools/list includes list_windows")

send({"jsonrpc":"2.0","id":3,"method":"tools/call",
      "params":{"name":"list_windows","arguments":{}}})
r = recv()
# MCP returns tool result wrapped in content[].text as JSON text.
content = r["result"]["content"]
assert isinstance(content, list) and content, f"empty content: {r}"
payload = json.loads(content[0]["text"]) if content[0].get("type") == "text" else content[0]
assert isinstance(payload, list), f"list_windows result not an array: {payload}"
print(f"  PASS  list_windows returned {len(payload)} window(s)")

if len(payload) < expected:
    print(f"FAIL: expected >= {expected} windows, got {len(payload)}", file=sys.stderr)
    sys.exit(1)

required_keys = {"id", "pid", "name", "session_active", "session_visible",
                 "session_focused", "z_order", "primary"}
for w in payload:
    missing = required_keys - set(w.keys())
    if missing:
        print(f"FAIL: window {w.get('id')} missing keys: {missing}", file=sys.stderr)
        sys.exit(1)
print("  PASS  each window has all required fields")

have_pose = sum(1 for w in payload if "pose" in w and "size" in w)
print(f"  INFO  {have_pose}/{len(payload)} windows report pose+size (shell-mode only)")

p.stdin.close(); p.wait(timeout=5)
PY

echo "test_list_windows: OK"

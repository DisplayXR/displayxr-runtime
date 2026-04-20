#!/usr/bin/env python3
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Windows helper for tests/mcp/test_list_windows.bat. Mirrors the Python
# block in the .sh test so cmd.exe users don't need Git Bash.
import json
import subprocess
import sys

adapter, expected = sys.argv[1], int(sys.argv[2])

p = subprocess.Popen(
    [adapter, "--target", "service"],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
)


def send(obj):
    b = json.dumps(obj).encode()
    p.stdin.write(f"Content-Length: {len(b)}\r\n\r\n".encode() + b)
    p.stdin.flush()


def recv():
    hdr = b""
    while not (hdr.endswith(b"\r\n\r\n") or hdr.endswith(b"\n\n")):
        c = p.stdout.read(1)
        if not c:
            raise RuntimeError("EOF")
        hdr += c
    n = 0
    for line in hdr.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            n = int(line.split(b":", 1)[1].strip())
    return json.loads(p.stdout.read(n))


try:
    send(
        {
            "jsonrpc": "2.0",
            "id": 1,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "test", "version": "1"},
            },
        }
    )
    recv()

    send({"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}})
    r = recv()
    names = {t["name"] for t in r["result"]["tools"]}
    assert "list_windows" in names, f"list_windows not registered: {sorted(names)}"
    print("  PASS  tools/list includes list_windows")

    send(
        {
            "jsonrpc": "2.0",
            "id": 3,
            "method": "tools/call",
            "params": {"name": "list_windows", "arguments": {}},
        }
    )
    r = recv()
    content = r["result"]["content"]
    assert isinstance(content, list) and content, f"empty content: {r}"
    payload = (
        json.loads(content[0]["text"]) if content[0].get("type") == "text" else content[0]
    )
    assert isinstance(payload, list), f"list_windows result not an array: {payload}"
    print(f"  PASS  list_windows returned {len(payload)} window(s)")

    if len(payload) < expected:
        print(f"FAIL: expected >= {expected} windows, got {len(payload)}", file=sys.stderr)
        sys.exit(1)

    required = {
        "id",
        "pid",
        "name",
        "session_active",
        "session_visible",
        "session_focused",
        "z_order",
        "primary",
    }
    for w in payload:
        missing = required - set(w.keys())
        if missing:
            print(f"FAIL: window {w.get('id')} missing keys: {missing}", file=sys.stderr)
            sys.exit(1)
    print("  PASS  each window has all required fields")

    have_pose = sum(1 for w in payload if "pose" in w and "size" in w)
    print(f"  INFO  {have_pose}/{len(payload)} windows report pose+size (shell-mode only)")

    p.stdin.close()
    p.wait(timeout=5)
    sys.exit(0)
except Exception as e:
    print(f"FAIL: {e}", file=sys.stderr)
    try:
        p.kill()
    except Exception:
        pass
    sys.exit(1)

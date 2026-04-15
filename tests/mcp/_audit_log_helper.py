#!/usr/bin/env python3
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Slice 6: audit log smoke. Trigger a write, confirm the log file grew
# with a line matching the tool name.

import json
import os
import subprocess
import sys
import time

adapter = sys.argv[1]


def audit_path():
    if os.name == "nt":
        return os.path.join(os.environ.get("APPDATA", ""), "DisplayXR", "mcp-audit.log")
    return os.path.join(os.path.expanduser("~"), ".config", "displayxr", "mcp-audit.log")


p = subprocess.Popen(
    [adapter, "--target", "service"],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
)

_id = 0


def next_id():
    global _id
    _id += 1
    return _id


def send(method, params=None):
    b = json.dumps({"jsonrpc": "2.0", "id": next_id(), "method": method, "params": params or {}}).encode()
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


def call(name, args):
    send("tools/call", {"name": name, "arguments": args})
    return recv()


try:
    send("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}})
    recv()

    path = audit_path()
    size_before = os.path.getsize(path) if os.path.exists(path) else 0

    call("apply_layout_preset", {"preset": "grid"})
    time.sleep(0.1)

    assert os.path.exists(path), f"audit file {path} not created"
    with open(path, "rb") as f:
        body = f.read()
    assert len(body) > size_before, "audit log did not grow"
    assert b"apply_layout_preset" in body, "audit log missing tool entry"
    print(f"  PASS  audit log at {path} grew from {size_before} to {len(body)} bytes")
    print(f"  PASS  contains 'apply_layout_preset' line")

    # Read tools (list_windows) should NOT add an audit entry.
    size_mid = len(body)
    call("list_windows", {})
    time.sleep(0.1)
    size_after = os.path.getsize(path)
    assert size_after == size_mid, f"read tool wrote to audit: {size_mid} -> {size_after}"
    print("  PASS  read tool did not add an audit entry")

    p.stdin.close()
    p.wait(timeout=5)
    print("test_audit_log: OK")
    print("")
    print("NOTE: to verify allowlist denial, restart service with")
    print("      DISPLAYXR_MCP_ALLOW=99999 and expect set_window_pose")
    print("      to return ok=false for any real client_id.")
except Exception as e:
    print(f"FAIL: {e}", file=sys.stderr)
    try:
        p.kill()
    except Exception:
        pass
    sys.exit(1)

#!/usr/bin/env python3
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Invoked by tests/mcp/test_service_handshake.bat on Windows. Sends a
# JSON-RPC initialize to the service via the stdio adapter and asserts a
# valid response. Exits 0 on pass, 1 on failure.
import json
import subprocess
import sys

adapter = sys.argv[1]
p = subprocess.Popen(
    [adapter, "--target", "service"],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
)


def send(obj):
    body = json.dumps(obj).encode()
    p.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
    p.stdin.flush()


def recv():
    hdr = b""
    while not (hdr.endswith(b"\r\n\r\n") or hdr.endswith(b"\n\n")):
        c = p.stdout.read(1)
        if not c:
            raise RuntimeError("EOF from adapter")
        hdr += c
    length = 0
    for line in hdr.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":", 1)[1].strip())
    return json.loads(p.stdout.read(length))


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
    r = recv()
    if r.get("id") != 1 or "result" not in r:
        print(f"FAIL: bad initialize response: {r}", file=sys.stderr)
        sys.exit(1)
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

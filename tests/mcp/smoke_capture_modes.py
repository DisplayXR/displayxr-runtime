#!/usr/bin/env python3
"""Smoke-test the capture_frame mode parameter against a running handle app.

Usage:
    smoke_capture_modes.py <adapter_path> <pid>

Calls capture_frame three times — default (no mode), mode=post-compose,
mode=projection-only — and asserts each returned PNG exists, is a real
PNG, and the projection-only path has the expected suffix.
"""
import json
import os
import subprocess
import sys

if len(sys.argv) != 3:
    print("usage: smoke_capture_modes.py <adapter> <pid>", file=sys.stderr)
    sys.exit(2)
adapter, pid = sys.argv[1], sys.argv[2]


def frame(obj):
    b = json.dumps(obj).encode()
    return b"Content-Length: %d\r\n\r\n" % len(b) + b


p = subprocess.Popen(
    [adapter, "--target", f"pid:{pid}"],
    stdin=subprocess.PIPE,
    stdout=subprocess.PIPE,
    stderr=subprocess.PIPE,
)


def read():
    h = b""
    while b"\r\n\r\n" not in h and b"\n\n" not in h:
        c = p.stdout.read(1)
        if not c:
            raise RuntimeError(p.stderr.read().decode("utf-8", "replace") or "EOF")
        h += c
    n = next(
        int(l.split(b":", 1)[1])
        for l in h.splitlines()
        if l.lower().startswith(b"content-length:")
    )
    return json.loads(p.stdout.read(n))


def call(i, method, params=None):
    msg = {"jsonrpc": "2.0", "id": i, "method": method}
    if params is not None:
        msg["params"] = params
    p.stdin.write(frame(msg))
    p.stdin.flush()
    return read()


def assert_capture_ok(reply, expected_mode_label, expected_suffix):
    d = reply["result"].get("structured") or reply["result"]
    if "content" in reply["result"] and not d.get("path"):
        # Some MCP servers fold structured into content[0].text.
        d = json.loads(reply["result"]["content"][0]["text"])
    assert "error" not in d, d
    assert d.get("path"), d
    assert d.get("path").endswith(expected_suffix), (d, expected_suffix)
    assert d.get("mode") == expected_mode_label, (d, expected_mode_label)
    assert d.get("size_bytes", 0) > 1024, d
    assert os.path.isfile(d["path"]), d
    with open(d["path"], "rb") as fh:
        sig = fh.read(8)
    assert sig == b"\x89PNG\r\n\x1a\n", d
    print(f"  PASS mode={expected_mode_label!r} path={os.path.basename(d['path'])} size={d['size_bytes']}")


try:
    call(
        1,
        "initialize",
        {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "smoke", "version": "0"},
        },
    )

    r = call(2, "tools/list")
    tools = {t["name"]: t for t in r["result"]["tools"]}
    assert "capture_frame" in tools, list(tools)
    schema = tools["capture_frame"].get("inputSchema") or {}
    print(f"capture_frame schema: {json.dumps(schema)[:200]}")

    print("default (no mode arg) -> expect post-compose:")
    r = call(3, "tools/call", {"name": "capture_frame", "arguments": {}})
    assert_capture_ok(r, "post-compose", ".png")

    print("explicit mode=post-compose:")
    r = call(4, "tools/call", {"name": "capture_frame", "arguments": {"mode": "post-compose"}})
    assert_capture_ok(r, "post-compose", ".png")

    print("mode=projection-only:")
    r = call(5, "tools/call", {"name": "capture_frame", "arguments": {"mode": "projection-only"}})
    assert_capture_ok(r, "projection-only", ".projection.png")

    print("mode=bogus -> expect error in payload:")
    r = call(6, "tools/call", {"name": "capture_frame", "arguments": {"mode": "bogus"}})
    d = r["result"].get("structured") or json.loads(r["result"]["content"][0]["text"])
    assert "error" in d, d
    print(f"  PASS error={d['error']!r}")

    print("\nALL CHECKS PASSED")
finally:
    try:
        p.stdin.close()
    except Exception:
        pass
    p.wait(timeout=3)

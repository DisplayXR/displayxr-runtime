#!/usr/bin/env python3
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Slice 3 round-trip: read current pose of the first shell window,
# move by a known delta, read back, assert match within tolerance,
# restore.

import json
import subprocess
import sys

adapter = sys.argv[1]

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
    body = json.dumps({"jsonrpc": "2.0", "id": next_id(), "method": method, "params": params or {}}).encode()
    p.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
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


def call_tool(name, args):
    send("tools/call", {"name": name, "arguments": args})
    r = recv()
    content = r["result"]["content"]
    if content[0].get("type") == "text":
        return json.loads(content[0]["text"])
    return content[0]


try:
    send(
        "initialize",
        {
            "protocolVersion": "2024-11-05",
            "capabilities": {},
            "clientInfo": {"name": "test", "version": "1"},
        },
    )
    recv()

    windows = call_tool("list_windows", {})
    if not windows:
        print("FAIL: no windows running under the service", file=sys.stderr)
        sys.exit(1)
    # Pick the first window that reports a pose (shell-mode only).
    target = next((w for w in windows if "pose" in w), None)
    if target is None:
        print("FAIL: no shell-mode windows reported a pose", file=sys.stderr)
        sys.exit(1)

    cid = target["id"]
    orig_pose = target["pose"]
    orig_size = target["size"]
    print(f"  INFO  target client_id={cid} name={target['name']!r}")
    print(
        f"  INFO  original pose: x={orig_pose['x']:.4f} y={orig_pose['y']:.4f} z={orig_pose['z']:.4f}"
    )

    # Apply a deliberate delta.
    dx, dy, dz = 0.03, -0.02, 0.05
    new_pose = dict(orig_pose)
    new_pose["x"] += dx
    new_pose["y"] += dy
    new_pose["z"] += dz

    r = call_tool(
        "set_window_pose",
        {"client_id": cid, "pose": new_pose, "size": orig_size},
    )
    assert r.get("ok"), f"set_window_pose failed: {r}"
    print("  PASS  set_window_pose accepted")

    # Read back.
    got = call_tool("get_window_pose", {"client_id": cid})
    got_pose = got["pose"]
    eps = 1e-3
    for k in ("x", "y", "z"):
        if abs(got_pose[k] - new_pose[k]) > eps:
            print(
                f"FAIL: {k} mismatch: got {got_pose[k]:.4f} want {new_pose[k]:.4f}",
                file=sys.stderr,
            )
            sys.exit(1)
    print("  PASS  get_window_pose matches applied pose (tol=1e-3)")

    # Restore.
    call_tool("set_window_pose", {"client_id": cid, "pose": orig_pose, "size": orig_size})
    print("  PASS  restored original pose")

    p.stdin.close()
    p.wait(timeout=5)
    print("test_set_window_pose: OK")
except Exception as e:
    print(f"FAIL: {e}", file=sys.stderr)
    try:
        p.kill()
    except Exception:
        pass
    sys.exit(1)

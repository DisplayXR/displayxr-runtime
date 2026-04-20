#!/usr/bin/env python3
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Slice 5: save_workspace -> perturb -> load_workspace round-trip.

import json
import subprocess
import sys
import time
import uuid

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
    r = recv()
    content = r["result"]["content"]
    if content[0].get("type") == "text":
        return json.loads(content[0]["text"])
    return content[0]


def poses_by_id(windows):
    return {w["id"]: w["pose"] for w in windows if "pose" in w}


try:
    send("initialize", {"protocolVersion": "2024-11-05", "capabilities": {}, "clientInfo": {"name": "t", "version": "1"}})
    recv()

    before = call("list_windows", {})
    origin = poses_by_id(before)
    if not origin:
        print("FAIL: no windows with pose", file=sys.stderr)
        sys.exit(1)

    ws = f"mcp-phase-b-test-{uuid.uuid4().hex[:8]}"

    r = call("save_workspace", {"name": ws})
    assert r.get("ok"), f"save_workspace failed: {r}"
    print(f"  PASS  save_workspace -> {r.get('path')}")

    # Perturb every pose.
    for wid, pose in origin.items():
        new_pose = dict(pose)
        new_pose["x"] += 0.04
        new_pose["y"] -= 0.03
        size = next(w["size"] for w in before if w["id"] == wid)
        call("set_window_pose", {"client_id": wid, "pose": new_pose, "size": size})
    time.sleep(0.2)

    # Confirm perturbation took effect (sanity).
    perturbed = poses_by_id(call("list_windows", {}))
    for wid, p0 in origin.items():
        if abs(perturbed[wid]["x"] - (p0["x"] + 0.04)) > 1e-3:
            print(f"FAIL: perturbation didn't land for {wid}", file=sys.stderr)
            sys.exit(1)
    print("  PASS  poses perturbed")

    r = call("load_workspace", {"name": ws})
    assert r.get("ok"), f"load_workspace failed: {r}"
    print(f"  PASS  load_workspace applied={r.get('applied')} missed={r.get('missed')}")
    time.sleep(0.2)

    restored = poses_by_id(call("list_windows", {}))
    for wid, p0 in origin.items():
        rp = restored[wid]
        for k in ("x", "y", "z"):
            if abs(rp[k] - p0[k]) > 1e-3:
                print(f"FAIL: {wid} {k}: got {rp[k]:.4f} want {p0[k]:.4f}", file=sys.stderr)
                sys.exit(1)
    print(f"  PASS  all {len(origin)} windows restored within 1e-3")

    p.stdin.close()
    p.wait(timeout=5)
    print("test_workspace_roundtrip: OK")
except Exception as e:
    print(f"FAIL: {e}", file=sys.stderr)
    try:
        p.kill()
    except Exception:
        pass
    sys.exit(1)

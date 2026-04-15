#!/usr/bin/env python3
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Slice 8: service list_sessions → pick PID → open Phase A per-session
# session via `--target pid:N` → fetch get_kooima_params.

import json
import subprocess
import sys

adapter = sys.argv[1]


def rpc_session(target):
    p = subprocess.Popen([adapter, "--target", target],
                         stdin=subprocess.PIPE, stdout=subprocess.PIPE)
    nid = {"i": 0}

    def next_id():
        nid["i"] += 1
        return nid["i"]

    def send(method, params=None):
        b = json.dumps({"jsonrpc": "2.0", "id": next_id(),
                        "method": method, "params": params or {}}).encode()
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
        c = r["result"]["content"]
        return json.loads(c[0]["text"]) if c[0].get("type") == "text" else c[0]

    send("initialize", {"protocolVersion": "2024-11-05", "capabilities": {},
                        "clientInfo": {"name": "t", "version": "1"}})
    recv()

    return p, call


try:
    svc_p, svc_call = rpc_session("service")
    sessions = svc_call("list_sessions", {})
    if not sessions:
        print("FAIL: service list_sessions empty", file=sys.stderr)
        sys.exit(1)
    required = {"client_id", "pid", "name", "session_active"}
    for s in sessions:
        missing = required - set(s.keys())
        if missing:
            print(f"FAIL: session missing keys: {missing}", file=sys.stderr)
            sys.exit(1)
    print(f"  PASS  service list_sessions returned {len(sessions)} session(s)")

    # Pick an active session and reach into its per-PID MCP server.
    target_session = next((s for s in sessions if s["session_active"]), sessions[0])
    pid = int(target_session["pid"])
    svc_p.stdin.close()
    svc_p.wait(timeout=5)

    app_p, app_call = rpc_session(f"pid:{pid}")
    kooima = app_call("get_kooima_params", {})
    assert isinstance(kooima, dict), f"unexpected get_kooima_params result: {kooima}"
    assert "view_count" in kooima or "view_count_recommended" in kooima, (
        f"kooima missing view_count: {kooima}"
    )
    print(f"  PASS  reached Phase A get_kooima_params via pid:{pid}")

    app_p.stdin.close()
    app_p.wait(timeout=5)

    print("test_session_routing: OK")
except Exception as e:
    print(f"FAIL: {e}", file=sys.stderr)
    sys.exit(1)

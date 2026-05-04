#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Windows Phase A end-to-end test — equivalent of the mac-only
# test_handshake.sh + test_core_tools.sh + test_diff_projection.sh
# scripts. Drives the runtime's per-app MCP server (in-process Phase A,
# bound to \\.\pipe\displayxr-mcp-<pid>) through one initialize +
# tools/list + several tools/call round-trips and asserts the full
# Phase A surface is registered.
#
# Run from Git Bash on a Windows + Leia SR machine after
# scripts\build_windows.bat all has produced the dev runtime + cube.
#
# Exit 0 = pass, nonzero = fail.

set -u
set -o pipefail

case "$(uname -s)" in
	MINGW*|MSYS*|CYGWIN*) ;;
	*) echo "skip: windows-only test" >&2; exit 0 ;;
esac

cd "$(dirname "$0")/../.."
ROOT="$(pwd)"

ADAPTER=""
for cand in \
    "$ROOT/_package/bin/displayxr-mcp.exe" \
    "$ROOT/build/_deps/displayxr_mcp-build/Release/displayxr-mcp.exe"; do
	if [[ -x "$cand" ]]; then
		ADAPTER="$cand"
		break
	fi
done
APP="$ROOT/test_apps/cube_handle_d3d11_win/build/cube_handle_d3d11_win.exe"
RUNTIME_JSON="$ROOT/build/Release/openxr_displayxr-dev.json"

if [[ -z "$ADAPTER" ]]; then
	echo "FAIL: displayxr-mcp.exe not found — run scripts\\build_windows.bat all first" >&2
	exit 1
fi
for f in "$APP" "$RUNTIME_JSON"; do
	if [[ ! -e "$f" ]]; then
		echo "FAIL: missing $f — run scripts\\build_windows.bat all first" >&2
		exit 1
	fi
done

export DISPLAYXR_MCP=1
export XR_RUNTIME_JSON="$RUNTIME_JSON"
export VK_LOADER_LAYERS_DISABLE='*'

LOG="$(mktemp)"
"$APP" >"$LOG" 2>&1 &
APP_PID=$!

cleanup() {
	kill "$APP_PID" 2>/dev/null || true
	wait "$APP_PID" 2>/dev/null || true
	rm -f "$LOG"
}
trap cleanup EXIT

# Wait for the cube's per-PID MCP server to bind. The adapter's --list
# enumerates active session pids; we don't try to match $APP_PID since
# Git Bash's $! is a bash-internal pid that does not equal the Windows
# pid of the launched .exe. We only require *some* session to appear,
# and rely on --target auto below to connect to the single session.
for _ in $(seq 1 50); do
	if "$ADAPTER" --list 2>/dev/null | grep -qE '^[0-9]+$'; then
		break
	fi
	sleep 0.2
done
if ! "$ADAPTER" --list 2>/dev/null | grep -qE '^[0-9]+$'; then
	echo "FAIL: no Phase A MCP session appeared after launching cube" >&2
	cat "$LOG" >&2 || true
	exit 1
fi

PYTHONIOENCODING=utf-8 python3 - "$ADAPTER" <<'PY'
import json
import subprocess
import sys

ADAPTER = sys.argv[1]
REQUIRED = {"echo", "tail_log", "list_sessions", "get_display_info",
            "get_runtime_metrics", "get_kooima_params",
            "get_submitted_projection", "diff_projection", "capture_frame"}


def frame(o):
    b = json.dumps(o).encode()
    return b"Content-Length: %d\r\n\r\n" % len(b) + b


def read_frame(p):
    hdr = b""
    while not (hdr.endswith(b"\r\n\r\n") or hdr.endswith(b"\n\n")):
        c = p.stdout.read(1)
        if not c:
            raise RuntimeError("EOF")
        hdr += c
    n = 0
    for line in hdr.splitlines():
        if line.lower().startswith(b"content-length:"):
            n = int(line.split(b":", 1)[1].strip())
    return json.loads(p.stdout.read(n))


def text_payload(r):
    c = r["result"]["content"]
    return json.loads(c[0]["text"]) if c[0].get("type") == "text" else c[0]


p = subprocess.Popen([ADAPTER, "--target", "auto"],
                     stdin=subprocess.PIPE, stdout=subprocess.PIPE)
try:
    p.stdin.write(frame({"jsonrpc": "2.0", "id": 1, "method": "initialize",
                         "params": {"protocolVersion": "2024-11-05", "capabilities": {},
                                    "clientInfo": {"name": "phase-a-win", "version": "1"}}}))
    p.stdin.flush()
    r = read_frame(p)
    name = r.get("result", {}).get("serverInfo", {}).get("name")
    assert name == "displayxr-mcp", r
    print(f"  PASS  initialize (server={name!r})")

    p.stdin.write(frame({"jsonrpc": "2.0", "id": 2, "method": "tools/list", "params": {}}))
    p.stdin.flush()
    names = {t["name"] for t in read_frame(p)["result"]["tools"]}
    missing = REQUIRED - names
    if missing:
        print(f"  FAIL  tools/list missing: {sorted(missing)}", file=sys.stderr)
        sys.exit(1)
    print(f"  PASS  tools/list registers all {len(REQUIRED)} required Phase A tools")

    p.stdin.write(frame({"jsonrpc": "2.0", "id": 3, "method": "tools/call",
                         "params": {"name": "echo", "arguments": {"hello": "phase-a"}}}))
    p.stdin.flush()
    r = read_frame(p)
    structured = r["result"].get("structured")
    assert structured == {"echo": {"hello": "phase-a"}}, r
    print("  PASS  tools/call echo round-trip")

    p.stdin.write(frame({"jsonrpc": "2.0", "id": 4, "method": "tools/call",
                         "params": {"name": "list_sessions", "arguments": {}}}))
    p.stdin.flush()
    payload = text_payload(read_frame(p))
    sessions = payload["sessions"] if isinstance(payload, dict) else payload
    assert isinstance(sessions, list) and sessions, payload
    print(f"  PASS  list_sessions returned {len(sessions)} session(s)")

    p.stdin.write(frame({"jsonrpc": "2.0", "id": 5, "method": "tools/call",
                         "params": {"name": "get_display_info", "arguments": {}}}))
    p.stdin.flush()
    info = text_payload(read_frame(p))
    assert isinstance(info, dict) and info, info
    print(f"  PASS  get_display_info ({len(info)} fields)")

    p.stdin.write(frame({"jsonrpc": "2.0", "id": 6, "method": "tools/call",
                         "params": {"name": "get_runtime_metrics", "arguments": {}}}))
    p.stdin.flush()
    metrics = text_payload(read_frame(p))
    assert isinstance(metrics, dict) and metrics, metrics
    print(f"  PASS  get_runtime_metrics ({len(metrics)} fields)")

    p.stdin.write(frame({"jsonrpc": "2.0", "id": 7, "method": "tools/call",
                         "params": {"name": "diff_projection", "arguments": {}}}))
    p.stdin.flush()
    proj = text_payload(read_frame(p))
    # diff_projection returns {ok, flags, ...}; flags can be empty for a
    # well-behaved app or carry e.g. "app_not_forwarding_locate_views_pose"
    # for the demo cube. Either is acceptable here — we just want the
    # tool to dispatch and return structured output.
    assert isinstance(proj, dict) and ("ok" in proj or "flags" in proj), proj
    print(f"  PASS  diff_projection (flags={proj.get('flags')})")
finally:
    try:
        p.stdin.close()
    except Exception:
        pass
    p.wait(timeout=5)

print("test_phase_a_win.sh: OK")
PY

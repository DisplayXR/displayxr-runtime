#!/usr/bin/env bash
# Copyright 2026, DisplayXR / Leia Inc.
# SPDX-License-Identifier: BSL-1.0
#
# Slice 1 (Phase B) — service-mode adapter handshake.
#
# Validates:
#   1. `--list` succeeds with no MCP session running (empty output, exit 0).
#   2. `--target service` exits non-zero when no service is running.
#   3. `--target auto` falls back to a unique handle-app session when no
#      service is running.
#   4. When the service is up with DISPLAYXR_MCP=1, `--target service`
#      completes a JSON-RPC initialize handshake.
#
# On macOS (no service binary) only steps 1-3 run. The script is also
# safe to invoke on Windows via Git Bash; a .bat equivalent lives
# alongside for cmd.exe consumption.

set -eu
set -o pipefail

ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$ROOT"

# Locate the adapter (macOS build tree, Windows _package).
ADAPTER=""
for cand in \
    "$ROOT/build/src/xrt/targets/mcp_adapter/displayxr-mcp" \
    "$ROOT/build/src/xrt/targets/mcp_adapter/displayxr-mcp.exe" \
    "$ROOT/_package/DisplayXR/bin/displayxr-mcp.exe" \
    "$ROOT/_package/bin/displayxr-mcp.exe"; do
    if [[ -x "$cand" ]]; then
        ADAPTER="$cand"
        break
    fi
done
if [[ -z "$ADAPTER" ]]; then
    echo "FAIL: displayxr-mcp not found; build first" >&2
    exit 1
fi

fail() {
    echo "FAIL: $*" >&2
    exit 1
}

echo "=== Slice 1 service-handshake ==="
echo "    adapter: $ADAPTER"

# 1. --list works with nothing running.
"$ADAPTER" --list >/dev/null || fail "--list returned non-zero"
echo "  PASS  --list (no sessions)"

# 2. --target service fails cleanly when no service is running.
if "$ADAPTER" --target service </dev/null >/dev/null 2>&1; then
    fail "--target service succeeded with no service running"
fi
echo "  PASS  --target service fails with no service"

# 3. --target auto falls through when no service. On macOS we can't
#    actually launch a handle app from this test without heavy setup,
#    so we assert the "no sessions" error path here; test_handshake.sh
#    covers the single-session case.
out=$("$ADAPTER" --target auto </dev/null 2>&1 || true)
if ! echo "$out" | grep -q "no running MCP sessions"; then
    fail "--target auto fallback did not report 'no running MCP sessions': $out"
fi
echo "  PASS  --target auto fallback"

# 4. Service handshake (Windows-only; skipped on macOS where the
#    service binary is not built).
SERVICE=""
for cand in \
    "$ROOT/_package/DisplayXR/bin/displayxr-service.exe" \
    "$ROOT/_package/bin/displayxr-service.exe" \
    "$ROOT/build/src/xrt/targets/service/displayxr-service"; do
    if [[ -x "$cand" ]]; then
        SERVICE="$cand"
        break
    fi
done

if [[ -z "$SERVICE" ]]; then
    echo "  SKIP  service handshake (no displayxr-service binary on this platform)"
    echo "test_service_handshake: OK"
    exit 0
fi

echo "    service: $SERVICE"
DISPLAYXR_MCP=1 "$SERVICE" --shell &
SVC_PID=$!
cleanup() { kill -TERM "$SVC_PID" 2>/dev/null || true; }
trap cleanup EXIT

# Wait up to 3 s for the service to bind its MCP endpoint.
for i in 1 2 3 4 5 6; do
    if "$ADAPTER" --target service </dev/null >/dev/null 2>&1; then
        break
    fi
    sleep 0.5
done

# Scripted JSON-RPC initialize handshake via python.
python3 - "$ADAPTER" <<'PY' || fail "initialize handshake"
import json, subprocess, sys
adapter = sys.argv[1]
p = subprocess.Popen([adapter, "--target", "service"],
                    stdin=subprocess.PIPE, stdout=subprocess.PIPE)
def send(obj):
    body = json.dumps(obj).encode()
    p.stdin.write(f"Content-Length: {len(body)}\r\n\r\n".encode() + body)
    p.stdin.flush()
def recv():
    hdr = b""
    while not hdr.endswith(b"\r\n\r\n") and not hdr.endswith(b"\n\n"):
        c = p.stdout.read(1)
        if not c: raise RuntimeError("eof")
        hdr += c
    length = 0
    for line in hdr.split(b"\r\n"):
        if line.lower().startswith(b"content-length:"):
            length = int(line.split(b":",1)[1].strip())
    return json.loads(p.stdout.read(length))

send({"jsonrpc":"2.0","id":1,"method":"initialize",
      "params":{"protocolVersion":"2024-11-05","capabilities":{},
                "clientInfo":{"name":"test","version":"1"}}})
r = recv()
assert r.get("id") == 1 and "result" in r, f"bad initialize: {r}"
p.stdin.close()
p.wait(timeout=5)
PY

echo "  PASS  initialize against service"
echo "test_service_handshake: OK"

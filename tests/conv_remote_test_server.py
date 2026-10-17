#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""A local test endpoint implementing the remote conversion contract v1 (ADR-057 §6).

Stdlib only. Serves on 127.0.0.1 (plain http is allowed on loopback only) and
prints ONE line, `PORT <n>`, once it listens, then serves until GET /__quit or
--lifetime seconds pass.

    GET  /v1/describe   tasks, limits, licence class, build stamp
    POST /v1/jobs       multipart `params` (JSON) + `image` | `left` + `right`
                        200: body = a fake SOG (a stored zip with meta.json),
                             header X-DXR-Result = the facts
    GET  /__stats       {"describe": n, "jobs": n, "last_params": {...}, "last_auth": bool}
    GET  /__quit        exit

--mode picks the job behaviour:
    ok              200 with a fake SOG, non-metric mono / metric stereo when a baseline is given
    refuse-metric   200, but the metric fit was refused (refusal metric_fit_refused, metric false)
    422             422 {"code": "unrectified_pair", "message": ...}: bad input, never retried
    503             503 on every job (a host that falls through)
    slow            sleeps --delay seconds before answering (cancel / timeout tests)
--token T makes every request need `Authorization: Bearer T` (401 otherwise).
--licence commercial|research_only|unknown, --build <commit> set what /v1/describe says.
"""

import argparse
import io
import json
import sys
import threading
import time
import zipfile
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer


def fake_sog(count: int) -> bytes:
    """A stored zip shaped like a SOG: meta.json (+ a tiny placeholder means file)."""
    meta = {
        "version": 2,
        "asset": {"generator": "conv_remote_test_server"},
        "count": count,
        "antialias": False,
        "means": {"mins": [-1, -1, 0.5], "maxs": [1, 1, 4], "files": ["means_l.webp", "means_u.webp"]},
    }
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w", compression=zipfile.ZIP_STORED) as z:
        z.writestr("meta.json", json.dumps(meta, indent=2))
        z.writestr("means_l.webp", b"RIFF\x00\x00\x00\x00WEBPfake-means-l")
        z.writestr("means_u.webp", b"RIFF\x00\x00\x00\x00WEBPfake-means-u")
    return buf.getvalue()


def parse_multipart(body: bytes, content_type: str):
    """{name: bytes} for a multipart/form-data body (enough for the tests)."""
    boundary = None
    for part in content_type.split(";"):
        part = part.strip()
        if part.startswith("boundary="):
            boundary = part[len("boundary="):].strip('"')
    if boundary is None:
        return {}
    out = {}
    delim = b"--" + boundary.encode()
    for chunk in body.split(delim):
        chunk = chunk.strip(b"\r\n")
        if not chunk or chunk == b"--":
            continue
        head, _, data = chunk.partition(b"\r\n\r\n")
        name = None
        for line in head.split(b"\r\n"):
            if line.lower().startswith(b"content-disposition"):
                for item in line.split(b";"):
                    item = item.strip()
                    if item.startswith(b"name="):
                        name = item[5:].strip(b'"').decode()
        if name is not None:
            out[name] = data
    return out


class State:
    def __init__(self, args):
        self.args = args
        self.lock = threading.Lock()
        self.describe = 0
        self.jobs = 0
        self.last_params = None
        self.last_auth = False


def make_handler(state: State):
    a = state.args

    class H(BaseHTTPRequestHandler):
        protocol_version = "HTTP/1.1"

        def log_message(self, *args):  # quiet
            pass

        def _send(self, code, body=b"", ctype="application/json", headers=None):
            self.send_response(code)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            for k, v in (headers or {}).items():
                self.send_header(k, v)
            self.end_headers()
            if body:
                try:
                    self.wfile.write(body)
                except (BrokenPipeError, ConnectionResetError):
                    pass

        def _authorised(self) -> bool:
            got = self.headers.get("Authorization", "")
            with state.lock:
                state.last_auth = got.startswith("Bearer ")
            return not a.token or got == "Bearer " + a.token

        def do_GET(self):
            if self.path == "/__quit":
                self._send(200, b"{}")
                threading.Thread(target=self.server.shutdown, daemon=True).start()
                return
            if self.path == "/__stats":
                with state.lock:
                    body = json.dumps({"describe": state.describe, "jobs": state.jobs,
                                       "last_params": state.last_params, "last_auth": state.last_auth})
                self._send(200, body.encode())
                return
            if self.path != "/v1/describe":
                self._send(404, b'{"code":"not_found"}')
                return
            if not self._authorised():
                self._send(401, b'{"code":"unauthorized","message":"bearer token required"}')
                return
            with state.lock:
                state.describe += 1
            body = {
                "contract": 1,
                "endpoint_id": a.endpoint_id,
                "tasks": [
                    {"task": "splats-mono", "formats": ["sog", "ply"], "max_long_side": a.max_long_side,
                     "max_bytes": 64 * 1024 * 1024, "rectified_required": False},
                    {"task": "splats-stereo", "formats": ["sog"], "max_long_side": a.max_long_side,
                     "max_bytes": 64 * 1024 * 1024, "rectified_required": True},
                ],
                "licence": {"class": a.licence, "name": "LicenseRef-test-model", "link": "https://example.invalid/licence"},
                "build": {"provider_commit": a.build, "model_id": "test/fake-splats@1",
                          "model_sha256": "ab" * 32, "runtime": "python-stdlib",
                          "runtime_version": sys.version.split()[0], "host_kind": "test", "host_id": a.host_id},
            }
            self._send(200, json.dumps(body).encode())

        def do_POST(self):
            if self.path != "/v1/jobs":
                self._send(404, b'{"code":"not_found"}')
                return
            length = int(self.headers.get("Content-Length", "0"))
            body = self.rfile.read(length) if length > 0 else b""
            if not self._authorised():
                self._send(401, b'{"code":"unauthorized","message":"bearer token required"}')
                return
            parts = parse_multipart(body, self.headers.get("Content-Type", ""))
            params = json.loads(parts.get("params", b"{}").decode() or "{}")
            with state.lock:
                state.jobs += 1
                state.last_params = params
            if a.mode == "503":
                self._send(503, b'{"code":"overloaded","message":"try the next host"}')
                return
            if a.mode == "422":
                self._send(422, json.dumps({"code": "unrectified_pair",
                                            "message": "this endpoint needs a rectified pair"}).encode())
                return
            if a.mode == "slow":
                time.sleep(a.delay)
            stereo = params.get("task") == "splats-stereo"
            image = parts.get("left" if stereo else "image", b"")
            if not image.startswith(b"\x89PNG"):
                self._send(422, b'{"code":"unsupported_input","message":"expected a PNG image"}')
                return
            baseline = params.get("baseline_m")
            refused = a.mode == "refuse-metric"
            metric = bool(baseline) and not refused
            result = {
                "build": {"provider_commit": a.build, "model_id": "test/fake-splats@1", "model_sha256": "ab" * 32,
                          "runtime": "python-stdlib", "runtime_version": sys.version.split()[0],
                          "host_kind": "test", "host_id": a.host_id},
                "coords": "opencv",
                "intrinsics": {"fx": 1000.0, "fy": 1000.0, "cx": 320.0 - (4.0 if metric else 0.0), "cy": 240.0,
                               "width": 640, "height": 480, "source": "estimated"},
                "metric": metric,
                "near_m": 0.8, "far_m": 6.5,
                "layers": [{"visible": True, "inpainted": False}, {"visible": False, "inpainted": True}],
                "splat_count": 1179648,
                "focus_suggestion": {"point": [0, 0, 1.9], "subject_m": 1.9, "near_m": 0.8, "far_m": 6.5,
                                     "source": "nearest-clump"},
                "timings_ms": {"total": 12},
            }
            if metric:
                result["baseline_m"] = baseline
                result["metric_fit"] = {"scale": 1.25, "source": "stereo", "inlier_fraction": 0.91,
                                        "mad_log": 0.04, "principal_point_shift_px": 4.0}
            if refused:
                result["refusal"] = {"code": "metric_fit_refused",
                                     "reason": "too few inliers (0.31 < 0.6): returned non-metric"}
                result["metric_fit"] = {"scale": 0.0, "source": "stereo", "inlier_fraction": 0.31, "mad_log": 0.4}
            fmt = params.get("format", "sog")
            blob = fake_sog(1179648) if fmt == "sog" else b"ply\nformat binary_little_endian 1.0\nend_header\n"
            result["format"] = fmt
            self._send(200, blob, "application/octet-stream", {"X-DXR-Result": json.dumps(result)})

    return H


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=0)
    ap.add_argument("--mode", default="ok", choices=["ok", "refuse-metric", "422", "503", "slow"])
    ap.add_argument("--delay", type=float, default=5.0)
    ap.add_argument("--token", default="")
    ap.add_argument("--licence", default="commercial", choices=["commercial", "research_only", "unknown"])
    ap.add_argument("--build", default="test-build-1")
    ap.add_argument("--endpoint-id", default="test-endpoint")
    ap.add_argument("--host-id", default="host-a")
    ap.add_argument("--max-long-side", type=int, default=1536)
    ap.add_argument("--lifetime", type=float, default=300.0)
    args = ap.parse_args()
    state = State(args)
    srv = ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(state))
    srv.daemon_threads = True
    print("PORT %d" % srv.server_address[1], flush=True)
    timer = threading.Timer(args.lifetime, srv.shutdown)
    timer.daemon = True
    timer.start()
    srv.serve_forever()


if __name__ == "__main__":
    main()

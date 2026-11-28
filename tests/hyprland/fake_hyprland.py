#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""A stand-in for Hyprland's IPC, for testing displayxr-hypr-geometry-bridge
and the panel lookup without a compositor (CI has none).

Serves, in --dir (= $XDG_RUNTIME_DIR/hypr/$HYPRLAND_INSTANCE_SIGNATURE):

- .socket.sock: one request per connection. `j/clients` answers the current
  content of --clients (re-read every time, so a test moves a window by
  rewriting that file), `j/monitors` answers --monitors, `monitors` answers
  --monitors-text; anything else answers "unknown request".
- .socket2.sock: every line written to the FIFO <dir>/events.fifo is sent to
  every connected event client, as Hyprland's `EVENT>>DATA` lines.

Stops on SIGTERM/SIGINT, removing its sockets.
"""

import argparse
import os
import signal
import socket
import sys
import threading


def read(path):
    with open(path, "rb") as f:
        return f.read()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", required=True)
    ap.add_argument("--clients", required=True)
    ap.add_argument("--monitors", required=True)
    ap.add_argument("--monitors-text", required=True)
    args = ap.parse_args()

    os.makedirs(args.dir, exist_ok=True)
    req_path = os.path.join(args.dir, ".socket.sock")
    ev_path = os.path.join(args.dir, ".socket2.sock")
    fifo_path = os.path.join(args.dir, "events.fifo")
    for p in (req_path, ev_path, fifo_path):
        if os.path.exists(p):
            os.unlink(p)
    os.mkfifo(fifo_path)

    req = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    req.bind(req_path)
    req.listen(16)
    ev = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    ev.bind(ev_path)
    ev.listen(16)

    event_clients = []
    lock = threading.Lock()
    stop = threading.Event()

    def serve_requests():
        while not stop.is_set():
            try:
                conn, _ = req.accept()
            except OSError:
                return
            with conn:
                conn.settimeout(1.0)
                try:
                    data = conn.recv(4096).decode(errors="replace").strip()
                except OSError:
                    continue
                if data == "j/clients":
                    reply = read(args.clients)
                elif data == "j/monitors":
                    reply = read(args.monitors)
                elif data == "monitors":
                    reply = read(args.monitors_text)
                elif data == "":
                    continue  # a liveness probe
                else:
                    reply = b"unknown request"
                try:
                    conn.sendall(reply)
                except OSError:
                    pass

    def accept_events():
        while not stop.is_set():
            try:
                conn, _ = ev.accept()
            except OSError:
                return
            with lock:
                event_clients.append(conn)

    def pump_fifo():
        while not stop.is_set():
            with open(fifo_path, "rb") as f:  # blocks until a writer opens it
                for line in f:
                    line = line.rstrip(b"\n") + b"\n"
                    with lock:
                        for c in list(event_clients):
                            try:
                                c.sendall(line)
                            except OSError:
                                event_clients.remove(c)

    for fn in (serve_requests, accept_events, pump_fifo):
        threading.Thread(target=fn, daemon=True).start()

    def on_signal(*_):
        stop.set()

    signal.signal(signal.SIGTERM, on_signal)
    signal.signal(signal.SIGINT, on_signal)
    print("fake-hyprland: serving", args.dir, flush=True)
    stop.wait()

    req.close()
    ev.close()
    with lock:
        for c in event_clients:
            c.close()
    for p in (req_path, ev_path, fifo_path):
        try:
            os.unlink(p)
        except OSError:
            pass
    return 0


if __name__ == "__main__":
    sys.exit(main())

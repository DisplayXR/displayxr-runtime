#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Reference feeder for the net_input provider (ADR-034 / #823 Phase 2).

Connects to the runtime's net_input provider on loopback TCP, streams
sine-wave motion-controller poses + a scripted button pattern for both
hands, and prints haptic events the runtime sends back. Doubles as the
executable specification of the wire protocol in
`src/xrt/drivers/net_input/net_input_proto.h` — the struct layouts below
are the normative v1 frame formats.

Wire protocol v1 (all little-endian; loopback only):

  handshake  both sides send immediately after connect:
             <II>  magic=0x49525844 ("DXRI"), version=1

  STATE      feeder -> provider, 72 bytes:
             <I4Bq13fI>  type=1, hand(0=L,1=R), active, buttons
             (bit0 select, bit1 menu), battery_pct (255=unknown),
             timestamp_ns (feeder monotonic; 0 = stamp on receipt),
             position[3], orientation quat[4] (x,y,z,w),
             linear_velocity[3], angular_velocity[3], reserved=0

  HAPTIC     provider -> feeder, 24 bytes:
             <IB3xffq>  type=2, hand, amplitude, frequency, duration_ns

Usage:
  python3 scripts/net_input_feeder.py                    # feed until ^C
  python3 scripts/net_input_feeder.py --duration 10      # feed 10 s
  python3 scripts/net_input_feeder.py --assert-haptic --duration 15
      # exit 0 iff at least one haptic event arrived (CI round-trip)
"""

import argparse
import math
import select
import socket
import struct
import sys
import time

MAGIC = 0x49525844  # "DXRI"
VERSION = 1
DEFAULT_PORT = 9427

MSG_STATE = 1
MSG_HAPTIC = 2

HELLO = struct.Struct("<II")
STATE = struct.Struct("<I4Bq13fI")
HAPTIC = struct.Struct("<IB3xffq")

BUTTON_SELECT = 1 << 0
BUTTON_MENU = 1 << 1


def state_packet(hand: int, t: float) -> bytes:
    """Sine-wave pose + scripted buttons for one hand at time t (seconds)."""
    side = -1.0 if hand == 0 else 1.0
    phase = math.pi if hand == 0 else 0.0

    # Lissajous-ish sweep in the tabletop-scale scene: LOCAL space sits at
    # stage height 1.6 m, so y ≈ 1.65 lands just above a test app's cube.
    x = side * 0.12 + 0.05 * math.sin(2.0 * math.pi * t / 3.0 + phase)
    y = 1.65 + 0.05 * math.sin(2.0 * math.pi * t / 2.0 + phase)
    z = -0.05
    # Gentle yaw wobble.
    half = 0.5 * 0.3 * math.sin(2.0 * math.pi * t / 4.0)
    quat = (0.0, math.sin(half), 0.0, math.cos(half))

    buttons = 0
    if int(t) % 2 == (0 if hand == 0 else 1):  # hands alternate select
        buttons |= BUTTON_SELECT
    if t % 5.0 < 0.5:
        buttons |= BUTTON_MENU

    return STATE.pack(
        MSG_STATE,
        hand,
        1,  # active
        buttons,
        87,  # battery_pct
        time.monotonic_ns(),
        x, y, z,
        *quat,
        0.0, 0.0, 0.0,  # linear velocity (provider predicts fine without)
        0.0, 0.0, 0.0,  # angular velocity
        0,  # reserved
    )


def recv_exact(sock: socket.socket, n: int) -> bytes:
    """Read exactly n bytes. MSG_WAITALL is unusable here: Windows raises
    WinError 10045 when the socket has a timeout set (create_connection)."""
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed during handshake")
        buf += chunk
    return buf


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--port", type=int, default=DEFAULT_PORT)
    ap.add_argument("--rate", type=float, default=120.0, help="state updates per second per hand")
    ap.add_argument("--duration", type=float, default=0.0, help="seconds to run; 0 = until ^C")
    ap.add_argument("--assert-haptic", action="store_true",
                    help="exit 0 only if at least one haptic event was received")
    args = ap.parse_args()

    sock = socket.create_connection(("127.0.0.1", args.port), timeout=5.0)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    # Handshake: send ours, read+validate theirs.
    sock.sendall(HELLO.pack(MAGIC, VERSION))
    theirs = recv_exact(sock, HELLO.size)
    magic, version = HELLO.unpack(theirs)
    if magic != MAGIC or version != VERSION:
        print(f"handshake mismatch: magic=0x{magic:08x} version={version}", file=sys.stderr)
        return 2
    print(f"connected to net_input on 127.0.0.1:{args.port} (proto v{version})")

    sock.setblocking(False)
    haptics_seen = 0
    start = time.monotonic()
    period = 1.0 / args.rate
    rx = b""

    try:
        while True:
            t = time.monotonic() - start
            if args.duration > 0 and t >= args.duration:
                break

            try:
                sock.sendall(state_packet(0, t) + state_packet(1, t))
            except (BrokenPipeError, ConnectionResetError):
                print("provider closed the connection", file=sys.stderr)
                break

            # Drain any haptic events.
            readable, _, _ = select.select([sock], [], [], 0)
            if readable:
                try:
                    chunk = sock.recv(4096)
                except BlockingIOError:
                    chunk = b""
                if not chunk:
                    print("provider closed the connection", file=sys.stderr)
                    break
                rx += chunk
                while len(rx) >= HAPTIC.size:
                    msg_type, hand, amplitude, frequency, duration_ns = HAPTIC.unpack(
                        rx[:HAPTIC.size])
                    rx = rx[HAPTIC.size:]
                    if msg_type != MSG_HAPTIC:
                        print(f"unexpected message type {msg_type}", file=sys.stderr)
                        return 2
                    haptics_seen += 1
                    print(f"HAPTIC hand={'L' if hand == 0 else 'R'} "
                          f"amplitude={amplitude:.2f} frequency={frequency:.1f} "
                          f"duration={duration_ns / 1e6:.1f}ms")

            time.sleep(period)
    except KeyboardInterrupt:
        pass
    finally:
        sock.close()

    print(f"fed {t:.1f}s, received {haptics_seen} haptic event(s)")
    if args.assert_haptic and haptics_seen == 0:
        print("FAIL: no haptic event received", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

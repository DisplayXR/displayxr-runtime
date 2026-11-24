# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Where the stereo camera stands: a small Python API over whatever moves it.

Positions are in millimetres in the DISPLAY's frame, the one SR reports eyes in: x to the
right, y up, z out of the screen towards the viewer, origin at the screen centre. A
positioner maps them onto its own axes and moves there; move_to() returns once the camera
has stopped (so the next photo is not blurred).

    pos = open_positioner("grbl:COM5", axes={"x": "X", "z": "Y"}, origin={"x": 0, "z": 600})
    pos.home()
    pos.move_to(x=-100, z=600)
    pos.close()

Kinds:
  none           the camera does not move (one position, wherever it is)
  manual         prints where to put the camera and waits for Enter
  grbl:<port>    a GRBL motion controller (Arduino + CNC shield, most belt/ball-screw rails
                 and XY gantries) over USB serial: G-code moves, '?' polled until Idle.
                 Needs pyserial.
  sim            logs the moves (dry runs and tests)
A robot arm is one more class with the same three methods.
"""
import time


class Positioner:
    """Base: move_to(x, y, z) in display mm, home(), close(). Unknown axes are ignored."""

    name = "none"

    def home(self):
        pass

    def move_to(self, x=None, y=None, z=None):
        pass

    def close(self):
        pass


class NoPositioner(Positioner):
    name = "none"


class SimPositioner(Positioner):
    name = "sim"

    def __init__(self, log=print):
        self.log = log
        self.at = {}

    def home(self):
        self.log("sim: home")
        self.at = {}

    def move_to(self, x=None, y=None, z=None):
        self.at.update({k: v for k, v in (("x", x), ("y", y), ("z", z)) if v is not None})
        self.log(f"sim: at {self.at}")


class ManualPositioner(Positioner):
    name = "manual"

    def __init__(self, ask=input, log=print):
        self.ask, self.log = ask, log

    def move_to(self, x=None, y=None, z=None):
        where = ", ".join(f"{k} = {v:+.0f} mm" for k, v in (("x", x), ("y", y), ("z", z)) if v is not None)
        self.ask(f"Put the camera at {where} (display frame: x right, y up, z out of the screen), then press Enter: ")


class GrblPositioner(Positioner):
    """GRBL 1.1 over serial. `axes` maps display axes to machine axes, e.g. {"x": "X", "z": "Y"};
    `origin` is the display position (mm) of machine zero; `sign` flips an axis (+1/-1)."""

    name = "grbl"

    def __init__(self, port, axes=None, origin=None, sign=None, feed_mm_min=3000, baud=115200,
                 timeout_s=120.0, log=print, serial_factory=None):
        real_port = serial_factory is None
        if real_port:
            import serial  # pyserial
            serial_factory = lambda: serial.Serial(port, baud, timeout=0.2)  # noqa: E731
        self.ser = serial_factory()
        self.axes = axes or {"x": "X"}
        self.origin = origin or {}
        self.sign = sign or {}
        self.feed = feed_mm_min
        self.timeout_s = timeout_s
        self.log = log
        if real_port:
            time.sleep(2.0)                    # GRBL resets when the port opens
        self.ser.write(b"\r\n\r\n")
        time.sleep(0.1)
        self.ser.reset_input_buffer()
        self._command("$X")                    # clear an alarm left from power-up
        self._command("G21 G90")               # millimetres, absolute

    def _command(self, line):
        self.ser.write((line + "\n").encode())
        end = time.time() + self.timeout_s
        while time.time() < end:
            reply = self.ser.readline().decode(errors="replace").strip()
            if reply == "ok":
                return
            if reply.startswith("error") or reply.startswith("ALARM"):
                raise RuntimeError(f"GRBL: {line!r} -> {reply}")
        raise RuntimeError(f"GRBL: no 'ok' for {line!r}")

    def _wait_idle(self):
        end = time.time() + self.timeout_s
        while time.time() < end:
            self.ser.write(b"?")
            reply = self.ser.readline().decode(errors="replace").strip()
            if reply.startswith("<Idle"):
                return
            if reply.startswith("<Alarm"):
                raise RuntimeError(f"GRBL alarm: {reply}")
            time.sleep(0.1)
        raise RuntimeError("GRBL: did not reach Idle")

    def home(self):
        self._command("$H")
        self._wait_idle()

    def machine_target(self, x=None, y=None, z=None):
        words = []
        for k, v in (("x", x), ("y", y), ("z", z)):
            if v is None or k not in self.axes:
                continue
            m = (v - self.origin.get(k, 0.0)) * self.sign.get(k, 1)
            words.append(f"{self.axes[k]}{m:.3f}")
        return words

    def move_to(self, x=None, y=None, z=None):
        words = self.machine_target(x, y, z)
        if not words:
            return
        self._command(f"G1 {' '.join(words)} F{self.feed}")
        self._wait_idle()
        time.sleep(0.3)                        # let the camera stop swinging

    def close(self):
        self.ser.close()


def open_positioner(spec, log=print, **kwargs):
    """'none' | 'manual' | 'sim' | 'grbl:<port>' -> Positioner."""
    kind, _, arg = (spec or "none").partition(":")
    if kind == "none":
        return NoPositioner()
    if kind == "manual":
        return ManualPositioner(log=log)
    if kind == "sim":
        return SimPositioner(log=log)
    if kind == "grbl":
        if not arg:
            raise ValueError("grbl needs a port: grbl:COM5")
        return GrblPositioner(arg, log=log, **kwargs)
    raise ValueError(f"unknown positioner {spec!r}")

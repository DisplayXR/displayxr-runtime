#!/usr/bin/env python3
"""Unit tests for tools/weave_qa's hardware-free parts.

Hermetic, standard library only:
  - leiasr_target: the [Service] ini edit, and the switch against a fake SR Eye Tracker
    (a thread that answers ini changes with tracking_target.status, as LeiaSR#948 does);
  - positioner: display-mm -> GRBL G-code against a fake serial port, and the kinds.
The image analysis (OpenCV) has its own fake-camera self-test:
    python tools/weave_qa/analyse.py --selftest

    python3 scripts/tests/test_weave_qa.py
"""
from __future__ import annotations

import os
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "tools" / "weave_qa"))

import leiasr_target  # noqa: E402
import positioner  # noqa: E402


def read(path):
    with open(path) as f:
        return f.read()


class ServiceKeys(unittest.TestCase):
    def test_adds_a_service_section_to_an_empty_file(self):
        out = leiasr_target._set_service_keys("", {"tracking_target": "stereo_camera"})
        self.assertIn("[Service]\ntracking_target = stereo_camera\n", out)

    def test_replaces_keys_in_place_and_keeps_everything_else(self):
        text = ("; comment\n[Weaver]\npattern = 1\n[Service]\nallow_network_clients = off\n"
                "tracking_target = face\n[AntiCrosstalk]\nxfactor_dc = 0.02\n")
        out = leiasr_target._set_service_keys(text, {"tracking_target": "stereo_camera",
                                                     "tracking_target_until": "123"})
        self.assertIn("; comment\n[Weaver]\npattern = 1\n[Service]\nallow_network_clients = off\n", out)
        self.assertIn("tracking_target = stereo_camera\ntracking_target_until = 123\n[AntiCrosstalk]", out)
        self.assertEqual(out.count("tracking_target ="), 1)
        self.assertIn("xfactor_dc = 0.02", out)

    def test_appends_to_a_service_section_at_the_end(self):
        out = leiasr_target._set_service_keys("[Service]\nallow_network_clients = off\n",
                                              {"tracking_target": "stereo_camera"})
        self.assertTrue(out.endswith("allow_network_clients = off\ntracking_target = stereo_camera\n"))


class FakeTracker(threading.Thread):
    """Answers ini edits the way SR Eye Tracker with LeiaSR#948 does."""

    def __init__(self, config_dir, delay_s=0.2):
        super().__init__(daemon=True)
        self.dir, self.delay_s, self.stop = config_dir, delay_s, False
        self.write("face")

    def write(self, target):
        with open(os.path.join(self.dir, "tracking_target.status"), "w") as f:
            f.write(f"target={target}\nalgorithm={'STEREOCAM' if target == 'stereo_camera' else 'MPO'}\n"
                    f"since={time.time():.3f}\n")

    def run(self):
        last = None
        while not self.stop:
            try:
                text = read(os.path.join(self.dir, "sr_config.ini"))
            except OSError:
                text = ""
            want = "stereo_camera" if "tracking_target = stereo_camera" in text else "face"
            if want != last:
                time.sleep(self.delay_s)
                self.write(want)
                last = want
            time.sleep(0.02)


class StereoCameraTarget(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = os.path.join(self.tmp.name, "config")
        os.makedirs(self.dir)
        self.original = "[Weaver]\npattern = 1\n"
        with open(os.path.join(self.dir, "sr_config.ini"), "w") as f:
            f.write(self.original)

    def tearDown(self):
        self.tmp.cleanup()

    def test_switches_and_restores_the_file_exactly(self):
        tracker = FakeTracker(self.dir)
        tracker.start()
        try:
            with leiasr_target.StereoCameraTarget(minutes=5, config_dir=self.dir, log=lambda *_: None):
                text = read(os.path.join(self.dir, "sr_config.ini"))
                self.assertIn("tracking_target = stereo_camera", text)
                until = int(text.split("tracking_target_until = ")[1].split()[0])
                self.assertAlmostEqual(until, time.time() + 300, delta=5)
                self.assertEqual(leiasr_target.read_status(self.dir)["target"], "stereo_camera")
            self.assertEqual(read(os.path.join(self.dir, "sr_config.ini")), self.original)
            self.assertEqual(leiasr_target.read_status(self.dir)["target"], "face")
        finally:
            tracker.stop = True

    def test_restores_after_an_exception(self):
        tracker = FakeTracker(self.dir)
        tracker.start()
        try:
            with self.assertRaises(KeyError):
                with leiasr_target.StereoCameraTarget(config_dir=self.dir, log=lambda *_: None):
                    raise KeyError("capture failed")
            self.assertEqual(read(os.path.join(self.dir, "sr_config.ini")), self.original)
        finally:
            tracker.stop = True

    def test_a_tracker_that_does_not_follow_is_reported_not_waited_for(self):
        with self.assertRaises(leiasr_target.TargetError) as e:
            with leiasr_target.StereoCameraTarget(config_dir=self.dir, log=lambda *_: None):
                pass
        self.assertIn("LEIASR_LIVE_TRACKING_TARGET", str(e.exception))
        self.assertEqual(read(os.path.join(self.dir, "sr_config.ini")), self.original)

    def test_a_switch_that_never_comes_times_out(self):
        FakeTracker(self.dir)   # writes face once, never answers
        t = leiasr_target.StereoCameraTarget(config_dir=self.dir, timeout_s=0.5, log=lambda *_: None)
        with self.assertRaises(leiasr_target.TargetError):
            t.__enter__()
        t.__exit__(None, None, None)
        self.assertEqual(read(os.path.join(self.dir, "sr_config.ini")), self.original)


class FakeGrbl:
    """A serial port that answers like GRBL 1.1 and records what it was sent."""

    def __init__(self):
        self.sent, self.replies = [], []

    def write(self, data):
        text = data.decode()
        if text == "?":
            self.replies.append("<Idle|MPos:0.000,0.000,0.000|FS:0,0>")
            return
        for line in filter(None, (l.strip() for l in text.splitlines())):
            self.sent.append(line)
            self.replies.append("ok")

    def readline(self):
        return (self.replies.pop(0) + "\r\n").encode() if self.replies else b""

    def reset_input_buffer(self):
        self.replies.clear()

    def close(self):
        pass


class Grbl(unittest.TestCase):
    def make(self, **kw):
        self.port = FakeGrbl()
        return positioner.GrblPositioner("FAKE", serial_factory=lambda: self.port, log=lambda *_: None, **kw)

    def test_setup_clears_the_alarm_and_sets_mm_absolute(self):
        self.make()
        self.assertEqual(self.port.sent[:2], ["$X", "G21 G90"])

    def test_display_mm_map_to_machine_axes_with_origin_and_sign(self):
        g = self.make(axes={"x": "X", "z": "Y"}, origin={"x": -300, "z": 400}, sign={"z": -1}, feed_mm_min=1200)
        g.move_to(x=-150, z=600)
        self.assertEqual(self.port.sent[-1], "G1 X150.000 Y-200.000 F1200")

    def test_an_axis_the_machine_lacks_is_not_sent(self):
        g = self.make(axes={"x": "X"})
        before = list(self.port.sent)
        g.move_to(y=50)
        self.assertEqual(self.port.sent, before)

    def test_home(self):
        g = self.make()
        g.home()
        self.assertEqual(self.port.sent[-1], "$H")


class Kinds(unittest.TestCase):
    def test_open(self):
        self.assertIsInstance(positioner.open_positioner("none"), positioner.NoPositioner)
        self.assertIsInstance(positioner.open_positioner("sim", log=lambda *_: None), positioner.SimPositioner)
        self.assertIsInstance(positioner.open_positioner("manual"), positioner.ManualPositioner)
        with self.assertRaises(ValueError):
            positioner.open_positioner("grbl")
        with self.assertRaises(ValueError):
            positioner.open_positioner("teleport")

    def test_manual_asks_with_the_display_position(self):
        asked = []
        positioner.ManualPositioner(ask=asked.append).move_to(x=-150, z=600)
        self.assertIn("x = -150 mm, z = +600 mm", asked[0])


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Unit tests for the INV-2.9 mode-switch easing check in scripts/check_displayxr_app.py.

Hermetic: each case writes a tiny synthetic app tree to a temp dir and lints it.

The runtime switches a rendering mode in one frame and never ramps the stereo
disparity, so an app that requests modes without displayxr-common's
dxr::ModeSwitch snaps between full parallax and flat. The check is per leg: a
Windows leg that routes through XrSessionUpdateModeSwitch says nothing about a
hand-written macOS leg.

    python3 scripts/tests/test_check_displayxr_app_mode_switch.py
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_displayxr_app as lint  # noqa: E402

DIRECT = """
void onKey(char c) {
    if (c == 'v') xr.pfnRequestDisplayRenderingModeEXT(xr.session, (xr.currentModeIndex + 1) % n);
}
"""

PROC_ADDR_ONLY = """
PFN_xrRequestDisplayRenderingModeDXR pfnRequestDisplayRenderingModeEXT = nullptr;
xrGetInstanceProcAddr(xr.instance, "xrRequestDisplayRenderingModeDXR", (PFN_xrVoidFunction*)&xr.pfnRequestDisplayRenderingModeEXT);
"""

VIA_STEP = """
#include "mode_switch.h"
static dxr::ModeSwitch g_modeSwitch;
void frame(float dt) {
    dxr::ModeSwitchFrame f = g_modeSwitch.step(dt, want, count, vcs, current, steady);
    if (f.fire) xr.pfnRequestDisplayRenderingModeEXT(xr.session, f.mode);
}
"""

VIA_WINDOWS_WRAPPER = """
void frame(float dt) { XrSessionUpdateModeSwitch(*xr, input, dt); }
void startup() { xr->pfnRequestDisplayRenderingModeEXT(xr->session, 1); }
"""


def lint_tree(files: dict) -> list:
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        for rel_path, text in files.items():
            p = root / rel_path
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text, encoding="utf-8")
        findings: list = []
        lint.check_mode_switch_easing(root, findings)
        return [f for f in findings if f.rule == "INV-2.9"]


class ModeSwitchEasingTest(unittest.TestCase):
    def test_direct_request_is_flagged(self):
        found = lint_tree({"main.cpp": DIRECT})
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0].level, lint.WARN)

    def test_proc_addr_lookup_alone_is_not_a_request(self):
        self.assertEqual(lint_tree({"main.cpp": PROC_ADDR_ONLY}), [])

    def test_step_is_clean(self):
        self.assertEqual(lint_tree({"main.mm": VIA_STEP}), [])

    def test_windows_wrapper_covers_its_leg(self):
        self.assertEqual(lint_tree({"windows/main.cpp": VIA_WINDOWS_WRAPPER}), [])

    def test_per_leg(self):
        # Eased Windows leg, hand-written macOS leg: only macOS is flagged.
        found = lint_tree({"windows/main.cpp": VIA_WINDOWS_WRAPPER, "macos/main.mm": DIRECT})
        self.assertEqual(len(found), 1)
        self.assertIn("macos", str(found[0].path))

    def test_comment_mention_does_not_ease(self):
        found = lint_tree({"main.cpp": "// TODO: use dxr::ModeSwitch\n" + DIRECT})
        self.assertEqual(len(found), 1)


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
"""Tests for installer/catalog_plan.ps1 (the runtime installer's catalog listing shim).

The shim turns `dxr-get catalog --json` into the INI file installer/Catalog.nsh
reads. These tests run it under PowerShell (`pwsh`, or `powershell` on Windows)
against a fake dxr-get that prints a canned listing, and check the selection
rules: default pre-ticks, /COMPONENTS= tokens, the runtime-range filter, the
offline and "no such verb" paths.

Skipped (exit 0) when no PowerShell is on PATH.

    python3 installer/tests/test_catalog_plan.py
"""
from __future__ import annotations

import configparser
import json
import os
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
SHIM = HERE.parent / "catalog_plan.ps1"
PWSH = shutil.which("pwsh") or shutil.which("powershell")

LISTING = {
    "schema": 2,
    "items": [
        {"id": "runtime", "class": "runtime", "name": "DisplayXR Runtime", "version": "2.70.0",
         "installed": None, "state": "missing", "size": 50000000},
        {"id": "leia-sr", "aliases": ["leia_plugin"], "class": "display_processor",
         "name": "Leia SR display processor", "version": "2.23.0", "installed": None,
         "state": "missing", "size": 31457280, "detected": "high",
         "requires": [{"kind": "runtime", "version": ">=2.66.8"}]},
        {"id": "open-conversion", "aliases": ["conversion_open"], "class": "conversion_provider",
         "name": "Open conversion provider", "version": "0.1.1", "installed": "0.1.0",
         "state": "outdated", "size": 15720487, "recommended": True, "summary": "Open models."},
        {"id": "leia-conversion", "aliases": ["leia_conversion"], "class": "conversion_provider",
         "name": "Leia conversion provider", "version": "0.1.0", "installed": None,
         "state": "missing", "size": 4194304,
         "requires": [{"kind": "runtime", "version": ">=2.41.0"},
                      {"kind": "external", "name": "NeurD runtime (Leia)", "met": False}]},
        {"id": "ultraleap", "class": "input_provider", "name": "Ultraleap hands", "version": "0.1.0",
         "installed": None, "state": "incompatible", "size": 2097152, "detected": "low",
         "runtime": {"min": "2.69.3", "max": "2.69.3"}},
        {"id": "browser", "class": "app", "name": "DisplayXR Browser", "version": "1.10.5",
         "installed": "1.10.5", "state": "ok", "size": 209715200},
        {"id": "unsigned-thing", "class": "input_provider", "name": "No build", "version": "1.0.0",
         "installed": None, "state": "not_fetchable", "size": 0},
    ],
}


@unittest.skipUnless(PWSH, "no PowerShell on PATH")
class ShimTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="dxr-catplan-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def fake_dxr_get(self, stdout: str, code: int) -> Path:
        """An executable that prints `stdout` and exits `code`, whatever its arguments."""
        payload = self.tmp / "out.txt"
        payload.write_text(stdout, encoding="utf-8")
        args_log = self.tmp / "args.txt"
        if os.name == "nt":
            exe = self.tmp / "dxr-get.cmd"
            exe.write_text(f'@echo off\r\necho %* > "{args_log}"\r\ntype "{payload}"\r\nexit /b {code}\r\n')
        else:
            exe = self.tmp / "dxr-get"
            exe.write_text(f'#!/bin/sh\necho "$@" > "{args_log}"\ncat "{payload}"\necho "log line" >&2\nexit {code}\n')
            exe.chmod(exe.stat().st_mode | stat.S_IXUSR)
        return exe

    def run_shim(self, stdout: str, code: int = 0, **kw) -> configparser.ConfigParser:
        out = self.tmp / "catalog.ini"
        cmd = [PWSH, "-NoProfile", "-NonInteractive", "-File", str(SHIM),
               "-DxrGet", str(self.fake_dxr_get(stdout, code)), "-Out", str(out)]
        for k, v in kw.items():
            cmd += ["-" + k, str(v)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        raw = out.read_bytes()
        raw.decode("ascii")  # the installer may be an ANSI build
        ini = configparser.ConfigParser(interpolation=None)
        ini.read_string(raw.decode("ascii"))
        return ini

    def items(self, ini) -> dict:
        n = int(ini["catalog"]["count"])
        return {ini[f"item{i}"]["id"]: ini[f"item{i}"] for i in range(1, n + 1)}

    # ---- the listing ----------------------------------------------------------
    def test_default_preticks(self):
        ini = self.run_shim(json.dumps(LISTING), RuntimeVersion="2.70.0")
        self.assertEqual(ini["catalog"]["state"], "ok")
        it = self.items(ini)
        self.assertNotIn("runtime", it)  # the installer is the runtime
        # Detected hardware and the recommended converter are ticked; nothing else.
        self.assertEqual({k for k, v in it.items() if v["selected"] == "1"}, {"leia-sr", "open-conversion"})
        self.assertEqual(it["leia-sr"]["label"], "Leia SR display processor 2.23.0 (download 30 MB)")
        self.assertEqual(it["open-conversion"]["label"], "Open conversion provider 0.1.0 to 0.1.1 (download 15 MB)")
        self.assertEqual(it["leia-sr"]["size_kb"], "30720")
        self.assertIn("matching hardware was detected", it["leia-sr"]["desc"])
        self.assertIn("Recommended.", it["open-conversion"]["desc"])

    def test_installed_and_unavailable_items_are_read_only(self):
        it = self.items(self.run_shim(json.dumps(LISTING), RuntimeVersion="2.70.0"))
        self.assertEqual((it["browser"]["status"], it["browser"]["ro"], it["browser"]["selected"]),
                         ("current", "1", "0"))
        self.assertIn("installed, up to date", it["browser"]["label"])
        self.assertEqual((it["unsigned-thing"]["status"], it["unsigned-thing"]["ro"]), ("unavailable", "1"))
        self.assertIn("no signed Windows build", it["unsigned-thing"]["desc"])

    def test_unmet_external_prerequisite_is_shown_not_hidden(self):
        it = self.items(self.run_shim(json.dumps(LISTING), RuntimeVersion="2.70.0"))
        lc = it["leia-conversion"]
        self.assertEqual((lc["status"], lc["ro"], lc["selected"]), ("missing", "0", "0"))
        self.assertIn("Needs, and does not install: NeurD runtime (Leia).", lc["desc"])

    def test_runtime_range_is_judged_against_the_runtime_being_installed(self):
        # Built for 2.69.3 exactly: refused on 2.70.0 ...
        it = self.items(self.run_shim(json.dumps(LISTING), RuntimeVersion="2.70.0"))
        self.assertEqual(it["ultraleap"]["status"], "unavailable")
        self.assertIn("built for DisplayXR Runtime 2.69.3", it["ultraleap"]["desc"])
        # ... offered on 2.69.3 although dxr-get judged it against the old runtime ...
        it = self.items(self.run_shim(json.dumps(LISTING), RuntimeVersion="2.69.3"))
        self.assertEqual((it["ultraleap"]["status"], it["ultraleap"]["ro"]), ("missing", "0"))
        self.assertEqual(it["ultraleap"]["selected"], "0")  # a low-confidence match never ticks
        # ... and a floor above the runtime being installed is refused too.
        it = self.items(self.run_shim(json.dumps(LISTING), RuntimeVersion="2.60.0"))
        self.assertEqual(it["leia-sr"]["status"], "unavailable")
        self.assertEqual(it["leia-sr"]["selected"], "0")
        self.assertIn("needs DisplayXR Runtime 2.66.8 or newer", it["leia-sr"]["desc"])

    # ---- /COMPONENTS= -----------------------------------------------------------
    def test_explicit_selection_replaces_the_default(self):
        ini = self.run_shim(json.dumps(LISTING), RuntimeVersion="2.70.0", Select="leia_conversion")
        it = self.items(ini)
        self.assertEqual({k for k, v in it.items() if v["selected"] == "1"}, {"leia-conversion"})
        self.assertEqual(ini["catalog"]["passthrough"], "")

    def test_keywords(self):
        sel = lambda s: {k for k, v in self.items(
            self.run_shim(json.dumps(LISTING), RuntimeVersion="2.70.0", Select=s)).items() if v["selected"] == "1"}
        self.assertEqual(sel("recommended"), {"open-conversion"})
        self.assertEqual(sel("detected"), {"leia-sr"})
        self.assertEqual(sel("default"), {"leia-sr", "open-conversion"})
        self.assertEqual(sel("none"), set())
        self.assertEqual(sel("recommended,leia-conversion"), {"open-conversion", "leia-conversion"})

    def test_requested_id_that_is_current_or_unavailable_is_not_selected(self):
        it = self.items(self.run_shim(json.dumps(LISTING), RuntimeVersion="2.70.0", Select="browser,ultraleap"))
        self.assertFalse([k for k, v in it.items() if v["selected"] == "1"])

    def test_unknown_requested_id_is_passed_through_and_junk_is_dropped(self):
        ini = self.run_shim(json.dumps(LISTING), RuntimeVersion="2.70.0", Select='acme.depth,bad"id,;rm')
        self.assertEqual(ini["catalog"]["passthrough"], "acme.depth")

    def test_more_items_than_slots_keeps_the_selected_ones(self):
        ini = self.run_shim(json.dumps(LISTING), RuntimeVersion="2.70.0", Slots=2)
        self.assertEqual((ini["catalog"]["count"], ini["catalog"]["more"]), ("2", "4"))
        self.assertEqual(set(self.items(ini)), {"leia-sr", "open-conversion"})

    def test_catalog_override_is_handed_to_dxr_get(self):
        self.run_shim(json.dumps(LISTING), RuntimeVersion="2.70.0", Catalog="C:/x/stable.json")
        self.assertEqual((self.tmp / "args.txt").read_text().split(), ["--catalog", "C:/x/stable.json", "catalog", "--json"])

    # ---- the other outcomes -----------------------------------------------------
    def test_bare_array_and_schema1_plan_vocabulary(self):
        plan = [{"component": "leia_plugin", "installed": "2.20.0", "state": "too_old", "target": "2.23.0", "size": 1048576}]
        it = self.items(self.run_shim(json.dumps(plan), RuntimeVersion="2.70.0", Select="leia_plugin"))
        self.assertEqual((it["leia_plugin"]["status"], it["leia_plugin"]["selected"]), ("outdated", "1"))

    def test_offline(self):
        ini = self.run_shim("", 10, Select="recommended,leia_plugin")
        self.assertEqual((ini["catalog"]["state"], ini["catalog"]["count"]), ("offline", "0"))
        self.assertEqual(ini["catalog"]["passthrough"], "leia_plugin")

    def test_signature_failure_passes_nothing_through(self):
        ini = self.run_shim("", 11, Select="leia_plugin")
        self.assertEqual((ini["catalog"]["state"], ini["catalog"]["passthrough"]), ("error", ""))

    def test_dxr_get_without_the_verb_is_unsupported(self):
        for stdout, code in (("usage: dxr-get ...", 2), ("runtime 2.69.2\nleia_plugin 2.23.0\n", 0), ("{not json", 0)):
            ini = self.run_shim(stdout, code, Select="leia_plugin")
            self.assertEqual(ini["catalog"]["state"], "unsupported", stdout)
            # An explicitly requested id still reaches `dxr-get install`.
            self.assertEqual(ini["catalog"]["passthrough"], "leia_plugin")

    def test_output_is_one_line_per_value(self):
        nasty = {"items": [{"id": "x", "class": "app", "name": "A\r\n[catalog]\nstate=ok", "version": "1.0.0",
                            "state": "missing", "summary": "caf\u00e9 \u2014 ok", "size": 10}]}
        ini = self.run_shim(json.dumps(nasty), RuntimeVersion="2.70.0")
        self.assertEqual(ini["catalog"]["count"], "1")
        self.assertNotIn("\n", ini["item1"]["name"])
        self.assertTrue(ini["item1"]["name"].startswith("A [catalog] state=ok"))


if __name__ == "__main__":
    if not PWSH:
        print("SKIP: no PowerShell (pwsh / powershell) on PATH")
        sys.exit(0)
    unittest.main()

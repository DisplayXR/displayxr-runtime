#!/usr/bin/env python3
"""Tests for installer/catalog_plan.ps1 (the runtime installer's catalog listing shim).

The shim joins three dxr-get reports (`list --json`, `detect --json`, `catalog`)
into the INI file installer/Catalog.nsh reads. These tests run it under
PowerShell (`pwsh`) against a fake dxr-get that answers each verb with a canned
document in the shape docs/specs/runtime/dxr-get.md gives, and check the
selection rules: default pre-ticks, /COMPONENTS= tokens, the runtime-range
filter, and what happens when a report is missing or the catalog is offline.

Skipped (exit 0) when no PowerShell is on PATH, and on Windows (the fake
dxr-get is a shell script).

    python3 installer/tests/test_catalog_plan.py
"""
from __future__ import annotations

import configparser
import copy
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
PWSH = None if os.name == "nt" else shutil.which("pwsh")

# `dxr-get list --json`
LIST = {"schema": 2, "items": [
    {"id": "runtime", "name": "DisplayXR Runtime", "class": "runtime", "registered_id": None, "publisher": "displayxr",
     "tier": None, "installed": None, "available": "2.70.0", "state": "not_installed", "installable": True, "receipt": False},
    {"id": "leia-sr", "name": "Leia SR display processor", "class": "display_processor", "registered_id": "leia-sr",
     "publisher": "displayxr", "tier": None, "installed": None, "available": "2.23.0", "state": "not_installed",
     "installable": True, "receipt": False},
    {"id": "open-conversion", "name": "Open conversion provider", "class": "conversion_provider",
     "registered_id": "open-conversion", "publisher": "displayxr", "tier": None, "installed": "0.1.0",
     "available": "0.1.1", "state": "outdated", "installable": True, "receipt": False},
    {"id": "leia-conversion", "name": "Leia conversion provider", "class": "conversion_provider",
     "registered_id": "leia-conversion", "publisher": "displayxr", "tier": None, "installed": None,
     "available": "0.1.0", "state": "not_installed", "installable": True, "receipt": False},
    {"id": "ultraleap", "name": "Ultraleap hands", "class": "input_provider", "registered_id": "ultraleap",
     "publisher": "displayxr", "tier": None, "installed": None, "available": "0.1.0", "state": "not_installed",
     "installable": True, "receipt": False},
    {"id": "browser", "name": "DisplayXR Browser", "class": "app", "registered_id": None, "publisher": "displayxr",
     "tier": None, "installed": "1.10.5", "available": "1.10.5", "state": "current", "installable": True, "receipt": True},
    {"id": "unsigned-thing", "name": "No build", "class": "input_provider", "registered_id": "x", "publisher": "displayxr",
     "tier": None, "installed": None, "available": None, "state": "not_installed", "installable": False, "receipt": False},
    {"id": "acme.viewer", "name": "Acme Viewer", "class": "app", "registered_id": None, "publisher": "acme",
     "tier": None, "installed": None, "available": None, "state": "not_installed", "installable": False,
     "receipt": False, "listing": "https://acme.example/viewer"},
]}

# `dxr-get detect --json`: keyed by the schema-1 name, with the item id inside.
DETECT = {
    "edids": [{"man": 44048, "prod": 16711}],
    "leia_plugin": {"match": True, "high_confidence": True, "evidence": "display EDID 44048/16711 DELL_8K32", "id": "leia-sr"},
    "ultraleap": {"match": True, "high_confidence": False, "evidence": "low", "id": "ultraleap"},
}


def asset(size):
    return {"windows-x64": {"version": "x", "file": "f.exe", "size": size, "sha256": "a" * 64,
                            "urls": ["https://example.invalid/f.exe"]}}


# `dxr-get catalog`: the schema-2 document.
DOC = {"schema": 2, "serial": 1, "publishers": {}, "items": [
    {"id": "runtime", "class": "runtime", "name": "DisplayXR Runtime", "version": "2.70.0", "assets": asset(50000000)},
    {"id": "leia-sr", "aliases": ["leia_plugin"], "class": "display_processor", "name": "Leia SR display processor",
     "version": "2.23.0", "requires": [{"kind": "runtime", "version": ">=2.66.8"}], "assets": asset(31457280)},
    {"id": "open-conversion", "aliases": ["conversion_open"], "class": "conversion_provider",
     "name": "Open conversion provider", "summary": "Open models.", "version": "0.1.1", "assets": asset(15720487),
     "recommended": True},
    {"id": "leia-conversion", "aliases": ["leia_conversion"], "class": "conversion_provider",
     "name": "Leia conversion provider", "version": "0.1.0", "assets": asset(4194304),
     "requires": [{"kind": "runtime", "version": ">=2.41.0"}, {"kind": "external", "name": "NeurD runtime (Leia)"}]},
    {"id": "ultraleap", "class": "input_provider", "name": "Ultraleap hands", "version": "0.1.0",
     "runtime": {"min": "2.69.3", "max": "2.69.3"}, "assets": asset(2097152),
     "requires": [{"kind": "runtime", "version": ">=2.69.3"}]},
    {"id": "browser", "class": "app", "name": "DisplayXR Browser", "version": "1.10.5", "assets": asset(209715200)},
]}


@unittest.skipUnless(PWSH, "no pwsh on PATH (or Windows)")
class ShimTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="dxr-catplan-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def fake_dxr_get(self, answers: dict) -> Path:
        """A dxr-get that answers `list`, `detect` and `catalog` with (stdout, exit code)."""
        for verb, (out, _code) in answers.items():
            (self.tmp / f"{verb}.out").write_text(out, encoding="utf-8")
        cases = "\n".join(
            f'    {verb}) cat "{self.tmp}/{verb}.out"; exit {code} ;;' for verb, (_out, code) in answers.items())
        exe = self.tmp / "dxr-get"
        exe.write_text(f"""#!/bin/sh
echo "$@" >> "{self.tmp}/args.txt"
echo "log line" >&2
verb=""
skip=0
for a in "$@"; do
  if [ "$skip" = 1 ]; then skip=0; continue; fi
  case "$a" in
    --catalog) skip=1 ;;
    --*) ;;
    *) [ -z "$verb" ] && verb="$a" ;;
  esac
done
case "$verb" in
{cases}
    *) echo "usage" >&2; exit 2 ;;
esac
""")
        exe.chmod(exe.stat().st_mode | stat.S_IXUSR)
        return exe

    def run_shim(self, answers: dict | None = None, **kw) -> configparser.ConfigParser:
        if answers is None:
            answers = {"list": (json.dumps(LIST), 0), "detect": (json.dumps(DETECT), 0), "catalog": (json.dumps(DOC), 0)}
        out = self.tmp / "catalog.ini"
        cmd = [PWSH, "-NoProfile", "-NonInteractive", "-File", str(SHIM),
               "-DxrGet", str(self.fake_dxr_get(answers)), "-Out", str(out)]
        kw.setdefault("RuntimeVersion", "2.70.0")
        for k, v in kw.items():
            cmd += ["-" + k, str(v)]
        r = subprocess.run(cmd, capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        raw = out.read_bytes().decode("ascii")  # the installer may be an ANSI build
        ini = configparser.ConfigParser(interpolation=None)
        ini.read_string(raw)
        return ini

    def items(self, ini) -> dict:
        n = int(ini["catalog"]["count"])
        return {ini[f"item{i}"]["id"]: ini[f"item{i}"] for i in range(1, n + 1)}

    def selected(self, **kw) -> set:
        return {k for k, v in self.items(self.run_shim(**kw)).items() if v["selected"] == "1"}

    # ---- the listing ----------------------------------------------------------
    def test_default_preticks(self):
        ini = self.run_shim()
        self.assertEqual(ini["catalog"]["state"], "ok")
        it = self.items(ini)
        self.assertNotIn("runtime", it)  # the installer is the runtime
        # Detected hardware and the recommended converter are ticked; nothing else.
        self.assertEqual({k for k, v in it.items() if v["selected"] == "1"}, {"leia-sr", "open-conversion"})
        self.assertEqual(it["leia-sr"]["label"], "Leia SR display processor 2.23.0 (download 30 MB)")
        self.assertEqual(it["open-conversion"]["label"], "Open conversion provider 0.1.0 to 0.1.1 (download 15 MB)")
        self.assertEqual(it["leia-sr"]["size_kb"], "30720")
        self.assertIn("matching hardware was detected", it["leia-sr"]["desc"])
        self.assertIn("Recommended. Open models.", it["open-conversion"]["desc"])

    def test_the_three_reports_are_asked_for_with_the_catalog_override(self):
        self.run_shim(Catalog="C:/x/stable.json")
        calls = [l.split() for l in (self.tmp / "args.txt").read_text().splitlines()]
        self.assertEqual(calls, [["--catalog", "C:/x/stable.json", "list", "--json"],
                                 ["--catalog", "C:/x/stable.json", "detect", "--json"],
                                 ["--catalog", "C:/x/stable.json", "catalog"]])

    def test_installed_and_unavailable_items_are_read_only(self):
        it = self.items(self.run_shim())
        self.assertEqual((it["browser"]["status"], it["browser"]["ro"], it["browser"]["selected"]), ("current", "1", "0"))
        self.assertEqual(it["browser"]["label"], "DisplayXR Browser 1.10.5 - installed")
        self.assertEqual((it["unsigned-thing"]["status"], it["unsigned-thing"]["ro"]), ("unavailable", "1"))
        self.assertIn("no signed Windows build", it["unsigned-thing"]["desc"])
        self.assertIn("publisher's page", it["acme.viewer"]["desc"])  # a link-out

    def test_list_states(self):
        lst = copy.deepcopy(LIST)
        by = {i["id"]: i for i in lst["items"]}
        by["leia-sr"].update(installed="2.23.0", state="rebuilt")          # same version, another build
        by["open-conversion"].update(installed="0.2.0", state="newer")     # never downgraded
        by["leia-conversion"].update(installed="0.0.9", available=None, state="installed", installable=False)
        it = self.items(self.run_shim({"list": (json.dumps(lst), 0), "detect": (json.dumps(DETECT), 0),
                                       "catalog": (json.dumps(DOC), 0)}))
        self.assertEqual((it["leia-sr"]["status"], it["leia-sr"]["selected"]), ("outdated", "1"))
        self.assertEqual((it["open-conversion"]["status"], it["open-conversion"]["selected"]), ("current", "0"))
        self.assertEqual(it["leia-conversion"]["status"], "current")

    def test_external_prerequisite_is_named(self):
        lc = self.items(self.run_shim())["leia-conversion"]
        self.assertEqual((lc["status"], lc["ro"], lc["selected"]), ("missing", "0", "0"))
        self.assertIn("Also needs, and does not install: NeurD runtime (Leia).", lc["desc"])

    def test_runtime_range_is_judged_against_the_runtime_being_installed(self):
        # Built for 2.69.3 exactly: refused on 2.70.0 ...
        it = self.items(self.run_shim())
        self.assertEqual(it["ultraleap"]["status"], "unavailable")
        self.assertIn("built for DisplayXR Runtime 2.69.3", it["ultraleap"]["desc"])
        # ... offered on 2.69.3, and a low-confidence match never ticks it ...
        it = self.items(self.run_shim(RuntimeVersion="2.69.3"))
        self.assertEqual((it["ultraleap"]["status"], it["ultraleap"]["ro"], it["ultraleap"]["selected"]),
                         ("missing", "0", "0"))
        # ... and a floor above the runtime being installed is refused too.
        it = self.items(self.run_shim(RuntimeVersion="2.60.0"))
        self.assertEqual((it["leia-sr"]["status"], it["leia-sr"]["selected"]), ("unavailable", "0"))
        self.assertIn("needs DisplayXR Runtime 2.66.8 or newer", it["leia-sr"]["desc"])

    # ---- /COMPONENTS= -----------------------------------------------------------
    def test_explicit_selection_replaces_the_default(self):
        ini = self.run_shim(Select="leia_conversion")
        self.assertEqual({k for k, v in self.items(ini).items() if v["selected"] == "1"}, {"leia-conversion"})
        self.assertEqual(ini["catalog"]["passthrough"], "")

    def test_keywords(self):
        self.assertEqual(self.selected(Select="recommended"), {"open-conversion"})
        self.assertEqual(self.selected(Select="detected"), {"leia-sr"})
        self.assertEqual(self.selected(Select="default"), {"leia-sr", "open-conversion"})
        self.assertEqual(self.selected(Select="none"), set())
        self.assertEqual(self.selected(Select="recommended,leia-conversion"), {"open-conversion", "leia-conversion"})

    def test_requested_id_that_is_current_or_unavailable_is_not_selected(self):
        ini = self.run_shim(Select="browser,ultraleap")
        self.assertFalse([k for k, v in self.items(ini).items() if v["selected"] == "1"])
        self.assertEqual(ini["catalog"]["passthrough"], "")

    def test_unknown_requested_id_is_passed_through_and_junk_is_dropped(self):
        ini = self.run_shim(Select='acme.depth,bad"id,;rm')
        self.assertEqual(ini["catalog"]["passthrough"], "acme.depth")

    def test_more_items_than_slots_keeps_the_selected_ones(self):
        ini = self.run_shim(Slots=2)
        self.assertEqual((ini["catalog"]["count"], ini["catalog"]["more"]), ("2", "5"))
        self.assertEqual(set(self.items(ini)), {"leia-sr", "open-conversion"})

    # ---- a report is missing ------------------------------------------------------
    def test_without_detect_and_document_the_rows_remain_and_nothing_is_ticked(self):
        ini = self.run_shim({"list": (json.dumps(LIST), 0), "detect": ("", 10), "catalog": ("", 10)})
        self.assertEqual(ini["catalog"]["state"], "ok")
        it = self.items(ini)
        self.assertFalse([k for k, v in it.items() if v["selected"] == "1"])
        self.assertEqual(it["leia-sr"]["label"], "Leia SR display processor 2.23.0 (download)")
        self.assertEqual(it["leia-sr"]["size_kb"], "0")
        # No document, so no range check: dxr-get install is what refuses (exit 41).
        self.assertEqual(it["ultraleap"]["status"], "missing")

    def test_schema1_document_is_not_used_but_the_legacy_name_from_detect_is(self):
        v1 = {"schema": 1, "components": {"leia_plugin": {"recommended": "2.23.0"}}}
        ini = self.run_shim({"list": (json.dumps(LIST), 0), "detect": (json.dumps(DETECT), 0),
                             "catalog": (json.dumps(v1), 0)}, Select="leia_plugin")
        self.assertEqual({k for k, v in self.items(ini).items() if v["selected"] == "1"}, {"leia-sr"})
        self.assertEqual(ini["catalog"]["passthrough"], "")

    def test_offline(self):
        ini = self.run_shim({"list": ("", 10)}, Select="recommended,leia_plugin")
        self.assertEqual((ini["catalog"]["state"], ini["catalog"]["count"]), ("offline", "0"))
        self.assertEqual(ini["catalog"]["passthrough"], "leia_plugin")
        # Offline is decided by the first call: the other two are not made.
        self.assertEqual(len((self.tmp / "args.txt").read_text().splitlines()), 1)

    def test_signature_failure_passes_nothing_through(self):
        ini = self.run_shim({"list": ("", 11)}, Select="leia_plugin")
        self.assertEqual((ini["catalog"]["state"], ini["catalog"]["passthrough"]), ("error", ""))

    def test_list_that_is_not_the_listing_is_an_error(self):
        for stdout, code in (("usage", 2), ("ID  CLASS  INSTALLED\n", 0), ("{not json", 0), ('{"schema":2}', 0)):
            ini = self.run_shim({"list": (stdout, code)}, Select="leia_plugin")
            self.assertEqual((ini["catalog"]["state"], ini["catalog"]["count"]), ("error", "0"), stdout)
            self.assertEqual(ini["catalog"]["passthrough"], "leia_plugin")

    def test_output_is_one_line_per_value(self):
        lst = {"schema": 2, "items": [{"id": "x", "class": "app", "name": "A\r\n[catalog]\nstate=ok", "installed": None,
                                       "available": "1.0.0", "state": "not_installed", "installable": True}]}
        doc = {"schema": 2, "items": [{"id": "x", "summary": "caf\u00e9 \u2014 ok", "assets": asset(10)}]}
        ini = self.run_shim({"list": (json.dumps(lst), 0), "detect": ("{}", 0), "catalog": (json.dumps(doc), 0)})
        self.assertEqual(ini["catalog"]["count"], "1")
        self.assertTrue(ini["item1"]["name"].startswith("A [catalog] state=ok"))
        self.assertIn("caf? ? ok", ini["item1"]["desc"])


if __name__ == "__main__":
    if not PWSH:
        print("SKIP: no pwsh on PATH (or running on Windows)")
        sys.exit(0)
    unittest.main()

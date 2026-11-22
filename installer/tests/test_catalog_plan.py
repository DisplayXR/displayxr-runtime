#!/usr/bin/env python3
"""Tests for installer/catalog_plan.ps1 (the runtime installer's catalog listing shim).

The shim turns one `dxr-get list --json --assume-runtime X.Y.Z` report into the
INI file installer/Catalog.nsh reads. These tests run it under PowerShell
(`pwsh`) against a fake dxr-get that prints a canned listing in the shape
docs/specs/runtime/dxr-get.md gives, and check what the shim adds: the call it
makes, the pre-tick and /COMPONENTS= rules, the row texts, and the offline and
failure outcomes. Compatibility, detection and sizes are dxr-get's answers and
are only copied, so they are fixture data here, not logic under test.

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


def row(id_, name, cls, **kw):
    r = {"id": id_, "name": name, "class": cls, "registered_id": None, "publisher": "displayxr", "tier": None,
         "installed": None, "available": "1.0.0", "state": "not_installed", "installable": True, "receipt": False,
         "aliases": [], "recommended": False, "detected": "none", "size": 0, "download": 0, "compatible": True,
         "prerequisites": []}
    r.update(kw)
    return r


# `dxr-get list --json --assume-runtime 2.70.0`
LIST = {"schema": 2, "items": [
    row("runtime", "DisplayXR Runtime", "runtime", available="2.70.0", size=50000000, download=50000000),
    row("leia-sr", "Leia SR display processor", "display_processor", aliases=["leia_plugin"], available="2.23.0",
        detected="high", evidence="display EDID 44048/16711 DELL_8K32", size=31457280, download=31457280),
    row("open-conversion", "Open conversion provider", "conversion_provider", aliases=["conversion_open"],
        installed="0.1.0", available="0.1.1", state="outdated", recommended=True, summary="Open models.",
        size=15720487, download=15720487),
    row("leia-conversion", "Leia conversion provider", "conversion_provider", aliases=["leia_conversion"],
        available="0.1.0", size=4194304, download=4194304,
        prerequisites=[{"name": "NeurD runtime (Leia)", "met": False, "installable": False}]),
    row("ultraleap", "Ultraleap hands", "input_provider", available="0.1.0", detected="low", size=2097152,
        download=2097152, compatible=False,
        incompatible_reason="ultraleap 0.1.0 works with runtime 2.69.3 at most; this machine would have 2.70.0"),
    row("browser", "DisplayXR Browser", "app", installed="1.10.5", available="1.10.5", state="current",
        receipt=True, size=209715200),
    row("unsigned-thing", "No build", "input_provider", available=None, installable=False),
    row("acme.viewer", "Acme Viewer", "app", publisher="acme", available=None, installable=False,
        listing="https://acme.example/viewer"),
]}


@unittest.skipUnless(PWSH, "no pwsh on PATH (or Windows)")
class ShimTests(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="dxr-catplan-"))
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def fake_dxr_get(self, stdout: str, code: int) -> Path:
        """A dxr-get that logs its arguments, prints `stdout` and exits `code`."""
        (self.tmp / "out.txt").write_text(stdout, encoding="utf-8")
        exe = self.tmp / "dxr-get"
        exe.write_text(f'#!/bin/sh\necho "$@" >> "{self.tmp}/args.txt"\necho "log line" >&2\n'
                       f'cat "{self.tmp}/out.txt"\nexit {code}\n')
        exe.chmod(exe.stat().st_mode | stat.S_IXUSR)
        return exe

    def run_shim(self, listing=None, code: int = 0, **kw) -> configparser.ConfigParser:
        stdout = json.dumps(LIST) if listing is None else (listing if isinstance(listing, str) else json.dumps(listing))
        out = self.tmp / "catalog.ini"
        cmd = [PWSH, "-NoProfile", "-NonInteractive", "-File", str(SHIM),
               "-DxrGet", str(self.fake_dxr_get(stdout, code)), "-Out", str(out)]
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

    def calls(self) -> list:
        return [l.split() for l in (self.tmp / "args.txt").read_text().splitlines()]

    # ---- the one call -----------------------------------------------------------
    def test_one_list_call_for_the_runtime_being_installed(self):
        self.run_shim()
        self.assertEqual(self.calls(), [["list", "--json", "--assume-runtime", "2.70.0"]])

    def test_abi_majors_and_catalog_override_are_passed_on(self):
        self.run_shim(AssumeAbi="display_processor=5,conversion_provider=1,bad=x,;rm=1", Catalog="C:/x/stable.json")
        self.assertEqual(self.calls(), [["--catalog", "C:/x/stable.json", "list", "--json", "--assume-runtime", "2.70.0",
                                         "--assume-abi", "display_processor=5", "--assume-abi", "conversion_provider=1"]])

    def test_without_a_version_nothing_is_assumed(self):
        self.run_shim(RuntimeVersion="not-a-version", AssumeAbi="display_processor=5")
        self.assertEqual(self.calls(), [["list", "--json"]])

    # ---- the listing ----------------------------------------------------------
    def test_default_preticks(self):
        ini = self.run_shim()
        self.assertEqual(ini["catalog"]["state"], "ok")
        it = self.items(ini)
        self.assertNotIn("runtime", it)  # the installer is the runtime
        # Detected hardware and the recommended converter are ticked; nothing else.
        self.assertEqual({k for k, v in it.items() if v["selected"] == "1"}, {"leia-sr", "open-conversion"})
        self.assertEqual(it["leia-sr"]["label"], "30 MB - Leia SR display processor 2.23.0")
        self.assertEqual(it["open-conversion"]["label"], "Update, 15 MB - Open conversion provider 0.1.0 to 0.1.1")
        self.assertEqual(it["leia-sr"]["size_kb"], "30720")
        self.assertTrue(it["leia-sr"]["desc"].startswith("Matching hardware was detected."))
        self.assertIn("Recommended. Open models.", it["open-conversion"]["desc"])

    def test_download_is_what_is_still_to_fetch(self):
        lst = copy.deepcopy(LIST)
        lst["items"][1]["download"] = 0          # the asset is in the download cache
        lst["items"][3]["download"] = 4194304 + 20971520   # plus a prerequisite's own installer
        it = self.items(self.run_shim(lst))
        self.assertEqual(it["leia-sr"]["label"], "Downloaded - Leia SR display processor 2.23.0")
        self.assertEqual(it["leia-sr"]["size_kb"], "0")
        self.assertEqual(it["leia-conversion"]["label"], "24 MB - Leia conversion provider 0.1.0")

    def test_installed_and_unavailable_items_are_read_only(self):
        it = self.items(self.run_shim())
        self.assertEqual((it["browser"]["status"], it["browser"]["ro"], it["browser"]["selected"]), ("current", "1", "0"))
        self.assertEqual(it["browser"]["label"], "Installed - DisplayXR Browser 1.10.5")
        self.assertEqual((it["unsigned-thing"]["status"], it["unsigned-thing"]["ro"]), ("unavailable", "1"))
        self.assertIn("no signed Windows build", it["unsigned-thing"]["desc"])
        self.assertIn("publisher's page", it["acme.viewer"]["desc"])  # a link-out
        self.assertEqual(it["acme.viewer"]["label"], "Not available - Acme Viewer")

    def test_status_leads_the_row_and_the_description_fits_its_box(self):
        # The list cuts a row on the right and the box under it holds about two
        # lines: the status is the first thing in the label, the reason the
        # first thing in the description, and no description is long.
        lst = copy.deepcopy(LIST)
        for i in lst["items"]:
            i["name"] = "Fixture: every external prerequisite detector there is, " + i["name"]
            i["summary"] = "A very long summary. " * 30
        lst["items"][4]["incompatible_reason"] = "needs a newer runtime. " * 30
        it = self.items(self.run_shim(lst))
        firsts = {"leia-sr": "30 MB - ", "open-conversion": "Update, 15 MB - ", "browser": "Installed - ",
                  "ultraleap": "Not available - ", "leia-conversion": "4 MB - "}
        for k, first in firsts.items():
            self.assertTrue(it[k]["label"].startswith(first), it[k]["label"])
        for v in it.values():
            self.assertLessEqual(len(v["desc"]), 170)
        self.assertTrue(it["ultraleap"]["desc"].startswith("Not available: needs a newer runtime."))
        self.assertTrue(it["leia-conversion"]["desc"].startswith("Also needs, and does not install: NeurD runtime"))

    def test_rows_dropped_first_are_the_not_available_ones(self):
        # More items than slots: ticked, then tickable, then installed are kept.
        ini = self.run_shim(Slots=4)
        self.assertEqual(set(self.items(ini)), {"leia-sr", "open-conversion", "leia-conversion", "browser"})
        self.assertEqual(ini["catalog"]["more"], "3")

    def test_list_states(self):
        lst = copy.deepcopy(LIST)
        by = {i["id"]: i for i in lst["items"]}
        by["leia-sr"].update(installed="2.23.0", state="rebuilt")          # same version, another build
        by["open-conversion"].update(installed="0.2.0", state="newer")     # never downgraded
        by["leia-conversion"].update(installed="0.0.9", available=None, state="installed", installable=False)
        it = self.items(self.run_shim(lst))
        self.assertEqual((it["leia-sr"]["status"], it["leia-sr"]["selected"]), ("outdated", "1"))
        self.assertEqual((it["open-conversion"]["status"], it["open-conversion"]["selected"]), ("current", "0"))
        self.assertEqual(it["leia-conversion"]["status"], "current")

    def test_incompatible_is_dxr_gets_verdict_with_its_reason(self):
        ul = self.items(self.run_shim())["ultraleap"]
        self.assertEqual((ul["status"], ul["ro"], ul["selected"]), ("unavailable", "1", "0"))
        self.assertIn("works with runtime 2.69.3 at most", ul["desc"])
        # An installed item is never called unavailable, whatever `compatible` says.
        lst = copy.deepcopy(LIST)
        lst["items"][4].update(installed="0.1.0", state="current")
        self.assertEqual(self.items(self.run_shim(lst))["ultraleap"]["status"], "current")

    def test_low_confidence_detection_never_ticks(self):
        lst = copy.deepcopy(LIST)
        lst["items"][4].update(compatible=True)
        ul = self.items(self.run_shim(lst))["ultraleap"]
        self.assertEqual((ul["status"], ul["selected"]), ("missing", "0"))

    def test_unmet_prerequisite_is_named_and_a_met_or_installable_one_is_not(self):
        lc = self.items(self.run_shim())["leia-conversion"]
        self.assertEqual((lc["status"], lc["ro"], lc["selected"]), ("missing", "0", "0"))
        self.assertIn("Also needs, and does not install: NeurD runtime (Leia) (not found)", lc["desc"])
        lst = copy.deepcopy(LIST)
        lst["items"][3]["prerequisites"] = [{"name": "A", "met": True, "installable": False},
                                            {"name": "B", "met": False, "installable": True}]
        self.assertNotIn("Also needs", self.items(self.run_shim(lst))["leia-conversion"]["desc"])

    # ---- /COMPONENTS= -----------------------------------------------------------
    def test_explicit_selection_by_alias_replaces_the_default(self):
        ini = self.run_shim(Select="leia_conversion")
        self.assertEqual({k for k, v in self.items(ini).items() if v["selected"] == "1"}, {"leia-conversion"})
        self.assertEqual(ini["catalog"]["passthrough"], "")

    def test_keywords(self):
        self.assertEqual(self.selected(Select="recommended"), {"open-conversion"})
        self.assertEqual(self.selected(Select="detected"), {"leia-sr"})
        self.assertEqual(self.selected(Select="default"), {"leia-sr", "open-conversion"})
        self.assertEqual(self.selected(Select="none"), set())
        self.assertEqual(self.selected(Select="recommended,leia-conversion"), {"open-conversion", "leia-conversion"})

    def test_requested_id_that_is_current_is_not_selected_and_not_passed_through(self):
        ini = self.run_shim(Select="browser")
        self.assertFalse([k for k, v in self.items(ini).items() if v["selected"] == "1"])
        self.assertEqual(ini["catalog"]["passthrough"], "")

    def test_requested_incompatible_item_is_handed_to_dxr_get_not_dropped(self):
        # dxr-get install refuses it (41) and the installer reports that. The row
        # stays what the page shows without /COMPONENTS=: greyed, not ticked.
        ini = self.run_shim(Select="ultraleap")
        ul = self.items(ini)["ultraleap"]
        self.assertEqual((ul["status"], ul["ro"], ul["selected"]), ("unavailable", "1", "0"))
        self.assertEqual(ini["catalog"]["passthrough"], "ultraleap")

    def test_requested_not_installable_item_is_handed_to_dxr_get_not_dropped(self):
        # A link-out and an item with no signed build: dxr-get answers 40.
        ini = self.run_shim(Select="acme.viewer,unsigned-thing")
        it = self.items(ini)
        for k in ("acme.viewer", "unsigned-thing"):
            self.assertEqual((it[k]["status"], it[k]["ro"], it[k]["selected"]), ("unavailable", "1", "0"))
        self.assertEqual(ini["catalog"]["passthrough"].split(), ["unsigned-thing", "acme.viewer"])

    def test_refused_item_is_passed_once_by_its_id_after_unknown_ids(self):
        # Named by alias and by id: one attempt, under the catalog id.
        lst = copy.deepcopy(LIST)
        lst["items"][4]["aliases"] = ["ultraleap_hands"]
        ini = self.run_shim(lst, Select="ultraleap_hands,ultraleap,no.such.id,leia_plugin")
        self.assertEqual(ini["catalog"]["passthrough"].split(), ["no.such.id", "ultraleap"])
        self.assertEqual({k for k, v in self.items(ini).items() if v["selected"] == "1"}, {"leia-sr"})

    def test_unavailable_item_is_passed_through_only_when_named(self):
        # Keywords and the default pre-ticks never reach for an unavailable item.
        for sel in ("default", "recommended,detected", "none"):
            self.assertEqual(self.run_shim(Select=sel)["catalog"]["passthrough"], "", sel)
        self.assertEqual(self.run_shim()["catalog"]["passthrough"], "")

    def test_none_selects_nothing_and_passes_nothing(self):
        # Interactive /COMPONENTS=none: the rows are still listed, none ticked.
        # (Unattended, Catalog.nsh does not run the shim at all for `none`.)
        ini = self.run_shim(Select="none")
        self.assertFalse([k for k, v in self.items(ini).items() if v["selected"] == "1"])
        self.assertEqual(ini["catalog"]["passthrough"], "")
        self.assertGreater(int(ini["catalog"]["count"]), 0)

    def test_unknown_requested_id_is_passed_through_and_junk_is_dropped(self):
        ini = self.run_shim(Select='acme.depth,bad"id,;rm')
        self.assertEqual(ini["catalog"]["passthrough"], "acme.depth")

    def test_more_items_than_slots_keeps_the_selected_ones(self):
        ini = self.run_shim(Slots=2)
        self.assertEqual((ini["catalog"]["count"], ini["catalog"]["more"]), ("2", "5"))
        self.assertEqual(set(self.items(ini)), {"leia-sr", "open-conversion"})

    # ---- the other outcomes -----------------------------------------------------
    def test_schema1_fallback_rows_have_no_recommendation(self):
        # On the schema-1 fallback dxr-get emits the same fields; nothing is recommended.
        lst = copy.deepcopy(LIST)
        lst["items"] = [i for i in lst["items"] if i["id"] in ("runtime", "leia-sr", "browser")]
        self.assertEqual(self.selected(listing=lst), {"leia-sr"})

    def test_offline(self):
        ini = self.run_shim('{"offline":true,"reason":"catalog unreachable or expired"}', 10, Select="recommended,leia_plugin")
        self.assertEqual((ini["catalog"]["state"], ini["catalog"]["count"]), ("offline", "0"))
        self.assertEqual(ini["catalog"]["passthrough"], "leia_plugin")

    def test_signature_failure_passes_nothing_through(self):
        ini = self.run_shim("", 11, Select="leia_plugin")
        self.assertEqual((ini["catalog"]["state"], ini["catalog"]["passthrough"]), ("error", ""))

    def test_output_that_is_not_the_listing_is_an_error(self):
        for stdout, code in (("usage", 2), ("ID  CLASS  INSTALLED\n", 0), ("{not json", 0), ('{"schema":2}', 0)):
            ini = self.run_shim(stdout, code, Select="leia_plugin")
            self.assertEqual((ini["catalog"]["state"], ini["catalog"]["count"]), ("error", "0"), stdout)
            self.assertEqual(ini["catalog"]["passthrough"], "leia_plugin")

    def test_output_is_one_line_per_value(self):
        lst = {"schema": 2, "items": [row("x", "A\r\n[catalog]\nstate=ok", "app", summary="caf\u00e9 \u2014 ok", download=10)]}
        ini = self.run_shim(lst)
        self.assertEqual(ini["catalog"]["count"], "1")
        self.assertTrue(ini["item1"]["name"].startswith("A [catalog] state=ok"))
        self.assertIn("caf? ? ok", ini["item1"]["desc"])


if __name__ == "__main__":
    if not PWSH:
        print("SKIP: no pwsh on PATH (or running on Windows)")
        sys.exit(0)
    unittest.main()

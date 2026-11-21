#!/usr/bin/env python3
"""Unit tests for scripts/apply_versions_bump.py (the versions-bump.yml write step).

    python3 scripts/tests/test_apply_versions_bump.py
"""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import apply_versions_bump as avb  # noqa: E402

BASE = """{
  "$schema": "https://json.schemastore.org/displayxr-versions.json",
  "runtime":     "v2.69.2",
  "shell":       "v2.4.2",
  "leia_plugin": "v2.23.0",
  "conversion_open": "v0.1.1",
  "mcp_tools":   "v0.5.7",
  "cnsdk_services": "v0.10.98"
}
"""


class ExistingField(unittest.TestCase):
    def test_bump_changes_one_value_and_keeps_formatting(self):
        text, what = avb.apply(BASE, "shell", "v2.4.3")
        self.assertEqual(what, "bumped")
        self.assertEqual(text, BASE.replace('"shell":       "v2.4.2"', '"shell":       "v2.4.3"'))

    def test_same_tag_is_a_noop(self):
        text, what = avb.apply(BASE, "shell", "v2.4.2")
        self.assertEqual((text, what), (BASE, "noop"))

    def test_unknown_field_is_refused(self):
        with self.assertRaises(ValueError):
            avb.apply(BASE, "no_such_component", "v1.0.0")

    def test_real_file_round_trips(self):
        real = (REPO_ROOT / "versions.json").read_text(encoding="utf-8")
        for field, tag in json.loads(real).items():
            if field.startswith("$"):
                continue
            self.assertEqual(avb.apply(real, field, tag), (real, "noop"))


class FirstRelease(unittest.TestCase):
    def test_first_release_fields_are_absent_from_the_real_file(self):
        # The table is only for fields that are not pinned yet. Once a field is
        # in versions.json, drop it from FIRST_RELEASE_FIELDS.
        real = json.loads((REPO_ROOT / "versions.json").read_text(encoding="utf-8"))
        for field in avb.FIRST_RELEASE_FIELDS:
            self.assertNotIn(field, real, f"{field} is pinned now: remove it from FIRST_RELEASE_FIELDS")

    def test_insert_after_anchor(self):
        text, what = avb.apply(BASE, "leia_conversion", "v0.1.0")
        self.assertEqual(what, "added")
        self.assertEqual(text, BASE.replace(
            '  "conversion_open": "v0.1.1",\n',
            '  "conversion_open": "v0.1.1",\n  "leia_conversion": "v0.1.0",\n'))
        self.assertEqual(json.loads(text)["leia_conversion"], "v0.1.0")

    def test_second_field_uses_fallback_anchor_then_the_first_one(self):
        # ultraleap goes after leia_conversion when that is pinned, else after conversion_open.
        alone, _ = avb.apply(BASE, "ultraleap", "v0.1.0")
        self.assertIn('  "conversion_open": "v0.1.1",\n  "ultraleap": "v0.1.0",\n', alone)
        both, _ = avb.apply(avb.apply(BASE, "leia_conversion", "v0.1.0")[0], "ultraleap", "v0.2.0")
        self.assertIn('  "leia_conversion": "v0.1.0",\n  "ultraleap": "v0.2.0",\n', both)

    def test_inserted_field_then_bumps_like_any_other(self):
        text, _ = avb.apply(BASE, "leia_conversion", "v0.1.0")
        text2, what = avb.apply(text, "leia_conversion", "v0.1.1")
        self.assertEqual(what, "bumped")
        self.assertEqual(json.loads(text2)["leia_conversion"], "v0.1.1")
        self.assertEqual(text2.count("leia_conversion"), 1)

    def test_anchor_as_last_key_gains_the_comma(self):
        last = '{\n  "runtime": "v1.0.0",\n  "conversion_open": "v0.1.1"\n}\n'
        text, _ = avb.apply(last, "leia_conversion", "v0.1.0")
        self.assertEqual(text, '{\n  "runtime": "v1.0.0",\n  "conversion_open": "v0.1.1",\n  "leia_conversion": "v0.1.0"\n}\n')

    def test_real_file_accepts_both_new_fields(self):
        real = (REPO_ROOT / "versions.json").read_text(encoding="utf-8")
        text = avb.apply(real, "leia_conversion", "v0.1.0")[0]
        text = avb.apply(text, "ultraleap", "v0.1.0")[0]
        after, before = json.loads(text), json.loads(real)
        self.assertEqual({k: v for k, v in after.items() if k not in avb.FIRST_RELEASE_FIELDS}, before)
        # Every pre-existing line is untouched.
        for line in real.splitlines():
            self.assertIn(line, text.splitlines())


if __name__ == "__main__":
    unittest.main()

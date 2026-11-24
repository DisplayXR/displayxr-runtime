#!/usr/bin/env python3
"""Unit tests for scripts/apply_versions_bump.py (the versions-bump.yml write step).

    python3 scripts/tests/test_apply_versions_bump.py
"""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from unittest import mock

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

# BASE after the Leia conversion provider's first release: the shape of the real
# file today, with ultraleap's preferred neighbour pinned.
PINNED = BASE.replace(
    '  "conversion_open": "v0.1.1",\n',
    '  "conversion_open": "v0.1.1",\n  "leia_conversion": "v0.1.0",\n')


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
    """The mechanism, on fixtures. `ultraleap` is the one real first-release field;
    where a case needs a second one, the table is patched with synthetic names so
    the case does not depend on which components happen to be unreleased."""

    # Two fields with no release: the second prefers the first as its neighbour.
    TWO = {"synth_one": "conversion_open", "synth_two": "synth_one"}

    def test_first_release_fields_are_absent_from_the_real_file(self):
        # The table is only for fields that are not pinned yet. Once a field is
        # in versions.json, drop it from FIRST_RELEASE_FIELDS.
        real = json.loads((REPO_ROOT / "versions.json").read_text(encoding="utf-8"))
        for field in avb.FIRST_RELEASE_FIELDS:
            self.assertNotIn(field, real, f"{field} is pinned now: remove it from FIRST_RELEASE_FIELDS")

    def test_the_real_table_is_what_this_file_assumes(self):
        # The cases below name `ultraleap` and its neighbour. If the table moves
        # on, they must be rewritten, not left passing on a stale assumption.
        self.assertEqual(avb.FIRST_RELEASE_FIELDS.get("ultraleap"), "leia_conversion")
        self.assertIn("leia_conversion", PINNED)
        self.assertNotIn("ultraleap", PINNED)

    def test_insert_after_anchor(self):
        text, what = avb.apply(PINNED, "ultraleap", "v0.1.0")
        self.assertEqual(what, "added")
        self.assertEqual(text, PINNED.replace(
            '  "leia_conversion": "v0.1.0",\n',
            '  "leia_conversion": "v0.1.0",\n  "ultraleap": "v0.1.0",\n'))
        self.assertEqual(json.loads(text)["ultraleap"], "v0.1.0")

    def test_absent_preferred_neighbour_falls_back(self):
        # BASE has no leia_conversion line, so ultraleap goes after FALLBACK_ANCHOR.
        self.assertNotIn("leia_conversion", BASE)
        text, what = avb.apply(BASE, "ultraleap", "v0.1.0")
        self.assertEqual(what, "added")
        self.assertEqual(text, BASE.replace(
            '  "conversion_open": "v0.1.1",\n',
            '  "conversion_open": "v0.1.1",\n  "ultraleap": "v0.1.0",\n'))

    def test_second_field_uses_fallback_anchor_then_the_first_one(self):
        # synth_two goes after synth_one when that is pinned, else after conversion_open.
        with mock.patch.dict(avb.FIRST_RELEASE_FIELDS, self.TWO, clear=True):
            alone, what = avb.apply(BASE, "synth_two", "v0.1.0")
            self.assertEqual(what, "added")
            self.assertIn('  "conversion_open": "v0.1.1",\n  "synth_two": "v0.1.0",\n', alone)
            first, what = avb.apply(BASE, "synth_one", "v0.1.0")
            self.assertEqual(what, "added")
            both, what = avb.apply(first, "synth_two", "v0.2.0")
            self.assertEqual(what, "added")
            self.assertIn(
                '  "conversion_open": "v0.1.1",\n  "synth_one": "v0.1.0",\n  "synth_two": "v0.2.0",\n  "mcp_tools":',
                both)

    def test_either_order_of_two_first_releases_keeps_every_other_line(self):
        with mock.patch.dict(avb.FIRST_RELEASE_FIELDS, self.TWO, clear=True):
            for order in (("synth_one", "synth_two"), ("synth_two", "synth_one")):
                with self.subTest(order=order):
                    text = BASE
                    for field in order:
                        text = avb.apply(text, field, "v0.1.0")[0]
                    after = json.loads(text)
                    self.assertEqual({k: v for k, v in after.items() if k not in self.TWO}, json.loads(BASE))
                    self.assertEqual({after[f] for f in self.TWO}, {"v0.1.0"})
                    for line in BASE.splitlines():
                        self.assertIn(line, text.splitlines())

    def test_inserted_field_then_bumps_like_any_other(self):
        text, _ = avb.apply(PINNED, "ultraleap", "v0.1.0")
        text2, what = avb.apply(text, "ultraleap", "v0.1.1")
        self.assertEqual(what, "bumped")
        self.assertEqual(text2, text.replace('"ultraleap": "v0.1.0"', '"ultraleap": "v0.1.1"'))
        self.assertEqual(text2.count("ultraleap"), 1)
        self.assertEqual(avb.apply(text2, "ultraleap", "v0.1.1"), (text2, "noop"))

    def test_anchor_as_last_key_gains_the_comma(self):
        last = '{\n  "runtime": "v1.0.0",\n  "leia_conversion": "v0.1.0"\n}\n'
        text, _ = avb.apply(last, "ultraleap", "v0.1.0")
        self.assertEqual(text, '{\n  "runtime": "v1.0.0",\n  "leia_conversion": "v0.1.0",\n  "ultraleap": "v0.1.0"\n}\n')

    def test_no_anchor_at_all_is_refused(self):
        with self.assertRaises(ValueError):
            avb.apply('{\n  "runtime": "v1.0.0"\n}\n', "ultraleap", "v0.1.0")

    def test_a_field_that_left_the_table_is_bumped_in_place_and_never_added(self):
        # leia_conversion had its first release: present, it bumps like any pin...
        text, what = avb.apply(PINNED, "leia_conversion", "v0.1.1")
        self.assertEqual(what, "bumped")
        self.assertEqual(text, PINNED.replace('"leia_conversion": "v0.1.0"', '"leia_conversion": "v0.1.1"'))
        # ...and absent, it is refused like any unknown field (no silent re-add).
        self.assertNotIn("leia_conversion", avb.FIRST_RELEASE_FIELDS)
        with self.assertRaises(ValueError):
            avb.apply(BASE, "leia_conversion", "v0.1.1")

    def test_real_file_bumps_the_released_provider_in_place(self):
        real = (REPO_ROOT / "versions.json").read_text(encoding="utf-8")
        text, what = avb.apply(real, "leia_conversion", "v9.9.9")
        self.assertEqual(what, "bumped")
        self.assertEqual(len(text.splitlines()), len(real.splitlines()))
        changed = [(a, b) for a, b in zip(real.splitlines(), text.splitlines()) if a != b]
        self.assertEqual(len(changed), 1)
        self.assertIn('"leia_conversion"', changed[0][0])
        self.assertIn('"v9.9.9"', changed[0][1])

    def test_real_file_accepts_every_first_release_field(self):
        real = (REPO_ROOT / "versions.json").read_text(encoding="utf-8")
        self.assertTrue(avb.FIRST_RELEASE_FIELDS, "nothing awaits a first release: rewrite this case on a fixture")
        text = real
        for field in avb.FIRST_RELEASE_FIELDS:
            text, what = avb.apply(text, field, "v0.1.0")
            self.assertEqual(what, "added", field)
        after, before = json.loads(text), json.loads(real)
        self.assertEqual({k: v for k, v in after.items() if k not in avb.FIRST_RELEASE_FIELDS}, before)
        self.assertEqual({k for k in after if k not in before}, set(avb.FIRST_RELEASE_FIELDS))
        # Every pre-existing line is untouched, and in the same order.
        kept = [line for line in text.splitlines() if line in real.splitlines()]
        self.assertEqual(kept, real.splitlines())
        # ultraleap lands directly after its preferred neighbour, which is pinned.
        lines = text.splitlines()
        at = next(i for i, line in enumerate(lines) if '"ultraleap"' in line)
        self.assertIn('"leia_conversion"', lines[at - 1])


if __name__ == "__main__":
    unittest.main()

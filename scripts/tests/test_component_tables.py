#!/usr/bin/env python3
"""The component tables that must agree, checked in one place.

A new component touches several hand-kept tables: the asset table the POSIX
orchestrator sources (scripts/lib/components.sh), its inline Windows mirror
(scripts/setup-displayxr.bat), the bump writer's first-release list
(scripts/apply_versions_bump.py), the ABI gate (scripts/check_plugin_abi.py)
and the catalog generator (scripts/gen_catalog.py). Nothing but a test keeps
them in step.

    python3 scripts/tests/test_component_tables.py
"""
from __future__ import annotations

import json
import re
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import apply_versions_bump as avb  # noqa: E402
import check_plugin_abi as abi  # noqa: E402
import gen_catalog  # noqa: E402

SH = (REPO_ROOT / "scripts/lib/components.sh").read_text(encoding="utf-8")
BAT = (REPO_ROOT / "scripts/setup-displayxr.bat").read_text(encoding="utf-8")
VERSIONS = json.loads((REPO_ROOT / "versions.json").read_text(encoding="utf-8"))


def sh_var(name: str) -> str | None:
    m = re.search(r'(?m)^' + re.escape(name) + r'="([^"]*)"', SH)
    return m.group(1) if m else None


def bat_var(name: str) -> str | None:
    m = re.search(r'(?m)^set "' + re.escape(name) + r'=([^"]*)"', BAT)
    return m.group(1) if m else None


class AwaitingFirstRelease(unittest.TestCase):
    def test_three_lists_agree(self):
        want = sorted(avb.FIRST_RELEASE_FIELDS)
        self.assertEqual(sorted(sh_var("COMPONENTS_AWAITING_FIRST_RELEASE").split()), want)
        self.assertEqual(sorted(bat_var("COMPONENTS_AWAITING_FIRST_RELEASE").split()), want)

    def test_an_awaiting_component_has_no_pin_and_full_plumbing(self):
        for name in avb.FIRST_RELEASE_FIELDS:
            with self.subTest(component=name):
                self.assertNotIn(name, VERSIONS)
                self.assertTrue(sh_var(f"COMPONENT_REPO_{name}"))
                self.assertTrue(sh_var(f"COMPONENT_EXE_WINDOWS_{name}"))
                self.assertEqual(bat_var(f"COMPONENT_REPO_{name}"), sh_var(f"COMPONENT_REPO_{name}"))
                self.assertEqual(bat_var(f"COMPONENT_EXE_{name}"), sh_var(f"COMPONENT_EXE_WINDOWS_{name}"))
                # Both gate on a plug-in ABI of their own.
                self.assertIn(name, abi.COMPONENT_GATES)
                self.assertEqual("DisplayXR/" + self.catalog(name)["repo"], abi.COMPONENT_GATES[name][0])
                self.assertEqual(sh_var(f"COMPONENT_REPO_{name}"), abi.COMPONENT_GATES[name][0])

    @staticmethod
    def catalog(name: str) -> dict:
        return next(c for c in gen_catalog.V2_ONLY_COMPONENTS if c["name"] == name)

    def test_anchor_of_a_first_release_field_is_a_real_or_awaiting_field(self):
        for field, anchor in avb.FIRST_RELEASE_FIELDS.items():
            self.assertTrue(anchor in VERSIONS or anchor in avb.FIRST_RELEASE_FIELDS, field)
        self.assertIn(avb.FALLBACK_ANCHOR, VERSIONS)


class OptIn(unittest.TestCase):
    def test_opt_in_flags_exist_in_both_orchestrators(self):
        sh = (REPO_ROOT / "scripts/setup-displayxr.sh").read_text(encoding="utf-8")
        for pair in sh_var("OPTIN_COMPONENTS").split():
            flag, comp = pair.split(":")
            with self.subTest(flag=flag):
                self.assertTrue(sh_var(f"COMPONENT_REPO_{comp}"), comp)
                self.assertIn(f"{flag})", sh)
                self.assertIn(f'if /i "%~1"=="{flag}"', BAT)

    def test_awaiting_components_are_opt_in_never_default(self):
        optin = {p.split(":")[1] for p in sh_var("OPTIN_COMPONENTS").split()}
        for name in avb.FIRST_RELEASE_FIELDS:
            self.assertIn(name, optin)
            # The .bat default chain never names them unconditionally.
            for line in BAT.splitlines():
                if f"call :install_component {name} " in line:
                    self.assertTrue(line.lstrip().lower().startswith("if "), line)


class CatalogTable(unittest.TestCase):
    def test_every_catalog_component_is_a_known_component(self):
        for comp in list(gen_catalog.COMPONENTS) + list(gen_catalog.V2_ONLY_COMPONENTS):
            with self.subTest(component=comp["name"]):
                self.assertEqual(sh_var(f"COMPONENT_REPO_{comp['pin']}"), "DisplayXR/" + comp["repo"])
                self.assertIn(comp["name"], gen_catalog.V2_ITEMS)

    def test_schema1_table_is_frozen_at_three(self):
        # Deployed dxr-get builds know these three names; a fourth changes the
        # bytes they verify and gains nothing (they ignore unknown components).
        self.assertEqual([c["name"] for c in gen_catalog.COMPONENTS], ["runtime", "leia_plugin", "browser"])


if __name__ == "__main__":
    unittest.main()

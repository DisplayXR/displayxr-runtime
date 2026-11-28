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

# The out-of-tree providers: each has its own repo, a plug-in ABI gate of its own
# and an opt-in flag, whether or not it has had its first release.
PROVIDERS = ("leia_conversion", "ultraleap", "tracking_open")


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

    def test_an_awaiting_component_has_no_pin(self):
        for name in avb.FIRST_RELEASE_FIELDS:
            with self.subTest(component=name):
                self.assertNotIn(name, VERSIONS)
                self.assertIn(name, PROVIDERS)

    def test_a_provider_is_pinned_or_awaiting_never_both_never_neither(self):
        for name in PROVIDERS:
            with self.subTest(component=name):
                self.assertNotEqual(name in VERSIONS, name in avb.FIRST_RELEASE_FIELDS)

    def test_bat_reads_a_pin_as_optional_only_while_it_awaits_a_release(self):
        optional = re.findall(r"(?m)call :read_pin_optional (\w+) ", BAT)
        self.assertEqual(sorted(optional), sorted(avb.FIRST_RELEASE_FIELDS))
        strict = re.findall(r"(?m)^call :read_pin (\w+) ", BAT)
        for name in PROVIDERS:
            if name in VERSIONS:
                self.assertIn(name, strict)

    def test_a_provider_has_full_plumbing(self):
        # Before its first release and after it.
        for name in PROVIDERS:
            with self.subTest(component=name):
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
        # A first release does not make a provider a default install.
        self.assertLessEqual(set(avb.FIRST_RELEASE_FIELDS), set(PROVIDERS))
        optin = {p.split(":")[1] for p in sh_var("OPTIN_COMPONENTS").split()}
        for name in PROVIDERS:
            self.assertIn(name, optin)
            self.assertIn(f"call :install_component {name} ", BAT)
            # The .bat default chain never names them unconditionally.
            for line in BAT.splitlines():
                if f"call :install_component {name} " in line:
                    self.assertTrue(line.lstrip().lower().startswith("if "), line)


class ProviderAbiGates(unittest.TestCase):
    """check_plugin_abi.py --component: every ABI a provider negotiates must match."""

    def run_gate(self, component, files):
        from unittest import mock
        import io
        import contextlib

        def fetch(url):
            for suffix, text in files.items():
                if url.endswith(suffix):
                    return text
            raise abi.urllib.error.HTTPError(url, 404, "Not Found", None, None)

        out, err = io.StringIO(), io.StringIO()
        with mock.patch.object(abi, "_fetch", fetch), contextlib.redirect_stdout(out), \
                contextlib.redirect_stderr(err):
            rc = abi.check_component(component, "v0.2.0", str(REPO_ROOT), None)
        return rc, out.getvalue(), err.getvalue()

    @staticmethod
    def header(prefix, n):
        return f"#define {prefix}_{n} {n}\n#define {prefix}_CURRENT {prefix}_{n}\n"

    def test_tracking_open_is_gated_on_both_of_its_abis(self):
        repo, headers = abi.COMPONENT_GATES["tracking_open"]
        self.assertEqual(set(headers), {abi.TRACKING_PLUGIN_HEADER_PATH, abi.INPUT_PLUGIN_HEADER_PATH})
        self.assertEqual(abi.COMPONENT_VENDORED_HEADERS["tracking_open"], "include/xrt")
        runtime = abi.tree_abis(REPO_ROOT)
        t = runtime["XRT_TRACKING_PLUGIN_API_VERSION_CURRENT"]
        i = runtime["XRT_INPUT_PLUGIN_API_VERSION_CURRENT"]
        cmake = 'set(DXR_RUNTIME_GIT_TAG "v2.53.0-17-ge5d7ffa57")\n'  # metadata, not a release tag
        good = {
            "/CMakeLists.txt": cmake,
            "/include/xrt/xrt_tracking_plugin.h": self.header("XRT_TRACKING_PLUGIN_API_VERSION", t),
            "/include/xrt/xrt_input_plugin.h": self.header("XRT_INPUT_PLUGIN_API_VERSION", i),
        }
        rc, out, _ = self.run_gate("tracking_open", good)
        self.assertEqual(rc, 0, out)
        self.assertNotIn("runtime_pin=", out)  # an unparsable metadata pin is not reported, nor fatal
        # Either ABI moving alone is a mismatch.
        for name, prefix, cur in (("xrt_tracking_plugin.h", "XRT_TRACKING_PLUGIN_API_VERSION", t),
                                  ("xrt_input_plugin.h", "XRT_INPUT_PLUGIN_API_VERSION", i)):
            with self.subTest(header=name):
                files = dict(good)
                files["/include/xrt/" + name] = self.header(prefix, cur + 1)
                rc, _, err = self.run_gate("tracking_open", files)
                self.assertEqual(rc, 1)
                self.assertIn(prefix + "_CURRENT", err)
        # A release without the hands role's header cannot be checked: rc 2, not a pass.
        files = dict(good)
        del files["/include/xrt/xrt_input_plugin.h"]
        self.assertEqual(self.run_gate("tracking_open", files)[0], 2)


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

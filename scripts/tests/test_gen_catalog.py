#!/usr/bin/env python3
"""Unit tests for scripts/gen_catalog.py's vendor-generic display plug-in path (#2737).

Hermetic: GitHub (releases, repo files, asset downloads) is stubbed, nothing is
signed and no installer is downloaded. Pins that
  - a display plug-in's displayxr-detect.json (schema 1) is validated and used for
    the catalog's label, requires.runtime, windows.registry_version and detect;
  - a release without the manifest falls back to the component's fallback readers
    (Leia: its EDID header), so older plug-in releases keep their detection;
  - the component list is the COMPONENTS table: a new display_plugin row needs no
    code, and one with neither manifest nor fallback gets no detect rules;
  - rule validation: exact / range ids, manufacturer-only rules downgraded to low,
    unknown kinds dropped, malformed rules rejected.

    python3 scripts/tests/test_gen_catalog.py
"""
from __future__ import annotations

import io
import json
import sys
import unittest
from pathlib import Path
from unittest import mock

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import gen_catalog as gen  # noqa: E402

LEIA_HEADER = """
static const uint16_t leia_edid_table[][2] = {
    {44048, 16711}, // DELL_8K32
    {44806, 3},     // ACER_GENERIC
};
"""

MANIFEST = {
    "schema": 1,
    "label": "Leia display plug-in (manifest)",
    "registry_version": "HKLM\\Software\\DisplayXR\\Plugins\\LeiaSR\\Version",
    "requires": {"runtime": ">=2.66.8"},
    "detect": [
        {"kind": "edid", "man": 44048, "prod": 16711, "confidence": "high", "note": "DELL_8K32"},
        {"kind": "edid", "man": 19501, "prod_min": 29440, "prod_max": 29447, "confidence": "high"},
        {"kind": "usb", "vid": 11012, "pid": 7, "confidence": "high", "note": "tracking camera"},
        {"kind": "teleport", "confidence": "high"},
    ],
}


class FakeResponse(io.BytesIO):
    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


def release(repo, assets):
    return {"assets": [{"name": n, "browser_download_url": f"https://example.invalid/{repo}/{n}",
                        "size": 1, "id": 1} for n in assets]}


class RuleValidation(unittest.TestCase):
    def test_exact_and_range(self):
        r, p = gen.validate_rule({"kind": "edid", "man": 1, "prod": 2, "confidence": "high"})
        self.assertEqual(r, {"kind": "edid", "man": 1, "prod": 2, "confidence": "high"})
        self.assertEqual(p, [])
        r, p = gen.validate_rule({"kind": "usb", "vid": 1, "pid_min": 2, "pid_max": 9, "confidence": "low"})
        self.assertEqual(r["pid_min"], 2)
        self.assertEqual(r["pid_max"], 9)

    def test_manufacturer_only_is_never_high(self):
        r, p = gen.validate_rule({"kind": "edid", "man": 44048, "confidence": "high"})
        self.assertEqual(r["confidence"], "low")
        self.assertTrue(p)

    def test_rejects(self):
        bad = [
            {"kind": "edid", "man": 70000, "prod": 1, "confidence": "high"},
            {"kind": "edid", "man": 1, "prod": 1, "prod_min": 1, "prod_max": 2, "confidence": "high"},
            {"kind": "edid", "man": 1, "prod_min": 5, "prod_max": 2, "confidence": "high"},
            {"kind": "usb", "vid": 1, "pid": 2, "confidence": "maybe"},
            {"kind": "registry_dword_min", "key": "HKCU\\X", "value": "v", "min": 1, "confidence": "high"},
            {"kind": "edid", "man": True, "prod": 1, "confidence": "high"},
        ]
        for rule in bad:
            r, p = gen.validate_rule(rule)
            self.assertIsNone(r, rule)
            self.assertTrue(p, rule)

    def test_unknown_kind_dropped(self):
        r, p = gen.validate_rule({"kind": "teleport", "confidence": "high"})
        self.assertIsNone(r)
        self.assertIn("unknown rule kind", p[0])

    def test_registry_rule_kept(self):
        r, _ = gen.validate_rule(dict(gen.SR_REGISTRY_DETECT))
        self.assertEqual(r, gen.SR_REGISTRY_DETECT)


class ManifestValidation(unittest.TestCase):
    def test_good(self):
        clean, probs = gen.validate_detect_manifest(MANIFEST)
        self.assertEqual(clean["label"], "Leia display plug-in (manifest)")
        self.assertEqual(clean["floor"], "2.66.8")
        self.assertEqual(len(clean["detect"]), 3)  # the unknown kind is dropped
        self.assertEqual(len(probs), 1)

    def test_wrong_schema(self):
        self.assertIsNone(gen.validate_detect_manifest(dict(MANIFEST, schema=2))[0])

    def test_no_usable_rule(self):
        m = dict(MANIFEST, detect=[{"kind": "teleport", "confidence": "high"}])
        self.assertIsNone(gen.validate_detect_manifest(m)[0])

    def test_bad_floor_ignored_not_fatal(self):
        clean, probs = gen.validate_detect_manifest(dict(MANIFEST, requires={"runtime": "2.66"}))
        self.assertNotIn("floor", clean)
        self.assertTrue(any("requires.runtime" in p for p in probs))


class BuildCatalog(unittest.TestCase):
    """build_catalog() with every network edge stubbed."""

    def run_build(self, releases, components=None):
        seen_reg = {}

        def fake_windows_block(comp, version, rel, verifier, download_dir, registry_version=None):
            seen_reg[comp["name"]] = registry_version
            return None

        def fake_urlopen(req, timeout=None):
            url = req.full_url if hasattr(req, "full_url") else req
            assert url.endswith("/" + gen.DETECT_MANIFEST), url
            return FakeResponse(json.dumps(MANIFEST).encode())

        def fake_repo_file(repo, path, ref):
            return LEIA_HEADER if path.endswith("leia_edid_table.h") else None

        versions = {"runtime": "v2.69.1", "leia_plugin": "v2.23.0", "browser": "v1.10.3", "acme_plugin": "v1.0.0"}
        with mock.patch.object(gen, "get_release", lambda comp, tag: releases.get(comp["repo"])), \
                mock.patch.object(gen, "windows_block", fake_windows_block), \
                mock.patch.object(gen.urllib.request, "urlopen", fake_urlopen), \
                mock.patch.object(gen, "repo_file", fake_repo_file), \
                mock.patch.dict(gen.FLOOR_READERS, {"browser_nsis": lambda comp, tag: "2.62.0",
                                                    "leia_cmake": lambda comp, tag: "2.60.0"}), \
                mock.patch.object(gen, "COMPONENTS", components or gen.COMPONENTS), \
                mock.patch.object(gen, "warn", lambda msg: None), \
                mock.patch.object(gen, "info", lambda msg: None):
            cat = gen.build_catalog(versions, gen.VERIFIERS["none"], "unused")
        return cat, seen_reg

    def test_manifest_drives_the_entry(self):
        rels = {"displayxr-leia-plugin": release("displayxr-leia-plugin", [gen.DETECT_MANIFEST])}
        cat, seen_reg = self.run_build(rels)
        leia = cat["components"]["leia_plugin"]
        self.assertEqual(leia["kind"], "display_plugin")
        self.assertEqual(leia["label"], "Leia display plug-in (manifest)")
        self.assertEqual(leia["requires"], {"runtime": ">=2.66.8"})
        self.assertEqual([r["kind"] for r in leia["detect"]], ["edid", "edid", "usb"])
        self.assertEqual(seen_reg["leia_plugin"], MANIFEST["registry_version"])
        # Non-plug-in components keep their table facts.
        self.assertEqual(cat["components"]["browser"]["requires"], {"runtime": ">=2.62.0"})
        self.assertEqual(cat["components"]["browser"]["label"], "DisplayXR Browser")
        self.assertNotIn("detect", cat["components"]["browser"])
        self.assertNotIn("requires", cat["components"]["runtime"])

    def test_fallback_without_manifest(self):
        rels = {"displayxr-leia-plugin": release("displayxr-leia-plugin", ["DisplayXRLeiaSRSetup-2.23.0.exe"])}
        cat, seen_reg = self.run_build(rels)
        leia = cat["components"]["leia_plugin"]
        self.assertEqual(leia["label"], "Leia display plug-in")
        self.assertEqual(leia["requires"], {"runtime": ">=2.60.0"})
        self.assertEqual(leia["detect"][0], {"kind": "edid", "man": 44048, "prod": 16711,
                                             "confidence": "high", "note": "DELL_8K32"})
        self.assertEqual(leia["detect"][1]["confidence"], "low")  # prod < 16: generic Acer id
        self.assertEqual(leia["detect"][-1], gen.SR_REGISTRY_DETECT)
        self.assertIsNone(seen_reg["leia_plugin"])

    def test_new_vendor_is_a_table_row(self):
        acme = {"name": "acme_plugin", "pin": "acme_plugin", "repo": "acme-plugin", "asset": "Acme-{v}.exe",
                "kind": "display_plugin", "args": ["/S"], "registry_version": "HKLM\\Software\\Acme\\Version"}
        comps = list(gen.COMPONENTS) + [acme]
        cat, _ = self.run_build({"acme-plugin": release("acme-plugin", [gen.DETECT_MANIFEST])}, comps)
        self.assertEqual(cat["components"]["acme_plugin"]["detect"][0]["note"], "DELL_8K32")
        # Without manifest and without fallback: listed, but never detected.
        cat, _ = self.run_build({}, comps)
        self.assertNotIn("detect", cat["components"]["acme_plugin"])
        self.assertEqual(cat["components"]["acme_plugin"]["kind"], "display_plugin")


class Signers(unittest.TestCase):
    """#2737 trust: a row's own `signers` are accepted for that row only, and the
    top-level signers.windows lists the base set plus the vendor signers in use."""

    def test_allowed_signers_are_per_component(self):
        acme = {"name": "acme_plugin", "signers": ["Acme Corp"]}
        self.assertEqual(gen.allowed_signers(acme), ["Leia, Inc.", "Acme Corp"])
        self.assertEqual(gen.allowed_signers({"name": "browser"}), ["Leia, Inc."])

    def test_top_level_signers(self):
        acme = {"name": "acme_plugin", "pin": "acme_plugin", "repo": "acme-plugin", "asset": "Acme-{v}.exe",
                "kind": "display_plugin", "args": ["/S"], "signers": ["Acme Corp"],
                "registry_version": "HKLM\\Software\\Acme\\Version"}

        def fake_windows_block(comp, version, rel, verifier, download_dir, registry_version=None):
            if comp["name"] != "acme_plugin":
                return None
            return {"version": version, "signer": "Acme Corp"}

        with mock.patch.object(gen, "get_release", lambda comp, tag: None), \
                mock.patch.object(gen, "windows_block", fake_windows_block), \
                mock.patch.object(gen, "repo_file", lambda *a: None), \
                mock.patch.dict(gen.FLOOR_READERS, {"browser_nsis": lambda c, t: None,
                                                    "leia_cmake": lambda c, t: None}), \
                mock.patch.object(gen, "COMPONENTS", list(gen.COMPONENTS) + [acme]), \
                mock.patch.object(gen, "warn", lambda msg: None), \
                mock.patch.object(gen, "info", lambda msg: None):
            cat = gen.build_catalog({"acme_plugin": "v1.0.0"}, gen.VERIFIERS["none"], "unused")
        self.assertEqual(cat["signers"]["windows"], ["Leia, Inc.", "Acme Corp"])


if __name__ == "__main__":
    unittest.main()

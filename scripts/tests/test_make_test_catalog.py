#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""
Unit tests for scripts/make_test_catalog.py (the dxr-get validation fixture).

Hermetic: a synthetic schema-1 catalog, no network. The signing test needs the
`cryptography` package and is skipped without it.

What must hold:
  * the fixture is a valid schema-2 catalog apart from its one deliberate
    defect (a signer that is not delegated to the item's publisher);
  * it carries every case the validation recipe in dxr-get.md walks through;
  * a production catalog key is refused, and what is signed verifies under the
    throwaway key and not under the production keys.
"""

import base64
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPTS = os.path.dirname(HERE)
sys.path.insert(0, SCRIPTS)
import gen_catalog  # noqa: E402
import make_test_catalog  # noqa: E402

HAVE_CRYPTO = importlib.util.find_spec("cryptography") is not None
SCRIPT = os.path.join(SCRIPTS, "make_test_catalog.py")


def slurp(path):
    with open(path, "rb") as f:
        return f.read()


def v1():
    def win(name, version, file, registry):
        sha = {"runtime": "a", "leia": "b", "browser": "c"}[name] * 64
        return {"version": version, "file": file, "size": 1000 + len(name), "sha256": sha,
                "signer": "Leia, Inc.", "urls": ["https://example.invalid/" + file], "args": ["/S"],
                "registry_version": registry}

    return {
        "schema": 1, "serial": 1795000000, "generated": "2026-11-18T00:00:00Z", "expires": "2026-12-18T00:00:00Z",
        "channel": "stable", "source": {"versions_json_sha": "0" * 40}, "signers": {"windows": ["Leia, Inc."]},
        "components": {
            "runtime": {"recommended": "2.70.0",
                        "windows": win("runtime", "2.70.0", "rt.exe", "HKLM\\Software\\DisplayXR\\Runtime\\Version")},
            "leia_plugin": {"recommended": "2.23.0", "kind": "display_plugin", "requires": {"runtime": ">=2.66.8"},
                            "windows": win("leia", "2.23.0", "leia.exe",
                                           "HKLM\\Software\\DisplayXR\\Plugins\\LeiaSR\\Version"),
                            "detect": [{"kind": "edid", "man": 44048, "prod": 16711, "confidence": "high"}]},
            "browser": {"recommended": "1.10.3", "requires": {"runtime": ">=2.62.0"},
                        "windows": win("browser", "1.10.3", "b.exe", "HKLM\\Software\\DisplayXR\\Browser\\Version")},
        },
    }


class Fixture(unittest.TestCase):
    def setUp(self):
        self.cat = make_test_catalog.build_fixture(v1())
        self.items = {i["id"]: i for i in self.cat["items"]}

    def test_valid_apart_from_the_deliberate_defect(self):
        self.assertEqual(make_test_catalog.expected_problems(self.cat), [])
        flagged = gen_catalog.validate_v2(self.cat)
        self.assertEqual(len(flagged), 1)
        self.assertIn("fixture.undelegated-signer", flagged[0])

    def test_same_run_values_as_the_v1_it_came_from(self):
        self.assertEqual(self.cat["schema"], 2)
        self.assertEqual(self.cat["serial"], 1795000000)
        self.assertEqual(self.cat["expires"], "2026-12-18T00:00:00Z")

    def test_every_detector_is_exercised_met_and_unmet(self):
        def kinds(iid):
            return sorted(r["kind"] for q in self.items[iid]["requires"] if q["kind"] == "external"
                          for r in q["detect"])

        self.assertEqual(kinds("fixture.detectors-met"),
                         ["file_exists", "registry_key_exists", "registry_sz_min", "service_exists"])
        self.assertEqual(kinds("fixture.detectors-unmet"), ["file_exists", "registry_key_exists", "service_exists"])
        for q in self.items["fixture.detectors-unmet"]["requires"]:
            self.assertNotIn("asset", q)  # detection only

    def test_refusals(self):
        self.assertEqual(self.items["fixture.runtime-too-new"]["runtime"], {"max": "1.0.0"})
        self.assertEqual(self.items["fixture.runtime-too-old"]["runtime"], {"min": "99.0.0"})
        abi = self.items["fixture.abi-mismatch"]
        self.assertEqual(abi["class"], "display_processor")
        self.assertEqual(abi["abi"], {"display_processor": {"min": 99, "max": 99}})
        # The runtime item says what the catalog's runtime implements, so the gate can judge.
        self.assertEqual(self.items["runtime"]["abi"]["display_processor"], {"min": 5, "max": 5})

    def test_multi_asset_item_has_a_signed_prerequisite_installer(self):
        ext = [q for q in self.items["fixture.multi-asset"]["requires"] if q["kind"] == "external"]
        self.assertEqual(len(ext), 1)
        asset = ext[0]["asset"]["windows-x64"]
        self.assertEqual(asset["signature"], {"kind": "authenticode", "signer": "Leia, Inc."})
        self.assertNotIn("installed", asset)
        self.assertNotEqual(asset["sha256"], self.items["fixture.multi-asset"]["assets"]["windows-x64"]["sha256"])

    def test_signer_cases(self):
        signers = [s["subject_cn"] for s in self.cat["publishers"]["fixture-other"]["signers"]["windows-x64"]]
        wrong = self.items["fixture.wrong-signer"]
        # Delegated on paper, so the catalog side passes; the file itself is signed by someone else.
        self.assertEqual(wrong["publisher"], "fixture-other")
        self.assertIn(wrong["assets"]["windows-x64"]["signature"]["signer"], signers)
        undelegated = self.items["fixture.undelegated-signer"]
        self.assertNotIn(undelegated["assets"]["windows-x64"]["signature"]["signer"], signers)

    def test_default_set_and_uninstall_rules(self):
        self.assertTrue(self.items["browser"]["recommended"])
        self.assertIn("UninstallString", self.items["browser"]["assets"]["windows-x64"]["uninstall"]["path"])
        self.assertIn("UninstallString", self.items["leia-sr"]["assets"]["windows-x64"]["uninstall"]["path"])
        self.assertNotIn("uninstall", self.items["runtime"]["assets"]["windows-x64"])

    def test_fixture_items_never_read_as_installed(self):
        for iid, it in self.items.items():
            if iid.startswith("fixture.") and "listing" not in it["assets"]["windows-x64"]:
                path = it["assets"]["windows-x64"]["installed"]["path"]
                self.assertIn("\\DisplayXR\\Fixture\\", path, iid)
                self.assertNotIn("uninstall", it["assets"]["windows-x64"], iid)

    def test_a_catalog_without_the_three_assets_is_refused(self):
        bad = v1()
        del bad["components"]["browser"]["windows"]
        with self.assertRaises(SystemExit):
            make_test_catalog.build_fixture(bad)


class CommandLine(unittest.TestCase):
    def run_script(self, *args):
        return subprocess.run([sys.executable, SCRIPT, *args], capture_output=True, text=True)

    def test_no_sign_build(self):
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "v1.json")
            with open(src, "w", encoding="utf-8") as f:
                json.dump(v1(), f)
            r = self.run_script("build", "--no-sign", "--v1", src, "--out", os.path.join(d, "out"))
            self.assertEqual(r.returncode, 0, r.stderr)
            data = slurp(os.path.join(d, "out", "stable.json"))
            self.assertFalse(os.path.exists(os.path.join(d, "out", "stable.json.sig")))
            # The generator's encoding: LF, no BOM, trailing newline.
            self.assertTrue(data.endswith(b"\n") and b"\r\n" not in data and not data.startswith(b"\xef\xbb\xbf"))
            self.assertEqual(json.loads(data)["schema"], 2)

    @unittest.skipUnless(HAVE_CRYPTO, "needs the cryptography package")
    def test_keygen_sign_and_verify(self):
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "v1.json")
            with open(src, "w", encoding="utf-8") as f:
                json.dump(v1(), f)
            r = self.run_script("keygen", "--out", d)
            self.assertEqual(r.returncode, 0, r.stderr)
            pub_hex = [line.split(": ")[1] for line in r.stdout.splitlines() if line.startswith("public key: ")][0]
            self.assertEqual(len(pub_hex), 64)
            self.assertIn("-DDXR_GET_TEST_ROOT_KEY=" + pub_hex, r.stdout)
            # A second keygen never overwrites a seed.
            self.assertNotEqual(self.run_script("keygen", "--out", d).returncode, 0)

            out = os.path.join(d, "out")
            r = self.run_script("build", "--key", os.path.join(d, "test-root.seed"), "--v1", src, "--out", out)
            self.assertEqual(r.returncode, 0, r.stderr)
            data = slurp(os.path.join(out, "stable.json"))
            sig_text = slurp(os.path.join(out, "stable.json.sig"))
            self.assertEqual(len(sig_text), 89)  # 88 base64 chars + LF, as dxr-get expects
            sig = base64.b64decode(sig_text.strip())
            Ed25519PublicKey.from_public_bytes(bytes.fromhex(pub_hex)).verify(sig, data)
            # ...and under no production key.
            self.assertIsNone(gen_catalog.verify_bytes(data, sig))
            with self.assertRaises(InvalidSignature):
                Ed25519PublicKey.from_public_bytes(bytes.fromhex(pub_hex)).verify(sig, data + b" ")


if __name__ == "__main__":
    unittest.main()

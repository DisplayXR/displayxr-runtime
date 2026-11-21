#!/usr/bin/env python3
"""Unit tests for scripts/gen_catalog.py -- the schema-2 derivation (ADR-064).

Hermetic: no network, no signing key, no `cryptography`. The schema-1 builder
talks to GitHub, so it is replaced by a fixed schema-1 catalog; what is pinned
here is that

  * asking for schema 2 (`--out-v2`) leaves the schema-1 file byte-identical,
    and those bytes are the ones the generator wrote before schema 2 existed;
  * the schema-2 document carries the same assets (hash, size, URLs, signer)
    and nothing schema 1 does not know;
  * the validator refuses the mistakes that matter (a signer the publisher was
    not delegated, a non-https URL, a duplicate id).

    python3 scripts/tests/test_gen_catalog.py
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "scripts"))

import gen_catalog  # noqa: E402

SHA_A = "a" * 64
SHA_B = "b" * 64
SHA_C = "c" * 64


def win(file: str, version: str, sha: str, args: list[str], reg: str) -> dict:
    return {
        "version": version,
        "file": file,
        "size": 1234567,
        "sha256": sha,
        "signer": "Leia, Inc.",
        "urls": [
            f"https://github.com/DisplayXR/x/releases/download/v{version}/{file}",
            "https://api.github.com/repos/DisplayXR/x/releases/assets/1",
        ],
        "args": args,
        "registry_version": reg,
    }


def v1_fixture() -> dict:
    """A schema-1 catalog in the generator's own key order."""
    return {
        "schema": 1,
        "serial": 1794877934,
        "generated": "2026-11-17T01:12:14Z",
        "expires": "2026-12-17T01:12:14Z",
        "channel": "stable",
        "source": {"versions_json_sha": "0123456789abcdef0123456789abcdef01234567"},
        "signers": {"windows": ["Leia, Inc."]},
        "components": {
            "runtime": {
                "recommended": "2.69.2",
                "windows": win("DisplayXRSetup-2.69.2.exe", "2.69.2", SHA_A, ["/S", "/NOSTART"],
                               "HKLM\\Software\\DisplayXR\\Runtime\\Version"),
            },
            "leia_plugin": {
                "recommended": "2.23.0",
                "kind": "display_plugin",
                "requires": {"runtime": ">=2.66.8"},
                # windows.version lags `recommended`: the carried-forward case.
                "windows": win("DisplayXRLeiaSRSetup-2.22.0.exe", "2.22.0", SHA_B, ["/S"],
                               "HKLM\\Software\\DisplayXR\\Plugins\\LeiaSR\\Version"),
                "detect": [
                    {"kind": "edid", "man": 44048, "prod": 16711, "confidence": "high", "note": "DELL_8K32"},
                    {"kind": "registry_dword_min", "key": "HKLM\\SOFTWARE\\Dimenco\\Simulated Reality",
                     "value": "NConnectedDevices", "min": 1, "confidence": "high", "note": "SR"},
                ],
            },
            # No windows block: a pin whose asset is not listable.
            "browser": {"recommended": "1.10.5", "requires": {"runtime": ">=2.62.0"}},
        },
    }


# sha256 of canonical_bytes(v1_fixture()) as written by the generator at
# origin/main before the schema-2 change. A change to the schema-1 serialisation
# (indent, key order, escaping, trailing newline) fails here: deployed dxr-get
# builds verify a signature over exactly these bytes.
V1_FIXTURE_SHA256 = "d74ac74ac6c016e91c1730e1d643e85d45445947147c8c30721df8f372709023"


class Schema1Untouched(unittest.TestCase):
    def test_canonical_bytes_are_pinned(self):
        data = gen_catalog.canonical_bytes(v1_fixture())
        self.assertEqual(hashlib.sha256(data).hexdigest(), V1_FIXTURE_SHA256)
        self.assertTrue(data.endswith(b"\n"))
        self.assertNotIn(b"\r", data)

    def test_build_v2_does_not_modify_v1(self):
        v1 = v1_fixture()
        before = copy.deepcopy(v1)
        v2 = gen_catalog.build_catalog_v2(v1)
        self.assertEqual(v1, before)
        # No aliasing either: editing the result must not reach the input.
        v2["items"][1]["detect"][0]["man"] = 0
        v2["items"][0]["assets"]["windows-x64"]["urls"].append("https://example.invalid/x")
        v2["source"]["versions_json_sha"] = "changed"
        self.assertEqual(v1, before)

    def run_generate(self, out: Path, out_v2: Path | None) -> None:
        saved = gen_catalog.build_catalog
        gen_catalog.build_catalog = lambda versions, verifier, download_dir, previous=None: v1_fixture()
        try:
            rc = gen_catalog.cmd_generate(argparse.Namespace(
                versions=str(REPO_ROOT / "versions.json"), download_dir=str(out / "dl"), previous=None,
                verifier="none", out=str(out), no_sign=True, out_v2=str(out_v2) if out_v2 else None))
        finally:
            gen_catalog.build_catalog = saved
        self.assertEqual(rc, 0)

    def test_generate_writes_identical_schema1_with_and_without_v2(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            self.run_generate(tmp / "a", None)
            self.run_generate(tmp / "b", tmp / "b2")
            a = (tmp / "a" / "stable.json").read_bytes()
            b = (tmp / "b" / "stable.json").read_bytes()
            self.assertEqual(a, b)
            self.assertEqual(hashlib.sha256(a).hexdigest(), V1_FIXTURE_SHA256)
            self.assertEqual(sorted(p.name for p in (tmp / "a").iterdir() if p.is_file()), ["stable.json"])
            v2 = json.loads((tmp / "b2" / "stable.json").read_bytes())
            self.assertEqual(v2["schema"], 2)
            self.assertFalse((tmp / "b2" / "stable.json.sig").exists())  # --no-sign

    def test_out_v2_must_not_be_the_schema1_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(SystemExit):
                self.run_generate(Path(tmp), Path(tmp))
            # The schema-1 file was written before the refusal and is intact.
            data = (Path(tmp) / "stable.json").read_bytes()
            self.assertEqual(hashlib.sha256(data).hexdigest(), V1_FIXTURE_SHA256)


class Schema2Content(unittest.TestCase):
    def setUp(self):
        self.v1 = v1_fixture()
        self.v2 = gen_catalog.build_catalog_v2(self.v1)
        self.items = {i["id"]: i for i in self.v2["items"]}

    def test_header_follows_schema1(self):
        for key in ("serial", "generated", "expires", "channel", "source"):
            self.assertEqual(self.v2[key], self.v1[key])
        self.assertEqual(self.v2["schema"], 2)
        self.assertEqual(self.v2["publishers"]["displayxr"]["signers"]["windows-x64"],
                         [{"kind": "authenticode", "subject_cn": "Leia, Inc."}])

    def test_items_and_aliases(self):
        self.assertEqual(list(self.items), ["runtime", "leia-sr", "browser"])
        self.assertEqual(self.items["leia-sr"]["aliases"], ["leia_plugin"])
        self.assertEqual(self.items["leia-sr"]["class"], "display_processor")
        self.assertEqual(self.items["leia-sr"]["registered_id"], "leia-sr")
        self.assertEqual(self.items["runtime"]["class"], "runtime")
        self.assertEqual(self.items["browser"]["class"], "app")
        self.assertNotIn("registered_id", self.items["browser"])

    def test_asset_is_the_same_file(self):
        for name, item_id in (("runtime", "runtime"), ("leia_plugin", "leia-sr")):
            w = self.v1["components"][name]["windows"]
            a = self.items[item_id]["assets"]["windows-x64"]
            for key in ("version", "file", "size", "sha256", "urls"):
                self.assertEqual(a[key], w[key])
            self.assertEqual(a["signature"], {"kind": "authenticode", "signer": w["signer"]})
            self.assertEqual(a["installer"]["args"], w["args"])
            self.assertEqual(a["installed"], {"kind": "registry_sz", "path": w["registry_version"]})

    def test_pin_and_carried_forward_asset_version_stay_distinct(self):
        self.assertEqual(self.items["leia-sr"]["version"], "2.23.0")
        self.assertEqual(self.items["leia-sr"]["assets"]["windows-x64"]["version"], "2.22.0")

    def test_requires_and_detect(self):
        self.assertEqual(self.items["leia-sr"]["requires"], [{"kind": "runtime", "version": ">=2.66.8"}])
        self.assertNotIn("requires", self.items["runtime"])
        self.assertEqual(self.items["leia-sr"]["detect"], self.v1["components"]["leia_plugin"]["detect"])

    def test_unlistable_component_has_no_assets(self):
        self.assertNotIn("assets", self.items["browser"])

    def test_nothing_is_guessed(self):
        # Schema 1 does not know these; the derivation must not invent them.
        for item in self.v2["items"]:
            for key in ("abi", "tier", "license", "conformance", "provenance"):
                self.assertNotIn(key, item)
            for asset in item.get("assets", {}).values():
                self.assertNotIn("uninstall", asset)

    def test_unknown_component_is_not_listed(self):
        self.v1["components"]["mystery"] = {"recommended": "1.0.0"}
        v2 = gen_catalog.build_catalog_v2(self.v1)
        self.assertEqual([i["id"] for i in v2["items"]], ["runtime", "leia-sr", "browser"])

    def test_generated_document_validates(self):
        self.assertEqual(gen_catalog.validate_v2(self.v2), [])


class Schema2Validation(unittest.TestCase):
    def setUp(self):
        self.v2 = gen_catalog.build_catalog_v2(v1_fixture())

    def problems(self) -> str:
        return "\n".join(gen_catalog.validate_v2(self.v2))

    def test_signer_must_be_delegated_to_the_publisher(self):
        self.v2["items"][0]["assets"]["windows-x64"]["signature"]["signer"] = "Somebody Else LLC"
        self.assertIn("is not one of the publisher's windows-x64 signers", self.problems())

    def test_signer_delegation_is_per_platform(self):
        pub = self.v2["publishers"]["displayxr"]
        pub["signers"] = {"linux-x86_64": pub["signers"]["windows-x64"]}
        self.assertIn("is not one of the publisher's windows-x64 signers", self.problems())

    def test_unknown_publisher(self):
        self.v2["items"][0]["publisher"] = "nobody"
        self.assertIn("publisher 'nobody' is not in publishers", self.problems())

    def test_plain_http_url(self):
        self.v2["items"][0]["assets"]["windows-x64"]["urls"] = ["http://example.invalid/a.exe"]
        self.assertIn("https", self.problems())

    def test_bad_hash_and_size(self):
        asset = self.v2["items"][0]["assets"]["windows-x64"]
        asset["sha256"] = "ABC"
        asset["size"] = 0
        text = self.problems()
        self.assertIn("sha256", text)
        self.assertIn("size", text)

    def test_duplicate_id_or_alias(self):
        self.v2["items"][2]["aliases"] = ["leia_plugin"]
        self.assertIn("used twice", self.problems())

    def test_loadable_class_needs_registered_id(self):
        del self.v2["items"][1]["registered_id"]
        self.assertIn("needs registered_id", self.problems())

    def test_unknown_class_and_requires_kind(self):
        self.v2["items"][0]["class"] = "game_profile"
        self.v2["items"][1]["requires"] = [{"kind": "magic"}, {"kind": "runtime", "version": "2.0"}]
        text = self.problems()
        self.assertIn("unknown class 'game_profile'", text)
        self.assertIn("requires kind 'magic'", text)
        self.assertIn("is not >=X.Y.Z", text)

    def test_external_requirement_and_unsigned_asset_are_allowed(self):
        self.v2["items"][1]["requires"].append({"kind": "external", "name": "vendor-service"})
        del self.v2["items"][0]["assets"]["windows-x64"]["signature"]
        self.assertEqual(gen_catalog.validate_v2(self.v2), [])


if __name__ == "__main__":
    unittest.main()

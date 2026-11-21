#!/usr/bin/env python3
"""Unit tests for scripts/gen_catalog.py -- the schema-2 derivation (ADR-064).

Hermetic: no network, no signing key, no `cryptography`. The schema-1 builder
talks to GitHub, so it is replaced by a fixed schema-1 catalog; what is pinned
here is that

  * asking for schema 2 (`--out-v2`) leaves the schema-1 file byte-identical,
    and those bytes are the ones the generator wrote before schema 2 existed;
  * the schema-2 document carries the same assets (hash, size, URLs, signer)
    and nothing schema 1 does not know;
  * the validator refuses the mistakes that matter (an unsigned installable
    asset, a signer the publisher was not delegated, a non-https URL, a
    duplicate id).

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

    def test_external_requirement_is_allowed(self):
        self.v2["items"][1]["requires"].append({"kind": "external", "name": "vendor-service"})
        self.assertEqual(gen_catalog.validate_v2(self.v2), [])

    def test_installable_asset_must_be_signed(self):
        del self.v2["items"][0]["assets"]["windows-x64"]["signature"]
        self.assertIn("an installable asset needs a signature", self.problems())

    def test_listing_only_asset_needs_no_signature(self):
        self.v2["items"][2]["assets"] = {"android-arm64": {"listing": {"url": "https://example.invalid/app"}}}
        self.assertEqual(gen_catalog.validate_v2(self.v2), [])
        self.v2["items"][2]["assets"]["android-arm64"]["listing"]["url"] = "http://example.invalid/app"
        self.assertIn("listing.url must be an https URL", self.problems())


# --------------------------------------------------------------------------- #2737

MANIFEST = {
    "schema": 1,
    "id": "leia-sr",
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

LEIA_HEADER = """
static const uint16_t leia_edid_table[][2] = {
    {44048, 16711},   // DELL_8K32
    {44806, 3},       // ACER_GENERIC
};
"""


class DetectRuleValidation(unittest.TestCase):
    def test_exact_and_range(self):
        r, p = gen_catalog.validate_rule({"kind": "edid", "man": 1, "prod": 2, "confidence": "high"})
        self.assertEqual(r, {"kind": "edid", "man": 1, "prod": 2, "confidence": "high"})
        self.assertEqual(p, [])
        r, _ = gen_catalog.validate_rule({"kind": "usb", "vid": 1, "pid_min": 2, "pid_max": 9,
                                          "confidence": "low"})
        self.assertEqual((r["pid_min"], r["pid_max"]), (2, 9))

    def test_vendor_only_is_never_high(self):
        for rule in ({"kind": "edid", "man": 44048, "confidence": "high"},
                     {"kind": "usb", "vid": 1133, "confidence": "high"}):
            r, p = gen_catalog.validate_rule(rule)
            self.assertEqual(r["confidence"], "low")
            self.assertTrue(p)

    def test_rejects(self):
        for rule in (
                {"kind": "edid", "man": 70000, "prod": 1, "confidence": "high"},
                {"kind": "edid", "man": 1, "prod": 1, "prod_min": 1, "prod_max": 2, "confidence": "high"},
                {"kind": "edid", "man": 1, "prod_min": 5, "prod_max": 2, "confidence": "high"},
                {"kind": "edid", "man": True, "prod": 1, "confidence": "high"},
                {"kind": "usb", "vid": 1, "pid": 2, "confidence": "maybe"},
                {"kind": "registry_dword_min", "key": "HKCU\\X", "value": "v", "min": 1, "confidence": "high"},
                {"kind": "file_exists", "path": "%ProgramFiles%\\a.dll"},
        ):
            r, p = gen_catalog.validate_rule(rule)
            self.assertIsNone(r, rule)
            self.assertTrue(p, rule)

    def test_unknown_kind_dropped(self):
        r, p = gen_catalog.validate_rule({"kind": "teleport", "confidence": "high"})
        self.assertIsNone(r)
        self.assertIn("unknown rule kind", p[0])

    def test_schema2_prerequisite_detectors_pass_through(self):
        for rule in (
                dict(gen_catalog.SR_REGISTRY_DETECT),
                {"kind": "registry_sz_min", "key": "HKLM\\K", "value": "Version", "min": "3.2"},
                {"kind": "registry_key_exists", "key": "HKLM\\K"},
                {"kind": "file_exists", "path": "C:\\Program Files\\Acme\\a.dll"},
                {"kind": "service_exists", "name": "AcmeSvc"},
        ):
            r, p = gen_catalog.validate_rule(rule)
            self.assertEqual(r, rule)
            self.assertEqual(p, [])

    def test_fixture_rules_are_clean(self):
        for rule in v1_fixture()["components"]["leia_plugin"]["detect"]:
            self.assertEqual(gen_catalog.validate_rule(rule), (rule, []))


class DetectManifestValidation(unittest.TestCase):
    def test_good(self):
        clean, probs = gen_catalog.validate_detect_manifest(MANIFEST)
        self.assertEqual(clean["id"], "leia-sr")
        self.assertEqual(clean["label"], "Leia display plug-in (manifest)")
        self.assertEqual(clean["floor"], "2.66.8")
        self.assertEqual(len(clean["detect"]), 3)  # the unknown kind is dropped
        self.assertEqual(len(probs), 1)

    def test_wrong_schema_or_no_rule(self):
        self.assertIsNone(gen_catalog.validate_detect_manifest(dict(MANIFEST, schema=2))[0])
        self.assertIsNone(gen_catalog.validate_detect_manifest(
            dict(MANIFEST, detect=[{"kind": "teleport", "confidence": "high"}]))[0])

    def test_bad_optional_fields_are_ignored(self):
        clean, probs = gen_catalog.validate_detect_manifest(
            dict(MANIFEST, id="Bad Id", requires={"runtime": "2.66"}))
        self.assertNotIn("id", clean)
        self.assertNotIn("floor", clean)
        self.assertEqual(len(probs), 3)


class FakeResponse:
    def __init__(self, data):
        self.data = data

    def read(self, n=-1):
        return self.data

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False


class DetectManifestIngest(unittest.TestCase):
    """build_catalog() with GitHub stubbed: the manifest drives detect / floor /
    registry key; without it the Leia header fallback gives today's rules."""

    def run_build(self, with_manifest, components=None, manifest=MANIFEST):
        seen = {}

        def release(path):
            repo = path.split("/")[3]
            names = [gen_catalog.DETECT_MANIFEST] if with_manifest else []
            return {"assets": [{"name": n, "browser_download_url": f"https://example.invalid/{repo}/{n}"}
                               for n in names]}

        def windows_block(comp, version, verifier, download_dir, registry_version=None):
            seen[comp["name"]] = registry_version
            return None

        saved = {k: getattr(gen_catalog, k) for k in (
            "api_json", "windows_block", "repo_file", "browser_floor", "leia_floor", "warn", "info",
            "COMPONENTS")}
        saved_urlopen = gen_catalog.urllib.request.urlopen
        try:
            gen_catalog.api_json = release
            gen_catalog.windows_block = windows_block
            gen_catalog.repo_file = lambda repo, path, ref: LEIA_HEADER if path.endswith("table.h") else None
            gen_catalog.browser_floor = lambda tag: "2.62.0"
            gen_catalog.leia_floor = lambda tag: "2.60.0"
            gen_catalog.warn = gen_catalog.info = lambda msg: None
            if components is not None:
                gen_catalog.COMPONENTS = components
            gen_catalog.urllib.request.urlopen = lambda req, timeout=None: FakeResponse(
                json.dumps(manifest).encode())
            versions = {"runtime": "v2.69.1", "leia_plugin": "v2.23.0", "browser": "v1.10.5",
                        "acme_plugin": "v1.0.0"}
            cat = gen_catalog.build_catalog(versions, gen_catalog.VERIFIERS["none"], "unused")
        finally:
            for k, v in saved.items():
                setattr(gen_catalog, k, v)
            gen_catalog.urllib.request.urlopen = saved_urlopen
        return cat, seen

    def test_manifest_drives_the_entry(self):
        cat, seen = self.run_build(True)
        leia = cat["components"]["leia_plugin"]
        self.assertEqual(leia["requires"], {"runtime": ">=2.66.8"})
        self.assertEqual([r["kind"] for r in leia["detect"]], ["edid", "edid", "usb"])
        self.assertEqual(seen["leia_plugin"], MANIFEST["registry_version"])
        self.assertNotIn("label", leia)  # schema 1 is unchanged in shape
        self.assertNotIn("id", leia)
        self.assertEqual(cat["components"]["browser"]["requires"], {"runtime": ">=2.62.0"})

    def test_fallback_without_manifest(self):
        cat, seen = self.run_build(False)
        leia = cat["components"]["leia_plugin"]
        self.assertEqual(leia["requires"], {"runtime": ">=2.60.0"})
        self.assertEqual(leia["detect"][0], {"kind": "edid", "man": 44048, "prod": 16711,
                                             "confidence": "high", "note": "DELL_8K32"})
        self.assertEqual(leia["detect"][1]["confidence"], "low")  # prod < 16
        self.assertEqual(leia["detect"][-1], gen_catalog.SR_REGISTRY_DETECT)
        self.assertIsNone(seen["leia_plugin"])
        self.assertEqual(gen_catalog.MANIFESTS, {})

    def test_v2_names_a_new_vendor_from_its_manifest(self):
        acme = {"name": "acme_plugin", "pin": "acme_plugin", "repo": "acme-plugin",
                "asset": "Acme-{v}.exe", "kind": "display_plugin", "args": ["/S"],
                "registry_version": "HKLM\\Software\\Acme\\Version"}
        manifest = dict(MANIFEST, id="acme-lightfield", label="Acme lightfield")
        cat, _ = self.run_build(True, list(gen_catalog.COMPONENTS) + [acme], manifest)
        v2 = gen_catalog.build_catalog_v2(cat, dict(gen_catalog.MANIFESTS))
        item = next(i for i in v2["items"] if i["id"] == "acme-lightfield")
        self.assertEqual(item["class"], "display_processor")
        self.assertEqual(item["registered_id"], "acme-lightfield")
        self.assertEqual(item["name"], "Acme lightfield")
        self.assertEqual(item["aliases"], ["acme_plugin"])
        self.assertEqual(len(item["detect"]), 3)
        # Leia keeps its V2_ITEMS identity whatever its manifest says.
        self.assertTrue(any(i["id"] == "leia-sr" for i in v2["items"]))
        # Without the manifests, a vendor V2_ITEMS does not know is not listed.
        v2 = gen_catalog.build_catalog_v2(cat)
        self.assertFalse(any(i["id"] == "acme-lightfield" for i in v2["items"]))


if __name__ == "__main__":
    unittest.main()

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


def extra_fixture() -> dict:
    """What build_extra_components returns: the schema-2-only components."""
    return {
        "conversion_open": {
            "recommended": "0.1.1",
            "windows": win("DisplayXROpenConversionSetup-0.1.1.exe", "0.1.1", SHA_C, ["/S"],
                           "HKLM\\Software\\DisplayXR\\Plugins\\OpenConversion\\Version"),
        },
        "leia_conversion": {
            "recommended": "0.1.0",
            "requires": {"runtime": ">=2.41.0"},
            "windows": win("DisplayXRLeiaConversionSetup-0.1.0.exe", "0.1.0", "d" * 64, ["/S"],
                           "HKLM\\Software\\DisplayXR\\Plugins\\LeiaConversion\\Version"),
        },
        "ultraleap": {
            "recommended": "0.1.0",
            "runtime_exact": "2.69.3",
            "requires": {"runtime": ">=2.69.3"},
            "windows": win("DisplayXRUltraleapSetup-0.1.0.7.exe", "0.1.0", "e" * 64, ["/S"],
                           "HKLM\\Software\\DisplayXR\\Plugins\\Ultraleap\\Version"),
        },
        "shell": {
            "recommended": "2.4.2",
            "requires": {"runtime": ">=2.36.0"},
            "windows": win("DisplayXRShellSetup-2.4.2.exe", "2.4.2", "f" * 64, ["/S"],
                           "HKLM\\Software\\DisplayXR\\WorkspaceControllers\\shell\\Version"),
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
        saved_extra = gen_catalog.build_extra_components
        gen_catalog.build_catalog = lambda versions, verifier, download_dir, previous=None: v1_fixture()
        gen_catalog.build_extra_components = lambda versions, verifier, download_dir: extra_fixture()
        try:
            rc = gen_catalog.cmd_generate(argparse.Namespace(
                versions=str(REPO_ROOT / "versions.json"), download_dir=str(out / "dl"), previous=None,
                verifier="none", out=str(out), no_sign=True, out_v2=str(out_v2) if out_v2 else None))
        finally:
            gen_catalog.build_catalog = saved
            gen_catalog.build_extra_components = saved_extra
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

    def test_uninstall_rules(self):
        # The engine copies this command into the install receipt. Known for the
        # Leia plug-in and the browser (their Add/Remove Programs keys); never
        # emitted for the runtime, and never without an asset to install.
        arp = "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"
        self.assertNotIn("uninstall", self.items["runtime"]["assets"]["windows-x64"])
        self.assertEqual(self.items["leia-sr"]["assets"]["windows-x64"]["uninstall"],
                         {"kind": "registry_command", "path": arp + "DisplayXRLeiaSR\\UninstallString", "args": ["/S"]})
        self.assertNotIn("assets", self.items["browser"])  # unlistable in the fixture: no rule without an asset
        v1 = v1_fixture()
        v1["components"]["browser"]["windows"] = win("DisplayXR-Browser-Setup-1.10.5.exe", "1.10.5", SHA_C, ["/S"],
                                                     "HKLM\\Software\\DisplayXR\\Browser\\Version")
        v2 = gen_catalog.build_catalog_v2(v1)
        self.assertEqual(v2["items"][2]["assets"]["windows-x64"]["uninstall"],
                         {"kind": "registry_command", "path": arp + "DisplayXR Browser\\UninstallString", "args": ["/S"]})
        self.assertEqual(gen_catalog.validate_v2(v2), [])
        # The schema-1 document knows nothing of it.
        self.assertNotIn(b"ninstall", gen_catalog.canonical_bytes(v1))

    def test_unknown_component_is_not_listed(self):
        self.v1["components"]["mystery"] = {"recommended": "1.0.0"}
        v2 = gen_catalog.build_catalog_v2(self.v1)
        self.assertEqual([i["id"] for i in v2["items"]], ["runtime", "leia-sr", "browser"])

    def test_generated_document_validates(self):
        self.assertEqual(gen_catalog.validate_v2(self.v2), [])


class Schema2OnlyComponents(unittest.TestCase):
    """The providers schema 1 cannot describe (V2_ONLY_COMPONENTS)."""

    def setUp(self):
        self.v2 = gen_catalog.build_catalog_v2(v1_fixture(), extra=extra_fixture())
        self.items = {i["id"]: i for i in self.v2["items"]}

    def test_they_follow_the_schema1_components_and_validate(self):
        self.assertEqual([i["id"] for i in self.v2["items"]],
                         ["runtime", "leia-sr", "browser", "open-conversion", "leia-conversion", "ultraleap",
                          "displayxr-shell"])
        self.assertEqual(gen_catalog.validate_v2(self.v2), [])

    def test_schema1_never_carries_them(self):
        names = {c["name"] for c in gen_catalog.V2_ONLY_COMPONENTS}
        self.assertFalse(names & {c["name"] for c in gen_catalog.COMPONENTS})
        self.assertFalse(names & set(v1_fixture()["components"]))

    def test_open_conversion_is_the_recommended_converter(self):
        item = self.items["open-conversion"]
        self.assertEqual((item["class"], item["registered_id"]), ("conversion_provider", "open-conversion"))
        self.assertIs(item["recommended"], True)
        self.assertEqual(item["aliases"], ["conversion_open"])
        self.assertNotIn("requires", item)  # it declares no floor; none is invented
        # Nothing else is recommended without a hardware match.
        self.assertEqual([i["id"] for i in self.v2["items"] if i.get("recommended")], ["open-conversion"])

    def test_user_facing_strings_do_not_name_the_vendor_converter_technology(self):
        # What a user reads of an item: its name, its summary, and the name of each
        # thing it requires (Dashboard rows, the installer's component page, `dxr-get`).
        # Ids, aliases, registry keys and a detect rule's `note` are not shown and stay.
        item = self.items["leia-conversion"]
        self.assertEqual(item["name"], "Leia conversion")
        self.assertEqual(item["summary"], "2D-to-3D conversion from the display maker.")
        self.assertEqual(item["requires"][1]["name"], "Leia conversion runtime")
        self.assertEqual((item["id"], item["registered_id"], item["aliases"]),
                         ("leia-conversion", "leia-conversion", ["leia_conversion"]))
        for i in self.v2["items"]:
            shown = [i.get("name", ""), i.get("summary", "")]
            shown += [r.get("name", "") for r in i.get("requires", [])]
            for text in shown:
                self.assertNotIn("neurd", text.lower(), "%s: %r" % (i["id"], text))

    def test_leia_conversion_declares_neurd_and_installs_nothing_for_it(self):
        item = self.items["leia-conversion"]
        self.assertEqual(item["aliases"], ["leia_conversion"])
        self.assertEqual(item["requires"][0], {"kind": "runtime", "version": ">=2.41.0"})
        ext = item["requires"][1]
        self.assertEqual((ext["kind"], ext["min_version"]), ("external", "0.4.6"))
        self.assertNotIn("asset", ext)       # declared and detected only: no public NeurD build exists
        self.assertNotIn("info_url", ext)    # and no public page to point at
        # The Windows floor and the Windows registry leg: all schema 2 can say today.
        self.assertEqual(sorted(ext), ["detect", "kind", "min_version", "name"])
        self.assertEqual(len(ext["detect"]), 1)
        rule = ext["detect"][0]
        self.assertEqual((rule["kind"], rule["key"], rule["value"], rule["min"]),
                         ("registry_sz_min", "HKLM\\SOFTWARE\\LeiaInc\\NeurD", "Version", "0.4.6"))
        self.assertIn("environment variable", rule["note"])  # what the rule cannot see is said, not guessed around
        self.assertNotIn("neurd", rule["note"].lower())  # the note is shown to the user: plain words only
        self.assertNotIn("confidence", rule)
        self.assertEqual(gen_catalog.validate_rule(rule), (rule, []))
        self.assertNotIn("runtime", item)    # a floor, not a range

    def test_ultraleap_is_pinned_to_one_runtime(self):
        item = self.items["ultraleap"]
        self.assertEqual((item["class"], item["registered_id"]), ("input_provider", "ultraleap"))
        self.assertEqual(item["runtime"], {"min": "2.69.3", "max": "2.69.3"})
        self.assertEqual(item["requires"][0], {"kind": "runtime", "version": ">=2.69.3"})
        self.assertEqual(item["requires"][1]["kind"], "external")
        self.assertEqual(item["assets"]["windows-x64"]["file"], "DisplayXRUltraleapSetup-0.1.0.7.exe")

    def test_shell_is_a_workspace_controller_the_user_picks(self):
        item = self.items["displayxr-shell"]
        self.assertEqual((item["class"], item["registered_id"]), ("workspace_controller", "shell"))
        self.assertEqual(item["aliases"], ["shell"])  # the versions.json pin: `/COMPONENTS=shell`
        self.assertNotIn("recommended", item)         # listed unticked; never installed by `--default`
        self.assertNotIn("detect", item)              # and no hardware evidence installs it
        self.assertNotIn("abi", item)                 # a process contract over IPC: no class ABI
        self.assertNotIn("license", item)             # no licence class fits it yet; not guessed
        self.assertEqual(item["requires"], [{"kind": "runtime", "version": ">=2.36.0"}])
        asset = item["assets"]["windows-x64"]
        row = "HKLM\\Software\\DisplayXR\\WorkspaceControllers\\shell\\"
        # An existing install is recognised by its registration row.
        self.assertEqual(asset["installed"], {"kind": "registry_sz", "path": row + "Version"})
        self.assertEqual(asset["uninstall"],
                         {"kind": "registry_command", "path": row + "UninstallString", "args": ["/S"]})
        # No launch-shortcut argument: an unattended install never resets the user's choice.
        self.assertEqual(asset["installer"]["args"], ["/S"])

    def test_shell_floor_comes_from_its_installer_and_is_required(self):
        comp = next(c for c in gen_catalog.V2_ONLY_COMPONENTS if c["name"] == "shell")
        self.assertEqual(comp["floor"], {"consumer_floor": "displayxr-shell-pvt"})
        saved = {n: getattr(gen_catalog, n) for n in ("repo_file", "windows_block", "warn", "info")}
        asked = []
        nsi = '; makensis -DMIN_RUNTIME_VERSION=0.0.0\n!ifndef MIN_RUNTIME_VERSION\n\t!define MIN_RUNTIME_VERSION "2.36.0"\n!endif\n'
        try:
            gen_catalog.warn = gen_catalog.info = lambda msg: None
            gen_catalog.windows_block = lambda comp, version, verifier, d, reg=None: None
            gen_catalog.repo_file = lambda repo, path, ref: asked.append((repo, path, ref)) or nsi
            out = gen_catalog.build_extra_components({"shell": "v2.4.2"}, lambda p: None, ".")
            self.assertEqual(out, {"shell": {"recommended": "2.4.2", "requires": {"runtime": ">=2.36.0"}}})
            # Read from the source repo's installer at the released tag (downstream-pins.json).
            self.assertEqual(asked, [("displayxr-shell-pvt", "installer/DisplayXRShellInstaller.nsi", "v2.4.2")])
            # Unreadable (a token without access to the private repo): not listed at
            # all, because its installer refuses a runtime below the floor.
            gen_catalog.repo_file = lambda repo, path, ref: None
            self.assertEqual(gen_catalog.build_extra_components({"shell": "v2.4.2"}, lambda p: None, "."), {})
        finally:
            for n, f in saved.items():
                setattr(gen_catalog, n, f)

    def test_metadata_is_not_aliased_into_the_document(self):
        self.items["leia-conversion"]["requires"][1]["detect"][0]["min"] = "9.9.9"
        again = gen_catalog.build_catalog_v2(v1_fixture(), extra=extra_fixture())
        self.assertEqual(again["items"][4]["requires"][1]["detect"][0]["min"], "0.4.6")

    def test_runtime_range_rules(self):
        bad = gen_catalog.build_catalog_v2(v1_fixture(), extra=extra_fixture())
        bad["items"][5]["runtime"] = {"min": "2.70.0", "max": "2.69.3"}
        problems = gen_catalog.validate_v2(bad)
        self.assertTrue(any("above runtime.max" in p for p in problems), problems)
        self.assertTrue(any("disagrees with requires" in p for p in problems), problems)

    def test_extra_is_keyword_only(self):
        # The second positional slot is `manifests` (#2740).
        with self.assertRaises(TypeError):
            gen_catalog.build_catalog_v2(v1_fixture(), None, extra_fixture())
        self.assertEqual(len(gen_catalog.build_catalog_v2(v1_fixture())["items"]), 3)

    def test_prerequisite_detect_rules_pass_the_rule_validator(self):
        # Every rule this table writes must survive validate_rule unchanged, and
        # none claims a confidence (that is for hardware evidence).
        n = 0
        for meta in gen_catalog.V2_ITEMS.values():
            for ext in meta.get("external", []):
                for rule in ext["detect"]:
                    clean, problems = gen_catalog.validate_rule(rule)
                    self.assertEqual((clean, problems), (rule, []), rule)
                    self.assertNotIn("confidence", rule)
                    n += 1
        self.assertEqual(n, 2)

    def test_asset_with_a_build_number_picks_the_highest_build(self):
        comp = next(c for c in gen_catalog.V2_ONLY_COMPONENTS if c["name"] == "ultraleap")
        assets = [{"name": n} for n in ("DisplayXRUltraleapSetup-0.1.0.3.exe", "DisplayXRUltraleapSetup-0.1.0.12.exe",
                                         "DisplayXRUltraleapSetup-0.1.00.99.exe", "DisplayXRUltraleapSetup-0.1.0.exe",
                                         "displayxr-ultraleap-0.1.0-linux-x86_64.tar.gz")]
        self.assertEqual(gen_catalog.pick_asset(comp, "0.1.0", assets)["name"], "DisplayXRUltraleapSetup-0.1.0.12.exe")
        self.assertIsNone(gen_catalog.pick_asset(comp, "0.2.0", assets))
        runtime = gen_catalog.COMPONENTS[0]
        self.assertEqual(gen_catalog.pick_asset(runtime, "2.69.2", [{"name": "DisplayXRSetup-2.69.2.exe"}])["name"],
                         "DisplayXRSetup-2.69.2.exe")

    def test_unsigned_release_is_listed_without_an_asset_and_still_validates(self):
        # ADR-064: every installable asset is signed. A provider whose release is not
        # signed yet (no `windows` block from the generator) is listed with nothing to
        # fetch; it becomes installable when a signed asset replaces the unsigned one.
        extra = extra_fixture()
        del extra["leia_conversion"]["windows"]
        del extra["ultraleap"]["windows"]
        v2 = gen_catalog.build_catalog_v2(v1_fixture(), extra=extra)
        items = {i["id"]: i for i in v2["items"]}
        self.assertNotIn("assets", items["leia-conversion"])
        self.assertNotIn("assets", items["ultraleap"])
        self.assertEqual(items["leia-conversion"]["aliases"], ["leia_conversion"])  # `dxr-get install leia_conversion`
        self.assertEqual(gen_catalog.validate_v2(v2), [])
        # And the validator refuses an installable asset that lost its signature.
        signed = gen_catalog.build_catalog_v2(v1_fixture(), extra=extra_fixture())
        del signed["items"][4]["assets"]["windows-x64"]["signature"]
        self.assertTrue(any("needs a signature" in p for p in gen_catalog.validate_v2(signed)))

    def test_unpinned_component_is_not_listed_and_does_not_touch_the_network(self):
        saved = gen_catalog.api_json
        gen_catalog.api_json = lambda path: self.fail("network access for an unpinned component: " + path)
        try:
            self.assertEqual(gen_catalog.build_extra_components({"runtime": "v2.69.2"}, lambda p: None, "."), {})
        finally:
            gen_catalog.api_json = saved


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
                {"kind": "file_exists", "path": "C:\\%WINDIR%\\a.dll"},
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

    def test_high_only_on_product_evidence(self):
        # A rule true on ordinary machines must never be an unattended install.
        for rule in (
                {"kind": "file_exists", "path": "C:\\Windows\\System32\\kernel32.dll", "confidence": "high"},
                {"kind": "service_exists", "name": "Spooler", "confidence": "high"},
                {"kind": "registry_key_exists", "key": "HKLM\\SOFTWARE\\Microsoft", "confidence": "high"},
                {"kind": "registry_sz_min", "key": "HKLM\\K", "value": "V", "min": "1", "confidence": "high"},
        ):
            r, p = gen_catalog.validate_rule(rule)
            self.assertEqual(r["confidence"], "low", rule)
            self.assertIn("downgraded to low", p[0])
        for rule in (
                {"kind": "edid", "man": 1, "prod": 2, "confidence": "high"},
                {"kind": "usb", "vid": 1, "pid_min": 2, "pid_max": 3, "confidence": "high"},
                dict(gen_catalog.SR_REGISTRY_DETECT),
        ):
            self.assertEqual(gen_catalog.validate_rule(rule), (rule, []))

    def test_unknown_keys_are_stripped(self):
        r, p = gen_catalog.validate_rule({"kind": "edid", "man": 1, "prod": 2, "confidence": "high",
                                          "url": "https://evil.example", "path": "C:\\x"})
        self.assertEqual(r, {"kind": "edid", "man": 1, "prod": 2, "confidence": "high"})
        self.assertIn("stripped", p[0])
        # A key valid for another kind is still foreign to this one.
        r, _ = gen_catalog.validate_rule({"kind": "service_exists", "name": "S", "min": 1})
        self.assertEqual(r, {"kind": "service_exists", "name": "S"})

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

    def test_floor_above_the_runtime_pin_drops_the_manifest(self):
        # versions.json pins runtime 2.69.1: a manifest asking for 9.0.0 would make
        # every ensure-for that detects the plug-in exit 40. The fallback is used.
        cat, seen = self.run_build(True, manifest=dict(MANIFEST, requires={"runtime": ">=9.0.0"}))
        leia = cat["components"]["leia_plugin"]
        self.assertEqual(leia["requires"], {"runtime": ">=2.60.0"})  # the fallback floor
        self.assertEqual(leia["detect"][-1], gen_catalog.SR_REGISTRY_DETECT)  # the fallback rules
        self.assertIsNone(seen["leia_plugin"])
        self.assertNotIn("leia_plugin", gen_catalog.MANIFESTS)
        # At the pin exactly is fine.
        cat, _ = self.run_build(True, manifest=dict(MANIFEST, requires={"runtime": ">=2.69.1"}))
        self.assertEqual(cat["components"]["leia_plugin"]["requires"], {"runtime": ">=2.69.1"})

    def test_manifest_rules_are_cleaned(self):
        rules = [{"kind": "file_exists", "path": "C:\\Windows\\System32\\kernel32.dll", "confidence": "high"},
                 {"kind": "edid", "man": 44048, "prod": 16711, "confidence": "high", "extra": 1}]
        cat, _ = self.run_build(True, manifest=dict(MANIFEST, detect=rules))
        self.assertEqual(cat["components"]["leia_plugin"]["detect"], [
            {"kind": "file_exists", "path": "C:\\Windows\\System32\\kernel32.dll", "confidence": "low"},
            {"kind": "edid", "man": 44048, "prod": 16711, "confidence": "high"}])

    def test_manifests_is_keyword_only(self):
        with self.assertRaises(TypeError):
            gen_catalog.build_catalog_v2(v1_fixture(), {})

    def test_v2_names_a_new_vendor_from_its_manifest(self):
        acme = {"name": "acme_plugin", "pin": "acme_plugin", "repo": "acme-plugin",
                "asset": "Acme-{v}.exe", "kind": "display_plugin", "args": ["/S"],
                "registry_version": "HKLM\\Software\\Acme\\Version"}
        manifest = dict(MANIFEST, id="acme-lightfield", label="Acme lightfield")
        cat, _ = self.run_build(True, list(gen_catalog.COMPONENTS) + [acme], manifest)
        v2 = gen_catalog.build_catalog_v2(cat, manifests=dict(gen_catalog.MANIFESTS))
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


class Schema2DetectValidation(unittest.TestCase):
    """validate_v2 checks item and external-requirement detect rules with validate_rule."""

    def setUp(self):
        self.v2 = gen_catalog.build_catalog_v2(v1_fixture())
        self.leia = next(i for i in self.v2["items"] if i["id"] == "leia-sr")

    def problems(self):
        return "\n".join(gen_catalog.validate_v2(self.v2))

    def test_generated_detect_is_clean(self):
        self.assertEqual(self.problems(), "")

    def test_item_rule_problems(self):
        self.leia["detect"].append({"kind": "file_exists", "path": "C:\\a.dll", "confidence": "high"})
        self.leia["detect"].append({"kind": "edid", "man": 1, "prod_min": 9, "prod_max": 2, "confidence": "high"})
        self.leia["detect"].append({"kind": "teleport"})
        text = self.problems()
        self.assertIn("detect[2]: file_exists: high confidence is not allowed", text)
        self.assertIn("detect[3]: edid: prod_min/prod_max", text)
        self.assertIn("detect[4]: unknown rule kind 'teleport'", text)

    def test_external_requirement_rules(self):
        self.leia.setdefault("requires", []).append({
            "kind": "external", "name": "Acme depth runtime", "info_url": "https://acme.example",
            "detect": [{"kind": "registry_sz_min", "key": "HKLM\\SOFTWARE\\Acme", "value": "Version",
                        "min": "3.2"}]})
        self.assertEqual(self.problems(), "")
        self.leia["requires"][-1]["detect"][0]["key"] = "HKCU\\SOFTWARE\\Acme"
        self.assertIn("requires 'Acme depth runtime' detect[0]", self.problems())

    def test_reserved_ids(self):
        self.leia["aliases"] = ["leia_plugin", "display_plugins"]
        self.assertIn("'display_plugins' is reserved", self.problems())
        self.leia["aliases"] = ["leia_plugin", "detected_items"]
        self.assertIn("'detected_items' is reserved", self.problems())


if __name__ == "__main__":
    unittest.main()

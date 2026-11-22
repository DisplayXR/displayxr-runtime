#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""
Build a schema-2 FIXTURE catalog signed with a THROWAWAY key, for validating
dxr-get on a test machine (docs/specs/runtime/dxr-get.md, "Validating on a
test machine").

Nothing here touches the production catalog key. A fixture signed by this
script is accepted only by a dxr-get that was COMPILED with the matching
public key (CMake -DDXR_GET_TEST_ROOT_KEY=<hex>), and such a build accepts
nothing else.

    make_test_catalog.py keygen --out DIR
        Writes DIR/test-root.seed (the private seed; keep it on the test box,
        delete it afterwards) and prints the public key as 64 hex digits and
        the CMake flag to build dxr-get with.

    make_test_catalog.py build --key DIR/test-root.seed --v1 URL|FILE --out DIR
        Writes DIR/stable.json + DIR/stable.json.sig.

    make_test_catalog.py build --no-sign --v1 FILE --out DIR
        The document alone (no `cryptography` needed), for a dxr-get run with
        DXR_GET_DEV=1 --no-verify-catalog (a test-root-key build of dxr-get only:
        every other build refuses the option), or to read what the fixture holds.

Why --v1. The fixture's installable assets are the REAL first-party
installers (runtime, Leia plug-in, browser), taken with their sizes, hashes
and URLs from a schema-1 catalog: the live one by default. They are really
signed, so a fetch really verifies, and no test binary has to be built, hosted
or signed. The fixture adds items around them that exercise what schema 1
cannot say. Every added id starts with `fixture.`.

The encoding and the structural check are the generator's
(gen_catalog.canonical_bytes, gen_catalog.validate_v2). The signature is made
here, not by the generator: gen_catalog refuses, on purpose, to sign with a
key that is not one of the production keys.
"""

import argparse
import base64
import copy
import hashlib
import json
import os
import sys
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_catalog  # noqa: E402

LIVE_V1 = "https://updates.displayxr.org/catalog/v1/stable.json"
PLATFORM = "windows-x64"
ARP = "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\"

# Things that exist on every Windows machine, and things that exist on none.
WINDOWS_KEY = "HKLM\\SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion"
ABSENT_KEY = "HKLM\\SOFTWARE\\DisplayXR\\Fixture\\NoSuchKey"
WINDOWS_FILE = "C:\\Windows\\System32\\kernel32.dll"
ABSENT_FILE = "C:\\DisplayXR-Fixture\\no-such-file.bin"
WINDOWS_SERVICE = "EventLog"
ABSENT_SERVICE = "DisplayXRFixtureNoSuchService"


def load_v1(src):
    if src.startswith("https://"):
        with urllib.request.urlopen(src, timeout=30) as r:
            return json.load(r)
    with open(src, encoding="utf-8") as f:
        return json.load(f)


def external(name, rules, asset=None):
    e = {"kind": "external", "name": name, "detect": rules,
         "info_url": "https://example.invalid/fixture/" + name.lower().replace(" ", "-")}
    if asset is not None:
        e["asset"] = {PLATFORM: asset}
    return e


def build_fixture(v1):
    """Schema-2 fixture from a schema-1 catalog. Pure: no network, no clock."""
    cat = gen_catalog.build_catalog_v2(v1)
    items = {i["id"]: i for i in cat["items"]}
    for need in ("runtime", "leia-sr", "browser"):
        if PLATFORM not in items.get(need, {}).get("assets", {}):
            sys.exit(f"error: the schema-1 catalog has no Windows asset for {need}; the fixture needs all three")
    first_party = cat["publishers"][gen_catalog.V2_PUBLISHER]
    real_signer = first_party["signers"][PLATFORM][0]["subject_cn"]
    rt_version = items["runtime"]["assets"][PLATFORM]["version"]
    browser_asset = items["browser"]["assets"][PLATFORM]
    leia_asset = items["leia-sr"]["assets"][PLATFORM]

    # What the runtime at the catalog's version implements, so the class ABI
    # gate has something to judge an about-to-be-installed runtime against.
    items["runtime"]["abi"] = {"display_processor": {"min": 5, "max": 5}, "input_provider": {"min": 1, "max": 1},
                               "conversion_provider": {"min": 1, "max": 1}, "tracking_provider": {"min": 1, "max": 1}}
    # `install --default`: the browser is this fixture's recommended item (the
    # Leia plug-in joins it on a Leia panel, through its detect rules).
    items["browser"]["recommended"] = True
    items["browser"]["summary"] = "Fixture: the recommended item of `install --default`."
    # The uninstall rules the first-party installers support (dxr-get.md, Receipts).
    browser_asset["uninstall"] = {"kind": "registry_command", "path": ARP + "DisplayXR Browser\\UninstallString",
                                  "args": ["/S"]}
    leia_asset["uninstall"] = {"kind": "registry_command", "path": ARP + "DisplayXRLeiaSR\\UninstallString",
                               "args": ["/S"]}

    # A second publisher, whose one delegated signer signed nothing real.
    cat["publishers"]["fixture-other"] = {
        "name": "Fixture: another publisher",
        "signers": {PLATFORM: [{"kind": "authenticode", "subject_cn": "DisplayXR Fixture Signer"}]},
    }

    def item(iid, name, summary, asset, publisher=gen_catalog.V2_PUBLISHER, **more):
        it = {"id": iid, "class": "app", "name": name, "summary": summary, "publisher": publisher,
              "version": asset["version"], "assets": {PLATFORM: copy.deepcopy(asset)}}
        it["assets"][PLATFORM].pop("uninstall", None)
        # A version value nothing writes: a fixture item always reads as "not
        # installed", so its plan is the same on every machine and every run.
        it["assets"][PLATFORM]["installed"] = {
            "kind": "registry_sz", "path": "HKLM\\Software\\DisplayXR\\Fixture\\" + iid + "\\Version"}
        it.update(more)
        return it

    # The browser's installer stands in for "an installer": the fixture items
    # are judged by `plan`, `list` and `fetch`. Installing one installs the browser.
    extra = [
        item("fixture.detectors-met", "Fixture: every external detector, met",
             "Four external prerequisites, one per detector, all present on any Windows machine.", browser_asset,
             requires=[
                 external("Fixture registry key", [{"kind": "registry_key_exists", "key": WINDOWS_KEY}]),
                 external("Fixture file", [{"kind": "file_exists", "path": WINDOWS_FILE}]),
                 external("Fixture service", [{"kind": "service_exists", "name": WINDOWS_SERVICE}]),
                 external("Fixture version value", [{"kind": "registry_sz_min", "key": WINDOWS_KEY,
                                                    "value": "CurrentBuild", "min": "1"}]),
             ]),
        item("fixture.detectors-unmet", "Fixture: every external detector, unmet",
             "Three detection-only external prerequisites that no machine has: reported, never installed.",
             browser_asset,
             requires=[
                 external("Fixture absent registry key", [{"kind": "registry_key_exists", "key": ABSENT_KEY}]),
                 external("Fixture absent file", [{"kind": "file_exists", "path": ABSENT_FILE}]),
                 external("Fixture absent service", [{"kind": "service_exists", "name": ABSENT_SERVICE}]),
             ]),
        item("fixture.runtime-too-new", "Fixture: runtime range refusal (max)",
             "Works only up to runtime 1.0.0: refused with exit 41 on any real runtime.", browser_asset,
             runtime={"max": "1.0.0"}),
        item("fixture.runtime-too-old", "Fixture: runtime range refusal (min)",
             "Needs runtime 99.0.0, which no catalog offers: refused with exit 41.", browser_asset,
             runtime={"min": "99.0.0"}),
        dict(item("fixture.abi-mismatch", "Fixture: class ABI refusal",
                  "A display processor built for ABI 99: refused with exit 41 once the runtime's ABI is known.",
                  leia_asset),
             **{"class": "display_processor", "registered_id": "fixture-abi-mismatch",
                "abi": {"display_processor": {"min": 99, "max": 99}},
                "requires": [{"kind": "runtime", "version": ">=" + rt_version}]}),
        item("fixture.multi-asset", "Fixture: an item with a prerequisite installer",
             "Two downloads: its own asset and an external prerequisite that carries an installer "
             "(the Leia plug-in's), run first when that plug-in is absent.", browser_asset,
             requires=[
                 {"kind": "runtime", "version": ">=" + rt_version},
                 external("Fixture prerequisite with an installer",
                          [{"kind": "registry_sz_min",
                            "key": "HKLM\\Software\\DisplayXR\\Plugins\\LeiaSR", "value": "Version", "min": "0.0.1"}],
                          asset={k: copy.deepcopy(v) for k, v in leia_asset.items()
                                 if k in ("version", "file", "size", "sha256", "urls", "signature", "installer")}),
             ]),
        item("fixture.wrong-signer", "Fixture: asset signed by another signer",
             "Declares a signer its publisher IS delegated, but the file is signed by someone else: "
             "downloads, then is refused with exit 21.", browser_asset, publisher="fixture-other"),
        item("fixture.undelegated-signer", "Fixture: signer not delegated to the publisher",
             "Declares the first-party signer under another publisher: refused before any download.",
             browser_asset, publisher="fixture-other"),
        {"id": "fixture.link-out", "class": "app", "name": "Fixture: a link-out item",
         "summary": "Listing only: shown with its link, never fetched.", "publisher": "fixture-other",
         "version": "1.0.0", "assets": {PLATFORM: {"listing": {"url": "https://example.invalid/fixture/link-out"}}}},
    ]
    by_id = {i["id"]: i for i in extra}
    by_id["fixture.wrong-signer"]["assets"][PLATFORM]["signature"]["signer"] = "DisplayXR Fixture Signer"
    assert by_id["fixture.undelegated-signer"]["assets"][PLATFORM]["signature"]["signer"] == real_signer
    cat["items"].extend(extra)
    cat["source"] = {"fixture": "scripts/make_test_catalog.py", "from_v1_serial": v1.get("serial")}
    return cat


def expected_problems(cat):
    """validate_v2 is right to flag the one deliberate defect; anything else is a real problem."""
    ok = "fixture.undelegated-signer"
    return [p for p in gen_catalog.validate_v2(cat) if ok not in p]


def cmd_keygen(args):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, "test-root.seed")
    if os.path.exists(path):
        sys.exit(f"error: {path} exists; delete it to make a new key")
    priv = Ed25519PrivateKey.generate()
    seed = priv.private_bytes(serialization.Encoding.Raw, serialization.PrivateFormat.Raw,
                              serialization.NoEncryption())
    pub = priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(base64.b64encode(seed) + b"\n")
    print(f"wrote {path} (a THROWAWAY private seed: keep it on the test machine, delete it afterwards)")
    print(f"public key: {pub.hex()}")
    print(f"build dxr-get with: -DDXR_GET_TEST_ROOT_KEY={pub.hex()}")
    return 0


def cmd_build(args):
    v1 = load_v1(args.v1)
    if v1.get("schema") != 1:
        sys.exit("error: --v1 must be a schema-1 catalog")
    cat = build_fixture(v1)
    problems = expected_problems(cat)
    if problems:
        sys.exit("error: the fixture is not a valid schema-2 catalog:\n  " + "\n  ".join(problems))
    data = gen_catalog.canonical_bytes(cat)
    os.makedirs(args.out, exist_ok=True)
    out_json = os.path.join(args.out, "stable.json")
    with open(out_json, "wb") as f:
        f.write(data)
    print(f"wrote {out_json} (serial {cat['serial']}, expires {cat['expires']}, {len(cat['items'])} items)")
    sig_path = out_json + ".sig"
    if args.no_sign:
        if os.path.exists(sig_path):
            os.remove(sig_path)
        print("--no-sign: no signature written")
        return 0
    if not args.key:
        sys.exit("error: --key FILE (from `keygen`) or --no-sign")
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    with open(args.key, "rb") as f:
        seed = base64.b64decode(f.read().strip())
    if len(seed) != 32:
        sys.exit("error: --key must hold base64 of a 32-byte seed")
    priv = Ed25519PrivateKey.from_private_bytes(seed)
    pub = priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    if hashlib.sha256(pub).hexdigest()[:8] in gen_catalog.PUBLIC_KEYS:
        sys.exit("error: that is a PRODUCTION catalog key. Test content is never signed with it.")
    with open(sig_path, "wb") as f:
        f.write(base64.b64encode(priv.sign(data)) + b"\n")
    print(f"wrote {sig_path}, signed with test key {pub.hex()}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen", help="make a throwaway Ed25519 key pair")
    k.add_argument("--out", required=True, metavar="DIR")
    k.set_defaults(func=cmd_keygen)
    b = sub.add_parser("build", help="write and sign the fixture catalog")
    b.add_argument("--v1", default=LIVE_V1, metavar="URL|FILE",
                   help="schema-1 catalog the real assets come from (default: the live one)")
    b.add_argument("--key", metavar="FILE", help="test-root.seed from `keygen`")
    b.add_argument("--no-sign", action="store_true")
    b.add_argument("--out", required=True, metavar="DIR")
    b.set_defaults(func=cmd_build)
    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

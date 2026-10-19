#!/usr/bin/env python3
"""Check the runtime installer's default conversion model pack resolves (#2234).

The Windows runtime installer pre-fetches the open conversion provider's default
pack. When the provider is not installed yet it fetches the pack straight from
the provider's repository, at the tag versions.json pins:

    https://raw.githubusercontent.com/DisplayXR/displayxr-conversion-open/
        <versions.json conversion_open>/packs/dxr-open-default.models.json

The tag is injected at build time (installer/CMakeLists.txt passes
/DDXR_CONVERSION_OPEN_TAG to makensis), and the URL template lives in
installer/DisplayXRInstaller.nsi. Before #2234 the default named a
displayxr-models "packs" release that never existed, so every install
pre-fetched nothing and said so only in install.log.

Two modes:

  (default)   offline: the .nsi derives its fallback URL from
              DXR_CONVERSION_OPEN_TAG and CMake injects that tag from
              versions.json. No network, so it is safe in lint.yml.
  --network   also GET the URL derived from versions.json and require HTTP 200
              and a schema-2 pack manifest named dxr-open-default. Run weekly by
              pin-rot-canary.yml: the pin moves by a publish-bot commit with no
              PR, and a file renamed in a repo this one does not own should not
              turn every PR red.

The .nsi deliberately keeps a literal default tag for a makensis run outside
CMake; it is NOT asserted equal to versions.json, because versions.json is
auto-bumped on every provider release and the literal is not.
"""

import argparse
import json
import re
import sys
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
NSI = ROOT / "installer" / "DisplayXRInstaller.nsi"
CMAKE = ROOT / "installer" / "CMakeLists.txt"
VERSIONS = ROOT / "versions.json"
PACK = "dxr-open-default"


def nsi_url_template(text):
    """The fallback DXR_MODELS_PACK_URL !define (the one that uses the tag)."""
    for m in re.finditer(r'^\s*!define\s+DXR_MODELS_PACK_URL\s+"([^"]+)"', text, re.M):
        if "${DXR_CONVERSION_OPEN_TAG}" in m.group(1):
            return m.group(1)
    return None


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--network", action="store_true", help="also GET the derived URL")
    args = ap.parse_args()

    errors = []
    tag = json.loads(VERSIONS.read_text()).get("conversion_open")
    if not tag:
        errors.append("versions.json has no conversion_open pin")
    nsi = NSI.read_text(encoding="utf-8", errors="replace")
    template = nsi_url_template(nsi)
    if template is None:
        errors.append(f"{NSI.relative_to(ROOT)}: no DXR_MODELS_PACK_URL default built from ${{DXR_CONVERSION_OPEN_TAG}}")
    elif not template.endswith(f"/packs/{PACK}.models.json"):
        errors.append(f"{NSI.relative_to(ROOT)}: default pack URL does not name packs/{PACK}.models.json: {template}")
    if "/DDXR_CONVERSION_OPEN_TAG=" not in CMAKE.read_text():
        errors.append(f"{CMAKE.relative_to(ROOT)}: makensis is not passed /DDXR_CONVERSION_OPEN_TAG")
    if errors:
        for e in errors:
            print(f"::error::{e}")
        return 1

    url = template.replace("${DXR_CONVERSION_OPEN_TAG}", tag)
    print(f"default pack URL (conversion_open {tag}): {url}")
    if not args.network:
        return 0

    try:
        req = urllib.request.Request(url, headers={"User-Agent": "displayxr-runtime-ci"})
        with urllib.request.urlopen(req, timeout=30) as r:
            status, body = r.status, r.read()
    except Exception as e:  # HTTPError included: a 404 lands here
        print(f"::error::default model pack URL does not resolve: {url} ({e})")
        return 1
    try:
        pack = json.loads(body)
    except ValueError as e:
        print(f"::error::{url} returned HTTP {status} but not JSON ({e})")
        return 1
    if status != 200 or pack.get("schema") != 2 or pack.get("pack") != PACK:
        print(f"::error::{url}: HTTP {status}, schema={pack.get('schema')!r}, pack={pack.get('pack')!r}; "
              f"expected 200, 2, {PACK!r}")
        return 1
    print(f"OK: HTTP {status}, schema 2 pack '{PACK}', {len(pack.get('models', []))} models")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0
"""Generate, sign and verify the DisplayXR component catalog (#2703, epic #2702).

The catalog is the signed index `dxr-get` reads to fetch DisplayXR components
(runtime, Leia SR plug-in, browser) on demand. It is published to
https://updates.displayxr.org/catalog/v1/stable.json (+ .sig) by
.github/workflows/publish-catalog.yml. Spec: docs/specs/runtime/component-catalog.md.

    gen_catalog.py generate --out DIR [--no-sign] [--versions versions.json]
                            [--download-dir DIR] [--verifier powershell|none]
                            [--previous URL|FILE]
    gen_catalog.py verify   DIR/stable.json [--sig DIR/stable.json.sig]

Signing key: env DXR_CATALOG_ED25519_KEY = base64 of the 32-byte Ed25519 seed.
The matching public key is hardcoded below (PUBLIC_KEYS) and compiled into dxr-get.

GitHub access: env GH_TOKEN / GITHUB_TOKEN, else `gh auth token`. Reading the
browser's declared runtime floor needs read access to DisplayXR/displayxr-browser-pvt;
without it the browser's `requires` is omitted (with a warning), never guessed.

Stdlib only, plus `cryptography` for Ed25519 (sign and verify).
"""

import argparse
import base64
import datetime
import hashlib
import json
import os
import re
import subprocess
import sys
import tempfile
import urllib.error
import urllib.request

ORG = "DisplayXR"
API = "https://api.github.com"

# Authenticode signer CNs a listed asset may carry. dxr-get compiles in the same set.
SIGNERS_WINDOWS = ["Leia, Inc."]

# Ed25519 public keys trusted for the catalog, base64 of the 32 raw bytes.
# Key id = first 8 hex of sha256(raw pubkey). Rotation: ADD a key here and in
# dxr-get, ship that dxr-get, then switch the signing secret (spec §Rotation).
PUBLIC_KEYS = {
    "9b762f9a": "dPxNHS5QLtsrezVWd0ZeOYcNVVFKwuoTu8bzUMEu9DQ=",
}

CATALOG_URL = "https://updates.displayxr.org/catalog/v1/stable.json"
EXPIRY_DAYS = 30

# Per-component publishing facts. `pin` is the versions.json field.
COMPONENTS = [
    {
        "name": "runtime",
        "pin": "runtime",
        "repo": "displayxr-runtime",
        "asset": "DisplayXRSetup-{v}.exe",
        "args": ["/S", "/NOSTART"],
        "registry_version": "HKLM\\Software\\DisplayXR\\Runtime\\Version",
    },
    {
        "name": "leia_plugin",
        "pin": "leia_plugin",
        "repo": "displayxr-leia-plugin",
        "asset": "DisplayXRLeiaSRSetup-{v}.exe",
        "kind": "display_plugin",
        "args": ["/S"],
        "registry_version": "HKLM\\Software\\DisplayXR\\Plugins\\LeiaSR\\Version",
    },
    {
        "name": "browser",
        "pin": "browser",
        "repo": "displayxr-browser",
        "asset": "DisplayXR-Browser-Setup-{v}.exe",
        "args": ["/S"],
        "registry_version": "HKLM\\Software\\DisplayXR\\Browser\\Version",
    },
]

# Fixed P2 detector (contract §6): the SR platform reports a connected SR device.
SR_REGISTRY_DETECT = {
    "kind": "registry_dword_min",
    "key": "HKLM\\SOFTWARE\\Dimenco\\Simulated Reality",
    "value": "NConnectedDevices",
    "min": 1,
    "confidence": "high",
    "note": "SR platform reports a connected SR device",
}

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def warn(msg):
    print(f"WARNING: {msg}", file=sys.stderr)
    if os.environ.get("GITHUB_ACTIONS") == "true":
        print(f"::warning::{msg}")


def info(msg):
    print(msg, file=sys.stderr)


# --------------------------------------------------------------------------- GitHub


_TOKEN = None


def gh_token():
    global _TOKEN
    if _TOKEN is None:
        _TOKEN = os.environ.get("GH_TOKEN") or os.environ.get("GITHUB_TOKEN") or ""
        if not _TOKEN:
            try:
                _TOKEN = subprocess.run(["gh", "auth", "token"], capture_output=True, text=True,
                                        check=True).stdout.strip()
            except (OSError, subprocess.CalledProcessError):
                _TOKEN = ""
    return _TOKEN


def api_get(path, accept="application/vnd.github+json"):
    headers = {"Accept": accept, "X-GitHub-Api-Version": "2022-11-28",
               "User-Agent": "displayxr-gen-catalog"}
    if gh_token():
        headers["Authorization"] = f"Bearer {gh_token()}"
    req = urllib.request.Request(API + path, headers=headers)
    with urllib.request.urlopen(req, timeout=60) as r:
        return r.read()


def api_json(path):
    return json.loads(api_get(path))


def repo_file(repo, path, ref):
    """Raw file contents of <ORG>/<repo>:<path> at <ref>, or None if unreadable."""
    try:
        return api_get(f"/repos/{ORG}/{repo}/contents/{path}?ref={ref}",
                       accept="application/vnd.github.raw").decode("utf-8")
    except urllib.error.HTTPError as e:
        warn(f"cannot read {ORG}/{repo}:{path}@{ref} (HTTP {e.code})")
        return None


def download(url, dest):
    """Stream url to dest; return (size, sha256)."""
    h = hashlib.sha256()
    size = 0
    req = urllib.request.Request(url, headers={"User-Agent": "displayxr-gen-catalog"})
    tmp = dest + ".part"
    with urllib.request.urlopen(req, timeout=300) as r, open(tmp, "wb") as f:
        while True:
            chunk = r.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
            f.write(chunk)
            size += len(chunk)
    os.replace(tmp, dest)
    return size, h.hexdigest()


def hash_file(path):
    h = hashlib.sha256()
    size = 0
    with open(path, "rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
            size += len(chunk)
    return size, h.hexdigest()


# --------------------------------------------------------------------------- Authenticode


def signer_powershell(path):
    """Return the leaf signer CN if the Authenticode signature is Valid, else None."""
    ps = (
        "$ErrorActionPreference='Stop';"
        "$s=Get-AuthenticodeSignature -LiteralPath $env:DXR_CAT_FILE;"
        "if($s.Status -ne 'Valid' -or -not $s.SignerCertificate){Write-Output ('STATUS:'+$s.Status);exit 0};"
        "Write-Output ('CN:'+$s.SignerCertificate.GetNameInfo("
        "[System.Security.Cryptography.X509Certificates.X509NameType]::SimpleName,$false))"
    )
    env = dict(os.environ, DXR_CAT_FILE=path)
    for exe in ("powershell", "pwsh"):
        try:
            out = subprocess.run([exe, "-NoProfile", "-NonInteractive", "-Command", ps],
                                 capture_output=True, text=True, env=env, check=True).stdout
        except (OSError, subprocess.CalledProcessError):
            continue
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("CN:"):
                return line[3:]
            if line.startswith("STATUS:"):
                warn(f"{os.path.basename(path)}: Authenticode status {line[7:]}")
                return None
    warn(f"{os.path.basename(path)}: no PowerShell available to check Authenticode")
    return None


VERIFIERS = {
    "powershell": signer_powershell,
    # Dry runs on a box without PowerShell: nothing verifies, so nothing is listed.
    "none": lambda path: None,
}


# --------------------------------------------------------------------------- floors / detect


SEMVER_RE = re.compile(r"^v?(\d+\.\d+\.\d+)$")


def browser_floor(tag):
    """Browser floor = its installer's declared MIN_RUNTIME_VERSION at the pinned tag.

    Where to look comes from downstream-pins.json consumer_floors (declared_floor),
    so this follows the same source the drift audit reads.
    """
    with open(os.path.join(REPO_ROOT, "downstream-pins.json"), encoding="utf-8") as f:
        pins = json.load(f)
    entry = pins.get("consumer_floors", {}).get("displayxr-browser-pvt", {})
    decl = entry.get("declared_floor") or {}
    if decl.get("kind") != "nsis_define" or not decl.get("file") or not decl.get("key"):
        warn("downstream-pins.json gives no usable browser declared_floor; omitting requires")
        return None
    text = repo_file("displayxr-browser-pvt", decl["file"], tag)
    if text is None:
        return None
    m = re.search(r'!define\s+' + re.escape(decl["key"]) + r'\s+"([0-9]+\.[0-9]+\.[0-9]+)"', text)
    if not m:
        warn(f"no {decl['key']} define in browser-pvt {decl['file']}@{tag}; omitting requires")
        return None
    return m.group(1)


def leia_floor(tag):
    """Leia floor = its installer's MIN_RUNTIME_VERSION, which installer/CMakeLists.txt
    derives from the DXR_RUNTIME_GIT_TAG pin in the top-level CMakeLists.txt."""
    inst = repo_file("displayxr-leia-plugin", "installer/CMakeLists.txt", tag)
    if inst is None or not re.search(r'set\(MIN_RUNTIME_VERSION\s+"\$\{CMAKE_MATCH_1\}"\)', inst) \
            or "DXR_RUNTIME_GIT_TAG" not in inst:
        warn(f"leia-plugin@{tag}: MIN_RUNTIME_VERSION derivation not recognised; omitting requires")
        return None
    top = repo_file("displayxr-leia-plugin", "CMakeLists.txt", tag)
    if top is None:
        return None
    m = re.search(r'set\(DXR_RUNTIME_GIT_TAG\s+"([^"]+)"', top)
    if not m or not SEMVER_RE.match(m.group(1)):
        warn(f"leia-plugin@{tag}: DXR_RUNTIME_GIT_TAG is not a vX.Y.Z tag; omitting requires")
        return None
    return SEMVER_RE.match(m.group(1)).group(1)


EDID_ROW_RE = re.compile(r"^\s*\{\s*(\d+)\s*,\s*(\d+)\s*\}\s*,?\s*(?://\s*(.*))?$")


def leia_detect(tag):
    """EDID detectors from the plug-in's own frozen table, plus the SR registry rule."""
    text = repo_file("displayxr-leia-plugin", "src/drv_leia/leia_edid_table.h", tag)
    rules = []
    if text is None:
        warn("leia EDID table unreadable; detect carries the registry rule only")
    else:
        body = text.split("leia_edid_table[][2] = {", 1)
        if len(body) != 2:
            warn("leia EDID table: array not found; detect carries the registry rule only")
        else:
            note = ""
            for line in body[1].split("};", 1)[0].splitlines():
                m = EDID_ROW_RE.match(line)
                if not m:
                    continue
                man, prod = int(m.group(1)), int(m.group(2))
                # A row without its own comment continues the previous row's range
                # (e.g. the Samsung product-id runs), so it inherits that note.
                if m.group(3):
                    note = m.group(3).strip()
                rules.append({
                    "kind": "edid",
                    "man": man,
                    "prod": prod,
                    "confidence": "low" if prod < 16 else "high",
                    "note": note,
                })
            if not rules:
                warn("leia EDID table parsed to zero rows")
    rules.append(dict(SR_REGISTRY_DETECT))
    return rules


# --------------------------------------------------------------------------- generate


def windows_block(comp, version, verifier, download_dir):
    repo, asset_name = comp["repo"], comp["asset"].format(v=version)
    tag = "v" + version
    try:
        rel = api_json(f"/repos/{ORG}/{repo}/releases/tags/{tag}")
    except urllib.error.HTTPError as e:
        warn(f"{comp['name']}: release {repo}@{tag} not found (HTTP {e.code}); no windows block")
        return None
    asset = next((a for a in rel.get("assets", []) if a.get("name") == asset_name), None)
    if asset is None:
        warn(f"{comp['name']}: {repo}@{tag} has no asset {asset_name}; no windows block")
        return None

    dest = os.path.join(download_dir, asset_name)
    if os.path.exists(dest) and os.path.getsize(dest) == asset["size"]:
        info(f"{comp['name']}: reusing {dest}")
        size, sha = hash_file(dest)
    else:
        info(f"{comp['name']}: downloading {asset['browser_download_url']}")
        size, sha = download(asset["browser_download_url"], dest)
    if size != asset["size"]:
        warn(f"{comp['name']}: downloaded {size} bytes, release says {asset['size']}; no windows block")
        return None
    if asset.get("digest") and asset["digest"] != "sha256:" + sha:
        warn(f"{comp['name']}: sha256 {sha} != release digest {asset['digest']}; no windows block")
        return None

    signer = verifier(dest)
    if signer not in SIGNERS_WINDOWS:
        warn(f"{comp['name']}: {asset_name} signer {signer!r} not in {SIGNERS_WINDOWS}; "
             "no windows block (unsigned / unverifiable assets are never listed)")
        return None
    info(f"{comp['name']}: {asset_name} {size} bytes sha256={sha} signer={signer!r}")

    return {
        "version": version,
        "file": asset_name,
        "size": size,
        "sha256": sha,
        "signer": signer,
        "urls": [
            asset["browser_download_url"],
            f"{API}/repos/{ORG}/{repo}/releases/assets/{asset['id']}",
        ],
        "args": list(comp["args"]),
        "registry_version": comp["registry_version"],
    }


def git_head():
    try:
        return subprocess.run(["git", "-C", REPO_ROOT, "rev-parse", "HEAD"], capture_output=True,
                              text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def iso_z(dt):
    return dt.strftime("%Y-%m-%dT%H:%M:%SZ")


def load_previous(src):
    """The currently published catalog, trusted only if its signature verifies."""
    try:
        if re.match(r"^https?://", src):
            req = lambda u: urllib.request.Request(u, headers={"User-Agent": "displayxr-gen-catalog"})
            with urllib.request.urlopen(req(src), timeout=60) as r:
                data = r.read()
            with urllib.request.urlopen(req(src + ".sig"), timeout=60) as r:
                sig_text = r.read()
        else:
            with open(src, "rb") as f:
                data = f.read()
            with open(src + ".sig", "rb") as f:
                sig_text = f.read()
    except (OSError, urllib.error.URLError) as e:
        warn(f"previous catalog {src} unavailable ({e}); no carry-forward")
        return None
    try:
        sig = base64.b64decode(sig_text.strip(), validate=True)
    except ValueError:
        sig = b""
    if not verify_bytes(data, sig):
        warn(f"previous catalog {src} does not verify; ignoring it")
        return None
    return json.loads(data)


def build_catalog(versions, verifier, download_dir, previous=None):
    now = datetime.datetime.now(datetime.timezone.utc).replace(microsecond=0)
    components = {}
    for comp in COMPONENTS:
        pin = versions.get(comp["pin"])
        m = SEMVER_RE.match(pin or "")
        if not m:
            warn(f"versions.json[{comp['pin']}] = {pin!r} is not vX.Y.Z; component skipped")
            continue
        version = m.group(1)
        tag = "v" + version
        entry = {"recommended": version}
        if "kind" in comp:
            entry["kind"] = comp["kind"]

        floor = None
        if comp["name"] == "leia_plugin":
            floor = leia_floor(tag)
        elif comp["name"] == "browser":
            floor = browser_floor(tag)
        if floor:
            entry["requires"] = {"runtime": ">=" + floor}

        win = windows_block(comp, version, verifier, download_dir)
        if not win and previous:
            # The new pin is not listable (typically: a release whose signed asset
            # has not replaced the unsigned CI one yet). Keep serving the last
            # verified, signed installer rather than making the component
            # unfetchable; windows.version says which one it is.
            old = previous.get("components", {}).get(comp["name"], {}).get("windows")
            if old and old.get("signer") in SIGNERS_WINDOWS:
                warn(f"{comp['name']}: carrying forward the previously listed {old.get('file')}")
                win = old
        if win:
            entry["windows"] = win
        if comp["name"] == "leia_plugin":
            entry["detect"] = leia_detect(tag)
        components[comp["name"]] = entry

    return {
        "schema": 1,
        "serial": int(now.timestamp()),
        "generated": iso_z(now),
        "expires": iso_z(now + datetime.timedelta(days=EXPIRY_DAYS)),
        "channel": "stable",
        "source": {"versions_json_sha": git_head()},
        "signers": {"windows": list(SIGNERS_WINDOWS)},
        "components": components,
    }


def canonical_bytes(catalog):
    return (json.dumps(catalog, indent=1, sort_keys=False, ensure_ascii=False) + "\n").encode("utf-8")


# --------------------------------------------------------------------------- sign / verify


def key_id(raw_pub):
    return hashlib.sha256(raw_pub).hexdigest()[:8]


def sign_bytes(data):
    from cryptography.hazmat.primitives import serialization
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey

    b64 = os.environ.get("DXR_CATALOG_ED25519_KEY", "").strip()
    if not b64:
        sys.exit("error: DXR_CATALOG_ED25519_KEY is not set (use --no-sign for a dry run)")
    seed = base64.b64decode(b64)
    if len(seed) != 32:
        sys.exit("error: DXR_CATALOG_ED25519_KEY must be base64 of a 32-byte seed")
    priv = Ed25519PrivateKey.from_private_bytes(seed)
    pub = priv.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)
    kid = key_id(pub)
    if kid not in PUBLIC_KEYS or base64.b64decode(PUBLIC_KEYS[kid]) != pub:
        sys.exit(f"error: signing key (id {kid}) is not one of the trusted PUBLIC_KEYS {sorted(PUBLIC_KEYS)}")
    info(f"signing with key id {kid}")
    return priv.sign(data)


def verify_bytes(data, sig):
    from cryptography.exceptions import InvalidSignature
    from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PublicKey

    for kid, b64 in PUBLIC_KEYS.items():
        raw = base64.b64decode(b64)
        assert key_id(raw) == kid, f"PUBLIC_KEYS id {kid} does not match its key"
        try:
            Ed25519PublicKey.from_public_bytes(raw).verify(sig, data)
            return kid
        except InvalidSignature:
            continue
    return None


def cmd_generate(args):
    with open(args.versions, encoding="utf-8") as f:
        versions = json.load(f)
    download_dir = args.download_dir or tempfile.mkdtemp(prefix="dxr-catalog-")
    os.makedirs(download_dir, exist_ok=True)
    previous = load_previous(args.previous) if args.previous else None
    catalog = build_catalog(versions, VERIFIERS[args.verifier], download_dir, previous)
    data = canonical_bytes(catalog)

    os.makedirs(args.out, exist_ok=True)
    out_json = os.path.join(args.out, "stable.json")
    with open(out_json, "wb") as f:
        f.write(data)
    info(f"wrote {out_json} ({len(data)} bytes, serial {catalog['serial']})")

    listed = [n for n, c in catalog["components"].items() if "windows" in c]
    info(f"windows blocks: {', '.join(listed) or 'none'}")

    sig_path = out_json + ".sig"
    if args.no_sign:
        if os.path.exists(sig_path):
            os.remove(sig_path)  # never leave a signature that no longer matches
        info("--no-sign: no signature written")
        return 0
    sig = sign_bytes(data)
    with open(sig_path, "wb") as f:
        f.write(base64.b64encode(sig) + b"\n")
    info(f"wrote {sig_path}")
    return 0


def cmd_verify(args):
    with open(args.catalog, "rb") as f:
        data = f.read()
    sig_path = args.sig or args.catalog + ".sig"
    with open(sig_path, "rb") as f:
        sig_text = f.read()
    if not sig_text.endswith(b"\n") or len(sig_text) != 89:
        print(f"FAIL: {sig_path} is not 88 base64 chars + LF", file=sys.stderr)
        return 1
    sig = base64.b64decode(sig_text.strip(), validate=True)
    kid = verify_bytes(data, sig)
    if not kid:
        print("FAIL: signature does not verify under any trusted key", file=sys.stderr)
        return 1

    if data.startswith(b"\xef\xbb\xbf") or b"\r\n" in data:
        print("FAIL: catalog has a BOM or CRLF line endings", file=sys.stderr)
        return 1
    cat = json.loads(data)
    problems = []
    if cat.get("schema") != 1:
        problems.append(f"schema {cat.get('schema')} != 1")
    exp = datetime.datetime.strptime(cat.get("expires", ""), "%Y-%m-%dT%H:%M:%SZ").replace(
        tzinfo=datetime.timezone.utc)
    if exp <= datetime.datetime.now(datetime.timezone.utc):
        problems.append(f"expired at {cat['expires']}")
    for name, comp in cat.get("components", {}).items():
        w = comp.get("windows")
        if not w:
            continue
        if w.get("signer") not in cat.get("signers", {}).get("windows", []):
            problems.append(f"{name}: signer {w.get('signer')!r} not in signers.windows")
        if not re.fullmatch(r"[0-9a-f]{64}", w.get("sha256", "")):
            problems.append(f"{name}: sha256 is not 64 lowercase hex")
    if problems:
        for p in problems:
            print(f"FAIL: {p}", file=sys.stderr)
        return 1
    print(f"OK: signature valid (key id {kid}), serial {cat['serial']}, expires {cat['expires']}")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("generate", help="build stable.json (+ .sig) from versions.json")
    g.add_argument("--out", required=True, help="output directory (writes stable.json, stable.json.sig)")
    g.add_argument("--versions", default=os.path.join(REPO_ROOT, "versions.json"))
    g.add_argument("--download-dir", help="where to keep downloaded installers (reused if same size)")
    g.add_argument("--no-sign", action="store_true", help="dry run: write stable.json only")
    g.add_argument("--previous", metavar="URL|FILE",
                   help=f"published catalog (e.g. {CATALOG_URL}); a component whose new pin is not "
                        "listable keeps that catalog's signed windows block")
    g.add_argument("--verifier", choices=sorted(VERIFIERS), default="powershell",
                   help="Authenticode check (default powershell; 'none' lists no windows blocks)")
    g.set_defaults(func=cmd_generate)

    v = sub.add_parser("verify", help="check a catalog's signature and basic invariants")
    v.add_argument("catalog", help="path to stable.json")
    v.add_argument("--sig", help="signature path (default <catalog>.sig)")
    v.set_defaults(func=cmd_verify)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

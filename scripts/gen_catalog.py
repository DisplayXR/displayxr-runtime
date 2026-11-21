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
                            [--previous URL|FILE] [--out-v2 DIR2]
    gen_catalog.py verify   DIR/stable.json [--sig DIR/stable.json.sig]
    gen_catalog.py validate-detect displayxr-detect.json

Display plug-ins (kind "display_plugin") describe themselves (#2737): each release
may attach a `displayxr-detect.json` (schema 1: id, label, registry_version,
requires.runtime, detect rules), which is validated and used for that component's
detect rules, runtime floor and registry key. A release without one falls back to
the old per-vendor parsing (Leia: its EDID header and installer CMake), so the
schema-1 bytes for today's releases are unchanged.

`--out-v2` additionally writes the schema-2 document (ADR-064, spec §Schema 2). The
three schema-1 components are derived from the schema-1 catalog of the same run,
which is written unchanged; the components in V2_ONLY_COMPONENTS (providers that
schema 1 cannot describe: an external prerequisite, a maximum runtime) are added
from their own releases. Nothing publishes it yet.

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

# Components listed in schema 2 ONLY. Schema 1 is frozen at the three above:
# deployed dxr-get builds know those names and ignore any other, and schema 1
# cannot say what these need said (an external prerequisite, a maximum runtime).
# A schema-1 client would read the Ultraleap provider's floor as "any newer
# runtime", which is exactly what its exact pin forbids (ADR-063).
#
# Same keys as COMPONENTS, plus:
#   asset_re       regex over the release's asset names, for an asset whose name
#                  carries more than the version ({v} is the escaped version)
#   floor          where the provider declares its minimum runtime, read at the
#                  released tag: {"file", "cmake_set": <variable>}
#   runtime_exact  where the provider pins the ONE runtime it was built at, read
#                  at the released tag: the item then carries runtime.min == max
# Nothing here is a version: every number is read from the release.
V2_ONLY_COMPONENTS = [
    {
        "name": "conversion_open",
        "pin": "conversion_open",
        "repo": "displayxr-conversion-open",
        "asset": "DisplayXROpenConversionSetup-{v}.exe",
        "args": ["/S"],
        "registry_version": "HKLM\\Software\\DisplayXR\\Plugins\\OpenConversion\\Version",
    },
    {
        "name": "leia_conversion",
        "pin": "leia_conversion",
        "repo": "displayxr-leia-conversion",
        "asset": "DisplayXRLeiaConversionSetup-{v}.exe",
        "args": ["/S"],
        "registry_version": "HKLM\\Software\\DisplayXR\\Plugins\\LeiaConversion\\Version",
        "floor": {"file": "CMakeLists.txt", "cmake_set": "DXR_CONV_MIN_RUNTIME_VERSION"},
    },
    {
        "name": "ultraleap",
        "pin": "ultraleap",
        "repo": "displayxr-ultraleap",
        # DisplayXRUltraleapSetup-<version>.<build>.exe
        "asset_re": r"DisplayXRUltraleapSetup-{v}\.(\d+)\.exe",
        "args": ["/S"],
        "registry_version": "HKLM\\Software\\DisplayXR\\Plugins\\Ultraleap\\Version",
        "runtime_exact": {"file": "CMakeLists.txt", "cmake_set": "DXR_RUNTIME_GIT_TAG"},
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


# --------------------------------------------------------------------------- detect manifest (#2737)

DETECT_MANIFEST = "displayxr-detect.json"

# Kinds the generator passes through, and the keys each may carry (anything else is
# stripped, so a vendor manifest cannot smuggle keys into the signed catalog). The
# first three are display-plug-in evidence; the rest are schema-2 prerequisite
# detectors (component-catalog.md). validate_rule() serves both: build_catalog()
# cleans manifest rules with it, and validate_v2() checks every item's `detect` and
# every external requirement's `detect` with it.
_COMMON_RULE_KEYS = ("kind", "confidence", "note")
RULE_KEYS = {
    "edid": ("man", "prod", "prod_min", "prod_max"),
    "usb": ("vid", "pid", "pid_min", "pid_max"),
    "registry_dword_min": ("key", "value", "min"),
    "registry_sz_min": ("key", "value", "min"),
    "registry_key_exists": ("key",),
    "file_exists": ("path",),
    "service_exists": ("name",),
}
RULE_KINDS = tuple(RULE_KEYS)
# `high` is an unattended install. Only evidence that names THE product may claim it:
# a product-specific edid / usb rule, or a device count a vendor's own platform
# reports. Anything that is true on ordinary machines (a file, a service, a
# registry key) could install a plug-in everywhere, so it is downgraded to `low`.
HIGH_OK_KINDS = ("edid", "usb", "registry_dword_min")
FLOOR_RE = re.compile(r"^>=\s*(\d+\.\d+\.\d+)$")
HKLM_RE = re.compile(r"^(HKLM|HKEY_LOCAL_MACHINE)\\.+", re.I)

# Filled by build_catalog() for the schema-2 derivation of the same run: component
# name -> validated manifest. Never serialised into schema 1.
MANIFESTS = {}


def _u16(v):
    return isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= 0xFFFF


def _str(v):
    return isinstance(v, str) and v != ""


def validate_rule(rule):
    """Return (clean_rule or None, [problems]).

    Unknown kinds are dropped (dxr-get ignores them anyway; this generator ships with
    the dxr-get that would read a new one), and so are keys a kind does not define.
    `high` survives only on HIGH_OK_KINDS, and on `edid` / `usb` only with a product
    id: a manufacturer- or vendor-only rule is downgraded to low, as dxr-get also
    enforces. A clean rule comes back unchanged, so `clean == rule` means "already
    clean"."""
    if not isinstance(rule, dict):
        return None, ["rule is not an object"]
    kind = rule.get("kind")
    if kind not in RULE_KINDS:
        return None, [f"unknown rule kind {kind!r} (dropped)"]
    probs = []
    allowed = _COMMON_RULE_KEYS + RULE_KEYS[kind]
    extra = [k for k in rule if k not in allowed]
    if extra:
        probs.append(f"{kind}: unknown key(s) {', '.join(map(repr, extra))} stripped")
    out = {k: v for k, v in rule.items() if k in allowed}
    conf = rule.get("confidence")
    if conf == "high" and kind not in HIGH_OK_KINDS:
        probs.append(f"{kind}: high confidence is not allowed for this kind; downgraded to low")
        out["confidence"] = "low"
    if kind in ("edid", "usb", "registry_dword_min") or conf is not None:
        if conf not in ("high", "low"):
            return None, [f"{kind}: confidence {conf!r} is not high|low"]
    if "note" in rule and not isinstance(rule["note"], str):
        return None, [f"{kind}: note is not a string"]
    if kind in ("edid", "usb"):
        vk, ik = ("man", "prod") if kind == "edid" else ("vid", "pid")
        if not _u16(rule.get(vk)):
            return None, [f"{kind}: {vk} must be an integer 0..65535"]
        has_exact = ik in rule
        has_range = f"{ik}_min" in rule or f"{ik}_max" in rule
        if has_exact and has_range:
            return None, [f"{kind}: give {ik} or {ik}_min/{ik}_max, not both"]
        if has_exact and not _u16(rule[ik]):
            return None, [f"{kind}: {ik} must be an integer 0..65535"]
        if has_range:
            lo, hi = rule.get(f"{ik}_min"), rule.get(f"{ik}_max")
            if not (_u16(lo) and _u16(hi) and lo <= hi):
                return None, [f"{kind}: {ik}_min/{ik}_max must both be integers 0..65535, min <= max"]
        if not has_exact and not has_range and conf == "high":
            # A manufacturer / vendor id alone also matches that vendor's ordinary
            # 2D monitors and devices: never an unattended install.
            probs.append(f"{kind}: {vk}-only rule downgraded to low confidence")
            out["confidence"] = "low"
    elif kind in ("registry_dword_min", "registry_sz_min"):
        if not (_str(rule.get("key")) and HKLM_RE.match(rule["key"])):
            return None, [f"{kind}: key must be an HKLM\\ path"]
        if not _str(rule.get("value")):
            return None, [f"{kind}: value must be a non-empty string"]
        mn = rule.get("min")
        if kind == "registry_dword_min":
            if not (isinstance(mn, int) and not isinstance(mn, bool) and 0 <= mn <= 0xFFFFFFFF):
                return None, [f"{kind}: min must be an integer 0..2^32-1"]
        elif not (_str(mn) and re.match(r"^\d+(\.\d+)*$", mn)):
            return None, [f"{kind}: min must be a dotted version string"]
    elif kind == "registry_key_exists":
        if not (_str(rule.get("key")) and HKLM_RE.match(rule["key"])):
            return None, [f"{kind}: key must be an HKLM\\ path"]
    elif kind == "file_exists":
        # Same test as dxr-get's absolute_windows_path(): a drive, and no '%' anywhere.
        if not (_str(rule.get("path")) and re.match(r"^[A-Za-z]:[\\/]", rule["path"]) and "%" not in rule["path"]):
            return None, [f"{kind}: path must be an absolute Windows path (no environment variables)"]
    elif kind == "service_exists":
        if not _str(rule.get("name")):
            return None, [f"{kind}: name must be a non-empty string"]
    return out, probs


def validate_detect_manifest(m):
    """Validate a displayxr-detect.json (schema 1).

    Returns (clean, problems). `clean` is None when the manifest is unusable as a
    whole; otherwise it holds id / label / registry_version / floor / detect, with
    bad optional fields ignored and bad rules dropped (each one reported)."""
    if not isinstance(m, dict):
        return None, ["manifest is not a JSON object"]
    if m.get("schema") != 1:
        return None, [f"schema {m.get('schema')!r} is not 1"]
    probs = []
    clean = {}
    if "id" in m:
        if isinstance(m["id"], str) and V2_ID_RE.match(m["id"]):
            clean["id"] = m["id"]
        else:
            probs.append(f"id {m['id']!r} is not a schema-2 item id (ignored)")
    if "label" in m:
        if isinstance(m["label"], str) and m["label"].strip():
            clean["label"] = m["label"].strip()
        else:
            probs.append("label is not a non-empty string (ignored)")
    if "registry_version" in m:
        reg = m["registry_version"]
        if isinstance(reg, str) and re.match(r"^(HKLM|HKEY_LOCAL_MACHINE)\\.+\\[^\\]+$", reg, re.I):
            clean["registry_version"] = reg
        else:
            probs.append("registry_version is not an HKLM\\...\\Value path (ignored)")
    if "requires" in m:
        req = m["requires"]
        rt = req.get("runtime") if isinstance(req, dict) else None
        fm = FLOOR_RE.match(rt) if isinstance(rt, str) else None
        if fm:
            clean["floor"] = fm.group(1)
        else:
            probs.append(f"requires.runtime {rt!r} is not '>=X.Y.Z' (ignored)")
    rules = m.get("detect")
    if not isinstance(rules, list):
        return None, probs + ["detect is not a list"]
    clean["detect"] = []
    for i, r in enumerate(rules):
        cr, rp = validate_rule(r)
        probs += [f"detect[{i}]: {x}" for x in rp]
        if cr is not None:
            clean["detect"].append(cr)
    if not clean["detect"]:
        return None, probs + ["no usable detect rule"]
    return clean, probs


def _ver(v):
    return tuple(int(x) for x in v.split("."))


def fetch_detect_manifest(comp, tag):
    """The pinned release's DETECT_MANIFEST, validated; None when absent or unusable."""
    try:
        rel = api_json(f"/repos/{ORG}/{comp['repo']}/releases/tags/{tag}")
    except urllib.error.HTTPError:
        return None  # windows_block reports a missing release
    asset = next((a for a in rel.get("assets", []) if a.get("name") == DETECT_MANIFEST), None)
    if asset is None:
        return None
    try:
        req = urllib.request.Request(asset["browser_download_url"],
                                     headers={"User-Agent": "displayxr-gen-catalog"})
        with urllib.request.urlopen(req, timeout=60) as r:
            m = json.loads(r.read(1 << 20).decode("utf-8"))
    except (OSError, urllib.error.URLError, ValueError) as e:
        warn(f"{comp['name']}: {DETECT_MANIFEST} unreadable ({e}); using the fallback")
        return None
    clean, probs = validate_detect_manifest(m)
    for x in probs:
        warn(f"{comp['name']}: {DETECT_MANIFEST}: {x}")
    if clean is None:
        warn(f"{comp['name']}: {DETECT_MANIFEST} rejected; using the fallback")
        return None
    info(f"{comp['name']}: using {DETECT_MANIFEST} ({len(clean['detect'])} rules)")
    return clean


def windows_block(comp, version, verifier, download_dir, registry_version=None):
    repo = comp["repo"]
    asset_name = comp["asset"].format(v=version) if "asset" in comp else None
    tag = "v" + version
    try:
        rel = api_json(f"/repos/{ORG}/{repo}/releases/tags/{tag}")
    except urllib.error.HTTPError as e:
        warn(f"{comp['name']}: release {repo}@{tag} not found (HTTP {e.code}); no windows block")
        return None
    asset = pick_asset(comp, version, rel.get("assets", []))
    if asset is None:
        wanted = asset_name or comp["asset_re"].format(v=re.escape(version))
        warn(f"{comp['name']}: {repo}@{tag} has no asset {wanted}; no windows block")
        return None
    asset_name = asset["name"]

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
        "registry_version": registry_version or comp["registry_version"],
    }


def pick_asset(comp, version, assets):
    """The release asset of a component: an exact name, or the highest build
    matching `asset_re` (group 1 = the build number)."""
    if "asset" in comp:
        name = comp["asset"].format(v=version)
        return next((a for a in assets if a.get("name") == name), None)
    rx = re.compile(comp["asset_re"].format(v=re.escape(version)))
    best = None
    for a in assets:
        m = rx.fullmatch(a.get("name", ""))
        if m and (best is None or int(m.group(1)) > best[0]):
            best = (int(m.group(1)), a)
    return best[1] if best else None


def cmake_set_value(repo, spec, tag):
    """The literal a provider's CMake file sets a variable to at a tag, as X.Y.Z, or None."""
    text = repo_file(repo, spec["file"], tag)
    if text is None:
        return None
    # Line-anchored: a comment quoting the shape (`# ... set(KEY "vX.Y.Z"`) is not the value.
    m = re.search(r'(?m)^[ \t]*set\(\s*' + re.escape(spec["cmake_set"]) + r'\s+"([^"]+)"', text)
    if not m or not SEMVER_RE.match(m.group(1)):
        warn(f"{repo}@{tag}: no usable {spec['cmake_set']} in {spec['file']}; omitted, not guessed")
        return None
    return SEMVER_RE.match(m.group(1)).group(1)


def build_extra_components(versions, verifier, download_dir):
    """Schema-2-only components, in the schema-1 component shape plus
    `runtime_exact`. A component with no pin has no release yet: not listed."""
    out = {}
    for comp in V2_ONLY_COMPONENTS:
        pin = versions.get(comp["pin"])
        if pin is None:
            info(f"{comp['name']}: no versions.json pin (no release yet); not listed")
            continue
        m = SEMVER_RE.match(pin)
        if not m:
            warn(f"versions.json[{comp['pin']}] = {pin!r} is not vX.Y.Z; component skipped")
            continue
        version = m.group(1)
        tag = "v" + version
        entry = {"recommended": version}
        if "runtime_exact" in comp:
            exact = cmake_set_value(comp["repo"], comp["runtime_exact"], tag)
            if exact is None:
                # Without the pin the item would read as "any runtime": never list that.
                warn(f"{comp['name']}: its runtime pin is unreadable at {tag}; not listed")
                continue
            entry["runtime_exact"] = exact
            entry["requires"] = {"runtime": ">=" + exact}
        elif "floor" in comp:
            floor = cmake_set_value(comp["repo"], comp["floor"], tag)
            if floor:
                entry["requires"] = {"runtime": ">=" + floor}
        win = windows_block(comp, version, verifier, download_dir)
        if win:
            entry["windows"] = win
        out[comp["name"]] = entry
    return out


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
    MANIFESTS.clear()
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

        # A display plug-in's own manifest first; the per-vendor parsing below is
        # the fallback for releases that predate it.
        manifest = fetch_detect_manifest(comp, tag) if comp.get("kind") == "display_plugin" else None
        runtime_pin = SEMVER_RE.match(versions.get("runtime") or "")
        if manifest is not None and manifest.get("floor") and runtime_pin and \
                _ver(manifest["floor"]) > _ver(runtime_pin.group(1)):
            # A floor no runtime in this catalog satisfies would make every
            # `ensure-for` that detects this plug-in exit 40 on every machine. The
            # manifest is wrong or ahead of the runtime pin: ignore ALL of it (its
            # rules too) and use the fallback, as for a release without one.
            warn(f"{comp['name']}: {DETECT_MANIFEST} requires runtime >={manifest['floor']}, above "
                 f"the pinned runtime {runtime_pin.group(1)}; manifest ignored, using the fallback")
            manifest = None
        if manifest is not None:
            MANIFESTS[comp["name"]] = manifest

        floor = None
        if manifest is not None:
            floor = manifest.get("floor")
        elif comp["name"] == "leia_plugin":
            floor = leia_floor(tag)
        elif comp["name"] == "browser":
            floor = browser_floor(tag)
        if floor:
            entry["requires"] = {"runtime": ">=" + floor}

        win = windows_block(comp, version, verifier, download_dir,
                            (manifest or {}).get("registry_version"))
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
        if manifest is not None:
            entry["detect"] = manifest["detect"]
        elif comp["name"] == "leia_plugin":
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


# --------------------------------------------------------------------------- schema 2 (ADR-064)
#
# Schema 2 is a SEPARATE document (catalog/v2/stable.json). Deployed dxr-get builds
# accept `schema == 1` only and treat anything else as offline, so schema 1 keeps
# being generated exactly as above and schema 2 is derived from it: a pure function
# of the schema-1 dict, with no network access and no second look at the assets.
# What schema 1 does not know (class ABI ranges, uninstall, trust tier, licence
# terms) is left out, never guessed. Spec: docs/specs/runtime/component-catalog.md.

V2_PUBLISHER = "displayxr"
V2_PLATFORM_WINDOWS = "windows-x64"

# Facts about the first-party components that schema 1 carries only implicitly.
# `uninstall` is deliberately absent for the runtime: dxr-get never removes the
# trunk. The three schema-2-only providers get theirs once their installers'
# key names have been read from a release.
# Keyed by the schema-1 component name (the versions.json field).
V2_ITEMS = {
    "runtime": {
        "id": "runtime",
        "class": "runtime",
        "name": "DisplayXR Runtime",
    },
    "leia_plugin": {
        "id": "leia-sr",
        "class": "display_processor",
        "registered_id": "leia-sr",
        "name": "Leia SR display processor",
        # Where the plug-in's own installer leaves its uninstall command (its
        # Add/Remove Programs key, installer/DisplayXRLeiaSRInstaller.nsi).
        "uninstall": {
            "path": "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DisplayXRLeiaSR\\UninstallString",
            "args": ["/S"],
        },
    },
    "browser": {
        "id": "browser",
        "class": "app",
        "name": "DisplayXR Browser",
        # The browser installer's Add/Remove Programs key (browser-pvt
        # installer/DisplayXRBrowserInstaller.nsi, `!define ARP`).
        "uninstall": {
            "path": "HKLM\\Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\DisplayXR Browser\\UninstallString",
            "args": ["/S"],
        },
    },
    # --- schema 2 only (V2_ONLY_COMPONENTS) ---
    "conversion_open": {
        "id": "open-conversion",
        "class": "conversion_provider",
        "registered_id": "open-conversion",
        "name": "Open conversion provider",
        "summary": "2D-to-3D conversion with open models. Needs no vendor software.",
        # ADR-064 §7: a setup UI pre-ticks it, so "Convert to 3D" exists on a
        # machine whose display vendor ships no converter.
        "recommended": True,
    },
    "leia_conversion": {
        "id": "leia-conversion",
        "class": "conversion_provider",
        "registered_id": "leia-conversion",
        "name": "Leia conversion provider",
        "summary": "2D-to-3D conversion with Leia's NeurD module.",
        # Declared and detected only (spec § Requires): NeurD has its own
        # installer, licence and update channel; DisplayXR never fetches it.
        # No info_url: its download page is not known here, and is not guessed.
        "external": [
            {
                "kind": "external",
                "name": "NeurD runtime (Leia)",
                "min_version": "0.4.6",
                "detect": [
                    {"kind": "registry_sz_min", "key": "HKLM\\SOFTWARE\\LeiaInc\\NeurD",
                     "value": "Version", "min": "0.4.6"},
                ],
            },
        ],
    },
    "ultraleap": {
        "id": "ultraleap",
        "class": "input_provider",
        "registered_id": "ultraleap",
        "name": "Ultraleap hands",
        "summary": "Hand tracking from an Ultraleap camera, as controllers and hand joints.",
        "external": [
            {
                "kind": "external",
                "name": "Ultraleap tracking software",
                "detect": [
                    {"kind": "file_exists",
                     "path": "C:\\Program Files\\Ultraleap\\LeapSDK\\lib\\x64\\LeapC.dll"},
                ],
                "info_url": "https://www.ultraleap.com/downloads/",
            },
        ],
    },
}

V2_CLASSES = ("runtime", "display_processor", "input_provider", "conversion_provider",
              "tracking_provider", "workspace_controller", "companion", "app", "sdk")
# Classes the runtime loads or spawns under a registered id.
V2_REGISTERED_CLASSES = ("display_processor", "input_provider", "conversion_provider",
                         "tracking_provider", "workspace_controller", "companion")
# Top-level keys of `dxr-get detect --json`, which also keys an object per item by
# its schema-1 name / id: an item must not take one of these names.
V2_RESERVED_IDS = ("edids", "usb", "display_plugins")
V2_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]*(\.[a-z0-9][a-z0-9-]*)?$")
V2_VERSION_REQ_RE = re.compile(r"^>=\d+\.\d+\.\d+$")
V2_VERSION_RE = re.compile(r"^\d+\.\d+\.\d+$")


def v2_asset(win, uninstall=None):
    """Schema-1 `windows` block -> schema-2 asset. Same bytes, same signer, regrouped.

    `uninstall` is the item's V2_ITEMS rule (schema 1 has no such field): where the
    component's own installer leaves its uninstall command, which dxr-get copies
    into the install receipt."""
    asset = {
        "version": win["version"],
        "file": win["file"],
        "size": win["size"],
        "sha256": win["sha256"],
        "urls": list(win["urls"]),
        "signature": {"kind": "authenticode", "signer": win["signer"]},
        "installer": {
            "kind": "exe",
            "args": list(win.get("args", [])),
            "scope": "machine",
            "elevation": "required",
        },
        "installed": {"kind": "registry_sz", "path": win["registry_version"]},
    }
    if uninstall:
        asset["uninstall"] = {"kind": "registry_command", "path": uninstall["path"],
                              "args": list(uninstall["args"])}
    return asset


def build_catalog_v2(v1, *, manifests=None, extra=None):
    """Schema-2 catalog for the components of a schema-1 catalog, then `extra`
    (build_extra_components: the schema-2-only ones). Modifies neither.

    `manifests` (component name -> validated displayxr-detect.json, as build_catalog
    collected them) names a display plug-in that V2_ITEMS does not know: its `id`
    becomes the item id and registered_id, its `label` the item name (#2737).

    Both are keyword-only: a caller with one argument (make_test_catalog.py) gets
    the three schema-1 components and nothing else."""
    items = []
    for name, comp in list(v1.get("components", {}).items()) + list((extra or {}).items()):
        meta = V2_ITEMS.get(name)
        man = (manifests or {}).get(name)
        if meta is None and comp.get("kind") == "display_plugin" and man and man.get("id"):
            meta = {"id": man["id"], "class": "display_processor", "registered_id": man["id"],
                    "name": man.get("label", man["id"])}
        if meta is None:
            warn(f"schema 2: no item metadata for component {name!r}; not listed")
            continue
        item = {"id": meta["id"]}
        if name != meta["id"]:
            item["aliases"] = [name]  # the schema-1 name, so `ensure-for <name>` keeps resolving
        item["class"] = meta["class"]
        if "registered_id" in meta:
            item["registered_id"] = meta["registered_id"]
        item["name"] = meta["name"]
        if "summary" in meta:
            item["summary"] = meta["summary"]
        item["publisher"] = V2_PUBLISHER
        item["version"] = comp["recommended"]
        if "runtime_exact" in comp:
            # Built at one runtime release and known-good only there (ADR-063).
            item["runtime"] = {"min": comp["runtime_exact"], "max": comp["runtime_exact"]}
        requires = []
        floor = comp.get("requires", {}).get("runtime")
        if floor:
            requires.append({"kind": "runtime", "version": floor})
        requires.extend(json.loads(json.dumps(meta.get("external", []))))  # deep copy
        if requires:
            item["requires"] = requires
        if "windows" in comp:
            item["assets"] = {V2_PLATFORM_WINDOWS: v2_asset(comp["windows"], meta.get("uninstall"))}
        if "detect" in comp:
            item["detect"] = [dict(r) for r in comp["detect"]]
        if meta.get("recommended"):
            item["recommended"] = True
        items.append(item)

    return {
        "schema": 2,
        "serial": v1["serial"],
        "generated": v1["generated"],
        "expires": v1["expires"],
        "channel": v1["channel"],
        "source": dict(v1["source"]),
        "publishers": {
            V2_PUBLISHER: {
                "name": "DisplayXR",
                "url": "https://displayxr.org",
                "signers": {
                    V2_PLATFORM_WINDOWS: [
                        {"kind": "authenticode", "subject_cn": cn}
                        for cn in v1.get("signers", {}).get("windows", [])
                    ],
                },
            },
        },
        "items": items,
    }


def _detect_problems(where, rules):
    """A detect array must hold only clean rules: validate_rule() would keep each one as is."""
    if rules is None:
        return []
    if not isinstance(rules, list):
        return [f"{where}: is not an array"]
    out = []
    for i, r in enumerate(rules):
        clean, probs = validate_rule(r)
        if clean is None or clean != r:
            out.append(f"{where}[{i}]: {'; '.join(probs) or 'not a clean rule'}")
    return out


def validate_v2(cat):
    """Structural problems of a schema-2 catalog, as strings. Empty list = valid."""
    problems = []
    publishers = cat.get("publishers")
    if not isinstance(publishers, dict):
        return ["publishers is not an object"]
    items = cat.get("items")
    if not isinstance(items, list):
        return ["items is not an array"]
    seen = set()
    for item in items:
        iid = item.get("id")
        where = f"item {iid!r}"
        if not isinstance(iid, str) or not V2_ID_RE.match(iid):
            problems.append(f"{where}: id is not [a-z0-9-] (optionally publisher.name)")
        for name in [iid] + list(item.get("aliases", [])):
            if name in seen:
                problems.append(f"{where}: id or alias {name!r} is used twice")
            if name in V2_RESERVED_IDS:
                problems.append(f"{where}: id or alias {name!r} is reserved")
            seen.add(name)
        problems += _detect_problems(f"{where} detect", item.get("detect"))
        cls = item.get("class")
        if cls not in V2_CLASSES:
            problems.append(f"{where}: unknown class {cls!r}")
        if cls in V2_REGISTERED_CLASSES and not item.get("registered_id"):
            problems.append(f"{where}: class {cls} needs registered_id")
        pub = publishers.get(item.get("publisher"))
        if pub is None:
            problems.append(f"{where}: publisher {item.get('publisher')!r} is not in publishers")
        rng = item.get("runtime")
        if rng is not None:
            lo, hi = rng.get("min"), rng.get("max")
            for bound in (lo, hi):
                if bound is not None and not V2_VERSION_RE.match(str(bound)):
                    problems.append(f"{where}: runtime bound {bound!r} is not X.Y.Z")
            if lo and hi and V2_VERSION_RE.match(lo) and V2_VERSION_RE.match(hi) \
                    and tuple(map(int, lo.split("."))) > tuple(map(int, hi.split("."))):
                problems.append(f"{where}: runtime.min {lo} is above runtime.max {hi}")
            stated = [r.get("version") for r in item.get("requires", []) if r.get("kind") == "runtime"]
            if lo and stated and stated != [">=" + lo]:
                problems.append(f"{where}: runtime.min {lo} disagrees with requires runtime {stated}")
        for req in item.get("requires", []):
            kind = req.get("kind")
            if kind not in ("runtime", "item", "external"):
                problems.append(f"{where}: requires kind {kind!r}")
            elif kind in ("runtime", "item") and not V2_VERSION_REQ_RE.match(req.get("version", ">=0.0.0")):
                problems.append(f"{where}: requires version {req.get('version')!r} is not >=X.Y.Z")
            elif kind == "external":
                problems += _detect_problems(f"{where} requires {req.get('name')!r} detect", req.get("detect"))
        for platform, asset in item.get("assets", {}).items():
            awhere = f"{where} {platform}"
            if "listing" in asset:  # link-out: nothing is downloaded or run
                if not str(asset["listing"].get("url", "")).startswith("https://"):
                    problems.append(f"{awhere}: listing.url must be an https URL")
                continue
            if not re.fullmatch(r"[0-9a-f]{64}", asset.get("sha256", "")):
                problems.append(f"{awhere}: sha256 is not 64 lowercase hex")
            if not isinstance(asset.get("size"), int) or asset["size"] <= 0:
                problems.append(f"{awhere}: size is not a positive integer")
            urls = asset.get("urls")
            if not urls or any(not str(u).startswith("https://") for u in urls):
                problems.append(f"{awhere}: urls must be a non-empty list of https URLs")
            sig = asset.get("signature")
            if sig is None:
                # ADR-064: every installable asset is signed; there is no hash-pinned-only form.
                problems.append(f"{awhere}: an installable asset needs a signature")
                continue
            allowed = (pub or {}).get("signers", {}).get(platform, [])
            if sig.get("kind") == "authenticode" and sig.get("signer") not in [
                    a.get("subject_cn") for a in allowed if a.get("kind") == "authenticode"]:
                problems.append(f"{awhere}: signer {sig.get('signer')!r} is not one of the "
                                f"publisher's {platform} signers")
    return problems


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

    write_signature(out_json, data, args.no_sign)

    if args.out_v2:
        if os.path.abspath(args.out_v2) == os.path.abspath(args.out):
            sys.exit("error: --out-v2 must be a different directory from --out")
        extra = build_extra_components(versions, VERIFIERS[args.verifier], download_dir)
        extra = build_extra_components(versions, VERIFIERS[args.verifier], download_dir)
        catalog2 = build_catalog_v2(catalog, manifests=MANIFESTS, extra=extra)
        problems = validate_v2(catalog2)
        if problems:
            sys.exit("error: generated schema-2 catalog is invalid: " + "; ".join(problems))
        data2 = canonical_bytes(catalog2)
        os.makedirs(args.out_v2, exist_ok=True)
        out2 = os.path.join(args.out_v2, "stable.json")
        with open(out2, "wb") as f:
            f.write(data2)
        info(f"wrote {out2} ({len(data2)} bytes, schema 2, {len(catalog2['items'])} items)")
        write_signature(out2, data2, args.no_sign)
    return 0


def write_signature(out_json, data, no_sign):
    sig_path = out_json + ".sig"
    if no_sign:
        if os.path.exists(sig_path):
            os.remove(sig_path)  # never leave a signature that no longer matches
        info("--no-sign: no signature written")
        return
    sig = sign_bytes(data)
    with open(sig_path, "wb") as f:
        f.write(base64.b64encode(sig) + b"\n")
    info(f"wrote {sig_path}")


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
    if cat.get("schema") not in (1, 2):
        problems.append(f"schema {cat.get('schema')} is not 1 or 2")
    if cat.get("schema") == 2:
        problems.extend(validate_v2(cat))
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


def cmd_validate_detect(args):
    with open(args.manifest, encoding="utf-8") as f:
        m = json.load(f)
    clean, probs = validate_detect_manifest(m)
    for x in probs:
        print(f"WARN: {x}", file=sys.stderr)
    if clean is None:
        print(f"FAIL: {args.manifest} is not a usable {DETECT_MANIFEST}", file=sys.stderr)
        return 1
    print(f"OK: {len(clean['detect'])} detect rules"
          + (f", id {clean['id']!r}" if "id" in clean else "")
          + (f", label {clean['label']!r}" if "label" in clean else "")
          + (f", requires runtime >={clean['floor']}" if "floor" in clean else ""))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    g = sub.add_parser("generate", help="build stable.json (+ .sig) from versions.json")
    g.add_argument("--out", required=True, help="output directory (writes stable.json, stable.json.sig)")
    g.add_argument("--versions", default=os.path.join(REPO_ROOT, "versions.json"))
    g.add_argument("--download-dir", help="where to keep downloaded installers (reused if same size)")
    g.add_argument("--no-sign", action="store_true", help="dry run: write stable.json only")
    g.add_argument("--out-v2", metavar="DIR",
                   help="also write the schema-2 document (ADR-064) to DIR/stable.json (+ .sig); "
                        "derived from the schema-1 catalog, which is written unchanged")
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

    d = sub.add_parser("validate-detect", help=f"check a display plug-in's {DETECT_MANIFEST} (schema 1)")
    d.add_argument("manifest", help=f"path to {DETECT_MANIFEST}")
    d.set_defaults(func=cmd_validate_detect)

    args = ap.parse_args()
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())

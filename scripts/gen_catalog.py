#!/usr/bin/env python3
# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0
"""Generate, sign and verify the DisplayXR component catalog (#2703, epic #2702).

The catalog is the signed index `dxr-get` reads to fetch DisplayXR components
(the runtime, the display plug-ins, the browser) on demand. It is published to
https://updates.displayxr.org/catalog/v1/stable.json (+ .sig) by
.github/workflows/publish-catalog.yml. Spec: docs/specs/runtime/component-catalog.md.

    gen_catalog.py generate --out DIR [--no-sign] [--versions versions.json]
                            [--download-dir DIR] [--verifier powershell|none]
                            [--previous URL|FILE]
    gen_catalog.py verify   DIR/stable.json [--sig DIR/stable.json.sig]
    gen_catalog.py validate-detect displayxr-detect.json

Display plug-ins (kind "display_plugin") describe themselves: each release attaches
a `displayxr-detect.json` (schema 1: label, registry_version, requires.runtime,
detect rules), which is ingested generically (#2737). A release without one falls
back to the component's `fallback` readers in COMPONENTS (Leia: its EDID header and
installer CMake), so older plug-in releases keep their detection.

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

# Per-component publishing facts -- the catalog's component list is this table, and
# nothing below branches on a component's name. `pin` is the versions.json field.
#   floor     -- how to read the component's runtime floor (a FLOOR_READERS key).
#   kind      -- "display_plugin": detect rules + label + floor come from the release's
#                DETECT_MANIFEST asset; `fallback` names the readers used when a
#                release predates the manifest (FLOOR_READERS / DETECT_READERS keys).
# Adding a display vendor = one row here (kind display_plugin, no fallback) and a
# displayxr-detect.json on each of its releases.
COMPONENTS = [
    {
        "name": "runtime",
        "pin": "runtime",
        "repo": "displayxr-runtime",
        "asset": "DisplayXRSetup-{v}.exe",
        "label": "DisplayXR runtime",
        "args": ["/S", "/NOSTART"],
        "registry_version": "HKLM\\Software\\DisplayXR\\Runtime\\Version",
    },
    {
        "name": "leia_plugin",
        "pin": "leia_plugin",
        "repo": "displayxr-leia-plugin",
        "asset": "DisplayXRLeiaSRSetup-{v}.exe",
        "kind": "display_plugin",
        "label": "Leia display plug-in",
        "args": ["/S"],
        "registry_version": "HKLM\\Software\\DisplayXR\\Plugins\\LeiaSR\\Version",
        "fallback": {"floor": "leia_cmake", "detect": "leia_edid_header"},
    },
    {
        "name": "browser",
        "pin": "browser",
        "repo": "displayxr-browser",
        "asset": "DisplayXR-Browser-Setup-{v}.exe",
        "label": "DisplayXR Browser",
        "floor": "browser_nsis",
        "args": ["/S"],
        "registry_version": "HKLM\\Software\\DisplayXR\\Browser\\Version",
    },
]

# The vendor-owned detect manifest a display plug-in release attaches (#2737).
DETECT_MANIFEST = "displayxr-detect.json"

# Leia fallback (releases without DETECT_MANIFEST): the SR platform reports a
# connected SR device.
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


def browser_floor(comp, tag):
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


def leia_floor(comp, tag):
    """Leia floor = its installer's MIN_RUNTIME_VERSION, which installer/CMakeLists.txt
    derives from the DXR_RUNTIME_GIT_TAG pin in the top-level CMakeLists.txt."""
    inst = repo_file(comp["repo"], "installer/CMakeLists.txt", tag)
    if inst is None or not re.search(r'set\(MIN_RUNTIME_VERSION\s+"\$\{CMAKE_MATCH_1\}"\)', inst) \
            or "DXR_RUNTIME_GIT_TAG" not in inst:
        warn(f"leia-plugin@{tag}: MIN_RUNTIME_VERSION derivation not recognised; omitting requires")
        return None
    top = repo_file(comp["repo"], "CMakeLists.txt", tag)
    if top is None:
        return None
    m = re.search(r'set\(DXR_RUNTIME_GIT_TAG\s+"([^"]+)"', top)
    if not m or not SEMVER_RE.match(m.group(1)):
        warn(f"leia-plugin@{tag}: DXR_RUNTIME_GIT_TAG is not a vX.Y.Z tag; omitting requires")
        return None
    return SEMVER_RE.match(m.group(1)).group(1)


EDID_ROW_RE = re.compile(r"^\s*\{\s*(\d+)\s*,\s*(\d+)\s*\}\s*,?\s*(?://\s*(.*))?$")


def leia_detect(comp, tag):
    """EDID detectors from the plug-in's own frozen table, plus the SR registry rule."""
    text = repo_file(comp["repo"], "src/drv_leia/leia_edid_table.h", tag)
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


FLOOR_READERS = {"browser_nsis": browser_floor, "leia_cmake": leia_floor}
DETECT_READERS = {"leia_edid_header": leia_detect}


# --------------------------------------------------------------------------- detect manifest (#2737)

RULE_KINDS = ("edid", "usb", "registry_dword_min")
FLOOR_RE = re.compile(r"^>=\s*(\d+\.\d+\.\d+)$")


def _u16(v):
    return isinstance(v, int) and not isinstance(v, bool) and 0 <= v <= 0xFFFF


def validate_rule(rule):
    """Return (clean_rule or None, [problems]). Unknown kinds are dropped: dxr-get
    ignores them, and this generator ships with the dxr-get that would read them."""
    probs = []
    if not isinstance(rule, dict):
        return None, ["rule is not an object"]
    kind = rule.get("kind")
    if kind not in RULE_KINDS:
        return None, [f"unknown rule kind {kind!r} (dropped)"]
    conf = rule.get("confidence")
    if conf not in ("high", "low"):
        return None, [f"{kind}: confidence {conf!r} is not high|low"]
    out = {"kind": kind}
    if kind in ("edid", "usb"):
        vk, ik = ("man", "prod") if kind == "edid" else ("vid", "pid")
        if not _u16(rule.get(vk)):
            return None, [f"{kind}: {vk} must be an integer 0..65535"]
        out[vk] = rule[vk]
        has_exact = ik in rule
        has_range = f"{ik}_min" in rule or f"{ik}_max" in rule
        if has_exact and has_range:
            return None, [f"{kind}: give {ik} or {ik}_min/{ik}_max, not both"]
        if has_exact:
            if not _u16(rule[ik]):
                return None, [f"{kind}: {ik} must be an integer 0..65535"]
            out[ik] = rule[ik]
        elif has_range:
            lo, hi = rule.get(f"{ik}_min"), rule.get(f"{ik}_max")
            if not (_u16(lo) and _u16(hi) and lo <= hi):
                return None, [f"{kind}: {ik}_min/{ik}_max must both be integers 0..65535, min <= max"]
            out[f"{ik}_min"], out[f"{ik}_max"] = lo, hi
        elif conf == "high":
            # A manufacturer / vendor id alone also matches that vendor's ordinary
            # 2D monitors and devices: never an unattended install. dxr-get
            # enforces the same downgrade.
            probs.append(f"{kind}: {vk}-only rule downgraded to low confidence")
            conf = "low"
    else:
        key, value, mn = rule.get("key"), rule.get("value"), rule.get("min")
        if not (isinstance(key, str) and re.match(r"^(HKLM|HKEY_LOCAL_MACHINE)\\", key, re.I)):
            return None, ["registry_dword_min: key must be an HKLM\\ path"]
        if not isinstance(value, str) or not value:
            return None, ["registry_dword_min: value must be a non-empty string"]
        if not (isinstance(mn, int) and not isinstance(mn, bool) and 0 <= mn <= 0xFFFFFFFF):
            return None, ["registry_dword_min: min must be an integer 0..2^32-1"]
        out.update(key=key, value=value, min=mn)
    out["confidence"] = conf
    if isinstance(rule.get("note"), str):
        out["note"] = rule["note"]
    return out, probs


def validate_detect_manifest(m):
    """Validate a displayxr-detect.json (schema 1).

    Returns (clean, problems). `clean` is None when the manifest is unusable as a
    whole; otherwise it holds label / registry_version / floor / detect, with bad
    rules dropped (each one reported in problems).
    """
    if not isinstance(m, dict):
        return None, ["manifest is not a JSON object"]
    if m.get("schema") != 1:
        return None, [f"schema {m.get('schema')!r} is not 1"]
    probs = []
    clean = {}
    label = m.get("label")
    if label is not None:
        if isinstance(label, str) and label.strip():
            clean["label"] = label.strip()
        else:
            probs.append("label is not a non-empty string (ignored)")
    reg = m.get("registry_version")
    if reg is not None:
        if isinstance(reg, str) and re.match(r"^(HKLM|HKEY_LOCAL_MACHINE)\\.+\\[^\\]+$", reg, re.I):
            clean["registry_version"] = reg
        else:
            probs.append("registry_version is not an HKLM\\...\\Value path (ignored)")
    req = m.get("requires")
    if req is not None:
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


# --------------------------------------------------------------------------- generate


def get_release(comp, tag):
    """The GitHub release JSON for <comp repo>@<tag>, or None."""
    try:
        return api_json(f"/repos/{ORG}/{comp['repo']}/releases/tags/{tag}")
    except urllib.error.HTTPError as e:
        warn(f"{comp['name']}: release {comp['repo']}@{tag} not found (HTTP {e.code})")
        return None


def fetch_detect_manifest(comp, rel):
    """The release's DETECT_MANIFEST, validated; None when absent or unusable."""
    asset = next((a for a in (rel or {}).get("assets", []) if a.get("name") == DETECT_MANIFEST), None)
    if asset is None:
        return None
    try:
        req = urllib.request.Request(asset["browser_download_url"],
                                     headers={"User-Agent": "displayxr-gen-catalog"})
        with urllib.request.urlopen(req, timeout=60) as r:
            data = r.read(1 << 20)
        m = json.loads(data.decode("utf-8"))
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


def windows_block(comp, version, rel, verifier, download_dir, registry_version=None):
    repo, asset_name = comp["repo"], comp["asset"].format(v=version)
    tag = "v" + version
    if rel is None:
        warn(f"{comp['name']}: no release {repo}@{tag}; no windows block")
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
        "registry_version": registry_version or comp["registry_version"],
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
        rel = get_release(comp, tag)

        # Self-description: a display plug-in's manifest, else the table + fallbacks.
        manifest = fetch_detect_manifest(comp, rel) if comp.get("kind") == "display_plugin" else None
        fallback = comp.get("fallback", {})
        label = (manifest or {}).get("label") or comp.get("label")

        entry = {"recommended": version}
        if "kind" in comp:
            entry["kind"] = comp["kind"]
        if label:
            entry["label"] = label

        if manifest is not None:
            floor = manifest.get("floor")
        else:
            reader = FLOOR_READERS.get(comp.get("floor") or fallback.get("floor"))
            floor = reader(comp, tag) if reader else None
        if floor:
            entry["requires"] = {"runtime": ">=" + floor}

        win = windows_block(comp, version, rel, verifier, download_dir,
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

        if comp.get("kind") == "display_plugin":
            if manifest is not None:
                entry["detect"] = manifest["detect"]
            elif DETECT_READERS.get(fallback.get("detect")):
                entry["detect"] = DETECT_READERS[fallback["detect"]](comp, tag)
            else:
                warn(f"{comp['name']}: no {DETECT_MANIFEST} and no fallback; no detect rules "
                     "(dxr-get will never install it unattended)")
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
    for name, comp in cat.get("components", {}).items():
        for i, r in enumerate(comp.get("detect") or []):
            cr, rp = validate_rule(r)
            if cr is None or cr != r:
                problems.append(f"{name}: detect[{i}] is not a clean rule: {'; '.join(rp) or 'normalised'}")
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

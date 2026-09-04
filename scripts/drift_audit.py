#!/usr/bin/env python3
"""Cross-repo drift audit for the DisplayXR org (epic #691, child #695).

Most findings in the 2026-07 ecosystem audit were the same handful of checks
run once by hand. This automates them so they run continuously (weekly workflow)
and can file an issue on the offending repo. It is read-only against the org via
the ``gh`` CLI — no checkout needed — so it runs from any machine with ``gh``
authenticated.

## Checks

1. FetchContent pins vs latest tag — a consumer that pins ``displayxr-mcp`` /
   ``displayxr-common`` / ``displayxr-runtime`` behind the dependency's newest
   semver tag (documented-intentional lags are allow-listed).
2. ``displayxr-common`` pin spread — flags when consumers pin *different*
   common versions (the audit found v1.1.1 / v1.1.2 / v1.2.0 in the wild).
3. Vendored-spec staleness — a spec copied with a sync stamp (e.g. the shell's
   ``workspace-controller-registration.md``) whose body md5 no longer matches the
   canonical copy (license headers stripped before hashing).
4. ``openxr_includes`` API-version matrix — ``XR_CURRENT_API_VERSION`` should be
   uniform across every repo that vendors the headers.
5. Demo three-pins rule — vendored header rev == loader pin == CI loader pin,
   per demo repo.
6. Prose version drift — a hardcoded ``vX.Y.Z`` in a doc/TSX file that disagrees
   with ``versions.json``.

## Usage

    python3 scripts/drift_audit.py            # print report, exit 1 if drift
    python3 scripts/drift_audit.py --dry-run  # same, but never emit issues
    python3 scripts/drift_audit.py --emit-issues  # open/update a de-duped
                                                  # issue on each offending repo

``--dry-run`` is the default-safe mode for local runs; the weekly workflow uses
``--emit-issues``. Network access is isolated behind ``gh_raw`` / ``list_tags``
so the check functions are pure and unit-testable.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import re
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
ORG = "DisplayXR"
ISSUE_MARKER = "<!-- drift-audit:auto -->"

# --------------------------------------------------------------------------
# Config — the only places that encode per-repo knowledge.
# --------------------------------------------------------------------------

# Repos whose CMake we scan for FetchContent pins of the deps below.
CMAKE_CANDIDATES = [
    "CMakeLists.txt",
    "src/CMakeLists.txt",
    "cmake/dependencies.cmake",
    "cmake/deps.cmake",
    # Every displayxr-demo-* repo declares its FetchContent pins here, NOT in the
    # root CMakeLists.txt. Omitting this path is why the 2026-08 common-pin drift
    # (demos sitting on v2.0.0/v2.1.0/v2.3.1 while common was v2.5.0) went
    # unreported for weeks despite the weekly audit running green.
    "common/CMakeLists.txt",
]
PIN_CONSUMERS = [
    "displayxr-runtime",
    "displayxr-shell-pvt",
    "displayxr-leia-plugin",
    "displayxr-cef-host",
    # Demos consume displayxr-common too — they were absent from this list, so
    # their pins were never audited at all.
    "displayxr-demo-gaussiansplat",
    "displayxr-demo-modelviewer",
    "displayxr-demo-mediaplayer",
    "displayxr-demo-avatar",
    "displayxr-demo-earthview",
]
# Repos that may pin a dep as a git SUBMODULE rather than via FetchContent. A
# FetchContent-only scan is blind to these: displayxr-unreal sat on the v2.0.0
# commit with nothing to notice it.
#
# Deliberately a bare repo LIST, not a {repo: {path: dep}} map — the check reads
# `.gitmodules` and self-discovers paths. A hardcoded path would rot the moment a
# repo restructures or drops the submodule (displayxr-unreal is doing exactly
# that in unreal#37, which moves the view math behind XR_DXR_view_rig). Absent
# `.gitmodules` is a silent skip, never a finding.
SUBMODULE_CONSUMERS = [
    "displayxr-unreal",
]
# dep repo name -> substring that identifies its GIT_REPOSITORY url
PIN_DEPS = {
    "displayxr-mcp": "displayxr-mcp",
    "displayxr-common": "displayxr-common",
    "displayxr-runtime": "displayxr-runtime",
}
# (consumer, dep) pairs whose lag is deliberate — don't flag.
INTENTIONAL_LAG: set[tuple[str, str]] = {
    # leia-plugin pins a runtime release behind head on purpose (ABI gate).
    ("displayxr-leia-plugin", "displayxr-runtime"),
}

# Specs copied into another repo with a sync stamp -> canonical source.
VENDORED_SPECS = [
    (
        "displayxr-shell-pvt",
        "docs/specs/workspace-controller-registration.md",
        "displayxr-runtime",
        "docs/specs/runtime/workspace-controller-registration.md",
    ),
]

# Repos that vendor the OpenXR headers, and where.
OPENXR_HEADER_PATHS = {
    "displayxr-runtime": "src/external/openxr_includes/openxr/openxr.h",
    "displayxr-demo-gaussiansplat": "openxr_includes/openxr/openxr.h",
    "displayxr-demo-modelviewer": "openxr_includes/openxr/openxr.h",
    "displayxr-demo-avatar": "openxr_includes/openxr/openxr.h",
    "displayxr-demo-earthview": "openxr_includes/openxr/openxr.h",
    # mediaplayer uses the unified src/ layout with headers under third_party/.
    "displayxr-demo-mediaplayer": "third_party/displayxr-openxr/openxr/openxr.h",
}
DEMO_REPOS = [
    "displayxr-demo-gaussiansplat",
    "displayxr-demo-modelviewer",
    "displayxr-demo-avatar",
    "displayxr-demo-earthview",
    "displayxr-demo-mediaplayer",
]
# Candidate files a demo provisions/pins its OpenXR loader from.
LOADER_PIN_CANDIDATES = [
    "scripts/build-with-deps.bat",
    "scripts/build_windows.bat",
    "windows/CMakeLists.txt",
    "CMakeLists.txt",
]
CI_CANDIDATES = [
    ".github/workflows/build-windows.yml",
    ".github/workflows/build-macos.yml",
]

# Prose files whose hardcoded version should match versions.json.
# (repo, path, versions.json key)
PROSE_CHECKS = [
    ("displayxr-website", "app/architecture/page.tsx", "runtime"),
]

SEMVER_TAG = re.compile(r"^v(\d+)\.(\d+)\.(\d+)$")

# `#define XR_DXR_<ext>_SPEC_VERSION <n>`. The ext segment is spelled
# inconsistently across consumers (Unity shouts DISPLAY_INFO, the runtime and
# Unreal use display_info), so match loosely and normalise to lowercase --
# which is also how the runtime names the header files.
DXR_SPEC_RE = re.compile(
    r"^\s*#\s*define\s+XR_DXR_([A-Za-z0-9_]+?)_SPEC_VERSION\s+(\d+)\s*$", re.M | re.I
)
NSIS_DEFINE_RE = r'define\s+{key}\s+"([0-9][0-9.]*)"'
RUNTIME_EXT_HEADER = "src/external/openxr_includes/openxr/XR_DXR_{ext}.h"
GIT_REPO_RE = re.compile(r"GIT_REPOSITORY\s+(\S+)")
GIT_TAG_RE = re.compile(r"GIT_TAG\s+([^\s)]+)")
XR_VER_RE = re.compile(
    r"XR_CURRENT_API_VERSION\s+XR_MAKE_VERSION\(\s*(\d+)\s*,\s*(\d+)\s*,\s*(\d+)\s*\)"
)
OPENXR_VER_RE = re.compile(r"(?:OPENXR_VER|release-|openxr_loader_windows-)[=\s]*?(\d+\.\d+\.\d+)")
VXYZ_RE = re.compile(r"\bv(\d+\.\d+\.\d+)\b")


# --------------------------------------------------------------------------
# Network layer (the only impure part).
# --------------------------------------------------------------------------

_raw_cache: dict[tuple[str, str], str | None] = {}
_tag_cache: dict[str, list[str]] = {}
_tag_sha_cache: dict[str, dict[str, str]] = {}  # repo -> {commit sha: semver tag}


def _gh_full(args: list[str]) -> tuple[int, str, str]:
    """Run gh and return (returncode, stdout, stderr).

    Read paths deliberately swallow failure (a repo the token can't see is a
    "skipped", not an error). WRITE paths must not: discarding stderr here is
    what let issue emission fail silently every week for two months. Anything
    that mutates state should use this and check.
    """
    try:
        out = subprocess.run(
            ["gh", *args],
            capture_output=True,
            text=True,
            check=False,
        )
    except FileNotFoundError:
        sys.exit("error: the 'gh' CLI is required and was not found on PATH")
    return out.returncode, out.stdout, out.stderr


def _gh(args: list[str]) -> str | None:
    rc, out, _ = _gh_full(args)
    return None if rc != 0 else out


def gh_raw(repo: str, path: str, ref: str = "HEAD") -> str | None:
    """Fetch a file's raw text from a repo, or None if it doesn't exist."""
    key = (repo, f"{path}@{ref}")
    if key in _raw_cache:
        return _raw_cache[key]
    text = _gh(
        [
            "api",
            "-H",
            "Accept: application/vnd.github.raw",
            f"repos/{ORG}/{repo}/contents/{path}?ref={ref}",
        ]
    )
    _raw_cache[key] = text
    return text


def list_tags(repo: str) -> list[str]:
    if repo in _tag_cache:
        return _tag_cache[repo]
    out = _gh(["api", "--paginate", f"repos/{ORG}/{repo}/tags", "--jq", ".[].name"])
    tags = out.split() if out else []
    _tag_cache[repo] = tags
    return tags


def submodule_sha(repo: str, path: str) -> str | None:
    """The commit a submodule is pinned at on the default branch, or None.

    A submodule shows up in the git tree as a `commit`-type entry, so a normal
    contents fetch can't read it.
    """
    out = _gh(
        [
            "api",
            f"repos/{ORG}/{repo}/git/trees/HEAD?recursive=1",
            "--jq",
            f'.tree[] | select(.type=="commit" and .path=="{path}") | .sha',
        ]
    )
    return out.strip() if out and out.strip() else None


def tag_for_sha(repo: str, sha: str) -> str | None:
    """Map a commit sha back to a vX.Y.Z tag on that repo, or None.

    None is meaningful, not merely unknown: it means the pinned commit is not a
    released tag — either an unreleased commit or (the dangerous case) an object
    reachable from no branch at all.

    Uses ``repos/:repo/tags`` because it reports the DEREFERENCED ``commit.sha``.
    Release tags here are annotated (``git tag -a``), so ``git/ref/tags/:tag``
    would yield the tag-object sha instead, which never equals the commit sha a
    submodule pins — every lookup would miss and report a bogus non-tag-pin.
    """
    if repo not in _tag_sha_cache:
        out = _gh(
            [
                "api",
                "--paginate",
                f"repos/{ORG}/{repo}/tags",
                "--jq",
                ".[] | .commit.sha + \" \" + .name",
            ]
        )
        mapping: dict[str, str] = {}
        for line in (out or "").splitlines():
            parts = line.split()
            if len(parts) == 2 and SEMVER_TAG.match(parts[1]):
                mapping.setdefault(parts[0], parts[1])
        _tag_sha_cache[repo] = mapping
    return _tag_sha_cache[repo].get(sha)


def parse_gitmodules(text: str) -> dict[str, str]:
    """Map submodule path -> url from a .gitmodules file."""
    mods: dict[str, str] = {}
    path = None
    for raw in text.splitlines():
        line = raw.strip()
        if line.startswith("path"):
            path = line.split("=", 1)[1].strip() if "=" in line else None
        elif line.startswith("url") and path and "=" in line:
            mods[path] = line.split("=", 1)[1].strip()
            path = None
    return mods


def latest_semver_tag(repo: str) -> str | None:
    best: tuple[int, int, int] | None = None
    best_tag = None
    for tag in list_tags(repo):
        m = SEMVER_TAG.match(tag)
        if not m:
            continue
        key = tuple(int(g) for g in m.groups())  # type: ignore[assignment]
        if best is None or key > best:
            best, best_tag = key, tag
    return best_tag


def semver_key(tag: str) -> tuple[int, int, int] | None:
    m = SEMVER_TAG.match(tag)
    return tuple(int(g) for g in m.groups()) if m else None  # type: ignore[return-value]


# --------------------------------------------------------------------------
# Pure helpers.
# --------------------------------------------------------------------------


def extract_fetchcontent_pins(cmake_text: str) -> dict[str, str]:
    """Map dep-name-substring -> GIT_TAG from FetchContent_Declare blocks."""
    pins: dict[str, str] = {}
    for m in GIT_REPO_RE.finditer(cmake_text):
        url = m.group(1).strip().strip('"')
        # look ahead a few lines for the matching GIT_TAG
        window = cmake_text[m.end() : m.end() + 200]
        tm = GIT_TAG_RE.search(window)
        if not tm:
            continue
        pins[url] = tm.group(1).strip().strip('"')
    return pins


def strip_doc_banner(text: str) -> str:
    """Drop a leading provenance banner so two copies of a doc compare fairly.

    A vendored copy legitimately carries a banner the canonical file must not
    have -- "> **Vendored copy.** … Last synced: <date>" -- and the canonical
    one does not. Everything above the first real paragraph is provenance.

    This replaces an earlier version that also stripped lines beginning `#`,
    `*` and `//`. In a MARKDOWN doc those are content, not comments: it ate the
    canonical file's `# Title` and its `**Status:**` paragraph while stopping
    dead at the vendored copy's `>` banner. The two could therefore never hash
    equal no matter how faithfully the copy was synced, so the finding it
    raised was permanently un-actionable -- verified by re-hashing a
    byte-perfect re-sync, which still compared unequal.
    """
    lines = text.splitlines()
    i = 0
    while i < len(lines):
        stripped = lines[i].strip()
        if not stripped or stripped.startswith(("<!--", ">", "SPDX")):
            i += 1
            continue
        break
    return "\n".join(lines[i:]).strip()


def body_md5(text: str) -> str:
    return hashlib.md5(strip_doc_banner(text).encode("utf-8")).hexdigest()


def find_xr_version(text: str) -> str | None:
    m = XR_VER_RE.search(text)
    return ".".join(m.groups()) if m else None


def find_loader_version(text: str) -> str | None:
    m = OPENXR_VER_RE.search(text)
    return m.group(1) if m else None


# --------------------------------------------------------------------------
# Findings.
# --------------------------------------------------------------------------


@dataclass
class Finding:
    repo: str
    category: str
    detail: str


@dataclass
class Report:
    findings: list[Finding] = field(default_factory=list)
    notes: list[str] = field(default_factory=list)

    def add(self, repo: str, category: str, detail: str) -> None:
        self.findings.append(Finding(repo, category, detail))

    def note(self, msg: str) -> None:
        self.notes.append(msg)


# --------------------------------------------------------------------------
# Checks.
# --------------------------------------------------------------------------


def check_fetchcontent_pins(report: Report) -> None:
    common_pins: dict[str, str] = {}  # consumer -> common tag
    for consumer in PIN_CONSUMERS:
        merged: dict[str, str] = {}
        for cand in CMAKE_CANDIDATES:
            text = gh_raw(consumer, cand)
            if text:
                merged.update(extract_fetchcontent_pins(text))
        for url, tag in merged.items():
            for dep, needle in PIN_DEPS.items():
                if needle not in url:
                    continue
                if dep == "displayxr-common":
                    common_pins[consumer] = tag
                if (consumer, dep) in INTENTIONAL_LAG:
                    continue
                latest = latest_semver_tag(dep)
                pk, lk = semver_key(tag), semver_key(latest or "")
                # A pin that isn't a vX.Y.Z tag (raw SHA, branch name) can never
                # compare against `latest`, so the stale-pin check below silently
                # skips it. That is how displayxr-cef-host came to pin a commit
                # reachable from NO branch in displayxr-common — it resolved only
                # because GitHub still serves unreachable objects, and a GC would
                # have broken the build with no warning. Flag it explicitly: an
                # unrankable pin is a worse problem than a merely stale one.
                if pk is None:
                    report.add(
                        consumer,
                        "non-tag-pin",
                        f"pins {dep} @ {tag!r}, which is not a vX.Y.Z tag — "
                        f"cannot be drift-checked, and if it is a raw SHA it may "
                        f"be unreachable (latest tag is {latest or 'unknown'})",
                    )
                    continue
                if latest and lk and pk < lk:
                    report.add(
                        consumer,
                        "stale-pin",
                        f"pins {dep} @ {tag} but latest tag is {latest}",
                    )
    # Submodule-pinned consumers (invisible to the FetchContent scan above).
    for consumer in SUBMODULE_CONSUMERS:
        gitmodules = gh_raw(consumer, ".gitmodules")
        if not gitmodules:
            continue  # no submodules (or repo dropped them) — nothing to check
        for path, url in parse_gitmodules(gitmodules).items():
            dep = next((d for d, needle in PIN_DEPS.items() if needle in url), None)
            if dep is None or (consumer, dep) in INTENTIONAL_LAG:
                continue
            sha = submodule_sha(consumer, path)
            if not sha:
                report.note(f"submodule: {consumer}:{path} — could not read pinned sha; skipped")
                continue
            tag = tag_for_sha(dep, sha)
            if tag is None:
                report.add(
                    consumer,
                    "non-tag-pin",
                    f"submodule {path} pins {dep} @ {sha[:8]}, which matches no "
                    f"vX.Y.Z tag — unrankable, and possibly unreachable",
                )
                continue
            common_pins.setdefault(consumer, tag)
            latest = latest_semver_tag(dep)
            pk, lk = semver_key(tag), semver_key(latest or "")
            if latest and pk and lk and pk < lk:
                report.add(
                    consumer,
                    "stale-pin",
                    f"submodule {path} pins {dep} @ {tag} but latest tag is {latest}",
                )

    # common pin spread
    distinct = set(common_pins.values())
    if len(distinct) > 1:
        spread = ", ".join(f"{c}={t}" for c, t in sorted(common_pins.items()))
        for consumer in common_pins:
            report.add(
                consumer,
                "common-pin-spread",
                f"displayxr-common pins disagree across consumers: {spread}",
            )


def check_vendored_specs(report: Report) -> None:
    for consumer, cpath, canon_repo, canon_path in VENDORED_SPECS:
        copy = gh_raw(consumer, cpath)
        canon = gh_raw(canon_repo, canon_path)
        if copy is None or canon is None:
            report.note(
                f"vendored-spec: could not fetch {consumer}:{cpath} or "
                f"{canon_repo}:{canon_path} — skipped"
            )
            continue
        if body_md5(copy) != body_md5(canon):
            report.add(
                consumer,
                "vendored-spec-stale",
                f"{cpath} body differs from canonical {canon_repo}:{canon_path}",
            )


def check_openxr_matrix(report: Report) -> None:
    versions: dict[str, str] = {}
    for repo, path in OPENXR_HEADER_PATHS.items():
        text = gh_raw(repo, path)
        if not text:
            report.note(f"openxr-matrix: no header at {repo}:{path} — skipped")
            continue
        v = find_xr_version(text)
        if v:
            versions[repo] = v
    distinct = set(versions.values())
    if len(distinct) > 1:
        spread = ", ".join(f"{r}={v}" for r, v in sorted(versions.items()))
        # attribute the finding to the minority (out-of-line) repos
        majority = max(distinct, key=lambda v: list(versions.values()).count(v))
        for repo, v in versions.items():
            if v != majority:
                report.add(
                    repo,
                    "openxr-version-drift",
                    f"XR_CURRENT_API_VERSION={v}; majority is {majority} ({spread})",
                )


def check_demo_three_pins(report: Report) -> None:
    for repo in DEMO_REPOS:
        header = gh_raw(repo, OPENXR_HEADER_PATHS.get(repo, ""))
        header_ver = find_xr_version(header) if header else None
        loader_ver = None
        for cand in LOADER_PIN_CANDIDATES:
            t = gh_raw(repo, cand)
            if t and find_loader_version(t):
                loader_ver = find_loader_version(t)
                break
        ci_ver = None
        for cand in CI_CANDIDATES:
            t = gh_raw(repo, cand)
            if t and find_loader_version(t):
                ci_ver = find_loader_version(t)
                break
        pins = {"header": header_ver, "loader": loader_ver, "ci": ci_ver}
        present = {k: v for k, v in pins.items() if v}
        if len(set(present.values())) > 1:
            report.add(
                repo,
                "demo-three-pins",
                "OpenXR pins disagree: "
                + ", ".join(f"{k}={v}" for k, v in pins.items()),
            )
        elif len(present) < 3:
            report.note(
                f"three-pins: {repo} — could not locate all pins "
                f"({', '.join(f'{k}={v}' for k, v in pins.items())})"
            )


def check_prose_versions(report: Report, versions: dict[str, str]) -> None:
    for repo, path, key in PROSE_CHECKS:
        want = versions.get(key, "").lstrip("v")
        if not want:
            continue
        text = gh_raw(repo, path)
        if not text:
            report.note(f"prose: no {repo}:{path} — skipped")
            continue
        found = set(VXYZ_RE.findall(text))
        stale = {v for v in found if v != want and v.count(".") == 2}
        # Only flag versions that look like a runtime version (same major).
        stale = {v for v in stale if v.split(".")[0] == want.split(".")[0]}
        if stale:
            report.add(
                repo,
                "prose-version-drift",
                f"{path} mentions {sorted(stale)} but versions.json {key}=v{want}",
            )


def _dxr_specs(text: str) -> dict[str, int]:
    """{ext_lowercase: spec_version} from any C header-ish blob."""
    out: dict[str, int] = {}
    for ext, ver in DXR_SPEC_RE.findall(text or ""):
        ext = ext.lower()
        # A file may mention an extension more than once (a struct comment
        # re-stating an older SPEC_VERSION); the #define wins by being the only
        # thing this regex matches, and the highest wins if there are several.
        out[ext] = max(out.get(ext, 0), int(ver))
    return out


def runtime_spec(ext: str, ref: str = "HEAD") -> int | None:
    text = gh_raw("displayxr-runtime", RUNTIME_EXT_HEADER.format(ext=ext), ref)
    if text is None:
        return None
    return _dxr_specs(text).get(ext)


def first_release_with_spec(ext: str, want: int, tags: list[str]) -> str | None:
    """Earliest release tag whose XR_DXR_<ext> spec is >= want.

    Binary search, so this costs ~log2(len(tags)) fetches rather than one per
    tag -- the runtime has enough releases that the linear form would dominate
    the whole audit's request budget. Valid because SPEC_VERSIONs only ever
    climb (see _monotonicity in downstream-pins.json).
    """
    lo, hi, best = 0, len(tags) - 1, None
    while lo <= hi:
        mid = (lo + hi) // 2
        got = runtime_spec(ext, tags[mid])
        # Extension absent at that tag == predates it entirely: search right.
        if got is not None and got >= want:
            best = tags[mid]
            hi = mid - 1
        else:
            lo = mid + 1
    return best


def check_unregistered_pins(report: Report) -> None:
    """Runtime pins that exist downstream but no downstream-pins.json track claims (#1247).

    The bump job only ever touches locations the manifest names, and it does so
    silently -- an unclaimed pin is not "skipped", it is invisible. That is how
    displayxr-leia-plugin's Android track sat at v2.8.0 while its Windows track
    tracked current releases, and it surfaced only because a human went looking
    after a near-miss. This check turns "someone happened to look" into a
    standing weekly one.

    We deliberately scan only repos ALREADY in the manifest: the question is not
    "which repos pin the runtime" (that is a human decision) but "does a
    registered repo pin it somewhere we are not maintaining".
    """
    import re as _re

    man = json.loads((REPO_ROOT / "downstream-pins.json").read_text(encoding="utf-8"))
    pins = man.get("runtime_tag_pins", {})

    # set(KEY "v1.2.3"  /  KEY: v1.2.3 -- the two shapes the bump job understands.
    cmake_rx = _re.compile(r'set\(\s*(DXR_RUNTIME_GIT_TAG[A-Z0-9_]*)\s+"([^"]+)"')
    yaml_rx = _re.compile(r"(?m)^\s*(RUNTIME_REF[A-Z0-9_]*):\s*(\S+)")

    for repo, spec in sorted(pins.items()):
        claimed: set[tuple[str, str]] = set()
        files: set[str] = {"CMakeLists.txt"}
        for track in spec.get("tracks", {}).values():
            for loc in track.get("locations", []):
                claimed.add((loc["file"], loc["key"]))
                files.add(loc["file"])
        # Also look where a pin would plausibly be added without telling us.
        files.update(
            ".github/workflows/build-%s.yml" % p for p in ("windows", "linux", "android", "macos")
        )

        for f in sorted(files):
            text = gh_raw(repo, f)
            if text is None:
                continue  # absent workflow (or unreadable) -- not a finding
            rx = cmake_rx if f.endswith(("CMakeLists.txt", ".cmake")) else yaml_rx
            for m in rx.finditer(text):
                key, val = m.group(1), m.group(2)
                if (f, key) in claimed:
                    continue
                report.add(
                    repo,
                    "unregistered-runtime-pin",
                    "%s defines %s = %s, but no downstream-pins.json track claims it -- "
                    "runtime-pin-bump.yml will never repin it and nothing will notice it "
                    "going stale. Add a track (auto_bump=true), or auto_bump=false WITH a "
                    "recorded reason if it is meant to lag. See #1247." % (f, key, val),
                )


def check_consumer_floors(report: Report) -> None:
    """Reconcile each wire-protocol consumer's real extension requirements
    against the runtime, and against whatever minimum it advertises.

    See the consumer_floors block in downstream-pins.json for why this axis
    exists and what it caught.
    """
    manifest = json.loads((REPO_ROOT / "downstream-pins.json").read_text())
    floors = manifest.get("consumer_floors", {})

    tags = sorted(
        (t for t in list_tags("displayxr-runtime") if SEMVER_TAG.match(t)),
        key=semver_key,
    )
    if not tags:
        report.note("consumer-floors: could not list runtime release tags — skipped")
        return

    for repo, spec in floors.items():
        if repo.startswith("_"):
            continue

        # 1. What does this consumer actually require?
        requires: dict[str, int] = dict(spec.get("requires") or {})
        for path in spec.get("spec_sources") or []:
            text = gh_raw(repo, path)
            if text is None:
                report.note(f"consumer-floors: could not fetch {repo}:{path} — skipped")
                continue
            for ext, ver in _dxr_specs(text).items():
                requires[ext] = max(requires.get(ext, 0), ver)

        if not requires:
            report.note(f"consumer-floors: no requirements resolved for {repo} — skipped")
            continue

        # A hand-maintained number is the thing this block exists to distrust,
        # so anchor it to prose in the consumer's own tree where one is named.
        anchor_path = spec.get("requires_anchor")
        if anchor_path and spec.get("requires"):
            anchor_text = gh_raw(repo, anchor_path) or ""
            for ext, ver in (spec.get("requires") or {}).items():
                if not re.search(rf"v?{ver}\b", anchor_text):
                    report.note(
                        f"consumer-floors: {repo} declares {ext} spec {ver} in "
                        f"downstream-pins.json but {anchor_path} does not mention "
                        f"it — re-derive from the source of truth"
                    )

        # 2. Can the runtime still serve it?
        derived: list[tuple[str, str]] = []
        for ext, want in sorted(requires.items()):
            have = runtime_spec(ext)
            if have is None:
                report.add(
                    repo,
                    "consumer-floor-extension-gone",
                    f"requires XR_DXR_{ext} spec {want} but the runtime no longer "
                    f"ships that extension header",
                )
                continue
            if have < want:
                report.add(
                    repo,
                    "consumer-floor-unsatisfiable",
                    f"requires XR_DXR_{ext} spec {want} but runtime HEAD is at "
                    f"spec {have} — the runtime cannot serve this consumer",
                )
                continue
            tag = first_release_with_spec(ext, want, tags)
            if tag:
                derived.append((tag, f"XR_DXR_{ext} spec {want}"))

        # A behavioural floor is one the spec derivation above CANNOT reach: a runtime
        # change that bumped no SPEC_VERSION but that the shipped consumer still needs.
        # Real case (runtime#1347): runtime#1336 routes an opaque present-owner to
        # CLIENT_TEXTURE with no extension change at all, and the opaque browser does
        # not present correctly without it. Derivation reads spec versions, so it sees
        # nothing -- and an under-declared floor is exactly the failure this block
        # exists to catch, so it must not depend on the change happening to touch a
        # header. Folded into the same max() so the strictest floor wins.
        bf = spec.get("behavioural_floor")
        if isinstance(bf, dict) and bf.get("min_runtime"):
            derived.append((bf["min_runtime"], bf.get("why") or "a behavioural runtime change"))

        if not derived:
            continue

        # 3. Does the advertised minimum match the real one?
        floor_tag, floor_why = max(derived, key=lambda dv: semver_key(dv[0]))
        decl = spec.get("declared_floor")
        if not decl:
            continue
        text = gh_raw(repo, decl["file"])
        if text is None:
            report.note(
                f"consumer-floors: could not fetch {repo}:{decl['file']} — "
                f"declared floor unchecked (derived floor is {floor_tag})"
            )
            continue
        m = re.search(NSIS_DEFINE_RE.format(key=re.escape(decl["key"])), text)
        if not m:
            report.add(
                repo,
                "consumer-floor-unreadable",
                f"{decl['file']} has no {decl['key']} literal to check "
                f"(derived floor is {floor_tag}, from {floor_why})",
            )
            continue
        declared = m.group(1)
        if semver_key(f"v{declared}") is None:
            report.note(f"consumer-floors: {repo} {decl['key']}={declared} is not vX.Y.Z — skipped")
            continue
        if semver_key(f"v{declared}") < semver_key(floor_tag):
            # Under-declared is the dangerous direction: the consumer installs
            # happily onto a runtime it cannot actually work against.
            report.add(
                repo,
                "consumer-floor-understated",
                f"{decl['file']} declares {decl['key']}={declared}, but "
                f"{floor_why} first shipped in runtime {floor_tag} — users "
                f"between {declared} and {floor_tag} are told the prerequisite "
                f"is satisfied and get a broken install",
            )
        elif semver_key(f"v{declared}") > semver_key(floor_tag):
            # Overstated only costs needless upgrades, so it is a note.
            report.note(
                f"consumer-floors: {repo} {decl['key']}={declared} is stricter "
                f"than needed ({floor_why} shipped in {floor_tag})"
            )


# --------------------------------------------------------------------------
# Issue emission.
# --------------------------------------------------------------------------


def render_body(findings: list[Finding]) -> str:
    lines = [
        ISSUE_MARKER,
        "",
        "Automated cross-repo drift audit (`scripts/drift_audit.py`, epic #691 / #695).",
        "",
    ]
    for f in findings:
        lines.append(f"- **{f.category}**: {f.detail}")
    lines += ["", "_Re-run `scripts/drift_audit.py` after fixing to clear this issue._"]
    return "\n".join(lines)


def _first_error_line(stderr: str) -> str:
    for line in (stderr or "").splitlines():
        line = line.strip()
        if line:
            return line
    return "no stderr"


def emit_issue(repo: str, findings: list[Finding]) -> bool:
    """Open/update the drift issue on `repo`. True iff it actually landed."""
    title = "Drift audit: pin/spec/version drift detected"
    body = render_body(findings)
    existing = _gh(
        [
            "issue",
            "list",
            "--repo",
            f"{ORG}/{repo}",
            "--state",
            "open",
            "--search",
            ISSUE_MARKER,
            "--json",
            "number",
            "--jq",
            ".[0].number",
        ]
    )
    num = (existing or "").strip()
    if num:
        rc, _, err = _gh_full(
            ["issue", "edit", num, "--repo", f"{ORG}/{repo}", "--body", body]
        )
        if rc != 0:
            print(f"  FAILED to update {ORG}/{repo}#{num}: {_first_error_line(err)}")
            return False
        print(f"  updated {ORG}/{repo}#{num}")
        return True

    rc, _, err = _gh_full(
        ["issue", "create", "--repo", f"{ORG}/{repo}", "--title", title, "--body", body]
    )
    if rc != 0:
        print(f"  FAILED to open an issue on {ORG}/{repo}: {_first_error_line(err)}")
        return False
    print(f"  opened new issue on {ORG}/{repo}")
    return True


def emit_aggregate_issue(by_repo: dict[str, list[Finding]]) -> bool:
    """Fallback channel: one issue on THIS repo covering every offender.

    Per-repo issues need a cross-repo token. The default GITHUB_TOKEN cannot
    write to sibling repos, and the displayxr-publish-bot App has no `issues`
    permission at all -- so without DRIFT_AUDIT_TOKEN the designed channel
    cannot work. It reported success anyway. This fallback always works,
    because a workflow's own token can always file an issue on its own repo.
    """
    title = "Drift audit: pin/spec/version drift detected across the org"
    lines = [
        ISSUE_MARKER,
        "",
        "Aggregated because per-repo issue emission is unavailable — see the",
        "workflow log for the exact error. To restore per-repo issues, set the",
        "`DRIFT_AUDIT_TOKEN` secret to an org-scoped PAT with `issues: write`,",
        "or grant the `displayxr-publish-bot` App the `issues` permission.",
        "",
    ]
    for repo, findings in sorted(by_repo.items()):
        lines.append(f"### `{repo}`")
        for f in findings:
            lines.append(f"- **{f.category}** — {f.detail}")
        lines.append("")
    body = "\n".join(lines)

    existing = _gh(
        [
            "issue", "list", "--repo", f"{ORG}/displayxr-runtime", "--state", "open",
            "--search", ISSUE_MARKER, "--json", "number", "--jq", ".[0].number",
        ]
    )
    num = (existing or "").strip()
    if num:
        rc, _, err = _gh_full(
            ["issue", "edit", num, "--repo", f"{ORG}/displayxr-runtime", "--body", body]
        )
        target = f"{ORG}/displayxr-runtime#{num}"
    else:
        rc, _, err = _gh_full(
            [
                "issue", "create", "--repo", f"{ORG}/displayxr-runtime",
                "--title", title, "--body", body,
            ]
        )
        target = f"{ORG}/displayxr-runtime"
    if rc != 0:
        print(f"  FAILED to file the aggregate issue on {target}: {_first_error_line(err)}")
        return False
    print(f"  filed the aggregate drift issue on {target}")
    return True


# --------------------------------------------------------------------------
# Main.
# --------------------------------------------------------------------------


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dry-run", action="store_true", help="print only, never emit issues")
    ap.add_argument("--emit-issues", action="store_true", help="open/update issues on offenders")
    args = ap.parse_args()

    versions = json.loads((REPO_ROOT / "versions.json").read_text())

    report = Report()
    check_fetchcontent_pins(report)
    check_vendored_specs(report)
    check_openxr_matrix(report)
    check_demo_three_pins(report)
    check_prose_versions(report, versions)
    check_consumer_floors(report)
    check_unregistered_pins(report)

    # Report
    by_repo: dict[str, list[Finding]] = {}
    for f in report.findings:
        by_repo.setdefault(f.repo, []).append(f)

    print("=" * 70)
    print("DisplayXR cross-repo drift audit")
    print("=" * 70)
    if not report.findings:
        print("No drift detected. OK")
    for repo, fs in sorted(by_repo.items()):
        print(f"\n{repo}:")
        for f in fs:
            print(f"  [{f.category}] {f.detail}")
    if report.notes:
        print("\nNotes (could not fully check):")
        for n in report.notes:
            print(f"  - {n}")

    if args.emit_issues and not args.dry_run:
        if not by_repo:
            print("\nNo findings — nothing to emit.")
            return 0

        print("\nEmitting issues:")
        failed = {repo: fs for repo, fs in sorted(by_repo.items()) if not emit_issue(repo, fs)}

        # In emit mode the issues ARE the deliverable, so the job's exit code
        # tracks whether the findings actually reached a human -- NOT whether
        # drift exists. Previously it returned 0 unconditionally while
        # emit_issue printed "opened new issue" without checking, so two
        # months of weekly runs went green having filed precisely nothing.
        if not failed:
            return 0

        print(
            f"::warning::per-repo issue emission failed for "
            f"{', '.join(sorted(failed))} — falling back to one aggregate issue "
            f"on displayxr-runtime. Set DRIFT_AUDIT_TOKEN (org-scoped PAT with "
            f"issues:write) to restore per-repo issues."
        )
        if emit_aggregate_issue(failed):
            return 0

        print("::error::drift was found but could not be reported anywhere.")
        return 1

    # Report / dry-run mode: non-zero exit is the gate signal.
    return 1 if report.findings else 0


if __name__ == "__main__":
    raise SystemExit(main())

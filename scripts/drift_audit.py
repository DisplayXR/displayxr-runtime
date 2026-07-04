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
]
PIN_CONSUMERS = [
    "displayxr-runtime",
    "displayxr-shell-pvt",
    "displayxr-leia-plugin",
    "displayxr-cef-host",
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


def _gh(args: list[str]) -> str | None:
    try:
        out = subprocess.run(
            ["gh", *args],
            capture_output=True,
            text=True,
            check=False,
        )
    except FileNotFoundError:
        sys.exit("error: the 'gh' CLI is required and was not found on PATH")
    if out.returncode != 0:
        return None
    return out.stdout


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


def strip_license_header(text: str) -> str:
    """Drop a leading comment/license block so bodies compare fairly."""
    lines = text.splitlines()
    i = 0
    while i < len(lines) and (
        not lines[i].strip()
        or lines[i].lstrip().startswith(("<!--", "//", "#", "*", "/*", "SPDX"))
    ):
        i += 1
    return "\n".join(lines[i:]).strip()


def body_md5(text: str) -> str:
    return hashlib.md5(strip_license_header(text).encode("utf-8")).hexdigest()


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
                if latest and pk and lk and pk < lk:
                    report.add(
                        consumer,
                        "stale-pin",
                        f"pins {dep} @ {tag} but latest tag is {latest}",
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


def emit_issue(repo: str, findings: list[Finding]) -> None:
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
        _gh(["issue", "edit", num, "--repo", f"{ORG}/{repo}", "--body", body])
        print(f"  updated {ORG}/{repo}#{num}")
    else:
        _gh(
            [
                "issue",
                "create",
                "--repo",
                f"{ORG}/{repo}",
                "--title",
                title,
                "--body",
                body,
            ]
        )
        print(f"  opened new issue on {ORG}/{repo}")


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

    # Report
    by_repo: dict[str, list[Finding]] = {}
    for f in report.findings:
        by_repo.setdefault(f.repo, []).append(f)

    print("=" * 70)
    print("DisplayXR cross-repo drift audit")
    print("=" * 70)
    if not report.findings:
        print("No drift detected. ✓")
    for repo, fs in sorted(by_repo.items()):
        print(f"\n{repo}:")
        for f in fs:
            print(f"  [{f.category}] {f.detail}")
    if report.notes:
        print("\nNotes (could not fully check):")
        for n in report.notes:
            print(f"  - {n}")

    if args.emit_issues and not args.dry_run:
        print("\nEmitting issues:")
        for repo, fs in sorted(by_repo.items()):
            emit_issue(repo, fs)
        # In emit mode the issues ARE the deliverable — don't also fail the job.
        return 0

    # Report / dry-run mode: non-zero exit is the gate signal.
    return 1 if report.findings else 0


if __name__ == "__main__":
    raise SystemExit(main())

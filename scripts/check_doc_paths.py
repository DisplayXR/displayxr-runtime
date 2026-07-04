#!/usr/bin/env python3
"""Lint relative links in the docs for dead targets.

Epic #691 item 10: README-referenced paths that no longer exist (a stale ADR
index, a pruned roadmap doc still linked, a moved spec) should fail CI instead
of rotting silently. This walks the Markdown docs and reports any *relative*
link whose target file/dir does not exist on disk.

Scope: ``docs/**/*.md`` plus the top-level ``README.md`` and ``CLAUDE.md``.
Skipped: absolute URLs (``http(s):``, ``mailto:`` …), pure ``#anchor`` links,
and in-page/section anchors (the ``#frag`` part of any link is stripped before
resolving — we validate the *file* exists, not the fragment).

Exit 0 if every relative link resolves, else 1 with a per-file report.
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent

# Inline Markdown links/images: [text](target) and ![alt](target).
LINK_RE = re.compile(r"!?\[[^\]]*\]\(\s*([^)]+?)\s*\)")

SKIP_PREFIXES = ("http://", "https://", "mailto:", "tel:", "#", "//", "data:")


# Inherited Monado docs are unmaintained and use Doxygen link syntax
# ([text](@ref symbol)), which is not a filesystem path — exclude the tree.
EXCLUDE_DIRS = (REPO / "docs" / "legacy-monado",)


def doc_files() -> list[Path]:
    files = [
        p
        for p in sorted((REPO / "docs").rglob("*.md"))
        if not any(ex in p.parents for ex in EXCLUDE_DIRS)
    ]
    for extra in ("README.md", "CLAUDE.md"):
        p = REPO / extra
        if p.exists():
            files.append(p)
    return files


def link_targets(text: str) -> list[str]:
    out = []
    for raw in LINK_RE.findall(text):
        target = raw.strip()
        # Drop an optional link title:  (path "Title")
        if " " in target and not target.startswith("<"):
            target = target.split(" ", 1)[0]
        target = target.strip("<>")
        out.append(target)
    return out


def is_local(target: str) -> bool:
    if not target:
        return False
    return not target.startswith(SKIP_PREFIXES)


def main() -> int:
    missing: list[tuple[Path, str]] = []
    checked = 0
    for md in doc_files():
        base = md.parent
        for target in link_targets(md.read_text(encoding="utf-8")):
            if not is_local(target):
                continue
            # Strip anchor / query — we only validate the file exists.
            path_part = target.split("#", 1)[0].split("?", 1)[0]
            if not path_part:
                continue  # pure in-page anchor
            checked += 1
            resolved = (base / path_part).resolve()
            if not resolved.exists():
                missing.append((md.relative_to(REPO), target))

    if missing:
        print(f"Dead relative links found ({len(missing)} of {checked} checked):")
        for src, target in missing:
            print(f"  {src} -> {target}")
        return 1
    print(f"All {checked} relative doc links resolve. ✓")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

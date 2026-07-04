#!/usr/bin/env python3
"""Generate the ADR index from ``docs/adr/ADR-*.md``.

The ADR list used to be hand-maintained in ``docs/README.md`` and drifted — a
new ADR-032 landed on disk while the index still stopped at 031 (epic #691,
item 10). This script makes the index a *derived artifact* so it can never
drift again:

  1. ``docs/adr/README.md``  — canonical standalone ADR index (fully generated).
  2. the ADR list in ``docs/README.md``, between the sentinels
         ``<!-- BEGIN ADR INDEX -->`` … ``<!-- END ADR INDEX -->``

Run with no args to (re)write both. Run with ``--check`` to verify they are up
to date without writing (exit 1 if stale) — this is what CI runs, so a new ADR
that isn't indexed fails the build.

Each entry's title is the text after ``ADR-NNN:`` in the file's ``# `` heading,
so the ADR file is the single source of truth for its own title.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ADR_DIR = REPO / "docs" / "adr"
DOCS_README = REPO / "docs" / "README.md"
ADR_README = ADR_DIR / "README.md"

BEGIN = "<!-- BEGIN ADR INDEX -->"
END = "<!-- END ADR INDEX -->"

ADR_FILE_RE = re.compile(r"^ADR-(\d{3})-.*\.md$")
HEADING_RE = re.compile(r"^#\s+ADR-(\d{3}):\s*(.+?)\s*$", re.M)

GEN_NOTE = (
    "> **Auto-generated** by `scripts/gen_adr_index.py` from the `ADR-*.md`\n"
    "> files in this directory. Do not edit by hand — add or edit an ADR and\n"
    "> re-run the script (CI enforces it via `--check`)."
)


def collect() -> list[tuple[str, str, str]]:
    """Return (number, title, filename) for every ADR-NNN-*.md, sorted."""
    entries: list[tuple[str, str, str]] = []
    for path in sorted(ADR_DIR.glob("ADR-*.md")):
        m = ADR_FILE_RE.match(path.name)
        if not m:
            continue  # skip README.md and anything not matching the pattern
        num = m.group(1)
        text = path.read_text(encoding="utf-8")
        hm = HEADING_RE.search(text)
        if not hm:
            sys.exit(f"error: {path.name}: no '# ADR-NNN: Title' heading found")
        if hm.group(1) != num:
            sys.exit(
                f"error: {path.name}: filename number {num} != heading "
                f"number {hm.group(1)}"
            )
        entries.append((num, hm.group(2).strip(), path.name))
    if not entries:
        sys.exit("error: no ADR files found under docs/adr/")
    return entries


def render_list(entries: list[tuple[str, str, str]], link_prefix: str) -> str:
    return "\n".join(
        f"- [ADR-{num}]({link_prefix}{fname}) — {title}"
        for num, title, fname in entries
    )


def build_adr_readme(entries: list[tuple[str, str, str]]) -> str:
    return (
        "# Architecture Decision Records\n\n"
        f"{GEN_NOTE}\n\n"
        f"{render_list(entries, '')}\n"
    )


def build_docs_readme(entries: list[tuple[str, str, str]], current: str) -> str:
    if BEGIN not in current or END not in current:
        sys.exit(
            f"error: {DOCS_README} is missing the ADR index sentinels\n"
            f"       add '{BEGIN}' and '{END}' around the ADR list first"
        )
    b = current.index(BEGIN)
    e = current.index(END)
    block = render_list(entries, "adr/")
    return current[: b + len(BEGIN)] + "\n" + block + "\n" + current[e:]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--check",
        action="store_true",
        help="verify the index is up to date; exit 1 if stale (no writes)",
    )
    args = ap.parse_args()

    entries = collect()
    want_adr = build_adr_readme(entries)
    want_docs = build_docs_readme(entries, DOCS_README.read_text(encoding="utf-8"))

    targets = [(ADR_README, want_adr), (DOCS_README, want_docs)]

    if args.check:
        stale = [p for p, want in targets if p.read_text(encoding="utf-8") != want]
        if stale:
            print("ADR index is stale — run: python3 scripts/gen_adr_index.py")
            for p in stale:
                print(f"  - {p.relative_to(REPO)}")
            return 1
        print(f"ADR index up to date ({len(entries)} ADRs). ✓")
        return 0

    for p, want in targets:
        p.write_text(want, encoding="utf-8")
        print(f"wrote {p.relative_to(REPO)}")
    print(f"indexed {len(entries)} ADRs (ADR-{entries[0][0]}..ADR-{entries[-1][0]}).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

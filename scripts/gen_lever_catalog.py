#!/usr/bin/env python3
"""Generate the CLI's Tier 1 + Tier 2 lever catalog from the design doc's census.

`docs/roadmap/control-panel-performance-settings.md` Appendix A is the census of
every `DXR_*` name the runtime reads, with a tier per name. Its Tier 1 (next
launch) and Tier 2 (service-scoped) rows are exactly what the Dashboard's
read-only "Developer settings" list shows (`displayxr-cli perf list --all`,
perf-settings Phase 2A). The list must never be hand-typed a second time, so
this script turns the census into a checked-in C header:

  docs/roadmap/control-panel-performance-settings.md  (Appendix A)
      -> src/xrt/targets/cli/cli_lever_catalog.h

Rules:
  * Only tables with a `Tier` column are read; a row whose Tier cell is not 1 or
    2 is skipped (Tier 3 / 4 never appear in the list; a non-digit cell is
    reported).
  * A Var cell naming several `DXR_*` variables ("`A` / `B`", "`A` (alias `B`)")
    yields one entry per name. Non-`DXR_` names (Android properties) are ignored.
  * description = first sentence of "What it does" (two when the first is very
    short), markdown stripped, capped;
    default = the Default cell, markdown stripped. Everything is transliterated
    to ASCII (the Windows console is CP437; generated files stay ASCII).
  * Output is sorted by tier, then name, so it is deterministic.

`--check` (run by the lint workflow) fails when the checked-in header differs
from what the census generates, AND when a catalog name no longer occurs as a
string literal anywhere under src/xrt (Appendix B's grep): a stale census row
would otherwise put a lever in the list that nothing reads.
"""
from __future__ import annotations

import argparse
import re
import sys
import unicodedata
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DOC = REPO / "docs" / "roadmap" / "control-panel-performance-settings.md"
OUT = REPO / "src" / "xrt" / "targets" / "cli" / "cli_lever_catalog.h"
SRC = REPO / "src" / "xrt"
SRC_EXTS = {".c", ".cpp", ".h", ".hpp", ".m", ".mm"}

DESC_MAX = 200
NAME_RE = re.compile(r"\bDXR_[A-Z0-9_]+\b")
CELL_SPLIT = re.compile(r"(?<!\\)\|")

TRANSLIT = {
    "—": "-", "–": "-", "−": "-", "‑": "-", "×": "x", "→": "->",
    "←": "<-", "⟹": "=>", "⇒": "=>", "≥": ">=", "≤": "<=", "≠": "!=",
    "∝": "~", "≈": "~", "“": '"', "”": '"', "‘": "'", "’": "'",
    "…": "...", "µ": "u", "±": "+/-", " ": " ", "•": "*", "·": "-",
    "°": " deg", "✓": "ok",
}


def ascii_only(s: str) -> str:
    out = []
    for ch in s:
        if ord(ch) < 128:
            out.append(ch)
        elif ch in TRANSLIT:
            out.append(TRANSLIT[ch])
        else:
            dec = unicodedata.normalize("NFKD", ch).encode("ascii", "ignore").decode("ascii")
            out.append(dec if dec else "?")
    return "".join(out)


def strip_md(s: str) -> str:
    s = s.replace("\\|", "|")
    s = re.sub(r"\[([^\]]*)\]\([^)]*\)", r"\1", s)  # [text](url) -> text
    s = s.replace("**", "").replace("`", "")
    s = re.sub(r"(?<![A-Za-z0-9])_([^_]+)_(?![A-Za-z0-9])", r"\1", s)  # _emph_ (not DXR_FOO_BAR)
    s = re.sub(r"\s+", " ", s)
    return ascii_only(s).strip()


ABBREV = ("e.g.", "i.e.", "vs.", "etc.", "approx.", "cf.")


SENTENCE_MIN = 40


def first_sentence(s: str) -> str:
    """The first sentence; a very short one ("#1488 kill switch.") takes the next one too."""
    for m in re.finditer(r"[.!?](?=\s)", s):
        end = m.end()
        tail = s[:end].lower()
        if any(tail.endswith(a) for a in ABBREV):
            continue
        if end < SENTENCE_MIN:
            continue
        return s[:end].strip()
    return s.strip()


def cap(s: str, n: int = DESC_MAX) -> str:
    if len(s) <= n:
        return s
    cut = s[: n - 3]
    sp = cut.rfind(" ")
    if sp > n // 2:
        cut = cut[:sp]
    return cut.rstrip(" ,;:-") + "..."


def split_row(line: str) -> list[str]:
    line = line.strip()
    if line.startswith("|"):
        line = line[1:]
    if line.endswith("|") and not line.endswith("\\|"):
        line = line[:-1]
    return [c.strip() for c in CELL_SPLIT.split(line)]


def parse_census(text: str):
    """Return (entries, skipped). entries: list of dicts; skipped: list of (names, reason)."""
    start = text.find("## Appendix A")
    end = text.find("## Appendix B")
    if start < 0 or end < 0:
        raise SystemExit("gen_lever_catalog: Appendix A / B headings not found in " + str(DOC))
    lines = text[start:end].splitlines()

    entries: dict[str, dict] = {}
    skipped: list[tuple[str, str]] = []
    header: list[str] | None = None
    for line in lines:
        if not line.lstrip().startswith("|"):
            header = None
            continue
        cells = split_row(line)
        if header is None:
            header = cells
            continue
        if all(re.fullmatch(r":?-{3,}:?", c) for c in cells if c):
            continue  # separator row
        if "Tier" not in header or "Var" not in header:
            continue  # not a lever table (e.g. "Test / dev — never exposed", property tables)

        cols = list(header)
        if len(cells) == len(cols) - 1 and "Mechanism" in cols:
            cols.remove("Mechanism")  # a few Lifecycle rows fold the mechanism into Read site
        if len(cells) > len(cols):
            cells = cells[: len(cols) - 1] + [" | ".join(cells[len(cols) - 1 :])]
        if len(cells) != len(cols):
            skipped.append((cells[0] if cells else "?", f"{len(cells)} cells for {len(cols)} columns"))
            continue
        row = dict(zip(cols, cells))
        names = NAME_RE.findall(row["Var"].replace("`", " "))
        if not names:
            continue
        tier_cell = strip_md(row["Tier"])
        if not re.fullmatch(r"[0-9]", tier_cell):
            skipped.append((" / ".join(names), f"Tier cell is {tier_cell!r}, not a single digit"))
            continue
        tier = int(tier_cell)
        if tier not in (1, 2):
            continue
        what = strip_md(row.get("What it does", ""))
        desc = cap(first_sentence(what))
        for name in dict.fromkeys(names):  # ordered de-dup
            if name in entries:
                skipped.append((name, "listed twice; first row kept"))
                continue
            entries[name] = {
                "name": name,
                "tier": tier,
                "proc": strip_md(row.get("Proc", "")),
                "default": strip_md(row.get("Default", "")),
                "description": desc,
            }
    ordered = sorted(entries.values(), key=lambda e: (e["tier"], e["name"]))
    return ordered, skipped


def c_str(s: str) -> str:
    s = ascii_only(s)
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def render(entries: list[dict]) -> str:
    n1 = sum(1 for e in entries if e["tier"] == 1)
    n2 = sum(1 for e in entries if e["tier"] == 2)
    out = [
        "// Copyright 2026, The DisplayXR Project",
        "// SPDX-License-Identifier: BSL-1.0",
        "/*!",
        " * @file",
        " * @brief  Tier 1 + Tier 2 runtime levers, for `displayxr-cli perf list --all`.",
        " *",
        " * GENERATED by scripts/gen_lever_catalog.py from Appendix A (the census) of",
        " * docs/roadmap/control-panel-performance-settings.md -- do not edit by hand.",
        " * Edit the census and re-run the script; the lint workflow runs it with",
        " * --check, which also fails when a name here no longer occurs in src/xrt.",
        " *",
        f" * {len(entries)} levers: {n1} Tier 1 (next launch), {n2} Tier 2 (service-scoped).",
        " *",
        " * This is a DESCRIPTION of the levers, not a way to set them: only the names",
        " * u_setting_is_managed() accepts resolve from the per-user / machine stores.",
        " * Every other lever here is read by its own getenv() at its call site and is",
        " * environment-only (perf-settings Phase 2A is read-only by design).",
        " */",
        "",
        "#pragma once",
        "",
        "#include <stddef.h>",
        "#include <string.h>",
        "",
        "struct cli_lever",
        "{",
        "\tconst char *name;         //!< The environment variable.",
        "\tint tier;                 //!< 1 = next launch, 2 = service-scoped.",
        "\tconst char *proc;         //!< Which process reads it (census Proc column).",
        "\tconst char *default_text; //!< The compiled default, as the census words it.",
        "\tconst char *description;  //!< First sentence of the census's \"What it does\".",
        "};",
        "",
        "// clang-format off",
        "static const struct cli_lever cli_lever_catalog[] = {",
    ]
    for e in entries:
        out.append(
            "\t{" + ", ".join([c_str(e["name"]), str(e["tier"]), c_str(e["proc"]), c_str(e["default"]),
                               c_str(e["description"])]) + "},"
        )
    out += [
        "};",
        "// clang-format on",
        "",
        "#define CLI_LEVER_CATALOG_COUNT (sizeof(cli_lever_catalog) / sizeof(cli_lever_catalog[0]))",
        "",
        "//! The catalog entry for @p name, or NULL.",
        "static inline const struct cli_lever *",
        "cli_lever_find(const char *name)",
        "{",
        "\tif (name == NULL) {",
        "\t\treturn NULL;",
        "\t}",
        "\tfor (size_t i = 0; i < CLI_LEVER_CATALOG_COUNT; i++) {",
        "\t\tif (strcmp(cli_lever_catalog[i].name, name) == 0) {",
        "\t\t\treturn &cli_lever_catalog[i];",
        "\t\t}",
        "\t}",
        "\treturn NULL;",
        "}",
        "",
    ]
    return "\n".join(out)


def source_literals() -> set[str]:
    found: set[str] = set()
    lit = re.compile(r'"(DXR_[A-Z0-9_]+)"')
    for p in SRC.rglob("*"):
        if p.suffix in SRC_EXTS and p.is_file():
            try:
                found.update(lit.findall(p.read_text(encoding="utf-8", errors="replace")))
            except OSError:
                pass
    return found


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="fail if the header is stale or a name is gone from src/xrt")
    ap.add_argument("--github", action="store_true", help="emit ::error:: annotations")
    args = ap.parse_args()

    entries, skipped = parse_census(DOC.read_text(encoding="utf-8"))
    text = render(entries)
    for names, why in skipped:
        print(f"note: skipped {names}: {why}", file=sys.stderr)

    rel_out = OUT.relative_to(REPO).as_posix()
    if not args.check:
        OUT.write_text(text, encoding="ascii", newline="\n")
        n1 = sum(1 for e in entries if e["tier"] == 1)
        print(f"wrote {rel_out}: {len(entries)} levers ({n1} Tier 1, {len(entries) - n1} Tier 2)")
        return 0

    ok = True
    current = OUT.read_text(encoding="utf-8") if OUT.exists() else ""
    if current.replace("\r\n", "\n") != text:
        ok = False
        msg = f"{rel_out} is out of date with the census; run: python3 scripts/gen_lever_catalog.py"
        print(f"::error file={rel_out}::{msg}" if args.github else f"FAIL: {msg}")

    lits = source_literals()
    stale = [e["name"] for e in entries if e["name"] not in lits]
    if stale:
        ok = False
        for name in stale:
            msg = (f"{name} is a Tier {next(e['tier'] for e in entries if e['name'] == name)} lever in the census "
                   "but no longer occurs as a string literal under src/xrt (stale census row)")
            print(f"::error file={DOC.relative_to(REPO).as_posix()}::{msg}" if args.github else f"FAIL: {msg}")
    if ok:
        print(f"{rel_out} matches the census ({len(entries)} levers, all present in src/xrt).")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

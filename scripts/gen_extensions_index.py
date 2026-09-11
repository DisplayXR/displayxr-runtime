#!/usr/bin/env python3
"""Generate the published `displayxr-extensions` surfaces from the headers.

The mirror's `README.md` used to be a frozen heredoc inside
`publish-extensions.yml`: it documented 5 of the 16 published extensions, and
nothing noticed for months (displayxr-extensions#2). The set of extensions is
mechanical — it is whatever `XR_DXR_*.h` headers exist — so it should never
have been hand-listed. Only the *notes* about each one need a human.

This script splits those two things apart:

  headers  (src/external/openxr_includes/openxr/XR_DXR_*.h)   -> the SET
  manifest (docs/specs/extensions/index.json)                 -> the NOTES

and hard-fails when they disagree, so a new header cannot land without a note
and a note cannot outlive its header. `--check` is what the `lint` workflow
runs on every PR; `--out DIR` is what `publish-extensions.yml` runs to write
the mirror's two generated files:

  README.md        — grouped tables, one row per extension, with SPEC_VERSION
                     and a link to the formal spec where one exists.
  extensions.json  — the same data, machine-readable, consumed by
                     displayxr-website (it merges longer editorial prose by
                     name and falls back to `summary`, so an extension can
                     never go missing from displayxr.org/extensions).

`XR_DXR_result_codes.h` is a shared header, not an extension: it defines no
`*_EXTENSION_NAME`, which is exactly how it is excluded here (no name list to
keep in sync).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
HEADER_DIR = REPO / "src" / "external" / "openxr_includes" / "openxr"
MANIFEST = REPO / "docs" / "specs" / "extensions" / "index.json"
SPEC_DIR = REPO / "docs" / "specs" / "extensions"

RUNTIME_URL = "https://github.com/DisplayXR/displayxr-runtime"
SPEC_URL_BASE = f"{RUNTIME_URL}/blob/main/docs/specs/extensions"

NAME_RE = re.compile(r'^#define\s+XR_DXR_[A-Z0-9_]+_EXTENSION_NAME\s+"(XR_DXR_\w+)"', re.M)
VERSION_RE = re.compile(r"^#define\s+(XR_DXR_\w+)_SPEC_VERSION\s+(\d+)", re.M)


def scan_headers() -> dict[str, dict]:
    """Return {extension name: {header, spec_version}} for every XR_DXR_*.h."""
    found: dict[str, dict] = {}
    for path in sorted(HEADER_DIR.glob("XR_DXR_*.h")):
        text = path.read_text(encoding="utf-8")
        names = NAME_RE.findall(text)
        if not names:
            continue  # shared header (XR_DXR_result_codes.h), not an extension
        for name in names:
            vm = next((m for m in VERSION_RE.finditer(text) if m.group(1) == name), None)
            if vm is None:
                sys.exit(f"error: {path.name}: {name} has no {name}_SPEC_VERSION")
            if name in found:
                sys.exit(f"error: {name} defined in two headers")
            found[name] = {"header": path.name, "spec_version": int(vm.group(2))}
    if not found:
        sys.exit(f"error: no XR_DXR_* extension headers found under {HEADER_DIR}")
    return found


def load_manifest() -> tuple[list[dict], dict[str, dict]]:
    data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    groups = data["groups"]
    return groups, data["extensions"]


def reconcile(
    headers: dict[str, dict],
    groups: list[dict],
    notes: dict[str, dict],
    github: bool = False,
) -> list[dict]:
    """Join headers with their notes; exit non-zero on any drift."""
    errors: list[str] = []
    group_ids = {g["id"] for g in groups}

    for name in sorted(set(headers) - set(notes)):
        errors.append(
            f"{name}: header {headers[name]['header']} has no entry in "
            f"docs/specs/extensions/index.json — add one (group, title, summary)"
        )
    for name in sorted(set(notes) - set(headers)):
        errors.append(
            f"{name}: entry in docs/specs/extensions/index.json names no published "
            f"header — remove it, or restore {name}.h"
        )
    for name, note in sorted(notes.items()):
        if note.get("group") not in group_ids:
            errors.append(f"{name}: unknown group {note.get('group')!r}")
        for field in ("title", "summary"):
            if not note.get(field):
                errors.append(f"{name}: missing {field!r}")

    if errors:
        for e in errors:
            print(f"::error::{e}" if github else f"error: {e}", file=sys.stderr)
        sys.exit(1)

    out = []
    for name, h in sorted(headers.items()):
        note = notes[name]
        spec = SPEC_DIR / f"{name}.md"
        out.append(
            {
                "name": name,
                "title": note["title"],
                "summary": note["summary"],
                "group": note["group"],
                "specVersion": h["spec_version"],
                "header": f"include/openxr/{h['header']}",
                "specUrl": f"{SPEC_URL_BASE}/{name}.md" if spec.exists() else None,
            }
        )
    return out


def render_readme(groups: list[dict], exts: list[dict]) -> str:
    by_group = {g["id"]: [e for e in exts if e["group"] == g["id"]] for g in groups}
    lines = [
        "# DisplayXR Extensions",
        "",
        "OpenXR extension headers for 3D-display runtimes. **Auto-published** from "
        f"[`displayxr-runtime`]({RUNTIME_URL}).",
        "",
        f"{len(exts)} extensions, in {len([g for g in groups if by_group[g['id']]])} groups: what the "
        "runtime tells apps about the display, how an app drives the view math and 2D/3D "
        "compositing, how it hands the runtime its native window, the surface a swappable "
        "workspace controller uses, the app-agent bridge, and frame capture.",
        "",
        "`Spec` links the formal specification where one is written; a dash means the header "
        "is the specification for now.",
        "",
    ]
    for g in groups:
        items = by_group[g["id"]]
        if not items:
            continue
        lines += [f"## {g['title']}", "", g["blurb"], ""]
        lines += [
            "| Extension | Header | Ver | Spec | Description |",
            "|---|---|:--:|:--:|---|",
        ]
        for e in items:
            spec = f"[spec]({e['specUrl']})" if e["specUrl"] else "—"
            lines.append(
                f"| `{e['name']}` | [`{Path(e['header']).name}`]({e['header']}) | "
                f"{e['specVersion']} | {spec} | {e['summary']} |"
            )
        lines.append("")

    lines += [
        "## Usage",
        "",
        "Copy the headers into your project's OpenXR include path, or add this repo as a submodule:",
        "",
        "```bash",
        "git submodule add https://github.com/DisplayXR/displayxr-extensions.git external/displayxr-extensions",
        "# Then add external/displayxr-extensions/include to your include path",
        "```",
        "",
        "`extensions.json` at the repo root is the same table, machine-readable — name, group, "
        "title, summary, `specVersion`, header path and spec URL — for tooling that wants the "
        "set without parsing headers.",
        "",
        "## Full Documentation",
        "",
        f"- [Extension Specs]({RUNTIME_URL}/tree/main/docs/specs/extensions) — formal specifications with examples",
        f"- [Getting Started]({RUNTIME_URL}/tree/main/docs/getting-started) — build and integrate with DisplayXR",
        f"- [displayxr-runtime]({RUNTIME_URL}) — the OpenXR runtime",
        "",
        "## Provisional naming",
        "",
        "`DXR` is DisplayXR's Khronos-registered OpenXR author ID. The `XR_DXR_*` extensions "
        "themselves are **provisional** — they are not yet registered in the Khronos OpenXR "
        "registry: extension numbers and `XrStructureType` values sit in a provisional "
        "experimental block (`1004999xxx`) pending official assignment. Extension names are "
        "expected to be stable; numeric values are not. (These extensions previously shipped "
        "under provisional `XR_EXT_*` names; the rename did not restart `SPEC_VERSION` — the "
        "interface history continues.) See [GOVERNANCE.md](GOVERNANCE.md).",
        "",
        "## License",
        "",
        "[Apache-2.0](LICENSE). These headers are original, DisplayXR-authored extension "
        "definitions — the same license the Khronos OpenXR headers use, chosen for its "
        "explicit patent grant on the path to Khronos ratification. The per-file "
        "`SPDX-License-Identifier` is authoritative.",
        "",
        "## Auto-Published",
        "",
        "Headers, this README and `extensions.json` are all generated from "
        "`displayxr-runtime` on every push to its main — the header set from "
        "`src/external/openxr_includes/openxr/XR_DXR_*.h`, the per-extension notes from "
        "`docs/specs/extensions/index.json`, by `scripts/gen_extensions_index.py`. Do not "
        "edit them here — changes will be overwritten. Open PRs against the runtime; a header "
        "whose note is missing fails that repo's `lint` workflow, which is why this table "
        "cannot fall behind the headers again.",
        "",
    ]
    return "\n".join(lines)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--out",
        metavar="DIR",
        help="write README.md + extensions.json into DIR (the extensions-repo clone)",
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="only verify headers and docs/specs/extensions/index.json agree",
    )
    ap.add_argument("--github", action="store_true", help="emit ::error:: annotations")
    args = ap.parse_args()

    groups, notes = load_manifest()
    exts = reconcile(scan_headers(), groups, notes, github=args.github)

    if args.check or not args.out:
        missing = [e["name"] for e in exts if not e["specUrl"]]
        print(f"{len(exts)} extensions, all with notes in {MANIFEST.relative_to(REPO)}. ✓")
        if missing:
            print(f"note: {len(missing)} without a formal spec yet: {', '.join(missing)}")
        return

    out = Path(args.out)
    (out / "README.md").write_text(render_readme(groups, exts), encoding="utf-8")
    (out / "extensions.json").write_text(
        json.dumps(exts, indent=2, ensure_ascii=False) + "\n", encoding="utf-8"
    )
    print(f"wrote README.md + extensions.json ({len(exts)} extensions) to {out}")


if __name__ == "__main__":
    main()

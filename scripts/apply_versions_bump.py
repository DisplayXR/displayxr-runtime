#!/usr/bin/env python3
# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0
"""Write one pin into versions.json, keeping the file's formatting.

Called by .github/workflows/versions-bump.yml (the "Apply bump" step). Spec:
docs/specs/runtime/versions-json-autobump.md.

    apply_versions_bump.py --field leia_plugin --tag v2.23.1 [--file versions.json]

Two cases:

  * The field exists: its value is replaced in place by a regex on the text, so
    column alignment, key order and whitespace survive (json.dump would rewrite
    the whole file on every bump).
  * The field does not exist: refused, unless the field is listed in
    FIRST_RELEASE_FIELDS. Those are components that have a repository and all
    their plumbing here, but no release yet. A pin that names a tag which does
    not exist breaks consumers (setup-displayxr hard-fails on the missing
    release, displayxr-installer's coverage check fails on a key it does not
    know), so such a field is kept OUT of versions.json and this script inserts
    it when the component's first release dispatches the bump. See the spec,
    section "A component with no release yet".

Exit codes: 0 written, or already at that tag (prints NOOP on stdout);
            1 refused (unknown field, or the file does not have the expected shape).
"""
from __future__ import annotations

import argparse
import json
import re
import sys

# field -> the existing field it is inserted after (keeps related pins together).
# Remove an entry once its field is in versions.json: from then on it is an
# ordinary field and this table has nothing to say about it.
FIRST_RELEASE_FIELDS = {
    "leia_conversion": "conversion_open",
    "ultraleap": "leia_conversion",
}

# Where a first-release field goes when its preferred neighbour is absent too.
FALLBACK_ANCHOR = "conversion_open"


def line_re(leaf: str) -> re.Pattern:
    return re.compile(r'(?m)^(\s*"' + re.escape(leaf) + r'":\s*")[^"]*(")')


def replace_field(text: str, leaf: str, tag: str) -> str:
    pat = line_re(leaf)
    hits = pat.findall(text)
    if len(hits) != 1:
        raise ValueError(f"expected exactly 1 occurrence of leaf '{leaf}' in versions.json, found {len(hits)}")
    return pat.sub(lambda m: m.group(1) + tag + m.group(2), text, count=1)


def insert_field(text: str, field: str, tag: str, data: dict) -> str:
    anchor = FIRST_RELEASE_FIELDS[field]
    if anchor not in data:
        anchor = FALLBACK_ANCHOR
    if anchor not in data:
        raise ValueError(f"cannot place new field '{field}': anchor '{anchor}' is not in versions.json")
    # The whole anchor line, with its indentation and optional trailing comma.
    m = re.search(r'(?m)^(?P<indent>[ \t]*)"' + re.escape(anchor) + r'":[ \t]*"[^"\n]*"(?P<comma>,?)[ \t]*$', text)
    if not m:
        raise ValueError(f"cannot place new field '{field}': no line for anchor '{anchor}'")
    indent = m.group("indent")
    if m.group("comma"):
        new_line = f'\n{indent}"{field}": "{tag}",'
        return text[:m.end()] + new_line + text[m.end():]
    # The anchor is the last key: it gains the comma, the new line has none.
    head = text[:m.end()].rstrip(" \t") + ","
    return head + f'\n{indent}"{field}": "{tag}"' + text[m.end():]


def apply(text: str, field: str, tag: str) -> tuple[str, str]:
    """Return (new_text, what_happened). what_happened: 'noop' | 'bumped' | 'added'."""
    data = json.loads(text)
    parts = field.split(".")
    cur = data
    for p in parts[:-1]:
        if p not in cur or not isinstance(cur[p], dict):
            raise ValueError(f"versions.json has no nested key path '{field}'")
        cur = cur[p]
    leaf = parts[-1]

    if leaf in cur:
        if cur[leaf] == tag:
            return text, "noop"
        new_text = replace_field(text, leaf, tag)
        what = "bumped"
    elif len(parts) == 1 and field in FIRST_RELEASE_FIELDS:
        new_text = insert_field(text, field, tag, data)
        what = "added"
    else:
        raise ValueError(f"versions.json has no field '{field}'")

    # Never write a file the consumers cannot parse, or one that changed more
    # than the one pin.
    after = json.loads(new_text)
    expect = json.loads(text)
    node = expect
    for p in parts[:-1]:
        node = node[p]
    node[leaf] = tag
    if after != expect:
        raise ValueError(f"rewriting '{field}' would change more than that one pin; refusing")
    return new_text, what


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--field", required=True)
    ap.add_argument("--tag", required=True)
    ap.add_argument("--file", default="versions.json")
    args = ap.parse_args()

    with open(args.file, encoding="utf-8", newline="") as f:
        text = f.read()
    try:
        new_text, what = apply(text, args.field, args.tag)
    except ValueError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    if what == "noop":
        print(f"NOOP: versions.json[{args.field}] already == {args.tag}; nothing to do")
        return 0
    with open(args.file, "w", encoding="utf-8", newline="") as f:
        f.write(new_text)
    if what == "added":
        print(f"ADDED: versions.json[{args.field}] = {args.tag} (first release of this component)")
    else:
        print(f"BUMPED: versions.json[{args.field}] -> {args.tag}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

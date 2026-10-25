#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""Generate tests/tests_dxr_abi_snapshot.cpp — the ABI pin for the published
XR_DXR_* extensions that shipped apps are compiled against.

Every struct, XrStructureType value and SPEC_VERSION in the pinned headers is in
shipped binaries (Unity, Unreal, the browser, the demos), and several consumers
hand-copy the values. A layout change, a renumbered type or a silent
SPEC_VERSION move is a regression for those binaries, so the generated test
turns each of them into a compile error. An ADDITIVE change (a new struct, a new
type value, a SPEC_VERSION bump) is expected to fail it too: re-run this script
and commit the diff, which makes the change deliberate and reviewable.

The script parses each header's `typedef struct X { ... } X;` blocks, compiles a
probe with the host C compiler that prints sizeof/offsetof for every member, and
emits static_asserts. Platform-gated blocks (Xlib, Wayland -> desktop Linux;
Android) are measured by defining that platform's macro for the probe and are
emitted under the same guard, so each platform's CI checks its own block. Every
pinned layout is pointer + fixed-width members only, which is identical on
LP64 and LLP64 64-bit targets; the asserts are skipped on 32-bit builds.

    python3 scripts/gen_dxr_abi_snapshot.py           # rewrite the test
    python3 scripts/gen_dxr_abi_snapshot.py --check   # exit 1 if stale
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
INCLUDE_DIR = ROOT / "src" / "external" / "openxr_includes"
HEADER_DIR = INCLUDE_DIR / "openxr"
OUT = ROOT / "tests" / "tests_dxr_abi_snapshot.cpp"

# The extension set proposed to the OpenXR WG
# (docs/roadmap/khronos-proposal-binding-cleanup.md §1). Add a header here to pin it.
# Cocoa precedes win32 on purpose: that include order failed to compile before
# the window-space layer got one shared guard, so the test pins it.
HEADERS = [
    "XR_DXR_cocoa_window_binding.h",
    "XR_DXR_win32_window_binding.h",
    "XR_DXR_xlib_window_binding.h",
    "XR_DXR_wayland_surface_binding.h",
    "XR_DXR_android_surface_binding.h",
    "XR_DXR_display_info.h",
    "XR_DXR_view_rig.h",
    "XR_DXR_local_3d_zone.h",
    "XR_DXR_display_zones.h",
]

# (guard emitted in the test, macros the probe defines to see the block)
PLATFORMS = {
    "": ("", []),
    "XR_DXR_xlib_window_binding.h": ("defined(__linux__) && !defined(__ANDROID__)", ["__linux__"]),
    "XR_DXR_wayland_surface_binding.h": ("defined(__linux__) && !defined(__ANDROID__)", ["__linux__"]),
    "XR_DXR_android_surface_binding.h": ("defined(__ANDROID__)", ["__ANDROID__"]),
}

STRUCT_RE = re.compile(r"typedef\s+struct\s+(Xr\w+DXR)\s*\{(.*?)\}\s*\1\s*;", re.S)
TYPE_RE = re.compile(r"^#define\s+(XR_TYPE_\w+_DXR)\s+\(\(XrStructureType\)(\d+)\)", re.M)
VERSION_RE = re.compile(r"^#define\s+(XR_DXR_\w+_SPEC_VERSION)\s+(\d+)", re.M)
MEMBER_RE = re.compile(r"([A-Za-z_]\w*)\s*(?:\[[^\]]*\])?\s*;")


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    return re.sub(r"//[^\n]*", " ", text)


def parse(header: str) -> tuple[dict[str, list[str]], list[tuple[str, int]], list[tuple[str, int]]]:
    raw = (HEADER_DIR / header).read_text()
    text = strip_comments(raw)
    structs: dict[str, list[str]] = {}
    for name, body in STRUCT_RE.findall(text):
        body = re.sub(r"^\s*#.*$", " ", body, flags=re.M)  # preprocessor lines inside a body
        members = []
        for decl in body.split(";"):
            m = MEMBER_RE.search(decl.strip() + ";")
            if m and m.group(1) not in members:
                members.append(m.group(1))
        structs[name] = members
    return structs, [(n, int(v)) for n, v in TYPE_RE.findall(raw)], [(n, int(v)) for n, v in VERSION_RE.findall(raw)]


def probe(header: str, structs: dict[str, list[str]], defines: list[str]) -> dict[str, tuple[int, list[int]]]:
    lines = ["#include <stddef.h>", "#include <stdio.h>", "#include <openxr/openxr.h>"]
    if header == "XR_DXR_display_zones.h":
        lines.append("#include <openxr/XR_DXR_local_3d_zone.h>")
    lines.append(f"#include <openxr/{header}>")
    lines.append("int main(void) {")
    for s, members in structs.items():
        offs = " ".join(f'printf(" %zu", offsetof({s}, {m}));' for m in members)
        lines.append(f'printf("{s} %zu", sizeof({s})); {offs} printf("\\n");')
    lines.append("return 0; }")
    cc = os.environ.get("CC", "cc")
    with tempfile.TemporaryDirectory() as tmp:
        src, exe = Path(tmp) / "probe.c", Path(tmp) / "probe"
        src.write_text("\n".join(lines) + "\n")
        cmd = [cc, "-std=c11", f"-I{INCLUDE_DIR}", *[f"-D{d}" for d in defines], str(src), "-o", str(exe)]
        subprocess.run(cmd, check=True)
        out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
    result = {}
    for line in out.splitlines():
        parts = line.split()
        result[parts[0]] = (int(parts[1]), [int(p) for p in parts[2:]])
    return result


def generate() -> str:
    out = [
        "// Copyright 2026, The DisplayXR Project",
        "// SPDX-License-Identifier: BSL-1.0",
        "/*!",
        " * @file",
        " * @brief  ABI pin for the published XR_DXR_* extensions — GENERATED, do not edit.",
        " *",
        " * Regenerate with scripts/gen_dxr_abi_snapshot.py after a DELIBERATE change.",
        " * A failure here means a struct layout, an XrStructureType value or a",
        " * SPEC_VERSION moved; shipped apps (and consumers that hand-copy the values)",
        " * are compiled against the old one. See",
        " * docs/roadmap/khronos-proposal-binding-cleanup.md §4 Stage 1.",
        " */",
        "",
        '#include "catch_amalgamated.hpp"',
        "",
        "#include <cstddef>",
        "#include <cstdint>",
        "#include <openxr/openxr.h>",
    ]
    out += [f"#include <openxr/{h}>" for h in HEADERS]
    out.append("")
    seen: set[str] = set()  # the window-space layer is declared by all five binding headers
    for header in HEADERS:
        guard, defines = PLATFORMS.get(header, PLATFORMS[""])
        structs, types, versions = parse(header)
        structs = {s: m for s, m in structs.items() if s not in seen}
        types = [(n, v) for n, v in types if n not in seen]
        seen.update(structs, (n for n, _ in types))
        measured = probe(header, structs, defines) if structs else {}
        out.append(f"// ---- {header}")
        if guard:
            out.append(f"#if {guard}")
        for n, v in versions:
            out.append(f'static_assert({n} == {v}, "{n} moved");')
        for n, v in types:
            out.append(f'static_assert({n} == (XrStructureType){v}, "{n} renumbered");')
        if structs:
            out.append("#if UINTPTR_MAX == UINT64_MAX")
            for s, members in structs.items():
                size, offs = measured[s]
                out.append(f'static_assert(sizeof({s}) == {size}, "{s} size changed");')
                for m, o in zip(members, offs):
                    out.append(f'static_assert(offsetof({s}, {m}) == {o}, "{s}::{m} moved");')
            out.append("#endif")
        if guard:
            out.append(f"#endif // {guard}")
        out.append("")
    out += [
        'TEST_CASE("XR_DXR_* ABI snapshot compiles", "[abi][extensions]")',
        "{",
        "\t// The pin is the static_asserts above; reaching here means it held.",
        "\tSUCCEED();",
        "}",
        "",
    ]
    return "\n".join(out)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--check", action="store_true", help="exit 1 if the committed test is stale")
    args = ap.parse_args()
    text = generate()
    if args.check:
        if not OUT.exists() or OUT.read_text() != text:
            print(f"{OUT.relative_to(ROOT)} is stale: re-run scripts/gen_dxr_abi_snapshot.py", file=sys.stderr)
            return 1
        return 0
    OUT.write_text(text)
    print(f"wrote {OUT.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

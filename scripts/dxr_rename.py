#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""
XR_EXT_* -> XR_DXR_* extension rename tool (runtime issue: DXR author-tag
registration, OpenXR-Docs PR #199).

DisplayXR's bespoke extensions shipped under the reserved Khronos EXT prefix;
this tool renames exactly the allowlisted DisplayXR-authored extensions to the
registered DXR vendor tag, across any DisplayXR repo, without touching the many
legitimate upstream Khronos/Monado XR_EXT_*/XR_KHR_* symbols.

Subcommands
  map        Derive the old->new symbol map from the runtime's extension
             headers and write dxr_rename_map.json (the shared PR artifact).
  inventory  Print the sorted set of all XR_EXT_*/XR_KHR_* extension-name
             tokens in a repo (for the before/after upstream-invariance diff).
  apply      Apply the map to a repo tree: exact-symbol content replacement
             + file renames. Idempotent.
  verify     Assert 0 remaining old-name references (including token-pasted
             fragments and filenames) in a repo tree.

Never does a substring EXT->DXR replacement: every rule is keyed on the 13
allowlisted extension names or on an exact symbol harvested from their
headers, with identifier-boundary lookarounds.
"""

import argparse
import json
import os
import re
import sys

# The 13 DisplayXR-authored extensions (snake_case names). Anything not on
# this list is upstream and must never be touched.
ALLOWLIST = [
    "atlas_capture",
    "cocoa_window_binding",
    "display_info",
    "display_zones",
    "local_3d_zone",
    "macos_gl_binding",
    "mcp_tools",
    "spatial_workspace",
    "view_rig",
    "weave",
    "win32_window_binding",
    "workspace_file_dialog",
    "xlib_window_binding",
]

HEADER_DIR = "src/external/openxr_includes/openxr"

# Directories never scanned (build trees, VCS, package outputs, engine caches).
EXCLUDE_DIRS = {
    ".git", "build", "build_vs2022", "build-mingw", "builddir", "_deps",
    "_package", "node_modules", "dist", "out", "cmake-build-debug",
    "cmake-build-release", "Library", "Intermediate", "Binaries", "Saved",
    "DerivedDataCache", ".gradle", ".idea", ".vs",
}
EXCLUDE_DIR_PREFIXES = ("build-", "build_")

# The map artifact keys ARE the old symbols — never rewrite or flag it.
EXCLUDE_FILES = {"dxr_rename_map.json"}

MAX_FILE_BYTES = 8 * 1024 * 1024

ID = r"A-Za-z0-9_"
LB = rf"(?<![{ID}])"          # identifier-boundary lookbehind
LA = rf"(?![{ID}])"           # identifier-boundary lookahead


def is_excluded_dir(name):
    return name in EXCLUDE_DIRS or name.startswith(EXCLUDE_DIR_PREFIXES)


def iter_text_files(root):
    for dirpath, dirnames, filenames in os.walk(root):
        dirnames[:] = [d for d in dirnames if not is_excluded_dir(d)]
        for fn in filenames:
            if fn in EXCLUDE_FILES:
                continue
            path = os.path.join(dirpath, fn)
            try:
                if os.path.getsize(path) > MAX_FILE_BYTES:
                    continue
                with open(path, "rb") as f:
                    chunk = f.read(8192)
                if b"\x00" in chunk:
                    continue
            except OSError:
                continue
            yield path


def read_text(path):
    with open(path, "r", encoding="utf-8", errors="surrogateescape") as f:
        return f.read()


def write_text(path, text):
    with open(path, "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(text)


# ---------------------------------------------------------------- map

def harvest_symbols(runtime_root):
    """Harvest exact old symbols from the 13 allowlisted headers."""
    hdr_dir = os.path.join(runtime_root, HEADER_DIR)
    symbols = set()
    pats = [
        re.compile(rf"{LB}(Xr[A-Za-z0-9]+EXT){LA}"),          # types
        re.compile(rf"{LB}(PFN_xr[A-Za-z0-9]+EXT){LA}"),      # PFN typedefs
        re.compile(rf"{LB}(xr[A-Za-z0-9]+EXT){LA}"),          # functions
        re.compile(rf"{LB}(XR_[A-Z0-9_]+_EXT){LA}"),          # enums/defines
    ]
    headers = []
    for name in ALLOWLIST:
        # accept either prefix so `map` works pre- and post-rename
        for prefix in ("XR_EXT_", "XR_DXR_"):
            p = os.path.join(hdr_dir, f"{prefix}{name}.h")
            if os.path.exists(p):
                headers.append(p)
                break
        else:
            sys.exit(f"ERROR: header for allowlisted extension '{name}' "
                     f"not found in {hdr_dir}")
    for path in headers:
        text = read_text(path)
        for pat in pats:
            for m in pat.finditer(text):
                symbols.add(m.group(1))

    # Safety net: drop anything that also appears in the upstream Khronos
    # headers — those would be upstream references, not our definitions.
    upstream = set()
    for up in ("openxr.h", "openxr_reflection.h", "openxr_platform.h",
               "openxr_reflection_structs.h",
               "openxr_reflection_parent_structs.h"):
        p = os.path.join(hdr_dir, up)
        if not os.path.exists(p):
            continue
        text = read_text(p)
        for pat in pats:
            for m in pat.finditer(text):
                upstream.add(m.group(1))
    ours = sorted(symbols - upstream)
    dropped = sorted(symbols & upstream)
    return ours, dropped


def build_map(runtime_root):
    exact_symbols, dropped = harvest_symbols(runtime_root)
    exact = {}
    for sym in exact_symbols:
        if sym.endswith("_EXT"):
            exact[sym] = sym[:-4] + "_DXR"
        elif sym.endswith("EXT"):
            exact[sym] = sym[:-3] + "DXR"
    return {
        "comment": "Generated by scripts/dxr_rename.py map — the shared "
                   "old->new mapping artifact for the XR_EXT_->XR_DXR_ "
                   "rename. Do not hand-edit.",
        "allowlist": ALLOWLIST,
        "exact_symbols": exact,
        "upstream_refs_excluded": dropped,
        "struct_type_block": {"old": "1000999", "new": "1004999"},
    }


# ---------------------------------------------------------------- rules

def build_rules(mapping):
    """Ordered (regex, replacement) content-rewrite rules."""
    rules = []
    # 1. Exact header-harvested symbols (types, functions, enums), longest
    #    first so e.g. PFN_xrFooEXT rewrites before xrFooEXT.
    for old in sorted(mapping["exact_symbols"], key=len, reverse=True):
        new = mapping["exact_symbols"][old]
        rules.append((re.compile(rf"{LB}{re.escape(old)}{LA}"), new))

    for name in mapping["allowlist"]:
        up = name.upper()
        # 2. Extension-name prefix forms. No trailing identifier boundary on
        #    purpose: catches XR_EXT_<name>_SPEC_VERSION, _EXTENSION_NAME,
        #    include-guard XR_EXT_<NAME>_H, filenames XR_EXT_<name>.h and the
        #    "XR_EXT_<name>" string literal in one rule. The (?![a-z0-9]) /
        #    (?![A-Z0-9]) guard prevents matching a longer distinct extension
        #    name (none of the 13 is a prefix of an upstream name).
        rules.append((re.compile(rf"{LB}XR_EXT_{name}(?![a-z0-9])"),
                      f"XR_DXR_{name}"))
        rules.append((re.compile(rf"{LB}XR_EXT_{up}(?![A-Z0-9])"),
                      f"XR_DXR_{up}"))
        # 3. Token-pasted fragment forms from the oxr state tracker
        #    (oxr_extension_support.h et al.) — prefix-less EXT_<name>
        #    fragments that a full-symbol grep misses; missing these compiles
        #    green but silently stops advertising the extension.
        rules.append((re.compile(rf"{LB}OXR_HAVE_EXT_{name}(?![a-z0-9])"),
                      f"OXR_HAVE_DXR_{name}"))
        rules.append((re.compile(
            rf"{LB}OXR_EXTENSION_SUPPORT_EXT_{name}(?![a-z0-9])"),
            f"OXR_EXTENSION_SUPPORT_DXR_{name}"))
        rules.append((re.compile(rf"{LB}EXT_{name}(?![a-z0-9])"),
                      f"DXR_{name}"))
        rules.append((re.compile(rf"{LB}EXT_{up}(?![A-Z0-9])"),
                      f"DXR_{up}"))

    # 4. Struct-type numeric block relocation: Khronos extension #999 is
    #    unassigned and collidable; move to a high experimental block. Only
    #    DisplayXR uses the 1004999xxx values, incl. hardcoded copies in
    #    non-C consumers (Unity C# bindings).
    old_blk = mapping["struct_type_block"]["old"]
    new_blk = mapping["struct_type_block"]["new"]
    rules.append((re.compile(rf"(?<![0-9]){old_blk}(?=[0-9]{{3}})"), new_blk))
    return rules


def rename_path_for(name_map_names, filename):
    """New basename if the file is named after an allowlisted extension."""
    out = filename
    for name in name_map_names:
        out = out.replace(f"XR_EXT_{name}", f"XR_DXR_{name}")
        out = out.replace(f"XR_EXT_{name.upper()}", f"XR_DXR_{name.upper()}")
    return out


# ---------------------------------------------------------------- apply

def cmd_apply(args):
    mapping = json.load(open(args.map))
    rules = build_rules(mapping)
    root = os.path.abspath(args.repo)
    changed, renamed = 0, 0
    for path in list(iter_text_files(root)):
        text = read_text(path)
        new_text = text
        for pat, repl in rules:
            new_text = pat.sub(repl, new_text)
        if new_text != text:
            write_text(path, new_text)
            changed += 1
            if args.verbose:
                print(f"  rewrote {os.path.relpath(path, root)}")
        base = os.path.basename(path)
        new_base = rename_path_for(mapping["allowlist"], base)
        if new_base != base:
            new_path = os.path.join(os.path.dirname(path), new_base)
            os.rename(path, new_path)
            renamed += 1
            print(f"  renamed {os.path.relpath(path, root)} -> {new_base}")
    print(f"apply: {changed} files rewritten, {renamed} files renamed "
          f"under {root}")


# ---------------------------------------------------------------- verify

def cmd_verify(args):
    mapping = json.load(open(args.map))
    root = os.path.abspath(args.repo)
    residues = []

    frag_pats = []
    for name in mapping["allowlist"]:
        up = name.upper()
        frag_pats.append(re.compile(rf"{LB}(?:XR_|OXR_HAVE_|"
                                    rf"OXR_EXTENSION_SUPPORT_)?EXT_"
                                    rf"(?:{name}(?![a-z0-9])|{up}(?![A-Z0-9]))"))
    sym_pats = [re.compile(rf"{LB}{re.escape(s)}{LA}")
                for s in mapping["exact_symbols"]]
    old_blk = mapping["struct_type_block"]["old"]
    num_pat = re.compile(rf"(?<![0-9]){old_blk}(?=[0-9]{{3}})")

    for path in iter_text_files(root):
        rel = os.path.relpath(path, root)
        base = os.path.basename(path)
        if rename_path_for(mapping["allowlist"], base) != base:
            residues.append(f"{rel}: stale filename")
        text = read_text(path)
        for lineno, line in enumerate(text.splitlines(), 1):
            for pat in frag_pats + sym_pats + [num_pat]:
                m = pat.search(line)
                if m:
                    residues.append(f"{rel}:{lineno}: {line.strip()[:120]}")
                    break

    if residues:
        print(f"verify FAILED: {len(residues)} residual old-name reference(s):")
        for r in residues[: args.limit]:
            print(f"  {r}")
        if len(residues) > args.limit:
            print(f"  ... and {len(residues) - args.limit} more")
        sys.exit(1)
    print(f"verify OK: no residual XR_EXT_ references to the "
          f"{len(mapping['allowlist'])} allowlisted extensions under {root}")


# ---------------------------------------------------------------- inventory

def cmd_inventory(args):
    root = os.path.abspath(args.repo)
    pat = re.compile(r"(?<![A-Za-z0-9_])XR_(?:EXT|KHR)_[a-z0-9_]+")
    names = set()
    for path in iter_text_files(root):
        for m in pat.finditer(read_text(path)):
            names.add(m.group(0))
    for n in sorted(names):
        print(n)


def cmd_map(args):
    mapping = build_map(args.runtime)
    out = args.output
    with open(out, "w") as f:
        json.dump(mapping, f, indent=2, sort_keys=True)
        f.write("\n")
    print(f"map: {len(mapping['exact_symbols'])} exact symbols across "
          f"{len(mapping['allowlist'])} extensions -> {out}")
    if mapping["upstream_refs_excluded"]:
        print("  excluded upstream-shared symbols: "
              + ", ".join(mapping["upstream_refs_excluded"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[1])
    sub = ap.add_subparsers(dest="cmd", required=True)

    default_map = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                               "dxr_rename_map.json")

    p = sub.add_parser("map", help="derive the symbol map from the headers")
    p.add_argument("--runtime", default=".",
                   help="displayxr-runtime checkout (default: cwd)")
    p.add_argument("--output", default=default_map)
    p.set_defaults(func=cmd_map)

    p = sub.add_parser("apply", help="apply the rename to a repo tree")
    p.add_argument("repo", help="repo root to rewrite")
    p.add_argument("--map", default=default_map)
    p.add_argument("--verbose", action="store_true")
    p.set_defaults(func=cmd_apply)

    p = sub.add_parser("verify", help="assert 0 residual old-name refs")
    p.add_argument("repo", help="repo root to check")
    p.add_argument("--map", default=default_map)
    p.add_argument("--limit", type=int, default=40)
    p.set_defaults(func=cmd_verify)

    p = sub.add_parser("inventory",
                       help="list all XR_EXT_/XR_KHR_ extension names in a repo")
    p.add_argument("repo", help="repo root to scan")
    p.set_defaults(func=cmd_inventory)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()

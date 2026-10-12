#!/usr/bin/env python3
"""
ABI compatibility check between this runtime tree and a leia-plugin tag.

The leia plug-in reports XRT_PLUGIN_API_VERSION_CURRENT from the runtime
headers it was built against (its CMakeLists.txt pins DXR_RUNTIME_GIT_TAG
via FetchContent). For the dev-orchestrator bundle (`versions.json`) to
boot correctly, runtime + leia must agree on the ABI major.

This script:
  1. Reads XRT_PLUGIN_API_VERSION_CURRENT from THIS tree's xrt_plugin.h.
  2. Fetches the leia plug-in's CMakeLists.txt at the requested tag and
     extracts DXR_RUNTIME_GIT_TAG.
  3. Fetches xrt_plugin.h from THAT runtime tag and extracts the same
     macro — that is what the plug-in's xrtPluginNegotiate() reports.
  4. Exit 0 on match, non-zero on mismatch with a clear diagnostic the
     calling workflow can post into an auto-opened issue.

Usage:
    check_plugin_abi.py --leia-tag v1.0.7
    check_plugin_abi.py --leia-tag v1.0.7 --runtime-tree .

Designed to be called from .github/workflows/versions-bump.yml and from
build-windows.yml's runtime self-bump step. No external deps beyond
urllib + the python3 stdlib already present on GitHub runners.
"""
from __future__ import annotations

import argparse
import re
import sys
import urllib.request
from pathlib import Path

GITHUB_RAW = "https://raw.githubusercontent.com"
RUNTIME_REPO = "DisplayXR/displayxr-runtime"
LEIA_REPO = "DisplayXR/displayxr-leia-plugin"
PLUGIN_HEADER_PATH = "src/xrt/include/xrt/xrt_plugin.h"
INPUT_PLUGIN_HEADER_PATH = "src/xrt/include/xrt/xrt_input_plugin.h"
CONVERSION_PLUGIN_HEADER_PATH = "src/xrt/include/xrt/xrt_conversion_plugin.h"

# Every plug-in ABI the runtime exposes: header -> the macro family whose
# `<PREFIX>_CURRENT` is that ABI's major. Three independent majors, one per
# plug-in type (display processor ADR-019, input provider ADR-034, conversion
# provider ADR-053); a loader rejects a different major before any dispatch.
# One table so the release gate (this script) and the downstream repin gate
# (downstream_pin_bump.py) read the same macros and can never disagree.
ABI_HEADERS: dict[str, str] = {
    PLUGIN_HEADER_PATH: "XRT_PLUGIN_API_VERSION",
    INPUT_PLUGIN_HEADER_PATH: "XRT_INPUT_PLUGIN_API_VERSION",
    CONVERSION_PLUGIN_HEADER_PATH: "XRT_CONVERSION_PLUGIN_API_VERSION",
}


def _fetch(url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": "displayxr-abi-check"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read().decode("utf-8")


def _read_local(path: Path) -> str:
    return path.read_text(encoding="utf-8")


def _resolve_api_version_current(
    plugin_h_text: str, prefix: str = "XRT_PLUGIN_API_VERSION"
) -> int:
    """
    Parse a plug-in ABI header and resolve `<prefix>_CURRENT` to an int.

    Each header defines `<prefix>_N` constants and then
    `#define <prefix>_CURRENT <prefix>_<N>`. We follow that one alias hop.
    The default prefix is the display-processor ABI (xrt_plugin.h); pass a
    value from ABI_HEADERS for the input / conversion ABIs. The `^#define`
    anchor keeps XRT_PLUGIN_API_VERSION from matching inside
    XRT_INPUT_PLUGIN_API_VERSION / XRT_CONVERSION_PLUGIN_API_VERSION.
    """
    p = re.escape(prefix)
    consts: dict[str, int] = {}
    for m in re.finditer(
        r"^\s*#\s*define\s+(" + p + r"_\d+)\s+(\d+)\s*$",
        plugin_h_text,
        re.MULTILINE,
    ):
        consts[m.group(1)] = int(m.group(2))

    alias = re.search(
        r"^\s*#\s*define\s+" + p + r"_CURRENT\s+(\S+)\s*$",
        plugin_h_text,
        re.MULTILINE,
    )
    if not alias:
        raise RuntimeError(f"{prefix}_CURRENT not found in the header")
    target = alias.group(1)
    if target.isdigit():
        return int(target)
    if target not in consts:
        raise RuntimeError(
            f"{prefix}_CURRENT aliases unknown {target!r}; "
            f"known constants: {sorted(consts)}"
        )
    return consts[target]


def tree_abis(tree_root: Path) -> dict[str, int | None]:
    """Every plug-in ABI major in a runtime checkout: {`<prefix>_CURRENT`: N},
    None for a header that does not exist in that tree."""
    out: dict[str, int | None] = {}
    for header, prefix in ABI_HEADERS.items():
        path = tree_root / header
        out[prefix + "_CURRENT"] = (
            _resolve_api_version_current(_read_local(path), prefix) if path.is_file() else None
        )
    return out


def _extract_dxr_runtime_git_tag(leia_cmakelists: str) -> str:
    """
    The leia plug-in's CMakeLists.txt sets DXR_RUNTIME_GIT_TAG as a CACHE
    STRING. Parse it to learn which runtime tag the plug-in was built
    against (and therefore which XRT_PLUGIN_API_VERSION_CURRENT it
    reports back from xrtPluginNegotiate).
    """
    m = re.search(
        r'set\s*\(\s*DXR_RUNTIME_GIT_TAG\s+"(v[0-9]+\.[0-9]+\.[0-9]+)"',
        leia_cmakelists,
    )
    if not m:
        raise RuntimeError(
            "DXR_RUNTIME_GIT_TAG not found in leia CMakeLists.txt — has the "
            "plug-in's FetchContent pin pattern changed?"
        )
    return m.group(1)


def runtime_abi_from_tree(tree_root: Path) -> int:
    return _resolve_api_version_current(_read_local(tree_root / PLUGIN_HEADER_PATH))


def runtime_abi_from_tag(runtime_tag: str) -> int:
    url = f"{GITHUB_RAW}/{RUNTIME_REPO}/{runtime_tag}/{PLUGIN_HEADER_PATH}"
    return _resolve_api_version_current(_fetch(url))


def leia_runtime_pin(leia_tag: str) -> str:
    url = f"{GITHUB_RAW}/{LEIA_REPO}/{leia_tag}/CMakeLists.txt"
    return _extract_dxr_runtime_git_tag(_fetch(url))


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument(
        "--leia-tag",
        help="leia plug-in tag to check (e.g. v1.0.7); required unless --print-tree-abis",
    )
    p.add_argument(
        "--print-tree-abis",
        action="store_true",
        help="print every plug-in ABI major (display / input / conversion) of --runtime-tree and exit",
    )
    p.add_argument(
        "--runtime-tree",
        default=".",
        help="path to runtime checkout (default: cwd)",
    )
    p.add_argument(
        "--runtime-tag",
        help="instead of --runtime-tree, fetch the runtime header at a tag",
    )
    args = p.parse_args()

    if args.print_tree_abis:
        try:
            for macro, value in tree_abis(Path(args.runtime_tree)).items():
                print(f"{macro} = {value if value is not None else '(header absent)'}")
        except Exception as e:  # noqa: BLE001
            print(f"ABI read FAILED: {e}", file=sys.stderr)
            return 2
        return 0
    if not args.leia_tag:
        p.error("--leia-tag is required (or pass --print-tree-abis)")

    try:
        if args.runtime_tag:
            runtime_abi = runtime_abi_from_tag(args.runtime_tag)
            runtime_label = f"runtime {args.runtime_tag}"
        else:
            runtime_abi = runtime_abi_from_tree(Path(args.runtime_tree))
            runtime_label = f"runtime tree @ {args.runtime_tree}"

        plugin_runtime_pin = leia_runtime_pin(args.leia_tag)
        plugin_abi = runtime_abi_from_tag(plugin_runtime_pin)
    except Exception as e:  # noqa: BLE001 — surface to caller as fatal
        print(f"ABI check FAILED to run: {e}", file=sys.stderr)
        return 2

    print(f"{runtime_label}: ABI v{runtime_abi}")
    print(
        f"leia {args.leia_tag} built against runtime "
        f"{plugin_runtime_pin}: ABI v{plugin_abi}"
    )

    if runtime_abi != plugin_abi:
        print(
            f"\nMISMATCH: runtime ABI v{runtime_abi} cannot load leia "
            f"{args.leia_tag} (ABI v{plugin_abi}). "
            f"Rebuild leia against the current runtime headers and tag a "
            f"new release before promoting this bundle.",
            file=sys.stderr,
        )
        return 1

    print(f"\nOK: ABI v{runtime_abi} on both sides.")
    return 0


if __name__ == "__main__":
    sys.exit(main())

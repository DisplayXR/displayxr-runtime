#!/usr/bin/env python3
"""
DisplayXR app linter — check an app source tree against the authoring invariants.

This is the machine-checkable companion to docs/guides/displayxr-app-rules.md.
It does NOT understand C/C++ semantically; it pattern-matches the specific
anti-patterns the rules doc calls out (the ones coding agents reliably get
wrong) and reports each with the INV-* rule it violates, a file:line, and a
one-line fix. It also validates the workspace manifest + icon sizes.

Conservative by design: every check is high-signal so a finding is almost
always real. Color (INV-4.6) and a couple of others are advisory (WARN), the
hard structural mistakes are errors.

Usage:
    check_displayxr_app.py <app-dir>            # lint one app
    check_displayxr_app.py test_apps/cube_handle_d3d11_win
    check_displayxr_app.py <dir> --strict       # warnings also fail (exit 1)
    check_displayxr_app.py --list-rules         # print the rule catalog

Exit 0 = clean (no errors; warnings allowed unless --strict).
Exit 1 = at least one error (or any warning under --strict).
Exit 2 = bad invocation.

No external deps — Python 3.6+ stdlib only (PNG dimensions are read from the
IHDR chunk directly, so Pillow is not required).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

RULES_DOC = "docs/guides/displayxr-app-rules.md"

# Directories never linted: shared reference code, vendored headers, build output.
EXCLUDE_DIRS = {
    "build", ".git", "third_party", "openxr_includes", "common",
    "_package", "__pycache__", "node_modules", ".vs", "out",
}
SOURCE_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".m", ".mm"}

# ---- rule catalog (kept in sync with docs/guides/displayxr-app-rules.md) ----
RULES = {
    "INV-3.1": "Locate into an XRT_MAX_VIEWS (8)-wide buffer; render/submit the active mode's viewCount, never a hardcoded 2.",
    "INV-4.3": "Per-tile render size = window/canvas x scaleXY, never display size.",
    "INV-4.6": "Request an sRGB swapchain (and store a correctly-encoded image); don't double-encode.",
    "INV-7.x": "Capture via xrCaptureAtlasEXT — never reintroduce an app-side CaptureAtlasRegion* readback.",
    "INV-7.2": "xrCaptureAtlasEXT pathPrefix takes NO extension; the runtime appends _atlas.png.",
    "INV-9.1": "Ship a <exe>.displayxr.json (schema_version=1, name 1-64, type 2d|3d) or the app won't appear in the workspace launcher.",
    "INV-9.2": "2D icon is 512x512 (`icon`); 3D icon is 1024x512 (`icon_3d`, requires `icon`); layout in {sbs-lr,sbs-rl,tb,bt}.",
}

ERROR, WARN = "ERROR", "WARN"


class Finding:
    __slots__ = ("level", "rule", "path", "line", "msg", "fix")

    def __init__(self, level, rule, path, line, msg, fix):
        self.level = level
        self.rule = rule
        self.path = path
        self.line = line
        self.msg = msg
        self.fix = fix


# --- compiled source patterns: (regex, level, rule, message, fix) ---
SRC_PATTERNS = [
    (re.compile(r"\bXrView\s+\w+\s*\[\s*2\s*\]"),
     ERROR, "INV-3.1",
     "XrView array hardcoded to [2] — quad modes have 4 views.",
     "Size it XrView views[8] (XRT_MAX_VIEWS) and locate with viewCapacityInput=8."),
    (re.compile(r"\bXrCompositionLayerProjectionView\s+\w+\s*\[\s*2\s*\]"),
     ERROR, "INV-3.1",
     "Projection-view array hardcoded to [2].",
     "Allocate eyeCount-sized (active mode's viewCount), e.g. std::vector<...>(eyeCount)."),
    (re.compile(r"\bdisplay(?:Pixel)?(?:Width|Height)\s*/\s*2\b"),
     ERROR, "INV-4.3",
     "Swapchain/tile size derived from display dimensions (display/2).",
     "Use window/canvas size x recommendedViewScaleX/Y, clamped to the swapchain tile capacity."),
    (re.compile(r"\bCaptureAtlasRegion(?:D3D11|D3D12|GL|VK|Metal)?\b"),
     ERROR, "INV-7.x",
     "Deprecated app-side atlas readback (CaptureAtlasRegion*) — removed in the #396 W6 refactor.",
     "Use xrCaptureAtlasEXT (Windows: dxr_capture::RequestRuntimeAtlasCapture; elsewhere call it inline)."),
    (re.compile(r"\bpathPrefix\b[^;\n]*\"[^\"]*\.png\""),
     WARN, "INV-7.2",
     "xrCaptureAtlasEXT pathPrefix contains a .png extension.",
     "Pass a prefix with NO extension — the runtime appends _atlas.png."),
    (re.compile(r"for\s*\([^;]*;[^;]*\b(?:eye|view|v|i)\s*<\s*2\b"),
     WARN, "INV-3.1",
     "Render/eye loop bounded by a literal 2.",
     "Bound by the active mode's viewCount (eyeCount), not 2 — clamp array reads to viewCountOutput."),
]

# Tokens that indicate the app uses an sRGB swapchain somewhere (for INV-4.6).
SRGB_TOKENS = re.compile(
    r"_UNORM_SRGB|_SRGB\b|SRGB8_ALPHA8|MTLPixelFormat\w*sRGB|VK_FORMAT_\w*_SRGB",
    re.IGNORECASE,
)
CREATES_SWAPCHAIN = re.compile(r"\bxrCreateSwapchain\b")
ICON_LAYOUTS = {"sbs-lr", "sbs-rl", "tb", "bt"}


def iter_source_files(root: Path):
    for p in sorted(root.rglob("*")):
        if not p.is_file() or p.suffix.lower() not in SOURCE_EXTS:
            continue
        if any(part in EXCLUDE_DIRS for part in p.relative_to(root).parts[:-1]):
            continue
        yield p


def rel(path: Path, root: Path) -> str:
    try:
        return str(path.relative_to(root))
    except ValueError:
        return str(path)


def png_dimensions(path: Path):
    """Return (width, height) for a PNG using its IHDR chunk, or None. Stdlib only."""
    try:
        with open(path, "rb") as f:
            head = f.read(24)
    except OSError:
        return None
    if len(head) < 24 or head[:8] != b"\x89PNG\r\n\x1a\n" or head[12:16] != b"IHDR":
        return None
    w = int.from_bytes(head[16:20], "big")
    h = int.from_bytes(head[20:24], "big")
    return (w, h)


def scan_sources(root: Path, findings: list):
    any_swapchain = False
    any_srgb = False
    swapchain_loc = None
    for path in iter_source_files(root):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        lines = text.splitlines()
        if SRGB_TOKENS.search(text):
            any_srgb = True
        for regex, level, rule, msg, fix in SRC_PATTERNS:
            for m in regex.finditer(text):
                line_no = text.count("\n", 0, m.start()) + 1
                findings.append(Finding(level, rule, rel(path, root), line_no, msg, fix))
        if CREATES_SWAPCHAIN.search(text):
            any_swapchain = True
            if swapchain_loc is None:
                m = CREATES_SWAPCHAIN.search(text)
                swapchain_loc = (rel(path, root), text.count("\n", 0, m.start()) + 1)
    # INV-4.6 advisory: creates a swapchain but no sRGB format token anywhere.
    if any_swapchain and not any_srgb:
        p, ln = swapchain_loc
        findings.append(Finding(
            WARN, "INV-4.6", p, ln,
            "Creates a swapchain but no sRGB format appears anywhere in the app.",
            "Request an sRGB swapchain (_UNORM_SRGB / GL_SRGB8_ALPHA8 / _SRGB / MTLPixelFormat*sRGB) and store an encoded image.",
        ))


def validate_manifest(mpath: Path, root: Path, findings: list):
    rp = rel(mpath, root)
    try:
        data = json.loads(mpath.read_text(encoding="utf-8"))
    except (OSError, ValueError) as e:
        findings.append(Finding(ERROR, "INV-9.1", rp, 1, f"Manifest is not valid JSON: {e}", "Fix the JSON syntax."))
        return
    if data.get("schema_version") != 1:
        findings.append(Finding(ERROR, "INV-9.1", rp, 1,
                                f"schema_version must be exactly 1 (got {data.get('schema_version')!r}).",
                                'Set "schema_version": 1.'))
    name = data.get("name")
    if not isinstance(name, str) or not (1 <= len(name) <= 64):
        findings.append(Finding(ERROR, "INV-9.1", rp, 1,
                                f"name must be a 1-64 char string (got {name!r}).", "Set a valid display name."))
    if data.get("type") not in ("2d", "3d"):
        findings.append(Finding(ERROR, "INV-9.1", rp, 1,
                                f'type must be "2d" or "3d" (got {data.get("type")!r}).', 'Set "type": "3d".'))

    # Icons (INV-9.2). Paths resolve relative to the manifest file.
    icon = data.get("icon")
    icon_3d = data.get("icon_3d")
    layout = data.get("icon_3d_layout", "sbs-lr")
    if icon_3d and not icon:
        findings.append(Finding(ERROR, "INV-9.2", rp, 1,
                                "icon_3d is set but icon (the 2D fallback) is not.",
                                "Add an `icon` (512x512) alongside `icon_3d`."))
    if icon_3d and layout not in ICON_LAYOUTS:
        findings.append(Finding(ERROR, "INV-9.2", rp, 1,
                                f"icon_3d_layout {layout!r} is invalid.",
                                "Use one of sbs-lr | sbs-rl | tb | bt (sbs-lr today)."))
    for key, want in (("icon", (512, 512)), ("icon_3d", (1024, 512))):
        val = data.get(key)
        if not val:
            continue
        ipath = (mpath.parent / val)
        if not ipath.exists():
            findings.append(Finding(ERROR, "INV-9.2", rp, 1,
                                    f"{key} -> {val!r} not found next to the manifest.",
                                    "Add the icon file (path is relative to the manifest)."))
            continue
        dims = png_dimensions(ipath)
        if dims and dims != want:
            findings.append(Finding(WARN, "INV-9.2", rel(ipath, root), 1,
                                    f"{key} is {dims[0]}x{dims[1]}; convention is {want[0]}x{want[1]}.",
                                    f"Re-export {key} at {want[0]}x{want[1]} (PNG)."))


def scan_manifests(root: Path, findings: list):
    manifests = [p for p in root.rglob("*.displayxr.json")
                 if not any(part in EXCLUDE_DIRS for part in p.relative_to(root).parts[:-1])]
    if not manifests:
        findings.append(Finding(WARN, "INV-9.1", str(root), 1,
                                "No *.displayxr.json found — the app won't appear in any workspace launcher.",
                                "Add <exe_basename>.displayxr.json (sidecar next to the exe, or registered drop-in)."))
        return
    for m in manifests:
        validate_manifest(m, root, findings)


def print_findings(findings: list, root: Path) -> None:
    if not findings:
        print(f"✓ check_displayxr_app: no issues in {root}")
        return
    order = {ERROR: 0, WARN: 1}
    findings.sort(key=lambda f: (order[f.level], f.rule, f.path, f.line))
    for f in findings:
        print(f"{f.level:5} {f.path}:{f.line}  [{f.rule}] {f.msg}")
        print(f"      fix: {f.fix}")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description="Lint a DisplayXR app against the authoring invariants.")
    ap.add_argument("app_dir", nargs="?", help="App source directory to check.")
    ap.add_argument("--strict", action="store_true", help="Treat warnings as failures.")
    ap.add_argument("--list-rules", action="store_true", help="Print the rule catalog and exit.")
    args = ap.parse_args(argv)

    if args.list_rules:
        print(f"Rules (see {RULES_DOC}):\n")
        for rule, desc in RULES.items():
            print(f"  {rule:8} {desc}")
        return 0

    if not args.app_dir:
        ap.error("app_dir is required (or use --list-rules)")
    root = Path(args.app_dir).resolve()
    if not root.is_dir():
        print(f"error: {root} is not a directory", file=sys.stderr)
        return 2

    findings: list = []
    scan_sources(root, findings)
    scan_manifests(root, findings)
    print_findings(findings, root)

    n_err = sum(1 for f in findings if f.level == ERROR)
    n_warn = sum(1 for f in findings if f.level == WARN)
    if findings:
        print(f"\n{n_err} error(s), {n_warn} warning(s). See {RULES_DOC} for the full rules.")
    if n_err or (args.strict and n_warn):
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())

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
    "INV-2.8": "Apps requesting MANUAL eye tracking SHOULD handle XrEventDataEyeTrackingStateChangedEXT (tracking loss is the app's problem in MANUAL).",
    "INV-3.1": "Locate into an XRT_MAX_VIEWS (8)-wide buffer; render/submit the active mode's viewCount, never a hardcoded 2.",
    "INV-4.3": "Per-tile render size = window/canvas x scaleXY, never display size.",
    "INV-4.6": "Request an sRGB swapchain (and store a correctly-encoded image); don't double-encode.",
    "INV-7.x": "Capture via xrCaptureAtlasEXT — never reintroduce an app-side CaptureAtlasRegion* readback.",
    "INV-7.2": "xrCaptureAtlasEXT pathPrefix takes NO extension; the runtime appends _atlas.png.",
    "INV-9.1": "Ship a <exe>.displayxr.json (schema_version=1, name 1-64, type 2d|3d) or the app won't appear in the workspace launcher.",
    "INV-9.2": "2D icon is 512x512 (`icon`); 3D icon is 1024x512 (`icon_3d`, requires `icon`); layout in {sbs-lr,sbs-rl,tb,bt}.",
    "INV-10.1": "Apps registering MCP tools (XR_EXT_mcp_tools) declare a manifest `id` (^[a-z0-9][a-z0-9-]{0,31}$) matching the xrSetMCPAppInfoEXT appId.",
}

# Manifest `id` / XrMCPAppInfoEXT appId slug (manifest spec §3.4).
APP_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,31}$")

ERROR, WARN, INFO = "ERROR", "WARN", "INFO"


class Finding:
    __slots__ = ("level", "rule", "path", "line", "msg", "fix")

    def __init__(self, level, rule, path, line, msg, fix):
        self.level = level
        self.rule = rule
        self.path = path
        self.line = line
        self.msg = msg
        self.fix = fix


# --- compiled source patterns: (regex, level, rule, message, fix, multiview_only) ---
# multiview_only=True patterns apply only to N-view EXTENSION apps; they're skipped
# for legacy / non-extension apps (which are legitimately fixed 2-view).
SRC_PATTERNS = [
    (re.compile(r"\bXrView\s+\w+\s*\[\s*2\s*\]"),
     ERROR, "INV-3.1",
     "XrView array hardcoded to [2] — quad modes have 4 views.",
     "Size it XrView views[8] (XRT_MAX_VIEWS) and locate with viewCapacityInput=8.", True),
    (re.compile(r"\bXrCompositionLayerProjectionView\s+\w+\s*\[\s*2\s*\]"),
     ERROR, "INV-3.1",
     "Projection-view array hardcoded to [2].",
     "Allocate eyeCount-sized (active mode's viewCount), e.g. std::vector<...>(eyeCount).", True),
    (re.compile(r"\bdisplay(?:Pixel)?(?:Width|Height)\s*/\s*2\b"),
     ERROR, "INV-4.3",
     "Swapchain/tile size derived from display dimensions (display/2).",
     "Use window/canvas size x recommendedViewScaleX/Y, clamped to the swapchain tile capacity.", False),
    (re.compile(r"\bCaptureAtlasRegion(?:D3D11|D3D12|GL|VK|Metal)?\b"),
     ERROR, "INV-7.x",
     "Deprecated app-side atlas readback (CaptureAtlasRegion*) — removed in the #396 W6 refactor.",
     "Use xrCaptureAtlasEXT (Windows: dxr_capture::RequestRuntimeAtlasCapture; elsewhere call it inline).", False),
    (re.compile(r"\bpathPrefix\b[^;\n]*\"[^\"]*\.png\""),
     WARN, "INV-7.2",
     "xrCaptureAtlasEXT pathPrefix contains a .png extension.",
     "Pass a prefix with NO extension — the runtime appends _atlas.png.", False),
    (re.compile(r"for\s*\([^;]*;[^;]*\b(?:eye|view|v|i)\s*<\s*2\b"),
     WARN, "INV-3.1",
     "Render/eye loop bounded by a literal 2.",
     "Bound by the active mode's viewCount (eyeCount), not 2 — clamp array reads to viewCountOutput.", True),
]

# Tokens that indicate the app uses an sRGB swapchain somewhere (for INV-4.6).
SRGB_TOKENS = re.compile(
    r"_UNORM_SRGB|_SRGB\b|SRGB8_ALPHA8|MTLPixelFormat\w*sRGB|VK_FORMAT_\w*_SRGB",
    re.IGNORECASE,
)
CREATES_SWAPCHAIN = re.compile(r"\bxrCreateSwapchain\b")
# An N-view extension app drives the rendering-mode enumeration; a legacy / fixed-2-view
# app does not. Used to gate the multiview-only checks (so legacy apps aren't false-flagged).
N_VIEW_MARKER = re.compile(
    r"xrEnumerateDisplayRenderingModesEXT|renderingModeCount|XrDisplayRenderingModeInfoEXT"
)
ICON_LAYOUTS = {"sbs-lr", "sbs-rl", "tb", "bt"}


def strip_comments(text: str) -> str:
    """Blank out // and /* */ comments so commented-out code isn't matched.
    Newlines are preserved so reported line numbers stay accurate."""
    text = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), text, flags=re.DOTALL)
    return re.sub(r"//[^\n]*", "", text)


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
    # First pass: read files once; detect whether this is an N-view extension app
    # and whether an sRGB swapchain format appears anywhere.
    files = []
    for path in iter_source_files(root):
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        files.append((path, strip_comments(text)))
    is_extension_app = any(N_VIEW_MARKER.search(t) for _, t in files)
    any_srgb = any(SRGB_TOKENS.search(t) for _, t in files)

    if not is_extension_app and files:
        findings.append(Finding(
            INFO, "note", str(root.name or root), 1,
            "Treated as a legacy / non-extension app (no rendering-mode enumeration) — "
            "multiview view-count checks (INV-3.1) skipped; fixed 2-view is valid here.",
            "If this is meant to be an N-view extension app, enumerate modes "
            "(xrEnumerateDisplayRenderingModesEXT, INV-2.3) and size view arrays to XRT_MAX_VIEWS.",
        ))

    swapchain_loc = None
    for path, text in files:
        for regex, level, rule, msg, fix, multiview_only in SRC_PATTERNS:
            if multiview_only and not is_extension_app:
                continue
            for m in regex.finditer(text):
                line_no = text.count("\n", 0, m.start()) + 1
                findings.append(Finding(level, rule, rel(path, root), line_no, msg, fix))
        if swapchain_loc is None:
            m = CREATES_SWAPCHAIN.search(text)
            if m:
                swapchain_loc = (rel(path, root), text.count("\n", 0, m.start()) + 1)

    # INV-4.6 advisory: creates a swapchain but no sRGB format appears anywhere.
    if swapchain_loc and not any_srgb:
        p, ln = swapchain_loc
        findings.append(Finding(
            WARN, "INV-4.6", p, ln,
            "No sRGB swapchain format detected — INV-4.6 recommends an sRGB swapchain.",
            "Request an sRGB swapchain (_UNORM_SRGB / GL_SRGB8_ALPHA8 / _SRGB / MTLPixelFormat*sRGB). "
            "A UNORM swapchain is valid ONLY if you store display-referred (already-encoded) bytes.",
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


def check_mcp_pairing(root: Path, findings: list):
    """INV-10.1 — XR_EXT_mcp_tools <-> manifest `id` pairing.

    If any source registers MCP tools, a manifest must declare a valid
    `id`; when the appId literal is extractable from the source, it must
    equal the manifest's. Manifests with a malformed `id` get a WARN
    regardless (soft failure per manifest spec §6 — the launcher still
    accepts the app, consumers fall back to the exe basename).
    """
    # Manifest ids.
    manifest_ids = {}
    for m in root.rglob("*.displayxr.json"):
        if any(part in EXCLUDE_DIRS for part in m.relative_to(root).parts[:-1]):
            continue
        try:
            data = json.loads(m.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            continue  # INV-9.1 already reported it.
        app_id = data.get("id")
        if app_id is None:
            continue
        if not isinstance(app_id, str) or not APP_ID_RE.match(app_id):
            findings.append(Finding(WARN, "INV-10.1", rel(m, root), 1,
                                    f"id {app_id!r} does not match ^[a-z0-9][a-z0-9-]{{0,31}}$ — consumers will ignore it.",
                                    "Use a lowercase slug (letters/digits/hyphens, no underscores — '__' is the MCP namespace separator)."))
            continue
        manifest_ids[rel(m, root)] = app_id

    # Source-side usage + declared appId literals.
    uses_mcp_tools = False
    declared = []  # (path, line, appId literal)
    appid_re = re.compile(r"\.appId\s*=\s*\"([^\"]*)\"|appId\s*,\s*\"([^\"]*)\"")
    for p in sorted(root.rglob("*")):
        if p.suffix.lower() not in SOURCE_EXTS or not p.is_file():
            continue
        if any(part in EXCLUDE_DIRS for part in p.relative_to(root).parts[:-1]):
            continue
        try:
            text = p.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if "xrRegisterMCPToolEXT" in text or "xrSetMCPAppInfoEXT" in text:
            uses_mcp_tools = True
        for i, line in enumerate(text.splitlines(), 1):
            m = appid_re.search(line)
            if m:
                declared.append((rel(p, root), i, m.group(1) or m.group(2)))

    if not uses_mcp_tools:
        return
    if not manifest_ids:
        findings.append(Finding(ERROR, "INV-10.1", str(root), 1,
                                "Source registers MCP tools (XR_EXT_mcp_tools) but no manifest declares an `id`.",
                                'Add "id": "<slug>" to the .displayxr.json — it is the agent-visible tool prefix.'))
        return
    for path, line, lit in declared:
        if lit and lit not in manifest_ids.values():
            findings.append(Finding(ERROR, "INV-10.1", path, line,
                                    f"xrSetMCPAppInfoEXT appId {lit!r} does not match any manifest id "
                                    f"({', '.join(sorted(set(manifest_ids.values())))}).",
                                    "Make the code and manifest agree — agents key tool names on this slug."))


def check_manual_tracking_event(root: Path, findings: list):
    """INV-2.8 (advisory) — MANUAL eye tracking <-> tracking-state event.

    An app that REQUESTS MANUAL mode has opted out of the vendor's grace
    period / collapse animation / auto 2D fallback — tracking loss becomes the
    app's problem, and the edge-triggered XrEventDataEyeTrackingStateChangedEXT
    (#441 v14) is the intended primitive for reacting to it. Conservative
    trigger: only fires when BOTH xrRequestEyeTrackingModeEXT is called AND the
    XR_EYE_TRACKING_MODE_MANUAL_EXT enum appears (merely printing "MANUAL" in a
    HUD doesn't flag), and no source references the event type.
    """
    requests_manual = None  # (path, line) of the first MANUAL enum use
    calls_request = False
    handles_event = False
    for p in sorted(root.rglob("*")):
        if p.suffix.lower() not in SOURCE_EXTS or not p.is_file():
            continue
        if any(part in EXCLUDE_DIRS for part in p.relative_to(root).parts[:-1]):
            continue
        try:
            text = strip_comments(p.read_text(encoding="utf-8", errors="replace"))
        except OSError:
            continue
        if "xrRequestEyeTrackingModeEXT" in text:
            calls_request = True
        m = re.search(r"\bXR_EYE_TRACKING_MODE_MANUAL_EXT\b", text)
        if m and requests_manual is None:
            requests_manual = (rel(p, root), text.count("\n", 0, m.start()) + 1)
        if ("XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_EXT" in text or
                "XrEventDataEyeTrackingStateChangedEXT" in text):
            handles_event = True
        # Apps built on test_apps/common delegate event polling to the shared
        # PollEvents(XrSessionManager&), which handles the event (common/ is
        # excluded from linting, so look for the call instead).
        if re.search(r"\bPollEvents\s*\(", text):
            handles_event = True

    if calls_request and requests_manual and not handles_event:
        path, line = requests_manual
        findings.append(Finding(
            WARN, "INV-2.8", path, line,
            "App requests MANUAL eye tracking but never handles XrEventDataEyeTrackingStateChangedEXT.",
            "Handle the event in your xrPollEvent loop (run your own loss transition, request a 2D "
            "mode when ready) — in MANUAL mode the vendor does no grace period or auto-fallback for you.",
        ))


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
        # ASCII on purpose: a checkmark glyph dies with UnicodeEncodeError on
        # cp1252 Windows consoles, turning a CLEAN lint into exit 1.
        print(f"OK check_displayxr_app: no issues in {root}")
        return
    order = {ERROR: 0, WARN: 1, INFO: 2}
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
    check_mcp_pairing(root, findings)
    check_manual_tracking_event(root, findings)
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

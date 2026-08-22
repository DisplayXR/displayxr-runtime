#!/usr/bin/env python3
"""
DisplayXR app linter — check an app source tree against the authoring invariants.

This is the machine-checkable companion to docs/guides/displayxr-app-rules.md.
It does NOT understand C/C++ semantically; it pattern-matches the specific
anti-patterns the rules doc calls out (the ones coding agents reliably get
wrong) and reports each with the INV-* rule it violates, a file:line, and a
one-line fix. It also validates the workspace manifest + icon sizes, and — for
apps that ship an AndroidManifest.xml — the Android pixel-exactness rules of
§11 (parsed from the manifest plus greps over Java/Kotlin/native sources). The
Android section no-ops entirely on a Windows/macOS/Linux app.

Conservative by design: every check is high-signal so a finding is almost
always real. Color (INV-4.6) and a couple of others are advisory (WARN), the
hard structural mistakes are errors.

Usage:
    check_displayxr_app.py <app-dir>            # lint one app
    check_displayxr_app.py test_apps/handle/cube_handle_d3d11_win
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
import xml.etree.ElementTree as ET
from pathlib import Path

RULES_DOC = "docs/guides/displayxr-app-rules.md"

# Directories never linted: shared reference code, vendored headers, build output.
EXCLUDE_DIRS = {
    "build", ".git", "third_party", "openxr_includes", "common",
    "_package", "__pycache__", "node_modules", ".vs", "out",
}
SOURCE_EXTS = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".m", ".mm"}
# JVM sources are linted for the Android rules only (§11) — the C/C++ checks
# above don't apply to them.
JVM_EXTS = {".java", ".kt"}

# ---- rule catalog (kept in sync with docs/guides/displayxr-app-rules.md) ----
RULES = {
    "F-1": "Run the frame loop from READY, gated on a 'session running' flag (not SYNCHRONIZED+); a compliant runtime only leaves READY on your first xrBeginFrame, so a SYNCHRONIZED+ gate deadlocks (black screen).",
    "INV-2.8": "Apps requesting MANUAL eye tracking SHOULD handle XrEventDataEyeTrackingStateChangedDXR (tracking loss is the app's problem in MANUAL).",
    "INV-3.1": "Locate into an XRT_MAX_VIEWS (8)-wide buffer; render/submit the active mode's viewCount, never a hardcoded 2.",
    "INV-4.3": "Per-tile render size = window/canvas x scaleXY, never display size.",
    "INV-4.6": "Request an sRGB swapchain (and store a correctly-encoded image); don't double-encode.",
    "INV-4.7": "Write every pixel of the imageRect you declare — clear partial-tile renders to (0,0,0,0) first (or shrink the rect); undefined pixels read as opaque magenta on MoltenVK and break transparent-bg.",
    "INV-5.9": "VK apps MUST use XR_KHR_vulkan_enable2 (the runtime creates the VkDevice via xrCreateVulkanDeviceKHR); an app-side vkCreateDevice = enable1, which forfeits the #868 weave-rate decoupling and the late-weave pacing.",
    "INV-7.x": "Capture via xrCaptureAtlasDXR — never reintroduce an app-side CaptureAtlasRegion* readback.",
    "INV-7.2": "xrCaptureAtlasDXR pathPrefix takes NO extension; the runtime appends _atlas.png.",
    "INV-9.1": "Ship a <exe>.displayxr.json (schema_version=1, name 1-64, type 2d|3d) or the app won't appear in the workspace launcher.",
    "INV-9.2": "2D icon is 512x512 (`icon`); 3D icon is 1024x512 (`icon_3d`, requires `icon`); layout in {sbs-lr,sbs-rl,tb,bt}.",
    "INV-10.1": "Apps registering MCP tools (XR_DXR_mcp_tools) declare a manifest `id` (^[a-z0-9][a-z0-9-]{0,31}$) matching the xrSetMCPAppInfoDXR appId.",
    # --- Android pixel-exactness (§11). All no-op unless an AndroidManifest.xml is found. ---
    "INV-1.4": "(Android) Declare PROPERTY_COMPAT_ALLOW_SANDBOXING_VIEW_BOUNDS_APIS=false — else getLocationOnScreen may go window-relative and every window weaves at the same wrong phase.",
    "INV-11.1": "(Android) resizeableActivity=true, no fixed screenOrientation, no min/maxAspectRatio — fixed activities invite invisible server-side size-compat/letterbox scaling that destroys the interlace.",
    "INV-11.2": "(Android) Never SurfaceHolder.setFixedSize() — SurfaceView then scales the buffer onto mScreenRect.",
    "INV-11.3": "(Android) Swapchain imageExtent must equal surface currentExtent on every recreate (runtime-owned for _handle apps; app-owned for present-owners).",
    "INV-11.4": "(Android) SurfaceView only, never TextureView; never lockCanvas()/ANativeWindow_lock on your own SurfaceView (poisons the BufferQueue for GL/VK).",
    "INV-11.5": "(Android) Honour getBufferTransformHint()/currentTransform (pin preTransform=IDENTITY) or the composer rotates the buffer under you.",
    "INV-11.6": "(Android) Opaque sRGB non-HDR buffers — no wide-gamut/HDR dataspace (setBuffersDataSpace, DATASPACE_BT2020, wide color gamut).",
    "INV-11.7": "(Android) No translucent app UI over the SurfaceView region — it alpha-blends over the woven, per-subpixel-interlaced pixels.",
    "INV-11.8": "(Android) Declare a full configChanges set so a relayout doesn't recreate the activity (and the OpenXR session) mid-session.",
    "INV-11.9": "(Android) Declare a <queries> block for the vendor display services (neutral intent action, or the documented package names) — package visibility is per calling uid, so an in-process app that omits it aborts in the vendor loader.",
}

# INV-11.9: the vendor-neutral action a <queries><intent> should target (R1 / L7).
NEUTRAL_VENDOR_ACTION = "org.displayxr.action.VENDOR_DISPLAY_SERVICE"
# Packages in these namespaces are the runtime/loader, not a vendor display service.
NON_VENDOR_PKG_PREFIXES = ("org.khronos.", "org.freedesktop.monado.", "org.displayxr.")

# Manifest `id` / XrMCPAppInfoDXR appId slug (manifest spec §3.4).
APP_ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]{0,31}$")

# Optional runtime-version floor (manifest spec §3.5) — MAJOR.MINOR.PATCH.
MIN_RUNTIME_RE = re.compile(r"^\d+\.\d+\.\d+$")

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
     "Use xrCaptureAtlasDXR (Windows: dxr_capture::RequestRuntimeAtlasCapture; elsewhere call it inline).", False),
    (re.compile(r"\bpathPrefix\b[^;\n]*\"[^\"]*\.png\""),
     WARN, "INV-7.2",
     "xrCaptureAtlasDXR pathPrefix contains a .png extension.",
     "Pass a prefix with NO extension — the runtime appends _atlas.png.", False),
    (re.compile(r"for\s*\([^;]*;[^;]*\b(?:eye|view|v|i)\s*<\s*2\b"),
     WARN, "INV-3.1",
     "Render/eye loop bounded by a literal 2.",
     "Bound by the active mode's viewCount (eyeCount), not 2 — clamp array reads to viewCountOutput.", True),
    (re.compile(r"(?:==|!=|>=|<=)\s*XR_SESSION_STATE_(?:SYNCHRONIZED|VISIBLE|FOCUSED)\b"
                r"|\bXR_SESSION_STATE_(?:SYNCHRONIZED|VISIBLE|FOCUSED)\s*(?:==|!=|>=|<=)"),
     WARN, "F-1",
     "Frame loop appears gated on a visibility session-state (SYNCHRONIZED/VISIBLE/FOCUSED).",
     "Gate the frame loop on a 'session running' flag (set true on READY, false on STOPPING) plus a "
     "live window/surface, and gate drawing on frameState.shouldRender. A spec-compliant runtime only "
     "advances READY->SYNCHRONIZED on your FIRST xrBeginFrame, so gating the loop on SYNCHRONIZED+ "
     "deadlocks at READY (black screen). See F-1 in docs/guides/displayxr-app-rules.md.", False),
]

# Tokens that indicate the app uses an sRGB swapchain somewhere (for INV-4.6).
# INV-5.9: a VK app that calls vkCreateDevice itself is using XR_KHR_vulkan_enable
# (enable1) — the app owns the device. enable2 apps have the RUNTIME create the
# device (xrCreateVulkanDeviceKHR) and therefore have no vkCreateDevice call, so
# this token is the definitive enable1 tell.
VK_CREATES_DEVICE = re.compile(r"\bvkCreateDevice\b")

SRGB_TOKENS = re.compile(
    r"_UNORM_SRGB|_SRGB\b|SRGB8_ALPHA8|MTLPixelFormat\w*sRGB|VK_FORMAT_\w*_SRGB",
    re.IGNORECASE,
)
CREATES_SWAPCHAIN = re.compile(r"\bxrCreateSwapchain\b")
# An N-view extension app drives the rendering-mode enumeration; a legacy / fixed-2-view
# app does not. Used to gate the multiview-only checks (so legacy apps aren't false-flagged).
N_VIEW_MARKER = re.compile(
    r"xrEnumerateDisplayRenderingModesDXR|renderingModeCount|XrDisplayRenderingModeInfoDXR"
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
            "(xrEnumerateDisplayRenderingModesDXR, INV-2.3) and size view arrays to XRT_MAX_VIEWS.",
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

    # INV-5.9 (enforced): a VK app that creates its own VkDevice is using
    # XR_KHR_vulkan_enable (enable1). enable1 forfeits the runtime-owned VkQueue
    # the #868 repaint needs (weave-rate decoupling) AND the late-weave pacing —
    # both hang off who creates the device. VK apps MUST use enable2, where the
    # runtime creates the device via xrCreateVulkanDeviceKHR (no vkCreateDevice).
    vk_device_loc = None
    for path, text in files:
        m = VK_CREATES_DEVICE.search(text)
        if m:
            vk_device_loc = (rel(path, root), text.count("\n", 0, m.start()) + 1)
            break
    if vk_device_loc:
        p, ln = vk_device_loc
        findings.append(Finding(
            ERROR, "INV-5.9", p, ln,
            "App calls vkCreateDevice -> XR_KHR_vulkan_enable (enable1). enable1 forfeits the "
            "runtime-owned VkQueue the #868 weave-rate decoupling needs and the late-weave "
            "pacing (both hang off who creates the VkDevice).",
            "Migrate to XR_KHR_vulkan_enable2: request XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME and "
            "create instance/device via xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR (no "
            "app-side vkCreateInstance / vkCreateDevice). Reference: test_apps/handle/cube_handle_vk_win.",
        ))

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

    # Optional runtime-version floor (spec §3.5). Soft failure: warn + ignore if malformed.
    min_rt = data.get("min_runtime")
    if min_rt is not None and not (isinstance(min_rt, str) and MIN_RUNTIME_RE.match(min_rt)):
        findings.append(Finding(WARN, "INV-9.1", rp, 1,
                                f"min_runtime {min_rt!r} is not a MAJOR.MINOR.PATCH string; it will be ignored.",
                                'Set "min_runtime": "2.0.6" (or remove it) — a malformed floor is dropped, not gated.'))


def check_mcp_pairing(root: Path, findings: list):
    """INV-10.1 — XR_DXR_mcp_tools <-> manifest `id` pairing.

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
        if "xrRegisterMCPToolDXR" in text or "xrSetMCPAppInfoDXR" in text:
            uses_mcp_tools = True
        for i, line in enumerate(text.splitlines(), 1):
            m = appid_re.search(line)
            if m:
                declared.append((rel(p, root), i, m.group(1) or m.group(2)))

    if not uses_mcp_tools:
        return
    if not manifest_ids:
        findings.append(Finding(ERROR, "INV-10.1", str(root), 1,
                                "Source registers MCP tools (XR_DXR_mcp_tools) but no manifest declares an `id`.",
                                'Add "id": "<slug>" to the .displayxr.json — it is the agent-visible tool prefix.'))
        return
    for path, line, lit in declared:
        if lit and lit not in manifest_ids.values():
            findings.append(Finding(ERROR, "INV-10.1", path, line,
                                    f"xrSetMCPAppInfoDXR appId {lit!r} does not match any manifest id "
                                    f"({', '.join(sorted(set(manifest_ids.values())))}).",
                                    "Make the code and manifest agree — agents key tool names on this slug."))


def check_manual_tracking_event(root: Path, findings: list):
    """INV-2.8 (advisory) — MANUAL eye tracking <-> tracking-state event.

    An app that REQUESTS MANUAL mode has opted out of the vendor's grace
    period / collapse animation / auto 2D fallback — tracking loss becomes the
    app's problem, and the edge-triggered XrEventDataEyeTrackingStateChangedDXR
    (#441 v14) is the intended primitive for reacting to it. Conservative
    trigger: only fires when BOTH xrRequestEyeTrackingModeDXR is called AND the
    XR_EYE_TRACKING_MODE_MANUAL_DXR enum appears (merely printing "MANUAL" in a
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
        if "xrRequestEyeTrackingModeDXR" in text:
            calls_request = True
        m = re.search(r"\bXR_EYE_TRACKING_MODE_MANUAL_EXT\b", text)
        if m and requests_manual is None:
            requests_manual = (rel(p, root), text.count("\n", 0, m.start()) + 1)
        if ("XR_TYPE_EVENT_DATA_EYE_TRACKING_STATE_CHANGED_DXR" in text or
                "XrEventDataEyeTrackingStateChangedDXR" in text):
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
            "App requests MANUAL eye tracking but never handles XrEventDataEyeTrackingStateChangedDXR.",
            "Handle the event in your xrPollEvent loop (run your own loss transition, request a 2D "
            "mode when ready) — in MANUAL mode the vendor does no grace period or auto-fallback for you.",
        ))



# ---------------------------------------------------------------------------
# Android pixel-exactness (docs/guides/displayxr-app-rules.md §11).
#
# Weaving is pixel-exact; Android is the one platform that can insert a scale
# or rotate between the app's buffer and the panel invisibly to the client
# (WindowState.mGlobalScale = mCompatScale * mOverrideScale). Every check below
# is gated on finding an AndroidManifest.xml, so this whole section no-ops for
# Windows / macOS / Linux apps.
# ---------------------------------------------------------------------------

ANDROID_NS = "http://schemas.android.com/apk/res/android"

# screenOrientation values that do NOT pin an orientation. Anything else
# (portrait, landscape, sensorPortrait, locked, nosensor, ...) is a fixed
# orientation and invites size-compat treatment.
FREE_ORIENTATIONS = {"unspecified", "behind", "sensor", "fullsensor", "user", "fulluser"}

# configChanges an XR activity must own itself or the framework recreates it
# (tearing down the OpenXR session + the display processor) on every relayout.
REQUIRED_CONFIG_CHANGES = [
    "orientation", "screensize", "screenlayout", "smallestscreensize",
    "keyboardhidden", "density",
]

SANDBOX_PROPERTY = "android.window.PROPERTY_COMPAT_ALLOW_SANDBOXING_VIEW_BOUNDS_APIS"

# --- source patterns, applied to JVM + native sources of an Android app ---
ANDROID_SRC_PATTERNS = [
    (re.compile(r"\bsetFixedSize\s*\("),
     ERROR, "INV-11.2",
     "SurfaceHolder.setFixedSize() — SurfaceView decouples mSurfaceWidth/Height from "
     "mScreenRect and SurfaceFlinger SCALES the buffer onto the view rect; the interlace "
     "does not survive the resample.",
     "Delete the call and render at whatever size the surface actually is "
     "(surfaceChanged width/height, or VkSurfaceCapabilitiesKHR::currentExtent)."),
    (re.compile(r"\bTextureView\b"),
     ERROR, "INV-11.4",
     "TextureView is composited through the view hierarchy as a GL texture (not a "
     "SurfaceFlinger layer) — transformable, filterable and a frame late. All three "
     "break a weave.",
     "Use a SurfaceView (AOSP: source.android.com/docs/core/graphics/arch-tv)."),
    (re.compile(r"\blockCanvas\s*\(|\bANativeWindow_lock\s*\("),
     ERROR, "INV-11.4",
     "lockCanvas()/ANativeWindow_lock connects the CPU producer to the SurfaceView's "
     "BufferQueue and PERMANENTLY poisons it for GL/Vulkan (later connects return "
     "BAD_VALUE \"already connected\").",
     "Never draw on your own SurfaceView — the runtime (or your VK swapchain) is the "
     "only producer (AOSP: source.android.com/docs/core/graphics/arch-sh)."),
    (re.compile(r"\bsetBuffersDataSpace\b|\bDATASPACE_(?:BT2020|SCRGB|DISPLAY_P3)\w*"
                r"|\bisWideColorGamut\b|\bsetColorMode\s*\(|\bCOLOR_MODE_HDR\b"),
     WARN, "INV-11.6",
     "Wide-gamut / HDR dataspace requested — colour management is then applied between "
     "your buffer and the panel, remapping the per-subpixel values the interlace depends on.",
     "Ask for a plain 8-bit sRGB surface and leave the dataspace alone (INV-4.6 / INV-11.6)."),
    (re.compile(r"\brequestedOrientation\s*=\s*[\w.]*SCREEN_ORIENTATION_"
                r"(?:PORTRAIT|LANDSCAPE|REVERSE_PORTRAIT|REVERSE_LANDSCAPE|SENSOR_PORTRAIT"
                r"|SENSOR_LANDSCAPE|USER_PORTRAIT|USER_LANDSCAPE|NOSENSOR|LOCKED)\b"),
     ERROR, "INV-11.1",
     "Orientation pinned programmatically — same size-compat/letterbox exposure as a "
     "manifest android:screenOrientation.",
     "Use SCREEN_ORIENTATION_FULL_SENSOR / UNSPECIFIED / USER, or don't set it at all."),
    (re.compile(r"\bsetZOrderOnTop\s*\("),
     INFO, "INV-11.7",
     "setZOrderOnTop() puts the SurfaceView above the window — deliberate is fine, "
     "accidental is not.",
     "Confirm nothing translucent is drawn over the weave rect; an OPAQUE overlay on top "
     "is fine, a translucent one alpha-blends onto per-subpixel-interlaced pixels."),
]

# App owns the presentation itself (an XR_DXR_weave client), so INV-11.3 / INV-11.5
# are ITS rules rather than the runtime's.
PRESENT_OWNER = re.compile(r"\bvkCreateSwapchainKHR\b|\beglSwapBuffers\b|\beglCreateWindowSurface\b")
HONOURS_EXTENT = re.compile(r"\bcurrentExtent\b")
HONOURS_TRANSFORM = re.compile(r"\bgetBufferTransformHint\b|\bpreTransform\b|\bcurrentTransform\b"
                               r"|\bsetBuffersTransform\b")


def iter_android_sources(root: Path):
    """JVM + native sources, used only by the §11 checks."""
    exts = SOURCE_EXTS | JVM_EXTS
    for p in sorted(root.rglob("*")):
        if not p.is_file() or p.suffix.lower() not in exts:
            continue
        if any(part in EXCLUDE_DIRS for part in p.relative_to(root).parts[:-1]):
            continue
        yield p


def _attr(el, name):
    return el.get("{%s}%s" % (ANDROID_NS, name))


def check_android_manifest(mpath: Path, root: Path, findings: list) -> None:
    rp = rel(mpath, root)
    try:
        tree = ET.parse(str(mpath))
    except (OSError, ET.ParseError) as e:
        findings.append(Finding(WARN, "INV-11.1", rp, 1,
                                f"AndroidManifest.xml could not be parsed ({e}) — Android checks skipped.",
                                "Fix the XML."))
        return
    manifest = tree.getroot()
    app = manifest.find("application")
    if app is None:
        return

    # --- INV-1.4: view-bounds sandboxing opt-out (app-scoped <property>) ---
    prop_val = None
    for prop in app.iter("property"):
        if _attr(prop, "name") == SANDBOX_PROPERTY:
            prop_val = (_attr(prop, "value") or "").strip().lower()
            break
    if prop_val is None:
        findings.append(Finding(
            INFO, "INV-1.4", rp, 1,
            f"No <property android:name=\"{SANDBOX_PROPERTY}\" android:value=\"false\" /> in "
            "<application>.",
            "Add it. On an OEM that applies OVERRIDE_SANDBOX_VIEW_BOUNDS_APIS, "
            "View.getLocationOnScreen() returns WINDOW-relative coords, every window reports "
            "(0,0), and side-by-side windows weave at the same wrong phase with no error "
            "anywhere. The opt-out is per-app (ADR-036 D6, #1033)."))
    elif prop_val != "false":
        findings.append(Finding(
            INFO, "INV-1.4", rp, 1,
            f"{SANDBOX_PROPERTY} is {prop_val!r}, not \"false\".",
            "Set android:value=\"false\" — \"true\" opts INTO window-relative "
            "getLocationOnScreen(), which breaks the weave phase anchor."))

    # --- INV-11.9: per-uid package visibility for the vendor display services ---
    # Visibility is enforced per CALLING uid; with the runtime in-process the vendor
    # plug-in calls getPackageInfo() from THIS app's uid. Either form satisfies it:
    # the neutral action (preferred) or literal <package> names.
    queries = manifest.find("queries")
    has_neutral = False
    has_vendor_pkg = False
    if queries is not None:
        for act in queries.iter("action"):
            if _attr(act, "name") == NEUTRAL_VENDOR_ACTION:
                has_neutral = True
        for pkg in queries.iter("package"):
            nm = _attr(pkg, "name") or ""
            # The runtime/loader packages are a different concern (finding the
            # runtime at all); only a package outside those namespaces can be a
            # vendor display service.
            if nm and not nm.startswith(NON_VENDOR_PKG_PREFIXES):
                has_vendor_pkg = True
    if not has_neutral and not has_vendor_pkg:
        findings.append(Finding(
            INFO, "INV-11.9", rp, 1,
            "<queries> names neither the vendor-neutral display-service action nor any vendor "
            "package — Android 11+ enforces package visibility per CALLING uid, and "
            "an in-process runtime makes the vendor display-processor plug-in call "
            "PackageManager.getPackageInfo() from THIS app's uid. The runtime APK declaring the "
            "packages does not help; visibility does not inherit.",
            "Add to <queries> either <intent><action android:name=\""
            + NEUTRAL_VENDOR_ACTION +
            "\"/></intent> (preferred), or the vendor display-configuration + head-tracking "
            "package names the vendor documents. Without it the process aborts ~150 ms after "
            "plug-in load with a CheckJNI 'java_object == null' in GetObjectClass "
            "(INV-11.9; L7 / ADR-036 D5)."))

    # --- INV-11.7: a translucent window theme blends over the weave ---
    for el, what in [(app, "<application>")] + [(a, "<activity>") for a in app.iter("activity")]:
        theme = _attr(el, "theme") or ""
        if "translucent" in theme.lower():
            findings.append(Finding(
                WARN, "INV-11.7", rp, 1,
                f"{what} declares a translucent theme ({theme}) — anything drawn over the "
                "SurfaceView hole punch is alpha-blended onto the woven, per-subpixel "
                "interlaced pixels.",
                "Use an opaque theme and keep app chrome outside the weave rect."))

    # --- INV-11.1: fixed aspect ratio (activity attrs + the legacy meta-data) ---
    for md in app.iter("meta-data"):
        if _attr(md, "name") in ("android.max_aspect", "android.min_aspect"):
            findings.append(Finding(
                ERROR, "INV-11.1", rp, 1,
                f"{_attr(md, 'name')} meta-data pins the aspect ratio — the window manager "
                "letterboxes and scales (mCompatScale) to honour it.",
                "Remove it; a weaving app must accept whatever rect it is given."))

    activities = list(app.iter("activity"))
    if not activities:
        return
    for act in activities:
        name = _attr(act, "name") or "<activity>"

        # INV-11.1 — resizeable
        resizeable = _attr(act, "resizeableActivity")
        if resizeable is None:
            findings.append(Finding(
                ERROR, "INV-11.1", rp, 1,
                f"{name}: android:resizeableActivity is not declared.",
                'Declare android:resizeableActivity="true". A non-resizable activity is '
                "exactly what makes the window manager apply size-compat scaling "
                "(WindowState.mGlobalScale), which is invisible to the client and destroys "
                "the interlace."))
        elif resizeable.strip().lower() != "true":
            findings.append(Finding(
                ERROR, "INV-11.1", rp, 1,
                f"{name}: android:resizeableActivity={resizeable!r} — a non-resizable activity "
                "gets size-compat/letterbox scaling applied server-side, invisibly.",
                'Set android:resizeableActivity="true".'))

        # INV-11.1 — fixed orientation
        orientation = _attr(act, "screenOrientation")
        if orientation is not None and orientation.strip().lower() not in FREE_ORIENTATIONS:
            findings.append(Finding(
                ERROR, "INV-11.1", rp, 1,
                f"{name}: android:screenOrientation={orientation!r} pins the orientation — a "
                "fixed-orientation activity is letterboxed and compat-scaled on a panel whose "
                "orientation differs.",
                "Drop the attribute (or use unspecified/fullSensor/user) and adapt at runtime; "
                "the runtime + DP re-derive the weave axes per orientation."))

        # INV-11.1 — fixed aspect ratio
        for attr_name in ("maxAspectRatio", "minAspectRatio"):
            val = _attr(act, attr_name)
            if val is not None:
                findings.append(Finding(
                    ERROR, "INV-11.1", rp, 1,
                    f"{name}: android:{attr_name}={val!r} pins the aspect ratio — the window "
                    "manager letterboxes and scales to honour it.",
                    f"Remove android:{attr_name}."))

        # INV-11.8 — configChanges
        cfg = (_attr(act, "configChanges") or "").lower()
        declared = set(x.strip() for x in cfg.split("|") if x.strip())
        missing = [c for c in REQUIRED_CONFIG_CHANGES if c not in declared]
        if missing:
            findings.append(Finding(
                WARN, "INV-11.8", rp, 1,
                f"{name}: android:configChanges is missing {'|'.join(missing)} — the framework "
                "recreates the activity (and with it the NativeActivity thread, the OpenXR "
                "session and the display processor) on those changes.",
                'Declare android:configChanges="orientation|keyboardHidden|screenSize|'
                'screenLayout|smallestScreenSize|density|uiMode|navigation|keyboard|'
                'layoutDirection" and handle onConfigurationChanged instead.'))


def check_android(root: Path, findings: list) -> None:
    """§11 — Android pixel-exactness. No-ops entirely for non-Android apps."""
    manifests = [m for m in sorted(root.rglob("AndroidManifest.xml"))
                 if not any(part in EXCLUDE_DIRS for part in m.relative_to(root).parts[:-1])]
    if not manifests:
        return  # Not an Android app — every check below is Android-only.

    for m in manifests:
        check_android_manifest(m, root, findings)

    # --- source-level checks ---
    files = []
    for path in iter_android_sources(root):
        try:
            files.append((path, strip_comments(path.read_text(encoding="utf-8", errors="replace"))))
        except OSError:
            continue

    for path, text in files:
        for regex, level, rule, msg, fix in ANDROID_SRC_PATTERNS:
            for mt in regex.finditer(text):
                line_no = text.count("\n", 0, mt.start()) + 1
                findings.append(Finding(level, rule, rel(path, root), line_no, msg, fix))

    # INV-11.3 / INV-11.5 are runtime invariants for _handle apps (the runtime owns
    # the VkSurfaceKHR, the swapchain and the present). They become APP rules only
    # for a present-owner — an XR_DXR_weave client that creates its own swapchain.
    owner_loc = None
    for path, text in files:
        mt = PRESENT_OWNER.search(text)
        if mt:
            owner_loc = (rel(path, root), text.count("\n", 0, mt.start()) + 1)
            break
    if not owner_loc:
        return
    p, ln = owner_loc
    if not any(HONOURS_EXTENT.search(t) for _, t in files):
        findings.append(Finding(
            INFO, "INV-11.3", p, ln,
            "App owns its own swapchain/present but never reads "
            "VkSurfaceCapabilitiesKHR::currentExtent.",
            "Size imageExtent from a freshly queried currentExtent on every create/recreate "
            "(and on every OUT_OF_DATE/SUBOPTIMAL). A mismatched extent is scaled at present "
            "time and the interlace is lost. Runtime reference: "
            "src/xrt/compositor/main/comp_target_swapchain.c select_extent()."))
    if not any(HONOURS_TRANSFORM.search(t) for _, t in files):
        findings.append(Finding(
            INFO, "INV-11.5", p, ln,
            "App owns its own swapchain/present but never references the buffer transform "
            "(getBufferTransformHint / preTransform / currentTransform).",
            "Pin preTransform to VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR (what the runtime does "
            "on Android, comp_target_swapchain.c) so the buffer scans out 1:1; otherwise the "
            "composer rotates + resamples it under you."))

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


def dedupe(findings: list) -> list:
    """Collapse identical (level, rule, path, line, msg) findings — two alternates of one
    regex can match the same line and reporting it twice is pure noise."""
    seen = set()
    out = []
    for f in findings:
        key = (f.level, f.rule, f.path, f.line, f.msg)
        if key in seen:
            continue
        seen.add(key)
        out.append(f)
    return out


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
    check_android(root, findings)
    findings = dedupe(findings)
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

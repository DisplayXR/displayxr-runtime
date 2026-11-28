#!/usr/bin/env python3
"""Unit tests for the ADR-066 live-mode-table checks in scripts/check_displayxr_app.py.

Hermetic: each case writes a tiny synthetic app tree to a temp dir and lints it.

  INV-2.10  an app that reads the rendering-mode table must handle
            XrEventDataRenderingModeTableChangedDXR and re-read the table on it
            (a screen's 3D mode changes under an unchanged active index; #2875).
  INV-4.2   such an app sizes its projection swapchain from
            XrDisplayRenderingWorstCaseDXR (display_info v28), or through
            displayxr-common >= v2.37.0 CreateSwapchain().
  INV-4.9   xrCreateSwapchain from a rendering-mode event handler is an error
            (ADR-066 Amendment 2: swapchains are never recreated).

    python3 scripts/tests/test_check_displayxr_app_live_mode_table.py
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_displayxr_app as lint  # noqa: E402

# Start-up read only: the #2852 shape.
READ_ONCE = """
static void initModes(App &app) {
    uint32_t n = 0;
    app.pfnEnum(app.session, 0, &n, nullptr);
    XrDisplayRenderingModeInfoDXR modes[8] = {};
    for (uint32_t i = 0; i < n; i++) modes[i].type = XR_TYPE_DISPLAY_RENDERING_MODE_INFO_DXR;
    xrEnumerateDisplayRenderingModesDXR(app.session, n, &n, modes);
}
void poll(App &app) {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(app.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_RENDERING_MODE_CHANGED_DXR:
            app.current = ((XrEventDataRenderingModeChangedDXR *)&ev)->currentModeIndex;
            break;
        default: break;
        }
    }
}
"""

# The #2875 shape: one enumerate function, called from the table-changed case.
REREADS_VIA_FUNCTION = """
static void EnumerateRenderingModes(App &app) {
    uint32_t n = 0;
    xrEnumerateDisplayRenderingModesDXR(app.session, 0, &n, nullptr);
}
void poll(App &app) {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(app.instance, &ev) == XR_SUCCESS) {
        switch (ev.type) {
        case XR_TYPE_EVENT_DATA_RENDERING_MODE_TABLE_CHANGED_DXR: {
            auto *e = (XrEventDataRenderingModeTableChangedDXR *)&ev;
            EnumerateRenderingModes(app);
            app.current = e->activeModeIndex;
            break;
        }
        default: break;
        }
    }
}
"""

# Same, but the enumerate is called through an opaque function pointer
# (the Android test apps' g_pfnEnumModes) inside an if/else-if chain.
REREADS_VIA_PFN = """
static PFN_xrEnumerateDisplayRenderingModesDXR g_pfnEnumModes = nullptr;
void load() {
    xrGetInstanceProcAddr(g_instance, "xrEnumerateDisplayRenderingModesDXR",
                          reinterpret_cast<PFN_xrVoidFunction *>(&g_pfnEnumModes));
}
void enumerate_rendering_modes() {
    uint32_t mc = 0;
    g_pfnEnumModes(g_session, 0, &mc, nullptr);
}
void poll() {
    if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
        handle();
    } else if (ev.type == XR_TYPE_EVENT_DATA_RENDERING_MODE_TABLE_CHANGED_DXR) {
        const auto *e = reinterpret_cast<const XrEventDataRenderingModeTableChangedDXR *>(&ev);
        if (e->session == g_session) {
            enumerate_rendering_modes();
        }
    }
}
"""

# Handles the event but only logs it.
HANDLES_NO_REREAD = """
void startup() { xrEnumerateDisplayRenderingModesDXR(s, 0, &n, nullptr); }
void poll() {
    switch (ev.type) {
    case XR_TYPE_EVENT_DATA_RENDERING_MODE_TABLE_CHANGED_DXR:
        LOG("table changed");
        break;
    default: break;
    }
}
"""

# displayxr-common route: XrSessionManager + PollEvents + CreateSwapchain.
VIA_COMMON = """
#include "xr_session.h"
XrSessionManager xr;
void startup() {
    xrGetInstanceProcAddr(xr.instance, "xrEnumerateDisplayRenderingModesDXR",
                          (PFN_xrVoidFunction *)&xr.pfnEnumerateDisplayRenderingModesEXT);
    CreateSwapchain(xr, 1);
}
void frame() {
    PollEvents(xr);
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
}
"""

PROJECTION_SWAPCHAIN_FROM_TABLE = """
void make() {
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.width = maxTileW * cols;
    xrCreateSwapchain(session, &ci, &sc);
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
}
"""

PROJECTION_SWAPCHAIN_FROM_WORST_CASE = """
void make() {
    XrDisplayRenderingWorstCaseDXR wc = {XR_TYPE_DISPLAY_RENDERING_WORST_CASE_DXR};
    XrSystemProperties props = {XR_TYPE_SYSTEM_PROPERTIES};
    props.next = &wc;
    xrGetSystemProperties(instance, systemId, &props);
    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.width = std::max(wc.maxAtlasWidthPixels, loopW);
    xrCreateSwapchain(session, &ci, &sc);
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
}
"""

RECREATE_ON_TABLE_CHANGE = """
void poll() {
    switch (ev.type) {
    case XR_TYPE_EVENT_DATA_RENDERING_MODE_TABLE_CHANGED_DXR: {
        EnumerateRenderingModes(app);
        xrDestroySwapchain(sc);
        xrCreateSwapchain(session, &ci, &sc);
        break;
    }
    default: break;
    }
}
"""

BRANCHES_ON_RECREATE_BIT = """
void onTable(const XrEventDataRenderingModeTableChangedDXR *e) {
    if (e->flags & XR_RENDERING_MODE_TABLE_CHANGE_RECREATE_SWAPCHAINS_BIT_DXR) rebuild();
}
"""

# A legacy app: never touches display_info.
LEGACY = """
void make() {
    xrCreateSwapchain(session, &ci, &sc);
    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
}
"""


def lint_tree(files: dict, rules=("INV-2.10", "INV-4.2", "INV-4.9")) -> list:
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        for rel_path, text in files.items():
            p = root / rel_path
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text, encoding="utf-8")
        findings: list = []
        lint.check_views_change_realloc(root, findings)
        lint.check_live_mode_table(root, findings)
        return [f for f in findings if f.rule in rules]


def by_rule(found: list, rule: str) -> list:
    return [f for f in found if f.rule == rule]


class TableChangedEventTest(unittest.TestCase):
    def test_read_once_is_an_error(self):
        found = by_rule(lint_tree({"main.mm": READ_ONCE}), "INV-2.10")
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0].level, lint.ERROR)

    def test_reread_through_own_function_is_clean(self):
        self.assertEqual(by_rule(lint_tree({"main.mm": REREADS_VIA_FUNCTION}), "INV-2.10"), [])

    def test_reread_through_opaque_pfn_is_clean(self):
        self.assertEqual(by_rule(lint_tree({"main.cpp": REREADS_VIA_PFN}), "INV-2.10"), [])

    def test_handled_without_reread_warns(self):
        found = by_rule(lint_tree({"main.cpp": HANDLES_NO_REREAD}), "INV-2.10")
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0].level, lint.WARN)

    def test_common_poll_events_covers_it(self):
        self.assertEqual(lint_tree({"main.cpp": VIA_COMMON}), [])

    def test_common_pinned_below_2_37_does_not(self):
        cmake = ('FetchContent_Declare(displayxr_common\n'
                 '    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git\n'
                 '    GIT_TAG v2.36.1)\n')
        found = lint_tree({"main.cpp": VIA_COMMON, "CMakeLists.txt": cmake})
        a = by_rule(found, "INV-2.10")
        self.assertEqual(len(a), 1)
        self.assertEqual(a[0].level, lint.ERROR)
        self.assertIn("v2.36.1", a[0].msg)
        self.assertEqual(len(by_rule(found, "INV-4.2")), 1)

    def test_common_pinned_at_2_37_is_clean(self):
        cmake = ('FetchContent_Declare(displayxr_common\n'
                 '    GIT_REPOSITORY https://github.com/DisplayXR/displayxr-common.git\n'
                 '    GIT_TAG v2.37.0)\n')
        self.assertEqual(lint_tree({"main.cpp": VIA_COMMON, "CMakeLists.txt": cmake}), [])

    def test_local_poll_events_is_not_common(self):
        local = READ_ONCE + "\nstatic bool PollEvents(App &app) { return true; }\nvoid f() { PollEvents(app); }\n"
        self.assertEqual(len(by_rule(lint_tree({"main.cpp": local}), "INV-2.10")), 1)

    def test_per_leg(self):
        found = by_rule(lint_tree({"windows/main.cpp": VIA_COMMON, "macos/main.mm": READ_ONCE}),
                        "INV-2.10")
        self.assertEqual(len(found), 1)
        self.assertTrue(found[0].path.startswith("macos"))

    def test_legacy_app_out_of_scope(self):
        self.assertEqual(lint_tree({"main.cpp": LEGACY}), [])


class WorstCaseSizingTest(unittest.TestCase):
    def test_table_sized_projection_swapchain_warns(self):
        found = by_rule(lint_tree({"main.mm": REREADS_VIA_FUNCTION + PROJECTION_SWAPCHAIN_FROM_TABLE}),
                        "INV-4.2")
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0].level, lint.WARN)
        self.assertIn("XrDisplayRenderingWorstCaseDXR", found[0].fix)

    def test_worst_case_sized_is_clean(self):
        self.assertEqual(
            lint_tree({"main.mm": REREADS_VIA_FUNCTION + PROJECTION_SWAPCHAIN_FROM_WORST_CASE}), [])

    def test_no_projection_layer_out_of_scope(self):
        # e.g. a weave-RPC probe: reads the table, never submits a projection layer.
        probe = REREADS_VIA_FUNCTION + "void q() { xrCreateSwapchain(session, &ci, &sc); }\n"
        self.assertEqual(by_rule(lint_tree({"main.cpp": probe}), "INV-4.2"), [])

    def test_per_leg(self):
        found = by_rule(lint_tree({
            "linux/main.cpp": REREADS_VIA_FUNCTION + PROJECTION_SWAPCHAIN_FROM_WORST_CASE,
            "android/main.cpp": REREADS_VIA_FUNCTION + PROJECTION_SWAPCHAIN_FROM_TABLE,
        }), "INV-4.2")
        self.assertEqual(len(found), 1)
        self.assertTrue(found[0].path.startswith("android"))


class NoRecreateTest(unittest.TestCase):
    def test_create_in_table_changed_handler_is_an_error(self):
        found = by_rule(lint_tree({"main.cpp": RECREATE_ON_TABLE_CHANGE}), "INV-4.9")
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0].level, lint.ERROR)

    def test_create_in_mode_changed_handler_is_an_error(self):
        src = RECREATE_ON_TABLE_CHANGE.replace("MODE_TABLE_CHANGED", "MODE_CHANGED")
        found = by_rule(lint_tree({"main.cpp": src}), "INV-4.9")
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0].level, lint.ERROR)

    def test_reserved_bit_warns(self):
        found = by_rule(lint_tree({"main.cpp": BRANCHES_ON_RECREATE_BIT}), "INV-4.9")
        self.assertEqual(len(found), 1)
        self.assertEqual(found[0].level, lint.WARN)

    def test_vendored_fallback_definition_is_not_a_use(self):
        vendored = ("#define XR_TYPE_EVENT_DATA_RENDERING_MODE_TABLE_CHANGED_DXR ((XrStructureType)1004999451)\n"
                    "static const XrRenderingModeTableChangeFlagsDXR "
                    "XR_RENDERING_MODE_TABLE_CHANGE_RECREATE_SWAPCHAINS_BIT_DXR = 1;\n")
        self.assertEqual(lint_tree({"compat.h": vendored}), [])

    def test_create_outside_handler_is_clean(self):
        self.assertEqual(by_rule(lint_tree({"main.mm": REREADS_VIA_FUNCTION + PROJECTION_SWAPCHAIN_FROM_WORST_CASE}),
                                 "INV-4.9"), [])


class TestAppsGateTest(unittest.TestCase):
    """scripts/lint_test_apps.py: the CI gate over test_apps/ with a baseline."""

    def run_gate(self, apps: dict, baseline: list) -> int:
        import json
        import lint_test_apps as gate
        with tempfile.TemporaryDirectory() as d:
            ta = Path(d) / "test_apps"
            for app, src in apps.items():
                (ta / app).mkdir(parents=True)
                (ta / app / "CMakeLists.txt").write_text("project(x)\n")
                (ta / app / "main.cpp").write_text(src)
            bl = Path(d) / "baseline.json"
            bl.write_text(json.dumps({"known": baseline}))
            return gate.main(["--test-apps", str(ta), "--baseline", str(bl)])

    def test_new_finding_fails(self):
        self.assertEqual(self.run_gate({"handle/a": READ_ONCE}, []), 1)

    def test_baselined_finding_passes(self):
        self.assertEqual(
            self.run_gate({"handle/a": READ_ONCE}, [{"app": "test_apps/handle/a", "rule": "INV-2.10"}]), 0)

    def test_stale_entry_does_not_fail(self):
        self.assertEqual(
            self.run_gate({"handle/a": LEGACY}, [{"app": "test_apps/handle/a", "rule": "INV-4.2"}]), 0)


if __name__ == "__main__":
    unittest.main()

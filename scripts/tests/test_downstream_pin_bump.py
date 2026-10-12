#!/usr/bin/env python3
"""Unit tests for scripts/downstream_pin_bump.py (the runtime-pin-bump gate).

Hermetic by default: synthetic header text, the manifest, and the working tree.
The history tests replay real runtime releases through the gate when the tags
are in the local clone (a full clone or `git fetch --tags`), and skip otherwise
-- lint.yml's depth-1 checkout skips them, a dev box runs them.

    python3 scripts/tests/test_downstream_pin_bump.py
"""
from __future__ import annotations

import subprocess
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import downstream_pin_bump as dpb  # noqa: E402

REPO_ROOT = dpb.REPO_ROOT

VK_OLD = """
/* doc comment mentioning (*fake_slot)(void); and #define XRT_DP_VK_HAS_FAKE 1 */
struct xrt_display_processor_vk {
	struct xrt_display_processor base;
	void (*destroy)(struct xrt_display_processor_vk *xdp);
	bool (*get_panel_size)(struct xrt_display_processor_vk *xdp,
	                       uint32_t *w, uint32_t *h);
};
#define XRT_DP_VK_HAS_PANEL_SIZE 1
#define XRT_DP_HAS_SLOT(xdp, slot) ((xdp) != NULL)
"""

VK_NEW_SLOT = VK_OLD.replace(
    "};",
    "\tvoid (*set_transparency_active)(struct xrt_display_processor_vk *xdp, bool active);\n};",
) + "#define XRT_DP_VK_HAS_TRANSPARENCY_ACTIVE 1\n"


def surf(text: str, name: str = "xrt_display_processor_vk.h") -> dict:
    s = dpb.surface_of(text, name)
    s["abi"] = None
    s["text"] = {name: dpb._norm(dpb.strip_comments(text))}
    return s


class SurfaceTests(unittest.TestCase):
    def test_parses_macros_and_slots_not_comments(self):
        s = dpb.surface_of(VK_OLD, "vk.h")
        self.assertEqual(set(s["macros"]), {"XRT_DP_VK_HAS_PANEL_SIZE"})  # not the fn-like XRT_DP_HAS_SLOT
        self.assertEqual(set(s["slots"]), {"xrt_display_processor_vk.destroy",
                                           "xrt_display_processor_vk.get_panel_size"})

    def test_new_macro_and_slot_trigger(self):
        trig, _ = dpb.diff_surfaces(surf(VK_OLD), surf(VK_NEW_SLOT))
        self.assertIn("new feature macro XRT_DP_VK_HAS_TRANSPARENCY_ACTIVE (xrt_display_processor_vk.h)", trig)
        self.assertIn("new vtable slot xrt_display_processor_vk.set_transparency_active "
                      "(xrt_display_processor_vk.h)", trig)

    def test_resigned_slot_triggers(self):
        new = VK_OLD.replace("uint32_t *h)", "uint32_t *h, float *dpi)")
        trig, _ = dpb.diff_surfaces(surf(VK_OLD), surf(new))
        self.assertEqual(len(trig), 1)
        self.assertIn("re-signed", trig[0])

    def test_abi_change_triggers(self):
        a, b = surf(VK_OLD), surf(VK_OLD)
        a["abi"], b["abi"] = 5, 6
        trig, _ = dpb.diff_surfaces(a, b)
        self.assertEqual(trig, ["plug-in ABI XRT_PLUGIN_API_VERSION_CURRENT 5 -> 6"])

    def test_comment_and_reflow_do_not_trigger(self):
        new = VK_OLD.replace("doc comment", "reworded doc comment").replace(
            "uint32_t *w, uint32_t *h", "uint32_t *w,\n\t\t\tuint32_t *h")
        trig, notes = dpb.diff_surfaces(surf(VK_OLD), surf(new))
        self.assertEqual(trig, [])
        self.assertEqual(notes, [])

    def test_helper_only_change_is_a_note_not_a_trigger(self):
        new = VK_OLD + "static inline bool helper(void) { return true; }\n"
        trig, notes = dpb.diff_surfaces(surf(VK_OLD), surf(new))
        self.assertEqual(trig, [])
        self.assertEqual(len(notes), 1)


class VerdictTests(unittest.TestCase):
    def test_manual_never_bumps(self):
        self.assertEqual(dpb.verdict("manual", "v1.0.0", "v9.0.0", [])["action"], "skip")

    def test_never_downgrades(self):
        d = dpb.verdict("features", "v2.21.1", "v2.20.9", [])
        self.assertEqual(d["action"], "skip")
        self.assertIn("newer", d["reason"])

    def test_same_tag_skips(self):
        self.assertEqual(dpb.verdict("features", "v2.21.1", "v2.21.1", [])["action"], "skip")


class ManifestTests(unittest.TestCase):
    def setUp(self):
        self.man = dpb.load_manifest()

    def test_every_track_has_a_valid_policy(self):
        for repo, spec in self.man["runtime_tag_pins"].items():
            for name, t in spec["tracks"].items():
                with self.subTest(track="%s/%s" % (repo, name)):
                    self.assertIn(t.get("bump_when"), dpb.POLICIES)
                    self.assertNotIn("auto_bump", t, "auto_bump was replaced by bump_when")
                    if t["bump_when"] == "manual":
                        self.assertTrue(t.get("reason"), "a manual track must record why")

    def test_leia_linux_is_feature_gated_on_the_vk_set(self):
        t = self.man["runtime_tag_pins"]["displayxr-leia-plugin"]["tracks"]["linux"]
        self.assertEqual(t["bump_when"], "features")
        self.assertIn("src/xrt/include/xrt/xrt_display_processor_vk.h", t["feature_headers"])
        self.assertEqual({l["file"] for l in t["locations"]},
                         {"CMakeLists.txt", ".github/workflows/build-linux.yml"})

    def test_feature_headers_exist(self):
        paths = set(self.man["feature_surface"]["headers"])
        for spec in self.man["runtime_tag_pins"].values():
            for t in spec["tracks"].values():
                paths.update(t.get("feature_headers") or [])
        for p in sorted(paths):
            with self.subTest(header=p):
                self.assertTrue((REPO_ROOT / p).is_file(), "%s is listed but does not exist" % p)

    def test_default_surface_covers_every_dp_header(self):
        # A new graphics-API DP header that is not listed would be invisible to
        # the features gate -- its new slots would never trigger a repin.
        # xrt_dp_*.h are DP-side contracts (e.g. xrt_dp_lift.h) whose XRT_DP_*_HAS_*
        # macros gate plug-in code exactly like the per-API vtable headers.
        inc = REPO_ROOT / "src/xrt/include/xrt"
        on_disk = {"src/xrt/include/xrt/" + p.name
                   for pat in ("xrt_display_processor*.h", "xrt_dp_*.h") for p in inc.glob(pat)}
        missing = on_disk - set(self.man["feature_surface"]["headers"])
        self.assertFalse(missing, "add to downstream-pins.json feature_surface.headers: %s" % sorted(missing))


CONV_H = "src/xrt/include/xrt/xrt_conversion_plugin.h"
INPUT_H = "src/xrt/include/xrt/xrt_input_plugin.h"


def conv_header(major: int) -> str:
    return ("#define XRT_CONVERSION_PLUGIN_API_VERSION_1 1\n"
            "#define XRT_CONVERSION_PLUGIN_API_VERSION_2 2\n"
            "#define XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT XRT_CONVERSION_PLUGIN_API_VERSION_%d\n" % major)


class PluginAbiTests(unittest.TestCase):
    """ADR-053: the gate reads every plug-in ABI major, not only the display one."""

    def test_resolver_reads_each_prefix_without_crosstalk(self):
        text = ("#define XRT_PLUGIN_API_VERSION_5 5\n#define XRT_PLUGIN_API_VERSION_CURRENT XRT_PLUGIN_API_VERSION_5\n"
                + conv_header(2))
        self.assertEqual(dpb._resolve_api_version_current(text), 5)
        self.assertEqual(dpb._resolve_api_version_current(text, "XRT_CONVERSION_PLUGIN_API_VERSION"), 2)
        with self.assertRaises(RuntimeError):  # the display prefix must not match inside the conversion one
            dpb._resolve_api_version_current(conv_header(1))

    def test_abi_table_covers_the_three_plugin_types_and_the_tree_resolves(self):
        self.assertEqual(set(dpb.ABI_HEADERS), {dpb.PLUGIN_HEADER_PATH, INPUT_H, CONV_H})
        for h, prefix in dpb.ABI_HEADERS.items():
            with self.subTest(header=h):
                text = (REPO_ROOT / h).read_text(encoding="utf-8")
                self.assertGreaterEqual(dpb._resolve_api_version_current(text, prefix), 1)

    def test_conversion_abi_change_triggers_features(self):
        a, b = surf(VK_OLD), surf(VK_OLD)
        a["abis"] = {"XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT": 1}
        b["abis"] = {"XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT": 2}
        trig, _ = dpb.diff_surfaces(a, b)
        self.assertEqual(trig, ["plug-in ABI XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT 1 -> 2"])

    def test_new_conversion_header_is_not_an_abi_change(self):
        a, b = surf(VK_OLD), surf(VK_OLD)
        a["abis"], b["abis"] = {}, {"XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT": 1}
        self.assertEqual(dpb.diff_surfaces(a, b)[0], [])

    def _with_headers(self, table):
        real = dpb.header_at
        dpb.header_at = lambda tag, path: table.get((tag, path))
        self.addCleanup(setattr, dpb, "header_at", real)

    def test_abi_track_with_conversion_header(self):
        disp = "#define XRT_PLUGIN_API_VERSION_5 5\n#define XRT_PLUGIN_API_VERSION_CURRENT XRT_PLUGIN_API_VERSION_5\n"
        P = dpb.PLUGIN_HEADER_PATH
        self._with_headers({
            ("v9.0.0", P): disp, ("v9.0.1", P): disp, ("v9.1.0", P): disp, ("v9.2.0", P): disp,
            ("v9.0.1", CONV_H): conv_header(1), ("v9.1.0", CONV_H): conv_header(1),
            ("v9.2.0", CONV_H): conv_header(2),
        })
        hdrs = [P, CONV_H]
        # Default gate (display ABI only) is blind to the conversion major.
        self.assertEqual(dpb.verdict("abi", "v9.1.0", "v9.2.0", [])["action"], "skip")
        # Opted in: the conversion major bump repins ...
        d = dpb.verdict("abi", "v9.1.0", "v9.2.0", [], hdrs)
        self.assertEqual(d["action"], "bump")
        self.assertEqual(d["triggers"], ["plug-in ABI XRT_CONVERSION_PLUGIN_API_VERSION_CURRENT 1 -> 2"])
        # ... a patch release with no major change does not ...
        self.assertEqual(dpb.verdict("abi", "v9.0.1", "v9.1.0", [], hdrs)["action"], "skip")
        # ... and the release that first ships the header does.
        d = dpb.verdict("abi", "v9.0.0", "v9.0.1", [], hdrs)
        self.assertEqual(d["action"], "bump")
        self.assertIn("xrt_conversion_plugin.h first ships at v9.0.1", d["triggers"])

    def test_abi_headers_rejects_a_non_abi_header(self):
        with self.assertRaises(SystemExit):
            dpb.abi_headers({"abi_headers": ["src/xrt/include/xrt/xrt_dp_lift.h"]})
        self.assertEqual(dpb.abi_headers(None), [dpb.PLUGIN_HEADER_PATH])

    def test_default_surface_lists_the_conversion_header(self):
        self.assertIn(CONV_H, dpb.load_manifest()["feature_surface"]["headers"])


def _have_tags(*tags: str) -> bool:
    return all(subprocess.run(["git", "-C", str(REPO_ROOT), "rev-parse", "--verify", "--quiet",
                               t + "^{commit}"], capture_output=True).returncode == 0 for t in tags)


@unittest.skipUnless(_have_tags("v2.20.1", "v2.21.0", "v2.21.1"), "runtime release tags not in this clone")
class HistoryTests(unittest.TestCase):
    """Replay of the case that motivated the features policy (leia-plugin#264/#265)."""

    def setUp(self):
        man = dpb.load_manifest()
        self.linux = dpb.feature_headers(man, man["runtime_tag_pins"]["displayxr-leia-plugin"]["tracks"]["linux"])

    def test_v2_21_1_triggers_on_transparency_active(self):
        d = dpb.verdict("features", "v2.21.0", "v2.21.1", self.linux)
        self.assertEqual(d["action"], "bump")
        self.assertIn("new feature macro XRT_DP_VK_HAS_TRANSPARENCY_ACTIVE (xrt_display_processor_vk.h)",
                      d["triggers"])

    def test_abi_gate_would_have_missed_it(self):
        self.assertEqual(dpb.verdict("abi", "v2.21.0", "v2.21.1", [])["action"], "skip")

    def test_v2_21_0_does_not_trigger(self):
        self.assertEqual(dpb.verdict("features", "v2.20.1", "v2.21.0", self.linux)["action"], "skip")


if __name__ == "__main__":
    unittest.main(verbosity=2)

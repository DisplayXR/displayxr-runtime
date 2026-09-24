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
        on_disk = {"src/xrt/include/xrt/" + p.name
                   for p in (REPO_ROOT / "src/xrt/include/xrt").glob("xrt_display_processor*.h")}
        missing = on_disk - set(self.man["feature_surface"]["headers"])
        self.assertFalse(missing, "add to downstream-pins.json feature_surface.headers: %s" % sorted(missing))


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

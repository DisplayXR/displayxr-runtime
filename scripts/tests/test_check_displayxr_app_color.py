#!/usr/bin/env python3
"""Unit tests for the INV-4.6 colour-swapchain checks in scripts/check_displayxr_app.py.

Hermetic: each case writes a tiny synthetic app tree to a temp dir and lints it.

The shapes are taken from real demo legs. Since the #1589 format-honest colour
model (vk_native in v2.21.7) an UNORM swapchain is read as LINEAR and encoded by
the runtime, so a leg that picks UNORM and stores display-referred bytes is
encoded twice and looks washed out. The Android leg of the Gaussian-splat demo
did exactly that through v1.29.0 while its desktop legs had moved to _SRGB, and
the old app-wide "any sRGB token anywhere" check was satisfied by the desktop
legs, so nothing flagged it.

    python3 scripts/tests/test_check_displayxr_app_color.py
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_displayxr_app as lint  # noqa: E402

ENUM = "xrEnumerateSwapchainFormats(s, n, &n, formats);\nxrCreateSwapchain(s, &ci, &sc);\n"

# The Android leg of displayxr-demo-gaussiansplat v1.29.0 (android/src/main/cpp/main.cpp:923).
ANDROID_UNORM_LIST = ENUM + """
const int64_t preferred[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM};
for (int64_t pref : preferred) { for (uint32_t i = 0; i < n; ++i) if (formats[i] == pref) fmt = pref; }
"""

# A desktop leg that stops at the first UNORM (displayxr-demo-avatar v0.14.0 macos/main.mm).
UNORM_FIRST_BREAK = ENUM + """
for (auto f : fmts) {
    if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM) { selectedFmt = f; break; }
    if (f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB) selectedFmt = f;
}
"""

# The correct shape: stop at the first _SRGB, remember a UNORM only as a fallback.
SRGB_FIRST_BREAK = ENUM + """
for (auto f : fmts) {
    if (f == VK_FORMAT_B8G8R8A8_SRGB || f == VK_FORMAT_R8G8B8A8_SRGB) { selectedFmt = f; break; }
    if (f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM) selectedFmt = f;
}
"""

# Choosing through a shared helper (no format token in the leg itself).
VIA_HELPER = ENUM + "fmt = gsChooseSwapchainFormat(formats, n);\n"

# A mutable-format list on an intermediate image, not a swapchain preference.
MUTABLE_LIST = ENUM + """
const VkFormat view_formats[] = {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB};
"""

SHARED_HELPER = """
bool gsIsSrgbFormat(VkFormat f) { return f == VK_FORMAT_R8G8B8A8_SRGB || f == VK_FORMAT_B8G8R8A8_SRGB; }
"""


def lint_tree(files: dict[str, str]) -> list:
    with tempfile.TemporaryDirectory() as d:
        root = Path(d)
        for rel, text in files.items():
            p = root / rel
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text(text)
        findings: list = []
        lint.scan_sources(root, findings)
        return [f for f in findings if f.rule == "INV-4.6"]


class Inv46PerLeg(unittest.TestCase):
    def test_android_unorm_list_flagged_although_desktop_legs_are_srgb(self):
        got = lint_tree({
            "android/src/main/cpp/main.cpp": ANDROID_UNORM_LIST,
            "macos/main.mm": SRGB_FIRST_BREAK,
            "linux/main.cpp": SRGB_FIRST_BREAK,
            "3dgs_common/gs_vulkan_utils.cpp": SHARED_HELPER,
        })
        self.assertEqual([(f.path, f.line) for f in got], [("android/src/main/cpp/main.cpp", 4)])
        self.assertIn("preference list naming only UNORM", got[0].msg)

    def test_unorm_first_break_flagged(self):
        got = lint_tree({"macos/main.mm": UNORM_FIRST_BREAK})
        self.assertEqual(len(got), 1)
        self.assertIn("stops at the first UNORM", got[0].msg)

    def test_fixed_tree_is_clean(self):
        got = lint_tree({
            "android/src/main/cpp/main.cpp": VIA_HELPER,
            "macos/main.mm": VIA_HELPER,
            "linux/main.cpp": VIA_HELPER,
            "3dgs_common/gs_vulkan_utils.cpp": SHARED_HELPER,
        })
        self.assertEqual(got, [])

    def test_srgb_first_scan_is_clean(self):
        self.assertEqual(lint_tree({"linux/main.cpp": SRGB_FIRST_BREAK}), [])

    def test_mutable_format_list_is_not_a_preference(self):
        self.assertEqual(lint_tree({"linux/main.cpp": MUTABLE_LIST}), [])

    def test_leg_with_no_srgb_anywhere_is_named(self):
        got = lint_tree({
            "android/src/main/cpp/main.cpp": ENUM,
            "macos/main.mm": SRGB_FIRST_BREAK,
        })
        self.assertEqual(len(got), 1)
        self.assertEqual(got[0].path, "android/src/main/cpp/main.cpp")
        self.assertIn("android/ leg", got[0].msg)

    def test_single_leg_app_without_srgb(self):
        got = lint_tree({"main.cpp": ENUM})
        self.assertEqual(len(got), 1)
        self.assertIn("No sRGB swapchain format detected", got[0].msg)

    def test_commented_out_unorm_list_is_ignored(self):
        got = lint_tree({
            "linux/main.cpp": VIA_HELPER + "// was: {VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM}\n",
            "shared/util.cpp": SHARED_HELPER,
        })
        self.assertEqual(got, [])


if __name__ == "__main__":
    unittest.main(verbosity=2)

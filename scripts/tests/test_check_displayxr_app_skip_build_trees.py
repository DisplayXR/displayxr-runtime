#!/usr/bin/env python3
"""Unit tests for the build-tree / git-ignore exclusion in
scripts/check_displayxr_app.py.

After a local Gradle build, AGP's externalNativeBuild output (.cxx/) holds the
FetchContent'd displayxr-common sources under _deps/, incl. its tests/. The
linter used to walk into it and report false INV-3.1 errors against those test
files (seen on test_apps/handle/cube_handle_vk_android, 2026-10-11). Every
directory walk now goes through is_excluded(): a fixed list of build-output
directory names, plus anything the enclosing git repo ignores.

Hermetic: each case writes a tiny synthetic app tree to a temp dir and lints it.

    python3 scripts/tests/test_check_displayxr_app_skip_build_trees.py
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import check_displayxr_app as lint  # noqa: E402

# Reads the active mode's view count but never opts into PRIMARY_MULTIVIEW_DXR:
# the shape INV-3.1 flags as an ERROR.
OFFENDING = """
void pick_mode(XrSession s)
{
    uint32_t renderingModeCount = 0;
    xrEnumerateDisplayRenderingModesDXR(s, 0, &renderingModeCount, nullptr);
}
"""

# A clean N-view app source (opts in, so INV-3.1 is silent for it).
CLEAN = """
void begin(XrSession s)
{
    XrViewConfigurationType t = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_MULTIVIEW_DXR;
}
"""

# The vk-discipline walker is a separate loop; pin it too.
VK_DRAIN = """
void RenderFrame()
{
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(q);
}
"""

BUILD_TREE_PATHS = [
    ".cxx/Debug/4x5y6z/arm64-v8a/_deps/displayxr_common-src/tests/test_views.cpp",
    ".externalNativeBuild/cmake/debug/arm64-v8a/offending.cpp",
    ".gradle/8.7/offending.cpp",
    "_deps/displayxr_common-src/tests/offending.cpp",
    "build/intermediates/offending.cpp",
]


def write_tree(root: Path, files: dict[str, str]) -> None:
    for rel, text in files.items():
        p = root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(text)


def inv31(root: Path) -> list:
    findings: list = []
    lint.scan_sources(root, findings)
    return [f for f in findings if f.rule == "INV-3.1"]


def git(root: Path, *args: str) -> None:
    subprocess.run(["git", "-C", str(root), *args], check=True, capture_output=True)


class SkipBuildTrees(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name).resolve()

    def tearDown(self):
        self._tmp.cleanup()

    def test_fixture_trips_inv31_when_in_source_tree(self):
        # Control: the offending source IS flagged where a real app source lives,
        # so the exclusion tests below are not passing vacuously.
        write_tree(self.root, {"src/main/cpp/offending.cpp": OFFENDING})
        got = inv31(self.root)
        self.assertEqual(len(got), 1, got)
        self.assertEqual(got[0].level, lint.ERROR)

    def test_build_trees_not_scanned(self):
        for path in BUILD_TREE_PATHS:
            with self.subTest(path=path):
                with tempfile.TemporaryDirectory() as d:
                    root = Path(d).resolve()
                    write_tree(root, {"src/main/cpp/main.cpp": "int main() { return 0; }\n", path: OFFENDING})
                    self.assertEqual(inv31(root), [])

    def test_build_tree_does_not_mask_real_opt_in(self):
        # A build tree's sources must not count for the app either way: a stray
        # PRIMARY_MULTIVIEW_DXR in .cxx/ must not silence a real app-side error.
        write_tree(self.root, {
            "src/main/cpp/main.cpp": OFFENDING,
            ".cxx/Debug/x/_deps/displayxr_common-src/common/views.cpp": CLEAN,
        })
        self.assertEqual(len(inv31(self.root)), 1)

    def test_vk_walker_skips_build_trees(self):
        write_tree(self.root, {".cxx/Debug/x/_deps/foo-src/render.cpp": VK_DRAIN})
        findings: list = []
        lint.check_vk_frame_discipline(self.root, findings)
        self.assertEqual(findings, [])

    def test_no_git_repo_falls_back_to_fixed_list(self):
        if shutil.which("git"):
            probe = subprocess.run(["git", "-C", str(self.root), "rev-parse", "--is-inside-work-tree"],
                                   capture_output=True)
            if probe.returncode == 0:
                self.skipTest("temp dir is inside a git work tree on this machine")
        self.assertEqual(lint.git_ignored_prefixes(self.root), ())

    @unittest.skipUnless(shutil.which("git"), "git not installed")
    def test_git_ignored_paths_not_scanned(self):
        git(self.root, "init", "-q")
        write_tree(self.root, {
            ".gitignore": "generated/\n*.gen.cpp\n",
            # Neutral, NOT CLEAN: an opt-in anywhere silences INV-3.1 app-wide.
            "src/main/cpp/main.cpp": "int main() { return 0; }\n",
            "generated/nested/offending.cpp": OFFENDING,
            "src/main/cpp/stub.gen.cpp": OFFENDING,
        })
        self.assertEqual(inv31(self.root), [])

    @unittest.skipUnless(shutil.which("git"), "git not installed")
    def test_tracked_file_in_ignored_dir_still_scanned(self):
        # git-ignore only hides UNTRACKED files: a committed source inside an
        # ignored directory is part of the app and must stay linted.
        git(self.root, "init", "-q")
        write_tree(self.root, {
            ".gitignore": "generated/\n",
            "generated/offending.cpp": OFFENDING,
        })
        git(self.root, "add", "-f", "generated/offending.cpp")
        self.assertEqual(len(inv31(self.root)), 1)


if __name__ == "__main__":
    unittest.main(verbosity=2)

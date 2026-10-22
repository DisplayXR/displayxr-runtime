#!/usr/bin/env python3
"""Unit tests for cmake/DisplayXRVersion.cmake -- the runtime's version (#1790).

Hermetic: every case builds a throwaway git repository (or a tree with no git
at all) holding a copy of the module, a versions.json and the export-subst'ed
.git_archival.txt, and runs the module in script mode (`cmake -P`), which
resolves exactly as the configure does. Needs git >= 2.35 (for `git archive`'s
%(describe:tags=...)) and cmake >= 3.22.

    python3 scripts/tests/test_runtime_version.py
"""
from __future__ import annotations

import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
MODULE = REPO_ROOT / "cmake" / "DisplayXRVersion.cmake"

# A git that ignores the developer's config (signing, hooks, default branch...).
GIT_ENV = {
    **os.environ,
    "GIT_CONFIG_GLOBAL": os.devnull,
    "GIT_CONFIG_NOSYSTEM": "1",
    "GIT_AUTHOR_NAME": "t",
    "GIT_AUTHOR_EMAIL": "t@example.invalid",
    "GIT_COMMITTER_NAME": "t",
    "GIT_COMMITTER_EMAIL": "t@example.invalid",
}
for _k in ("DXR_RUNTIME_VERSION_OVERRIDE", "GIT_DIR", "GIT_WORK_TREE"):
    GIT_ENV.pop(_k, None)


def git(cwd: Path, *args: str) -> str:
    return subprocess.run(["git", *args], cwd=cwd, env=GIT_ENV, check=True,
                          capture_output=True, text=True).stdout.strip()


def resolve(root: Path, *defs: str) -> tuple[int, dict[str, str], str]:
    """Run the module copy under `root` in script mode -> (rc, values, stderr)."""
    p = subprocess.run(["cmake", *defs, "-P", str(root / "cmake" / "DisplayXRVersion.cmake")],
                       cwd=root, env=GIT_ENV, capture_output=True, text=True)
    vals = dict(line.split("=", 1) for line in p.stdout.splitlines() if "=" in line)
    return p.returncode, vals, p.stderr


def make_tree(root: Path, runtime_pin: str | None = "v2.40.0") -> None:
    (root / "cmake").mkdir(parents=True)
    shutil.copy(MODULE, root / "cmake" / MODULE.name)
    shutil.copy(REPO_ROOT / ".git_archival.txt", root / ".git_archival.txt")
    shutil.copy(REPO_ROOT / ".gitattributes", root / ".gitattributes")
    if runtime_pin is not None:
        (root / "versions.json").write_text('{\n  "runtime": "%s",\n  "shell": "v9.9.9"\n}\n' % runtime_pin)


def commit(repo: Path, msg: str) -> str:
    (repo / "f.txt").write_text(msg + "\n")
    git(repo, "add", "-A")
    git(repo, "commit", "-q", "-m", msg)
    return git(repo, "rev-parse", "HEAD")


class VersionTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = Path(tempfile.mkdtemp(prefix="dxr-version-test-"))
        self.repo = self.tmp / "repo"
        make_tree(self.repo)
        git(self.repo, "init", "-q", "-b", "main")
        commit(self.repo, "initial")
        git(self.repo, "tag", "v2.39.0")
        commit(self.repo, "release 2.41.0")
        git(self.repo, "tag", "v2.41.0")  # lightweight, like /release's tags

    def tearDown(self) -> None:
        shutil.rmtree(self.tmp, ignore_errors=True)

    def assertVersion(self, vals: dict[str, str], ver: str, desc: str, pkg: str,
                      release: str, source: str) -> None:
        self.assertEqual(vals.get("DXR_RUNTIME_VERSION"), ver, vals)
        self.assertEqual(".".join(vals.get(k, "?") for k in (
            "DXR_RUNTIME_VERSION_MAJOR", "DXR_RUNTIME_VERSION_MINOR", "DXR_RUNTIME_VERSION_PATCH")), ver)
        self.assertEqual(vals.get("DXR_RUNTIME_VERSION_DESCRIBE"), desc, vals)
        self.assertEqual(vals.get("DXR_RUNTIME_VERSION_PACKAGE"), pkg, vals)
        self.assertEqual(vals.get("DXR_RUNTIME_VERSION_IS_RELEASE"), release, vals)
        self.assertEqual(vals.get("DXR_RUNTIME_VERSION_SOURCE"), source, vals)

    def dev(self, base: str, n: int) -> tuple[str, str]:
        sha = git(self.repo, "describe", "--tags", "--long").rsplit("-g", 1)[1]
        return f"v{base}-{n}-g{sha}", f"{base}+{n}.g{sha}"

    # --- git ------------------------------------------------------------------
    def test_tag_build_is_exactly_the_tag(self):
        rc, vals, err = resolve(self.repo)
        self.assertEqual(rc, 0, err)
        self.assertVersion(vals, "2.41.0", "v2.41.0", "2.41.0", "1", "git")

    def test_annotated_tag_build_is_exactly_the_tag(self):
        commit(self.repo, "release 2.41.1")
        git(self.repo, "tag", "-a", "-m", "annotated", "v2.41.1")
        rc, vals, err = resolve(self.repo)
        self.assertEqual(rc, 0, err)
        self.assertVersion(vals, "2.41.1", "v2.41.1", "2.41.1", "1", "git")

    def test_dev_build_is_the_last_release_plus_describe(self):
        commit(self.repo, "a")
        commit(self.repo, "b")
        desc, pkg = self.dev("2.41.0", 2)
        rc, vals, err = resolve(self.repo)
        self.assertEqual(rc, 0, err)
        self.assertVersion(vals, "2.41.0", desc, pkg, "0", "git")
        self.assertNotIn("CMake Warning", err)

    def test_non_release_tags_are_ignored(self):
        commit(self.repo, "a")
        git(self.repo, "tag", "v2.42.0-rc1")  # nearer, but not vX.Y.Z
        git(self.repo, "tag", "dcv3-moved")  # nearer, not a version at all
        desc, pkg = self.dev("2.41.0", 1)
        rc, vals, err = resolve(self.repo)
        self.assertEqual(rc, 0, err)
        self.assertVersion(vals, "2.41.0", desc, pkg, "0", "git")

    # --- versions.json fallback -------------------------------------------------
    def test_shallow_tagless_clone_falls_back_to_versions_json(self):
        commit(self.repo, "a")
        clone = self.tmp / "shallow"
        git(self.tmp, "clone", "-q", "--depth", "1", "--no-tags", f"file://{self.repo}", str(clone))
        sha = git(clone, "rev-parse", "--short=9", "HEAD")
        rc, vals, err = resolve(clone)
        self.assertEqual(rc, 0, err)
        self.assertVersion(vals, "2.40.0", f"v2.40.0-dev-g{sha}", f"2.40.0+dev.g{sha}", "0", "versions.json")
        self.assertIn("CMake Warning", err)
        self.assertIn("shallow", err)

    def test_plain_copy_without_git_falls_back_to_versions_json(self):
        copy = self.tmp / "copy"
        make_tree(copy)
        rc, vals, err = resolve(copy)
        self.assertEqual(rc, 0, err)
        self.assertVersion(vals, "2.40.0", "v2.40.0-dev", "2.40.0+dev", "0", "versions.json")
        self.assertIn("CMake Warning", err)

    def test_tree_inside_another_checkout_ignores_its_tags(self):
        outer = self.tmp / "outer"
        outer.mkdir()
        git(outer, "init", "-q", "-b", "main")
        (outer / "x").write_text("x\n")
        git(outer, "add", "-A")
        git(outer, "commit", "-q", "-m", "outer")
        git(outer, "tag", "v9.9.9")
        make_tree(outer / "vendored" / "displayxr-runtime")
        rc, vals, err = resolve(outer / "vendored" / "displayxr-runtime")
        self.assertEqual(rc, 0, err)
        self.assertEqual(vals["DXR_RUNTIME_VERSION"], "2.40.0")  # not the outer v9.9.9
        self.assertEqual(vals["DXR_RUNTIME_VERSION_SOURCE"], "versions.json")

    def test_nothing_to_go_on_is_fatal(self):
        bare = self.tmp / "bare"
        make_tree(bare, runtime_pin=None)
        rc, vals, err = resolve(bare)
        self.assertNotEqual(rc, 0)
        self.assertIn("Cannot determine the DisplayXR runtime version", err)
        self.assertEqual(vals, {})

    # --- git archive / GitHub source tarballs ------------------------------------
    def extract(self, ref: str) -> Path:
        out = self.tmp / f"archive-{ref}"
        tar = self.tmp / f"{ref}.tar"
        git(self.repo, "archive", "-o", str(tar), "--prefix", "src/", ref)
        with tarfile.open(tar) as t:
            t.extractall(out)
        return out / "src"

    def test_archive_of_a_tag_is_exactly_the_tag(self):
        commit(self.repo, "a")
        rc, vals, err = resolve(self.extract("v2.41.0"))
        self.assertEqual(rc, 0, err)
        self.assertVersion(vals, "2.41.0", "v2.41.0", "2.41.0", "1", "archive")

    def test_archive_of_a_dev_commit_carries_its_describe(self):
        commit(self.repo, "a")
        commit(self.repo, "b")
        desc, pkg = self.dev("2.41.0", 2)
        rc, vals, err = resolve(self.extract("HEAD"))
        self.assertEqual(rc, 0, err)
        self.assertVersion(vals, "2.41.0", desc, pkg, "0", "archive")

    # --- explicit override ----------------------------------------------------
    def test_override_wins_and_is_validated(self):
        commit(self.repo, "a")
        rc, vals, err = resolve(self.repo, "-DDXR_RUNTIME_VERSION_OVERRIDE=2.50.3")
        self.assertEqual(rc, 0, err)
        self.assertVersion(vals, "2.50.3", "v2.50.3", "2.50.3", "1", "override")
        rc, vals, err = resolve(self.repo, "-DDXR_RUNTIME_VERSION_OVERRIDE=v2.50.3-4-gabcdef012")
        self.assertEqual(rc, 0, err)
        self.assertVersion(vals, "2.50.3", "v2.50.3-4-gabcdef012", "2.50.3+4.gabcdef012", "0", "override")
        rc, _, err = resolve(self.repo, "-DDXR_RUNTIME_VERSION_OVERRIDE=1.6")
        self.assertNotEqual(rc, 0)
        self.assertIn("DXR_RUNTIME_VERSION_OVERRIDE", err)

    def test_override_from_environment(self):
        p = subprocess.run(["cmake", "-P", str(self.repo / "cmake" / "DisplayXRVersion.cmake")],
                           env={**GIT_ENV, "DXR_RUNTIME_VERSION_OVERRIDE": "3.0.1"},
                           capture_output=True, text=True)
        self.assertEqual(p.returncode, 0, p.stderr)
        self.assertIn("DXR_RUNTIME_VERSION=3.0.1", p.stdout.splitlines())

    # --- the real tree ----------------------------------------------------------
    def test_this_checkout_never_reports_the_old_placeholder(self):
        rc, vals, err = resolve(REPO_ROOT)
        self.assertEqual(rc, 0, err)
        self.assertNotEqual(vals["DXR_RUNTIME_VERSION"], "1.6.0")
        self.assertTrue(vals["DXR_RUNTIME_VERSION_DESCRIBE"].startswith("v" + vals["DXR_RUNTIME_VERSION"]))


if __name__ == "__main__":
    unittest.main(verbosity=2)

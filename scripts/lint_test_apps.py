#!/usr/bin/env python3
"""Gate the in-tree test apps on a chosen set of app-linter rules (CI: lint.yml).

Runs scripts/check_displayxr_app.py over every app under test_apps/ and fails on
any finding of the gated rules that the baseline does not list. Only the gated
rules are looked at, so the linter's other, older findings on these apps never
fail a PR; and the baseline lets a rule be gated on the day it lands, while some
apps still break it, without letting a NEW app (or a new leg) break it too.

The gated rules are the ADR-066 live-mode contract (they were added after the
in-tree apps drifted from it twice in one week):

    INV-2.10  re-read the rendering-mode table on XrEventDataRenderingModeTableChangedDXR
    INV-4.2   size the projection swapchain at XrDisplayRenderingWorstCaseDXR
    INV-4.9   never xrCreateSwapchain from an event handler

The baseline (scripts/lint_test_apps_baseline.json) is a list of
{"app", "rule"} pairs: known findings, each to be fixed and then removed. Both
WARN and ERROR findings fail when not listed. An entry that no longer matches a
finding is reported as stale (a notice, not a failure) so the fix PR, or the
next one, can drop it with --update-baseline.

    python3 scripts/lint_test_apps.py                    # gate (exit 1 on a new finding)
    python3 scripts/lint_test_apps.py --update-baseline  # rewrite the baseline to the current findings
    python3 scripts/lint_test_apps.py --rules INV-2.10   # gate a different rule set

Stdlib only.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
import check_displayxr_app as lint  # noqa: E402

DEFAULT_RULES = ("INV-2.10", "INV-4.2", "INV-4.9")
DEFAULT_BASELINE = HERE / "lint_test_apps_baseline.json"
APP_MARKERS = ("CMakeLists.txt", "build.gradle", "build.gradle.kts", "settings.gradle", "settings.gradle.kts")


def discover_apps(test_apps: Path) -> list:
    """test_apps/<app> and test_apps/<group>/<app> directories that carry their own
    build file. Shared code (any `common` directory) is not an app."""
    apps = []
    for d in sorted(list(test_apps.glob("*/")) + list(test_apps.glob("*/*/"))):
        if not d.is_dir() or "common" in d.relative_to(test_apps).parts:
            continue
        if any((d / m).exists() for m in APP_MARKERS):
            apps.append(d)
    return apps


def load_baseline(path: Path) -> set:
    if not path.exists():
        return set()
    data = json.loads(path.read_text(encoding="utf-8"))
    return {(e["app"], e["rule"]) for e in data.get("known", [])}


def write_baseline(path: Path, pairs: set, rules) -> None:
    data = {
        "_comment": ("Known findings of the gated app-linter rules on test_apps/ "
                     "(scripts/lint_test_apps.py). Fix the app, then remove its entry; "
                     "never add one to land a new app."),
        "rules": list(rules),
        "known": [{"app": a, "rule": r} for a, r in sorted(pairs)],
    }
    path.write_text(json.dumps(data, indent=2) + "\n", encoding="utf-8")


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--rules", default=",".join(DEFAULT_RULES), help="Comma-separated rule ids to gate.")
    ap.add_argument("--baseline", default=str(DEFAULT_BASELINE), help="Baseline JSON path.")
    ap.add_argument("--update-baseline", action="store_true", help="Rewrite the baseline and exit 0.")
    ap.add_argument("--test-apps", default=str(REPO / "test_apps"), help="test_apps directory.")
    args = ap.parse_args(argv)

    rules = tuple(r.strip() for r in args.rules.split(",") if r.strip())
    unknown = [r for r in rules if r not in lint.RULES]
    if unknown:
        print(f"error: unknown rule id(s): {', '.join(unknown)}", file=sys.stderr)
        return 2
    test_apps = Path(args.test_apps).resolve()
    base_root = test_apps.parent
    baseline_path = Path(args.baseline)
    baseline = load_baseline(baseline_path)
    gha = os.environ.get("GITHUB_ACTIONS") == "true"

    current = set()
    new = []
    apps = discover_apps(test_apps)
    for app in apps:
        app_id = app.relative_to(base_root).as_posix()
        for f in lint.lint_app(app):
            if f.rule not in rules or f.level not in (lint.ERROR, lint.WARN):
                continue
            current.add((app_id, f.rule))
            if (app_id, f.rule) not in baseline:
                new.append((app_id, f))

    if args.update_baseline:
        write_baseline(baseline_path, current, rules)
        print(f"wrote {baseline_path} ({len(current)} known finding(s))")
        return 0

    for app_id, f in new:
        path = f"{app_id}/{f.path}"
        print(f"NEW   {f.level} {path}:{f.line}  [{f.rule}] {f.msg}")
        print(f"      fix: {f.fix}")
        if gha:
            msg = f"[{f.rule}] {f.msg} Fix: {f.fix}".replace("%", "%25").replace("\n", " ")
            print(f"::error file={path},line={f.line},title={f.rule}::{msg}")

    stale = sorted(baseline - current)
    for app_id, rule in stale:
        print(f"STALE baseline entry {app_id} [{rule}]: no longer found -- remove it "
              f"(python3 scripts/lint_test_apps.py --update-baseline)")
        if gha:
            print(f"::notice title=stale lint baseline::{app_id} [{rule}] is fixed; drop it from "
                  f"{baseline_path.name}")

    known = len(current & baseline)
    print(f"\n{len(apps)} app(s), rules {', '.join(rules)}: {len(new)} new finding(s), "
          f"{known} baselined, {len(stale)} stale baseline entr{'y' if len(stale) == 1 else 'ies'}.")
    return 1 if new else 0


if __name__ == "__main__":
    sys.exit(main())

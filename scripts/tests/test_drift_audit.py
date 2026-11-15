#!/usr/bin/env python3
"""Unit tests for scripts/drift_audit.py's existing-issue lookup.

Hermetic: the gh calls are stubbed, nothing touches the network.

    python3 scripts/tests/test_drift_audit.py
"""
from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
import drift_audit as da  # noqa: E402

MARK = da.ISSUE_MARKER


def issue(number: int, title: str = da.ISSUE_TITLE, body: str | None = None) -> dict:
    return {"number": number, "title": title, "body": f"{MARK}\n\nfindings" if body is None else body}


class PickMarkedIssue(unittest.TestCase):
    def test_none_open(self):
        self.assertIsNone(da.pick_marked_issue([], da.ISSUE_TITLE))

    def test_marker_matched_in_body(self):
        # The whole point: the marker is an HTML comment that GitHub search does
        # not index, so the match must happen on the body client-side.
        self.assertEqual(da.pick_marked_issue([issue(7)], da.ISSUE_TITLE), "7")

    def test_same_title_without_marker_is_not_ours(self):
        self.assertIsNone(da.pick_marked_issue([issue(7, body="hand-written")], da.ISSUE_TITLE))

    def test_null_body(self):
        self.assertIsNone(da.pick_marked_issue([{"number": 3, "title": da.ISSUE_TITLE, "body": None}],
                                               da.ISSUE_TITLE))

    def test_per_repo_and_aggregate_kept_apart(self):
        # displayxr-runtime holds both kinds; both carry the marker.
        issues = [issue(10), issue(11, title=da.AGGREGATE_ISSUE_TITLE)]
        self.assertEqual(da.pick_marked_issue(issues, da.ISSUE_TITLE), "10")
        self.assertEqual(da.pick_marked_issue(issues, da.AGGREGATE_ISSUE_TITLE), "11")

    def test_duplicates_pick_newest_stably(self):
        issues = [issue(5), issue(18), issue(9)]
        self.assertEqual(da.pick_marked_issue(issues, da.ISSUE_TITLE), "18")


class EmitIssue(unittest.TestCase):
    def run_emit(self, list_result, emit=da.emit_issue, arg=None):
        calls = []

        def fake(args):
            calls.append(args)
            if args[:2] == ["issue", "list"]:
                return list_result
            return (0, "", "")

        with mock.patch.object(da, "_gh_full", side_effect=fake), mock.patch("builtins.print"):
            ok = emit("displayxr-cef-host", [da.Finding("displayxr-cef-host", "pin", "x")]) if arg is None \
                else emit(arg)
        return ok, calls

    def test_updates_existing_instead_of_creating(self):
        ok, calls = self.run_emit((0, json.dumps([issue(4), issue(11)]), ""))
        self.assertTrue(ok)
        self.assertEqual(calls[1][:3], ["issue", "edit", "11"])
        self.assertFalse(any(c[:2] == ["issue", "create"] for c in calls))

    def test_lookup_does_not_use_search(self):
        _, calls = self.run_emit((0, "[]", ""))
        self.assertNotIn("--search", calls[0])
        self.assertIn("body", calls[0][calls[0].index("--json") + 1])

    def test_creates_when_none_open(self):
        ok, calls = self.run_emit((0, "[]", ""))
        self.assertTrue(ok)
        self.assertEqual(calls[1][:2], ["issue", "create"])

    def test_list_failure_opens_nothing(self):
        ok, calls = self.run_emit((1, "", "HTTP 403"))
        self.assertFalse(ok)
        self.assertEqual(len(calls), 1)

    def test_aggregate_updates_its_own_issue(self):
        listing = json.dumps([issue(2585), issue(2600, title=da.AGGREGATE_ISSUE_TITLE)])
        ok, calls = self.run_emit(
            (0, listing, ""),
            emit=da.emit_aggregate_issue,
            arg={"displayxr-cef-host": [da.Finding("displayxr-cef-host", "pin", "x")]},
        )
        self.assertTrue(ok)
        self.assertEqual(calls[1][:3], ["issue", "edit", "2600"])


if __name__ == "__main__":
    unittest.main()

#!/usr/bin/env python3
# Copyright 2026, The DisplayXR Project
# SPDX-License-Identifier: BSL-1.0
"""The `level` argument of the U_LOG_*IFL* macros must be a single operand.

`U_LOG_IFL(level, cond_level, ...)` (src/xrt/auxiliary/util/u_logging.h)
expands to `if ((cond_level) <= (level-as-written))` -- `level` is pasted
WITHOUT parentheses. So a call written

    U_LOG_IFL(first ? U_LOGGING_WARN : U_LOGGING_INFO, u_log_get_global_level(), ...)

becomes `if (((global) <= first) ? U_LOGGING_WARN : U_LOGGING_INFO)`, which is
always true (both enumerators are non-zero): the "INFO after the first one"
branch is logged whatever the global level is, and u_log() itself does not
filter. With a comparison inside the ternary (`n < 16 ? ... : ...`) newer Clang
rejects the expansion outright as a chained comparison, which is how this was
found (the macOS build of comp_multi_weave_macos.c failed on Xcode's newer
Clang in a downstream repo's CI).

The macro is deliberately left as it is: u_logging.h is in the shared-header
closure that gates the out-of-tree Ultraleap provider (downstream-pins.json,
`bump_when: layout`, ADR-063), so touching its bytes forces a provider rebuild
and release. The call sites parenthesise instead, and this test keeps it so.

Hermetic: reads src/ only.
"""
import re
import sys
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
CALL = re.compile(r"\bU_LOG(?:_XDEV)?_IFL(?:_HEX)?\(")
# Operators that bind looser than `<=` (or are `<=`'s own level) and so change
# the meaning of `(cond) <= level` when `level` is pasted bare.
LOOSE = re.compile(r"\?|<=|>=|==|!=|&&|\|\||(?<![<>-])[<>](?![<>=])|(?<![&])&(?![&])|(?<![|])\|(?![|])")


def first_arg(text, start):
	"""The text of the first macro argument, starting just after '('."""
	depth, j = 0, start
	while j < len(text):
		c = text[j]
		if c in "([{":
			depth += 1
		elif c in ")]}":
			if depth == 0:
				return text[start:j]
			depth -= 1
		elif c == "," and depth == 0:
			return text[start:j]
		j += 1
	return text[start:]


def top_level(arg):
	"""`arg` with every parenthesised sub-expression blanked out."""
	out, depth = [], 0
	for c in arg:
		if c in "([":
			depth += 1
		elif c in ")]":
			depth -= 1
			continue
		if depth == 0:
			out.append(c)
	return "".join(out)


def offenders(text):
	for m in CALL.finditer(text):
		arg = first_arg(text, m.end())
		if LOOSE.search(top_level(arg)):
			yield text.count("\n", 0, m.start()) + 1, arg.strip()


class LevelArgs(unittest.TestCase):
	def test_scanner_flags_a_bare_ternary(self):
		bad = "U_LOG_IFL(first ? U_LOGGING_WARN : U_LOGGING_INFO, u_log_get_global_level(), \"x\");"
		self.assertEqual(len(list(offenders(bad))), 1)
		bad2 = "U_LOG_IFL(n < 16 ? U_LOGGING_WARN : U_LOGGING_INFO,\n\tu_log_get_global_level(), \"x\");"
		self.assertEqual(len(list(offenders(bad2))), 1)
		bad3 = "U_LOG_XDEV_IFL_HEX(a == b ? L1 : L2, c, xdev, d, n);"
		self.assertEqual(len(list(offenders(bad3))), 1)

	def test_scanner_accepts_a_parenthesised_or_plain_level(self):
		ok = (
		    "U_LOG_IFL((first ? U_LOGGING_WARN : U_LOGGING_INFO), u_log_get_global_level(), \"a ? b\");\n"
		    "U_LOG_IFL(U_LOGGING_INFO, c->log_level, \"x < y\");\n"
		    "U_LOG_IFL(get_level(a < b), c->log_level, \"x\");\n"
		    "U_LOG_IFL_T(c->settings.log_level, \"%d\", a ? 1 : 2);\n"
		)
		self.assertEqual(list(offenders(ok)), [])

	def test_tree_has_no_bare_level_expression(self):
		found = []
		for p in sorted((ROOT / "src").rglob("*")):
			if p.suffix not in (".c", ".cpp", ".h", ".hpp", ".m", ".mm"):
				continue
			if "external" in p.relative_to(ROOT).parts:
				continue
			text = p.read_text(encoding="utf-8", errors="replace")
			for line, arg in offenders(text):
				found.append("%s:%d: level argument `%s` must be parenthesised" %
				             (p.relative_to(ROOT), line, arg))
		self.assertEqual(found, [], "\n" + "\n".join(found))


if __name__ == "__main__":
	sys.exit(0 if unittest.main(exit=False).result.wasSuccessful() else 1)

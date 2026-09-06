#!/usr/bin/env python3
"""Android versionCode / versionName arithmetic for the DisplayXR runtime APK.

This is the HOST-SIDE TWIN of the closures in
``src/xrt/targets/openxr_android/build.gradle`` (``getVersionCode`` /
``getVersionString``).  Gradle owns the value that goes into the APK; this
script owns the value CI expects to find there.  Two independent
implementations of one rule is the point -- CI compares what gradle actually
stamped into the built APK against what this computes, so a silent change to
either side fails the build instead of shipping.

Why this exists (#1379, #1226)
------------------------------
``git describe`` without ``--tags`` only considers ANNOTATED tags.  DisplayXR
release tags are lightweight, so every APK from v2.14.5 to v2.16.14 named
itself ``v2.14.5-<n>-g<sha>`` while being built from a v2.16.x commit.

The one-line fix (add ``--tags``) is actively harmful on its own: the old
``"%02d%01d%01d%05d"`` string format used MINIMUM field widths, so a two-digit
minor or patch widened the string.  ``v2.16.12-291`` rendered as
``"02161200291"`` = 2,161,200,291 > ``Integer.MAX_VALUE`` (2,147,483,647);
``Integer.parseInt`` threw, a bare ``catch (ignored) { return null }`` swallowed
it, and the APK would have shipped with an EMPTY versionCode -- the #1226
broken-upgrade failure.

The scheme
----------
Positional arithmetic, not string concatenation::

    code = major * 100_000_000     # <= 20   (weight 1e8)
         + minor *   1_000_000     # <= 99   (weight 1e6)
         + patch *      10_000     # <= 99   (weight 1e4)
         + commits                 # <= 9999 (weight 1e0)

Every field is strictly narrower than its weight, so ordering by ``code`` is
exactly lexicographic ordering by ``(major, minor, patch, commits)``.
Monotonic for as long as major stays <= 20 (2,099,999,999 <= INT32_MAX, and
also under Google Play's 2,100,000,000 ceiling).

Continuity: retail units in the field carry 214,500,291 (``v2.14.5-291`` under
the OLD format) and Android refuses a downgrade, so the new scheme must exceed
that.  ``v2.16.12-291`` -> 216,120,291 and ``v2.16.14-4`` -> 216,140,004, both
above the floor.  A future ``v3.0.0`` -> 300,000,000 beats every possible v2
value (worst case ``v2.99.99-9999`` = 299,999,999).

Usage
-----
    android_version_code.py --selftest
    android_version_code.py --describe v2.16.14-4-gdcd9c22e8
    android_version_code.py --git /path/to/repo          # runs git describe
    android_version_code.py --git . --print name         # versionName instead
"""

import argparse
import re
import subprocess
import sys

# Integer.MAX_VALUE -- Android's versionCode is a signed 32-bit int.
INT32_MAX = 2147483647
# Google Play's own (lower) ceiling.
PLAY_CAP = 2100000000
# Highest versionCode ever SHIPPED under the old format (v2.14.5-291, on every
# retail unit today).  Android refuses downgrades, so a release must exceed it.
SHIPPED_FLOOR = 214500291

# Field weights and inclusive maxima, in most-significant-first order.
FIELDS = (
    ("major", 100000000, 20),
    ("minor", 1000000, 99),
    ("patch", 10000, 99),
    ("commits", 1, 9999),
)

# `git describe --tags --long --match 'v[0-9]*'` output.
DESCRIBE_LONG_RE = re.compile(
    r"^v(?P<major>\d+)\.(?P<minor>\d+)\.(?P<patch>\d+)-(?P<commits>\d+)-g(?P<hash>[0-9a-f]+)(?P<dirty>-dirty)?$"
)


class VersionCodeError(ValueError):
    """Raised instead of returning a wrong/None code -- never swallow this."""


def parse_describe(describe):
    """Parse `git describe --tags --long` output into (major, minor, patch, commits)."""
    m = DESCRIBE_LONG_RE.match(describe.strip())
    if not m:
        raise VersionCodeError(
            "%r is not vMAJOR.MINOR.PATCH-<commits>-g<sha> -- "
            "did you forget `git describe --tags --long --match 'v[0-9]*'`?" % describe
        )
    return tuple(int(m.group(g)) for g in ("major", "minor", "patch", "commits"))


def version_code(major, minor, patch, commits):
    """Compute the Android versionCode. Raises VersionCodeError; never returns None."""
    values = dict(major=major, minor=minor, patch=patch, commits=commits)
    for name, _weight, cap in FIELDS:
        v = values[name]
        if v < 0 or v > cap:
            raise VersionCodeError(
                "%s=%d is outside 0..%d -- the positional versionCode encoding "
                "would collapse and stop being monotonic" % (name, v, cap)
            )
    code = sum(values[name] * weight for name, weight, _cap in FIELDS)
    if code > INT32_MAX:
        raise VersionCodeError(
            "versionCode %d exceeds Integer.MAX_VALUE (%d)" % (code, INT32_MAX)
        )
    return code


def version_code_from_describe(describe):
    return version_code(*parse_describe(describe))


def version_name_from_describe(describe):
    """versionName as gradle emits it: 'vX.Y.Z' on the tag, 'vX.Y.Z-N-gsha' past it.

    Gradle gets this straight from `git describe --tags --dirty` (no --long);
    this reconstructs the same string from the --long form so CI needs only one
    git invocation.
    """
    m = DESCRIBE_LONG_RE.match(describe.strip())
    if not m:
        raise VersionCodeError("%r is not a --long describe" % describe)
    base = "v%s.%s.%s" % (m.group("major"), m.group("minor"), m.group("patch"))
    if int(m.group("commits")) != 0:
        base += "-%s-g%s" % (m.group("commits"), m.group("hash"))
    if m.group("dirty"):
        base += "-dirty"
    return base


def git_describe_long(repo):
    out = subprocess.check_output(
        ["git", "describe", "--tags", "--long", "--dirty", "--match", "v[0-9]*"],
        cwd=repo,
        text=True,
    ).strip()
    if not out:
        raise VersionCodeError("git describe produced no output in %s" % repo)
    return out


def selftest():
    ok = True

    def check(label, got, want):
        nonlocal ok
        if got != want:
            print("FAIL %-52s got %r want %r" % (label, got, want))
            ok = False
        else:
            print("ok   %-52s %r" % (label, got))

    def raises(label, fn):
        nonlocal ok
        try:
            fn()
        except VersionCodeError:
            print("ok   %-52s raised" % label)
            return
        print("FAIL %-52s did NOT raise" % label)
        ok = False

    # --- the two values the fix is actually about -------------------------
    check("v2.16.12-291 (the release that exposed #1379)",
          version_code_from_describe("v2.16.12-291-gb31ffd65d"), 216120291)
    check("v2.16.14-4  (origin/main when the fix landed)",
          version_code_from_describe("v2.16.14-4-gdcd9c22e8"), 216140004)

    # --- monotonicity vs what retail units already carry -------------------
    check("shipped floor is v2.14.5-291 under the OLD format",
          SHIPPED_FLOOR, 214500291)
    for d in ("v2.16.12-0-gaaaaaaaaa", "v2.16.12-291-gb31ffd65d",
              "v2.16.14-4-gdcd9c22e8", "v2.17.0-0-gaaaaaaaaa",
              "v3.0.0-0-gaaaaaaaaa"):
        c = version_code_from_describe(d)
        check("%s beats the shipped floor" % d, c > SHIPPED_FLOOR, True)

    # --- strict ordering across a realistic release ladder -----------------
    ladder = [
        "v2.16.12-0-ga", "v2.16.12-1-ga", "v2.16.12-291-ga",
        "v2.16.13-0-ga", "v2.16.14-0-ga", "v2.16.99-9999-ga",
        "v2.17.0-0-ga", "v2.99.99-9999-ga", "v3.0.0-0-ga",
        "v10.0.0-0-ga", "v20.99.99-9999-ga",
    ]
    codes = [version_code_from_describe(d) for d in ladder]
    check("ladder is strictly increasing",
          all(a < b for a, b in zip(codes, codes[1:])), True)

    # --- headroom ----------------------------------------------------------
    check("v2.99.99-9999 (worst v2)   = 299,999,999",
          version_code_from_describe("v2.99.99-9999-ga"), 299999999)
    check("v3.0.0 beats every v2",
          version_code_from_describe("v3.0.0-0-ga") >
          version_code_from_describe("v2.99.99-9999-ga"), True)
    check("v20.99.99-9999 (absolute max) fits int32",
          version_code_from_describe("v20.99.99-9999-ga"), 2099999999)
    check("absolute max is under Google Play's cap",
          version_code_from_describe("v20.99.99-9999-ga") <= PLAY_CAP, True)

    # --- the traps that must FAIL LOUDLY, never return None ----------------
    raises("major 21 overflows the encoding",
           lambda: version_code_from_describe("v21.0.0-0-ga"))
    raises("minor 100 breaks the field width",
           lambda: version_code_from_describe("v2.100.0-0-ga"))
    raises("patch 100 breaks the field width",
           lambda: version_code_from_describe("v2.0.100-0-ga"))
    raises("commits 10000 breaks the field width",
           lambda: version_code_from_describe("v2.0.0-10000-ga"))
    raises("a bare SHA (tagless clone, #1226) is rejected",
           lambda: version_code_from_describe("dcd9c22e8"))
    raises("empty describe is rejected",
           lambda: version_code_from_describe(""))

    # --- the OLD format, reproduced, to show why --tags alone was harmful --
    old = "%02d%01d%01d%05d" % (2, 16, 12, 291)
    check("old format for v2.16.12-291 is 11 digits", old, "02161200291")
    check("old format overflows int32", int(old) > INT32_MAX, True)

    # --- versionName -------------------------------------------------------
    check("versionName on the tag", version_name_from_describe("v2.16.14-0-gdcd9c22e8"), "v2.16.14")
    check("versionName past the tag",
          version_name_from_describe("v2.16.14-4-gdcd9c22e8"), "v2.16.14-4-gdcd9c22e8")
    check("versionName keeps -dirty",
          version_name_from_describe("v2.16.14-4-gdcd9c22e8-dirty"), "v2.16.14-4-gdcd9c22e8-dirty")

    print("\n%s" % ("SELFTEST PASSED" if ok else "SELFTEST FAILED"))
    return 0 if ok else 1


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    g = ap.add_mutually_exclusive_group(required=True)
    g.add_argument("--selftest", action="store_true", help="run the unit tests and exit")
    g.add_argument("--describe", metavar="STR", help="a `git describe --tags --long` string")
    g.add_argument("--git", metavar="REPO", help="run git describe in REPO")
    ap.add_argument("--print", dest="what", choices=("code", "name", "both"), default="code",
                    help="what to print (default: code)")
    ap.add_argument("--floor", type=int, default=None,
                    help="fail if the computed code is <= FLOOR (use %d for the shipped floor)" % SHIPPED_FLOOR)
    args = ap.parse_args(argv)

    if args.selftest:
        return selftest()

    describe = args.describe if args.describe else git_describe_long(args.git)
    try:
        code = version_code_from_describe(describe)
        name = version_name_from_describe(describe)
    except VersionCodeError as e:
        print("error: %s" % e, file=sys.stderr)
        return 2

    if args.floor is not None and code <= args.floor:
        print("error: versionCode %d is not above the floor %d -- installed units "
              "would refuse it as a downgrade" % (code, args.floor), file=sys.stderr)
        return 3

    if args.what == "code":
        print(code)
    elif args.what == "name":
        print(name)
    else:
        print("%s %d" % (name, code))
    return 0


if __name__ == "__main__":
    sys.exit(main())

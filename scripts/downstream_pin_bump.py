#!/usr/bin/env python3
"""
Downstream runtime-tag pin bump -- decision + rewrite.

Companion to check_plugin_abi.py, which answers the same ABI question in the
opposite direction (is a released plug-in compatible with this runtime?). This
script asks: now that the runtime shipped <tag>, should each downstream repo
repin to it, and if so, rewrite its pins.

The gate is the plug-in ABI, NOT the version number. A vendor plug-in's
installer regex-derives MIN_RUNTIME_VERSION from its runtime pin, so raising the
pin raises the installer's minimum-runtime floor. Chasing every patch release
would make a plug-in installer refuse a runtime it works fine against, buying
nothing. So: repin only when XRT_PLUGIN_API_VERSION_CURRENT actually changed.

Spec: docs/specs/runtime/downstream-pin-bump.md

Usage:
    downstream_pin_bump.py decide  --new-tag v2.8.0 [--repo R] [--json]
    downstream_pin_bump.py rewrite --new-tag v2.8.0 --repo R --track windows --dir ./checkout

No external deps beyond the stdlib (GitHub runners have python3).
"""
from __future__ import annotations

import argparse
import json
import re
import sys
import urllib.error
import urllib.request
from pathlib import Path

GITHUB_RAW = "https://raw.githubusercontent.com"
RUNTIME_REPO = "DisplayXR/displayxr-runtime"

# Reuse check_plugin_abi.py's resolver rather than reimplementing it. That
# matters for correctness, not just tidiness: XRT_PLUGIN_API_VERSION_CURRENT is
# an ALIAS (#define ..._CURRENT ..._VERSION_<N>) which then resolves to an int,
# so a naive "match a number" regex silently finds nothing. One definition of
# "what the ABI is" also means the two directions of the gate can never disagree.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from check_plugin_abi import runtime_abi_from_tag as _runtime_abi_from_tag  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST = REPO_ROOT / "downstream-pins.json"

TAG_RE = re.compile(r"^v[0-9]+\.[0-9]+\.[0-9]+$")


def fetch(url: str) -> str:
    with urllib.request.urlopen(url, timeout=30) as r:
        return r.read().decode("utf-8", "replace")


def load_manifest() -> dict:
    return json.loads(MANIFEST.read_text(encoding="utf-8"))


# --------------------------------------------------------------------------
# pin location matching
#
# Two shapes, both anchored so we rewrite the pin and nothing that merely
# mentions it (comments naming the key are common in these files).
# --------------------------------------------------------------------------
def _pin_regex(kind: str, key: str) -> re.Pattern:
    k = re.escape(key)
    if kind == "cmake_set":
        # set(KEY "v1.2.3"   -- keep the literal shape the downstream Rule-5
        # checks regex for; group(2) is the tag.
        return re.compile(r"(set\(\s*" + k + r'\s+")([^"]+)(")')
    if kind == "yaml_env":
        # KEY: v1.2.3   (line-anchored: a bare "RUNTIME_REF" in prose must not match)
        return re.compile(r"(?m)^(\s*" + k + r":\s*)(\S+)([ \t]*(?:#.*)?)$")
    raise SystemExit("unknown location kind: %s" % kind)


def read_pin(text: str, kind: str, key: str) -> str | None:
    m = _pin_regex(kind, key).search(text)
    return m.group(2) if m else None


def rewrite_pin(text: str, kind: str, key: str, new_tag: str) -> tuple[str, int]:
    rx = _pin_regex(kind, key)
    n = 0

    def sub(m: re.Match) -> str:
        nonlocal n
        n += 1
        if kind == "yaml_env":
            return m.group(1) + new_tag + m.group(3)
        return m.group(1) + new_tag + m.group(3)

    return rx.sub(sub, text), n


def abi_at_tag(tag: str):
    """XRT_PLUGIN_API_VERSION_CURRENT as of a runtime tag, or None."""
    try:
        return _runtime_abi_from_tag(tag)
    except Exception as e:  # HTTP, parse, or unknown-alias -- all non-fatal here
        print("  ! cannot resolve plug-in ABI at %s: %s" % (tag, e))
        return None


def pinned_tag_upstream(repo: str, loc: dict) -> str | None:
    """The tag a downstream repo currently pins, read from its default branch."""
    url = "%s/DisplayXR/%s/HEAD/%s" % (GITHUB_RAW, repo, loc["file"])
    try:
        return read_pin(fetch(url), loc["kind"], loc["key"])
    except urllib.error.HTTPError as e:
        print("  ! cannot read %s/%s (HTTP %s)" % (repo, loc["file"], e.code))
        return None


# --------------------------------------------------------------------------
def cmd_decide(args: argparse.Namespace) -> int:
    if not TAG_RE.match(args.new_tag):
        print("refusing non-canonical tag %r (want vX.Y.Z)" % args.new_tag)
        return 2

    new_abi = abi_at_tag(args.new_tag)
    if new_abi is None:
        print("FATAL: could not resolve the plug-in ABI at %s" % args.new_tag)
        return 1
    print("runtime %s -> plug-in ABI v%s" % (args.new_tag, new_abi))

    decisions = []
    man = load_manifest()
    for repo, spec in man["runtime_tag_pins"].items():
        if args.repo and repo != args.repo:
            continue
        for track, t in spec["tracks"].items():
            d = {"repo": repo, "track": track, "new_tag": args.new_tag, "new_abi": new_abi}
            if not t.get("auto_bump"):
                d.update(action="skip", reason="track is human-owned (auto_bump=false): "
                                               + t.get("reason", "no reason recorded"))
                decisions.append(d)
                continue

            cur = pinned_tag_upstream(repo, t["locations"][0])
            d["current_tag"] = cur
            if cur is None:
                d.update(action="error", reason="could not read the current pin")
            elif cur == args.new_tag:
                d.update(action="skip", reason="already pinned to %s" % args.new_tag)
            else:
                cur_abi = abi_at_tag(cur)
                d["current_abi"] = cur_abi
                if cur_abi is None:
                    d.update(action="error", reason="could not read ABI at currently-pinned %s" % cur)
                elif cur_abi == new_abi:
                    # The load-bearing case: a patch release. Repinning would only
                    # raise this plug-in's installer MIN_RUNTIME_VERSION floor.
                    d.update(action="skip",
                             reason="ABI unchanged (%s at both %s and %s) -- repinning would raise "
                                    "the installer's minimum-runtime floor for no gain"
                                    % (new_abi, cur, args.new_tag))
                else:
                    d.update(action="bump",
                             reason="plug-in ABI changed %s -> %s between %s and %s"
                                    % (cur_abi, new_abi, cur, args.new_tag))
            decisions.append(d)

    for d in decisions:
        print("  [%-5s] %s/%s: %s" % (d["action"], d["repo"], d["track"], d["reason"]))

    if args.json:
        Path(args.json).write_text(json.dumps(decisions, indent=2), encoding="utf-8")
        print("wrote %s" % args.json)

    return 1 if any(d["action"] == "error" for d in decisions) else 0


def cmd_rewrite(args: argparse.Namespace) -> int:
    if not TAG_RE.match(args.new_tag):
        print("refusing non-canonical tag %r" % args.new_tag)
        return 2

    man = load_manifest()
    try:
        track = man["runtime_tag_pins"][args.repo]["tracks"][args.track]
    except KeyError:
        print("no such repo/track in the manifest: %s/%s" % (args.repo, args.track))
        return 2
    if not track.get("auto_bump"):
        print("refusing: %s/%s is auto_bump=false" % (args.repo, args.track))
        return 2

    root = Path(args.dir)
    # A track is an atomic group: verify EVERY location is writable and matches
    # before touching any of them, so we can never leave the repo half-bumped
    # (its Rule-5 CI check hard-fails on exactly that state).
    planned = []
    for loc in track["locations"]:
        p = root / loc["file"]
        if not p.is_file():
            print("FATAL: missing %s" % p)
            return 1
        raw = p.read_bytes()
        text = raw.decode("utf-8")
        cur = read_pin(text, loc["kind"], loc["key"])
        if cur is None:
            print("FATAL: no %s pin (%s) found in %s" % (loc["key"], loc["kind"], loc["file"]))
            return 1
        new_text, n = rewrite_pin(text, loc["kind"], loc["key"], args.new_tag)
        if n != 1:
            print("FATAL: %s matched %d times in %s -- expected exactly 1" % (loc["key"], n, loc["file"]))
            return 1
        planned.append((p, cur, new_text))

    changed = 0
    for p, cur, new_text in planned:
        if cur == args.new_tag:
            print("  = %s already %s" % (p.name, args.new_tag))
            continue
        # newline="" preserves the file's existing CRLF endings byte-for-byte.
        with open(p, "w", encoding="utf-8", newline="") as fh:
            fh.write(new_text)
        print("  + %s: %s -> %s" % (p.name, cur, args.new_tag))
        changed += 1

    print("rewrote %d file(s) for %s/%s" % (changed, args.repo, args.track))
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    d = sub.add_parser("decide", help="decide, per repo/track, whether to repin")
    d.add_argument("--new-tag", required=True)
    d.add_argument("--repo")
    d.add_argument("--json", help="also write the decisions to this path")
    d.set_defaults(fn=cmd_decide)

    w = sub.add_parser("rewrite", help="rewrite one track's pins in a local checkout")
    w.add_argument("--new-tag", required=True)
    w.add_argument("--repo", required=True)
    w.add_argument("--track", required=True)
    w.add_argument("--dir", required=True)
    w.set_defaults(fn=cmd_rewrite)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == "__main__":
    sys.exit(main())

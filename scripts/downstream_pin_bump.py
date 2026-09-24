#!/usr/bin/env python3
"""
Downstream runtime-tag pin bump -- decision + rewrite.

Companion to check_plugin_abi.py, which answers the same ABI question in the
opposite direction (is a released plug-in compatible with this runtime?). This
script asks: now that the runtime shipped <tag>, should each downstream track
repin to it, and if so, rewrite its pins.

Each track carries a POLICY (`bump_when` in downstream-pins.json):

  abi       repin only when XRT_PLUGIN_API_VERSION_CURRENT changed between the
            pinned tag and the new tag. A vendor plug-in's installer regex-derives
            MIN_RUNTIME_VERSION from its runtime pin, so chasing patch releases
            would make it refuse a runtime it works fine against.
  features  repin when the plug-in-facing FEATURE SURFACE changed: a
            `XRT_*_HAS_*` feature macro added / removed / redefined, a vtable
            (function-pointer) slot added / removed / re-signed, or the plug-in
            ABI changed, in the track's feature headers. Catches the append-only
            slot + feature macro that ADR-020 adds WITHOUT an ABI bump -- the case
            the abi gate is blind to (leia-plugin#264 guards on
            XRT_DP_VK_HAS_TRANSPARENCY_ACTIVE, first shipped in v2.21.1 at ABI 5;
            at the old pin that code silently compiled out).
  manual    human-owned; never touched.

Spec: docs/specs/runtime/downstream-pin-bump.md

Usage:
    downstream_pin_bump.py decide  --new-tag v2.8.0 [--repo R] [--json out.json]
    downstream_pin_bump.py verdict --from v2.21.0 --to v2.21.1 --repo R --track T
    downstream_pin_bump.py verdict --from v2.21.0 --to v2.21.1 --policy features
    downstream_pin_bump.py rewrite --new-tag v2.8.0 --repo R --track windows --dir ./checkout

`verdict` is the dry run: it computes the decision between two runtime tags
without reading any downstream repo and without writing anything.

No external deps beyond the stdlib (GitHub runners have python3).
"""
from __future__ import annotations

import argparse
import json
import re
import subprocess
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
from check_plugin_abi import _resolve_api_version_current  # noqa: E402
from check_plugin_abi import PLUGIN_HEADER_PATH  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parent.parent
MANIFEST = REPO_ROOT / "downstream-pins.json"

TAG_RE = re.compile(r"^v[0-9]+\.[0-9]+\.[0-9]+$")
POLICIES = ("abi", "features", "manual")


def fetch(url: str) -> str:
    req = urllib.request.Request(url, headers={"User-Agent": "displayxr-pin-bump"})
    with urllib.request.urlopen(req, timeout=30) as r:
        return r.read().decode("utf-8", "replace")


def load_manifest() -> dict:
    return json.loads(MANIFEST.read_text(encoding="utf-8"))


def semver(tag: str) -> tuple[int, int, int]:
    a, b, c = tag.lstrip("v").split(".")
    return int(a), int(b), int(c)


def track_policy(track: dict) -> str:
    """`bump_when`, validated. There is no fallback: an unknown or missing
    policy is a manifest error, not a silent 'abi'."""
    p = track.get("bump_when")
    if p not in POLICIES:
        raise SystemExit("track has invalid bump_when %r (want one of %s)" % (p, "/".join(POLICIES)))
    return p


def feature_headers(man: dict, track: dict | None) -> list[str]:
    if track and track.get("feature_headers"):
        return list(track["feature_headers"])
    return list(man["feature_surface"]["headers"])


# --------------------------------------------------------------------------
# reading runtime headers at a tag
#
# Prefer the local clone (fast, offline, what the dry run and the tests use);
# fall back to raw.githubusercontent.com when the tag is not present locally --
# runtime-pin-bump.yml checks out depth 1, so the PINNED tag usually is not.
# --------------------------------------------------------------------------
def _git(*args: str) -> subprocess.CompletedProcess:
    return subprocess.run(["git", "-C", str(REPO_ROOT), *args], capture_output=True, text=True)


def header_at(tag: str, path: str) -> str | None:
    """Header text at a runtime tag, or None when the file does not exist there."""
    if _git("rev-parse", "--verify", "--quiet", tag + "^{commit}").returncode == 0:
        r = _git("show", "%s:%s" % (tag, path))
        return r.stdout if r.returncode == 0 else None
    try:
        return fetch("%s/%s/%s/%s" % (GITHUB_RAW, RUNTIME_REPO, tag, path))
    except urllib.error.HTTPError as e:
        if e.code == 404:
            return None
        raise


def abi_at_tag(tag: str):
    """XRT_PLUGIN_API_VERSION_CURRENT as of a runtime tag, or None."""
    try:
        text = header_at(tag, PLUGIN_HEADER_PATH)
        if text is None:
            raise RuntimeError("%s absent at %s" % (PLUGIN_HEADER_PATH, tag))
        return _resolve_api_version_current(text)
    except Exception as e:  # HTTP, parse, or unknown-alias -- all non-fatal here
        print("  ! cannot resolve plug-in ABI at %s: %s" % (tag, e))
        return None


# --------------------------------------------------------------------------
# the feature surface
#
# What a plug-in can guard on or call: feature macros, vtable slots, the ABI.
# Deliberately NOT the raw header text -- comment edits, reflowed docs and new
# static-inline helpers change nothing a plug-in compiles differently against,
# and treating them as triggers would turn this back into tag-chasing.
# --------------------------------------------------------------------------
_COMMENT_RX = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
# Object-like feature macros only: `#define XRT_DP_VK_HAS_FOO 1`. The lookahead
# excludes function-like macros (`#define XRT_DP_HAS_SLOT(...)`).
_MACRO_RX = re.compile(r"(?m)^[ \t]*#[ \t]*define[ \t]+(XRT_\w*_HAS_\w+)(?=[ \t]|$)[ \t]*(.*?)[ \t]*$")
_STRUCT_RX = re.compile(r"\bstruct\s+(\w+)\s*\{")
_SLOT_RX = re.compile(r"([^;{}]*?\(\s*\*\s*(\w+)\s*\)\s*\([^;{}]*\))\s*;")


def strip_comments(text: str) -> str:
    return _COMMENT_RX.sub(" ", text)


def _norm(decl: str) -> str:
    s = re.sub(r"\s+", " ", decl).strip()
    return re.sub(r"\s*([*(),])\s*", r"\1", s)


def _struct_bodies(code: str):
    for m in _STRUCT_RX.finditer(code):
        depth, i = 1, m.end()
        while i < len(code) and depth:
            if code[i] == "{":
                depth += 1
            elif code[i] == "}":
                depth -= 1
            i += 1
        yield m.group(1), code[m.end():i - 1]


def surface_of(text: str, header: str) -> dict:
    """{"macros": {name: (value, header)}, "slots": {struct.member: (decl, header)}}."""
    code = strip_comments(text)
    macros = {m.group(1): (m.group(2), header) for m in _MACRO_RX.finditer(code)}
    slots = {}
    for struct, body in _struct_bodies(code):
        for s in _SLOT_RX.finditer(body):
            slots["%s.%s" % (struct, s.group(2))] = (_norm(s.group(1)), header)
    return {"macros": macros, "slots": slots}


def surface_at(tag: str, headers: list[str]) -> dict:
    out = {"macros": {}, "slots": {}, "abi": None, "text": {}}
    for h in headers:
        text = header_at(tag, h)
        name = Path(h).name
        out["text"][name] = _norm(strip_comments(text)) if text is not None else None
        if text is None:
            continue
        s = surface_of(text, name)
        out["macros"].update(s["macros"])
        out["slots"].update(s["slots"])
        if h == PLUGIN_HEADER_PATH:
            out["abi"] = _resolve_api_version_current(text)
    return out


def diff_surfaces(old: dict, new: dict) -> tuple[list[str], list[str]]:
    """(triggers, notes). A trigger is a change a plug-in could guard on; a note
    is a header that changed textually without touching the feature surface."""
    trig: list[str] = []
    if old.get("abi") is not None and new.get("abi") is not None and old["abi"] != new["abi"]:
        trig.append("plug-in ABI XRT_PLUGIN_API_VERSION_CURRENT %s -> %s" % (old["abi"], new["abi"]))

    om, nm = old["macros"], new["macros"]
    for k in sorted(nm.keys() - om.keys()):
        trig.append("new feature macro %s (%s)" % (k, nm[k][1]))
    for k in sorted(om.keys() - nm.keys()):
        trig.append("feature macro %s removed (%s)" % (k, om[k][1]))
    for k in sorted(om.keys() & nm.keys()):
        if om[k][0] != nm[k][0]:
            trig.append("feature macro %s redefined %r -> %r (%s)" % (k, om[k][0], nm[k][0], nm[k][1]))

    os_, ns = old["slots"], new["slots"]
    for k in sorted(ns.keys() - os_.keys()):
        trig.append("new vtable slot %s (%s)" % (k, ns[k][1]))
    for k in sorted(os_.keys() - ns.keys()):
        trig.append("vtable slot %s removed (%s)" % (k, os_[k][1]))
    for k in sorted(os_.keys() & ns.keys()):
        if os_[k][0] != ns[k][0]:
            trig.append("vtable slot %s re-signed: %s -> %s (%s)" % (k, os_[k][0], ns[k][0], ns[k][1]))

    notes = [
        "%s changed without touching the feature surface" % h
        for h in sorted(new.get("text", {}))
        if old.get("text", {}).get(h) != new["text"][h]
    ] if not trig else []
    return trig, notes


# --------------------------------------------------------------------------
# the verdict: one track, one (pinned, new) pair
# --------------------------------------------------------------------------
def verdict(policy: str, cur: str, new: str, headers: list[str]) -> dict:
    """Pure decision given two runtime tags. Returns action/reason/triggers."""
    d = {"policy": policy, "current_tag": cur, "new_tag": new, "triggers": []}
    if policy == "manual":
        return dict(d, action="skip", reason="track is human-owned (bump_when=manual)")
    if cur == new:
        return dict(d, action="skip", reason="already pinned to %s" % new)
    if not TAG_RE.match(cur or ""):
        return dict(d, action="error", reason="pinned value %r is not a vX.Y.Z tag" % cur)
    if semver(cur) > semver(new):
        # A patch on an older line must never DOWNGRADE a pin -- and under the
        # features policy a backwards diff reads as "macros removed", a trigger.
        return dict(d, action="skip", reason="pinned %s is newer than %s" % (cur, new))

    if policy == "abi":
        cur_abi, new_abi = abi_at_tag(cur), abi_at_tag(new)
        d.update(current_abi=cur_abi, new_abi=new_abi)
        if cur_abi is None or new_abi is None:
            return dict(d, action="error", reason="could not resolve the plug-in ABI at %s or %s" % (cur, new))
        if cur_abi == new_abi:
            # The load-bearing case: a patch release. Repinning would only
            # raise this plug-in's installer MIN_RUNTIME_VERSION floor.
            return dict(d, action="skip",
                        reason="ABI unchanged (%s at both %s and %s) -- repinning would raise "
                               "the installer's minimum-runtime floor for no gain" % (new_abi, cur, new))
        return dict(d, action="bump", triggers=["plug-in ABI %s -> %s" % (cur_abi, new_abi)],
                    reason="plug-in ABI changed %s -> %s between %s and %s" % (cur_abi, new_abi, cur, new))

    # features
    try:
        old_s, new_s = surface_at(cur, headers), surface_at(new, headers)
    except Exception as e:
        return dict(d, action="error", reason="could not read feature headers: %s" % e)
    trig, notes = diff_surfaces(old_s, new_s)
    d["headers"] = [Path(h).name for h in headers]
    d["notes"] = notes
    if not trig:
        extra = (" (%s)" % "; ".join(notes)) if notes else ""
        return dict(d, action="skip",
                    reason="no feature macro, vtable slot or ABI change in %s between %s and %s%s -- "
                           "repinning would raise the plug-in's runtime floor for no gain"
                           % (", ".join(d["headers"]), cur, new, extra))
    return dict(d, action="bump", triggers=trig,
                reason="feature surface changed between %s and %s: %s" % (cur, new, "; ".join(trig)))


def pinned_tag_upstream(repo: str, loc: dict) -> str | None:
    """The tag a downstream repo currently pins, read from its default branch."""
    url = "%s/DisplayXR/%s/HEAD/%s" % (GITHUB_RAW, repo, loc["file"])
    try:
        return read_pin(fetch(url), loc["kind"], loc["key"])
    except urllib.error.HTTPError as e:
        print("  ! cannot read %s/%s (HTTP %s)" % (repo, loc["file"], e.code))
        return None


def pr_text(d: dict, track: dict) -> tuple[str, str]:
    """(title, markdown body) for the downstream PR."""
    title = "chore(ci): repin runtime %s -> %s (%s)" % (d["current_tag"], d["new_tag"], d["track"])
    files = ", ".join("`%s` %s" % (l["file"], l["key"]) for l in track["locations"])
    lines = [
        "Automated repin: this repo's %s runtime pin moves **%s -> %s**." % (d["track"], d["current_tag"], d["new_tag"]),
        "",
        "Locations written (one commit, so the Rule-5 drift check stays green): %s." % files,
        "",
        "**Policy:** `bump_when=%s`." % d["policy"],
        "",
        "**Triggered by:**",
    ]
    lines += ["- %s" % t for t in d.get("triggers") or [d["reason"]]]
    lines.append("")
    if d["policy"] == "features":
        lines += [
            "Feature-gated, not tag-chasing: this track repins only when a `XRT_*_HAS_*` feature",
            "macro, a vtable slot, or the plug-in ABI changed in %s between the pinned" % ", ".join("`%s`" % h for h in d.get("headers", [])),
            "tag and the new one. At the old pin, code guarded on a new macro compiles out",
            "silently; a release that changes none of these is skipped, because raising the pin",
            "raises the plug-in's runtime floor for no gain.",
        ]
    else:
        lines += [
            "ABI-gated, not tag-chasing: a release that changes no plug-in ABI is skipped,",
            "because raising the pin also raises this repo's installer MIN_RUNTIME_VERSION",
            "floor and would make it refuse a runtime it works fine against.",
        ]
    lines += [
        "",
        "See docs/specs/runtime/downstream-pin-bump.md in displayxr-runtime. **Merge only once",
        "CI is green** -- if the new runtime genuinely breaks the build, that is the signal",
        "this PR exists to surface.",
        "",
        "Opened by runtime-pin-bump.yml from displayxr-runtime@%s" % d["new_tag"],
    ]
    return title, "\n".join(lines) + "\n"


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
        return m.group(1) + new_tag + m.group(3)

    return rx.sub(sub, text), n


# --------------------------------------------------------------------------
def cmd_decide(args: argparse.Namespace) -> int:
    if not TAG_RE.match(args.new_tag):
        print("refusing non-canonical tag %r (want vX.Y.Z)" % args.new_tag)
        return 2

    decisions = []
    man = load_manifest()
    for repo, spec in man["runtime_tag_pins"].items():
        if args.repo and repo != args.repo:
            continue
        for track_name, t in spec["tracks"].items():
            policy = track_policy(t)
            if policy == "manual":
                d = verdict(policy, "", args.new_tag, [])
                d["reason"] += ": " + t.get("reason", "no reason recorded")
            else:
                cur = pinned_tag_upstream(repo, t["locations"][0])
                if cur is None:
                    d = {"policy": policy, "current_tag": None, "new_tag": args.new_tag,
                         "action": "error", "reason": "could not read the current pin", "triggers": []}
                else:
                    d = verdict(policy, cur, args.new_tag, feature_headers(man, t))
            d.update(repo=repo, track=track_name)
            if d["action"] == "bump":
                d["pr_title"], d["pr_body"] = pr_text(d, t)
            decisions.append(d)

    for d in decisions:
        print("  [%-5s] %s/%s (%s): %s" % (d["action"], d["repo"], d["track"], d["policy"], d["reason"]))

    if args.json:
        Path(args.json).write_text(json.dumps(decisions, indent=2), encoding="utf-8")
        print("wrote %s" % args.json)

    return 1 if any(d["action"] == "error" for d in decisions) else 0


def cmd_verdict(args: argparse.Namespace) -> int:
    """Dry run between two runtime tags. Reads no downstream repo, writes nothing."""
    man = load_manifest()
    track = None
    if args.repo or args.track:
        try:
            track = man["runtime_tag_pins"][args.repo]["tracks"][args.track]
        except KeyError:
            print("no such repo/track in the manifest: %s/%s" % (args.repo, args.track))
            return 2
    policy = args.policy or (track_policy(track) if track else "features")
    d = verdict(policy, args.from_tag, args.to_tag, feature_headers(man, track))
    print("%s -> %s  policy=%s  => %s" % (args.from_tag, args.to_tag, policy, d["action"].upper()))
    for t in d.get("triggers") or []:
        print("  trigger: %s" % t)
    for n in d.get("notes") or []:
        print("  note:    %s" % n)
    print("  reason:  %s" % d["reason"])
    if args.json:
        print(json.dumps(d, indent=2))
    return 1 if d["action"] == "error" else 0


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
    if track_policy(track) == "manual":
        print("refusing: %s/%s is bump_when=manual" % (args.repo, args.track))
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
        text = p.read_bytes().decode("utf-8")
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

    v = sub.add_parser("verdict", help="dry run: the decision between two runtime tags")
    v.add_argument("--from", dest="from_tag", required=True)
    v.add_argument("--to", dest="to_tag", required=True)
    v.add_argument("--repo")
    v.add_argument("--track")
    v.add_argument("--policy", choices=("abi", "features"),
                   help="override the track's policy (default: the track's, else features)")
    v.add_argument("--json", action="store_true", help="also print the decision as JSON")
    v.set_defaults(fn=cmd_verdict)

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

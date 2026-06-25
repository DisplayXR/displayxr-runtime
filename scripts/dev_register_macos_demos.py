#!/usr/bin/env python3
# Copyright 2026, DisplayXR
# SPDX-License-Identifier: BSL-1.0
"""
Dev helper (#61, macOS): register the sibling demo repos' macOS builds with the
workspace launcher so they appear on the spatial desktop.

The macOS shell discovers apps via .displayxr.json manifests. In-tree test apps
ship theirs under <app>/displayxr/ and are found automatically; the standalone
demos live in sibling repos (~/Documents/GitHub/displayxr-demo-*) and aren't on
the launcher's scan path. This mirrors what a demo *installer* would do: drop a
registered manifest (carrying an absolute exe_path + absolute icon paths) into
~/Library/Application Support/DisplayXR/apps/, which the launcher scans.

Run after building the demos for macOS. Re-run to refresh. Idempotent.
"""
import json, os, glob

HOME = os.path.expanduser("~")
APPS = os.path.join(HOME, "Library/Application Support/DisplayXR/apps")
GH = os.path.join(HOME, "Documents/GitHub")


def find_macos_binary(repo):
    for pat in ("build/macos/*_macos", "build/*_macos", "build/macos/*", "build/*_handle_*_macos"):
        for b in sorted(glob.glob(os.path.join(repo, pat))):
            if os.path.isfile(b) and os.access(b, os.X_OK) and not b.endswith(".json"):
                return b
    return None


def find_manifest(repo, base):
    # Prefer a macOS manifest matching the binary; fall back to any non-android one.
    prefs = [
        os.path.join(repo, "macos/displayxr", base + ".displayxr.json"),
        os.path.join(repo, "displayxr", base + ".displayxr.json"),
    ]
    for p in prefs:
        if os.path.isfile(p):
            return p
    cands = [m for m in glob.glob(os.path.join(repo, "**/*.displayxr.json"), recursive=True)
             if "android" not in m and "/build/" not in m]
    return cands[0] if cands else None


def main():
    os.makedirs(APPS, exist_ok=True)
    written = []
    for repo in sorted(glob.glob(os.path.join(GH, "displayxr-demo-*"))):
        exe = find_macos_binary(repo)
        if not exe:
            continue
        base = os.path.basename(exe)
        out = {"schema_version": 1, "name": base, "type": "3d",
               "category": "demo", "exe_path": exe}
        man = find_manifest(repo, base)
        if man:
            mdir = os.path.dirname(man)
            try:
                d = json.load(open(man))
            except Exception:
                d = {}
            out["name"] = d.get("name", base)
            out["type"] = d.get("type", "3d")
            out["category"] = d.get("category", "demo")
            if d.get("description"):
                out["description"] = d["description"]
            icon = d.get("icon")
            ip = os.path.join(mdir, icon) if icon else None
            if ip and os.path.isfile(ip):
                out["icon"] = ip
                i3 = d.get("icon_3d")
                i3p = os.path.join(mdir, i3) if i3 else None
                if i3p and os.path.isfile(i3p):
                    out["icon_3d"] = i3p
                    out["icon_3d_layout"] = d.get("icon_3d_layout", "sbs-lr")
        outp = os.path.join(APPS, base + ".displayxr.json")
        json.dump(out, open(outp, "w"), indent=2)
        written.append((out["name"], exe, "icon" if out.get("icon") else "text-tile"))

    print("Registered %d macOS demo(s) → %s" % (len(written), APPS))
    for name, exe, ic in written:
        print("  %-28s %-10s %s" % (name, ic, exe))


if __name__ == "__main__":
    main()

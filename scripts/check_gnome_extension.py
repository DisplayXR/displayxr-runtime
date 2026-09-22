#!/usr/bin/env python3
"""Lint the GNOME Shell extension shipped in contrib/gnome-shell/ (#1663).

The extension has to load on every GNOME Shell our Linux packages are
installed on, and those span the 45.0 module-system break: GNOME 45+ loads an
extension as an ES module, GNOME 40-44 with the legacy importer, for which ESM
syntax is a parse error. So the extension ships two thin entry points over one
shared `lib.js`, and `scripts/linux/displayxr-gnome-extension-enable` puts the
right one in the `extension.js` slot at install/login time.

Three failures that shape is exposed to, all of them silent at build time and
loud only on a user's desktop, are what this checks:

  1. `metadata.json`'s `shell-version` not covering a release we ship a .deb
     for. GNOME refuses to load an extension that does not name its version,
     so a missing entry is a whole distribution silently losing windowed
     weaving and capture exclusion. The list of releases is read from
     build-linux.yml's DebInstall matrix, so adding a release there without
     widening the extension fails here rather than in the field.

  2. ESM syntax reaching the legacy files. `lib.js` and the GNOME 40-44 entry
     point must parse under the SCRIPT goal (where `import`/`export` is a
     syntax error) AND `lib.js` must equally parse as a module, because both
     loaders read that one file. This compiles them, with gjs — the same
     engine GNOME Shell runs — rather than trusting a grep.

  3. An entry point that is not the form its loader expects: no
     `export default` in the modern one, no top-level `init()` in the legacy
     one.

Hardware-free, no network, ~1 s.

  python3 scripts/check_gnome_extension.py [--require-gjs]
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
UUID = "window-geometry@displayxr.org"
EXT_DIR = os.path.join(ROOT, "contrib", "gnome-shell", UUID)
WORKFLOW = os.path.join(ROOT, ".github", "workflows", "build-linux.yml")

MODERN_ENTRY = "extension.js"        # GNOME 45+: ES module + Extension class
LEGACY_ENTRY = "extension-gnome42.js"  # GNOME 40-44: legacy importer + init()
SHARED_LIB = "lib.js"

# The GNOME Shell that each Ubuntu release we package for ships. Only releases
# named by build-linux.yml's install matrix are required; the map is here so an
# unknown release fails loudly instead of being skipped.
UBUNTU_GNOME_SHELL = {
    "20.04": 3,
    "22.04": 42,
    "24.04": 46,
    "25.04": 48,
    "26.04": 50,
}

# The first shell that loads extensions as ES modules. Everything below it
# needs the legacy entry point.
FIRST_ESM_SHELL = 45

errors = []
notes = []


def fail(msg):
    errors.append(msg)


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


def packaged_releases():
    """The Ubuntu releases build-linux.yml installs the .deb into."""
    if not os.path.exists(WORKFLOW):
        fail(f"missing {WORKFLOW} — cannot tell which releases we package for")
        return []
    found = []
    for line in read(WORKFLOW).splitlines():
        m = re.match(r"\s*release:\s*\[(.*)\]\s*$", line)
        if m:
            found += re.findall(r'"([\d.]+)"', m.group(1))
    if not found:
        fail(f"no `release: [...]` matrix found in {WORKFLOW}")
    return sorted(set(found))


def check_metadata():
    path = os.path.join(EXT_DIR, "metadata.json")
    try:
        meta = json.loads(read(path))
    except Exception as e:  # noqa: BLE001 - report, don't raise
        fail(f"metadata.json is not valid JSON: {e}")
        return
    if meta.get("uuid") != UUID:
        fail(f'metadata.json uuid is {meta.get("uuid")!r}, must equal the directory name {UUID!r}')
    if not isinstance(meta.get("version"), int):
        fail("metadata.json version must be an integer (GNOME compares it numerically)")

    declared_raw = meta.get("shell-version")
    if not isinstance(declared_raw, list) or not declared_raw:
        fail("metadata.json shell-version must be a non-empty list of strings")
        return
    declared = set()
    for v in declared_raw:
        if not isinstance(v, str):
            fail(f"shell-version entry {v!r} must be a string")
            continue
        declared.add(int(v.split(".")[0]))

    releases = packaged_releases()
    required = {}
    for rel in releases:
        if rel not in UBUNTU_GNOME_SHELL:
            fail(
                f"build-linux.yml packages for Ubuntu {rel}, which is not in "
                f"UBUNTU_GNOME_SHELL in {os.path.basename(__file__)} — add it "
                "(and make sure the extension covers that GNOME)"
            )
            continue
        required[UBUNTU_GNOME_SHELL[rel]] = rel

    missing = sorted(m for m in required if m not in declared)
    if missing:
        fail(
            "metadata.json shell-version does not cover "
            + ", ".join(f"GNOME {m} (Ubuntu {required[m]})" for m in missing)
            + f" — declared: {sorted(declared)}. GNOME refuses to load an extension "
            "that does not name its own version, so that release loses windowed "
            "weaving and capture exclusion entirely."
        )
    notes.append(
        "shell-version covers "
        + ", ".join(f"GNOME {m} (Ubuntu {r})" for m, r in sorted(required.items()))
    )

    # An entry below 45 is only loadable through the legacy entry point, and
    # vice versa: both forms must exist for the declared range to be honest.
    if any(v < FIRST_ESM_SHELL for v in declared) and not os.path.exists(
        os.path.join(EXT_DIR, LEGACY_ENTRY)
    ):
        fail(
            f"shell-version declares a pre-{FIRST_ESM_SHELL} shell but {LEGACY_ENTRY} "
            "does not exist — such a shell cannot parse an ES module"
        )
    if any(v >= FIRST_ESM_SHELL for v in declared) and not os.path.exists(
        os.path.join(EXT_DIR, MODERN_ENTRY)
    ):
        fail(f"shell-version declares {FIRST_ESM_SHELL}+ but {MODERN_ENTRY} does not exist")


ESM_STATEMENT = re.compile(r"^\s*(?:import|export)\b", re.M)


def check_shapes():
    modern = read(os.path.join(EXT_DIR, MODERN_ENTRY))
    legacy = read(os.path.join(EXT_DIR, LEGACY_ENTRY))
    lib = read(os.path.join(EXT_DIR, SHARED_LIB))

    if "export default" not in modern:
        fail(f"{MODERN_ENTRY} has no `export default` — GNOME 45+ loads that, nothing else")
    if "gi://" not in modern:
        fail(f"{MODERN_ENTRY} does not import from gi:// — is it really the ESM entry point?")
    if f"'./{SHARED_LIB}'" not in modern and f'"./{SHARED_LIB}"' not in modern:
        fail(f"{MODERN_ENTRY} does not import ./{SHARED_LIB} — the logic must stay shared")

    if ESM_STATEMENT.search(legacy):
        fail(f"{LEGACY_ENTRY} contains ESM syntax; the pre-45 loader cannot parse it")
    if not re.search(r"^function init\(", legacy, re.M):
        fail(f"{LEGACY_ENTRY} has no top-level `function init(` — the pre-45 loader calls it")
    if "imports.gi" not in legacy:
        fail(f"{LEGACY_ENTRY} does not use imports.gi — is it really the legacy entry point?")
    if f"imports.{SHARED_LIB[:-3]}" not in legacy:
        fail(f"{LEGACY_ENTRY} does not load {SHARED_LIB} — the logic must stay shared")

    if ESM_STATEMENT.search(lib):
        fail(
            f"{SHARED_LIB} contains an import/export statement. It is loaded by BOTH "
            "module systems, which is only possible while it has neither."
        )
    if "globalThis.displayxrWindowGeometry" not in lib:
        fail(f"{SHARED_LIB} no longer publishes itself on globalThis")


def gjs_check(require):
    gjs = shutil.which("gjs")
    if not gjs:
        msg = "gjs not installed — skipped the syntax check"
        if require:
            fail(msg.replace("skipped", "cannot run") + " (--require-gjs)")
        else:
            notes.append(msg)
        return

    with tempfile.TemporaryDirectory() as tmp:
        # SCRIPT goal. `new Function(src)` compiles without running, and a
        # function body rejects import/export exactly as the legacy loader does.
        checker = os.path.join(tmp, "compile-as-script.js")
        with open(checker, "w", encoding="utf-8") as f:
            f.write(
                "const GLib = imports.gi.GLib;\n"
                "for (const path of ARGV) {\n"
                "    const [, bytes] = GLib.file_get_contents(path);\n"
                "    new Function(new TextDecoder().decode(bytes));\n"
                "    print('script-goal ok: ' + path);\n"
                "}\n"
            )
        for name in (SHARED_LIB, LEGACY_ENTRY):
            r = subprocess.run(
                [gjs, checker, os.path.join(EXT_DIR, name)],
                capture_output=True,
                text=True,
            )
            if r.returncode != 0:
                fail(f"{name} does not compile under the script goal:\n{r.stderr.strip()}")
            else:
                notes.append(f"{name}: parses as a legacy (script) module")

        # MODULE goal. gjs reports a parse error before it resolves imports,
        # so an unresolvable `resource:///org/gnome/shell/...` (there is no
        # GNOME Shell here) is expected and fine; a SyntaxError is not.
        for name in (SHARED_LIB, MODERN_ENTRY):
            r = subprocess.run(
                [gjs, "-m", os.path.join(EXT_DIR, name)],
                capture_output=True,
                text=True,
            )
            out = r.stdout + r.stderr
            if "Failed to parse module" in out or "SyntaxError" in out:
                fail(f"{name} does not parse as an ES module:\n{out.strip()}")
            else:
                notes.append(f"{name}: parses as an ES module")


def main():
    require_gjs = "--require-gjs" in sys.argv[1:]
    for name in (MODERN_ENTRY, LEGACY_ENTRY, SHARED_LIB, "metadata.json"):
        if not os.path.exists(os.path.join(EXT_DIR, name)):
            fail(f"missing {os.path.join('contrib/gnome-shell', UUID, name)}")
    if errors:
        report()
        return 1
    check_metadata()
    check_shapes()
    gjs_check(require_gjs)
    return report()


def report():
    for n in notes:
        print(f"  {n}")
    if errors:
        print()
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        print(f"\n{len(errors)} problem(s) in contrib/gnome-shell/{UUID}", file=sys.stderr)
        return 1
    print(f"\ncontrib/gnome-shell/{UUID} is consistent. OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())

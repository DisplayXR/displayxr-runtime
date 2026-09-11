#!/usr/bin/env python3
"""Assert the CNSDK pins that must name the SAME LeiaInc/CNSDK release agree.

Three pins in the ecosystem name one CNSDK release, each taking a different
asset from it:

  1. versions.json -> "cnsdk_services"          the Android tablet bundle's
                                                device-service + headTracking-service APKs
  2. .github/workflows/build-android.yml
     -> env.CNSDK_TAG                           the runtime leia-variant APK's
                                                cnsdk-android-*.zip (loader shim + AAR)
  3. the CNSDK that the pinned leia_plugin
     release was built against                  displayxr-leia-plugin's own CNSDK_TAG

THIS SCRIPT CHECKS 1 == 2 ONLY, AND THAT IS DELIBERATE.

Pin 3 is already enforced, and enforced *better*, by the "Inspect APK contents"
step of build-android.yml: it reads the CNSDK version the bundled loader .so
reports, reads the cnsdk-android-<ver> string the bundled plug-in .so embeds,
and requires them equal -- provenance from the artifacts, not from two
hand-typed tags. Both pins matched their own config on v2.16.2/v2.16.3 and
still shipped a broken pair, which is exactly why that check reads the
binaries. Re-deriving pin 3 here by scraping another repo's workflow YAML over
the network would be a weaker duplicate of an artifact-based assertion, and
would make this repo's CI red for a file rename in a repo it does not own.

So: the artifact check covers 2 <-> 3. This script covers 1 <-> 2. Together
they close the triangle, each at the point where the evidence is strongest.

Why exactness (not a floor) matters at all -- the long-form version is the
comment block above CNSDK_TAG in build-android.yml: the CNSDK loader shim
refuses a plug-in built against a different apiVersion, leia_cnsdk_create
fails, the runtime falls back to the no-DP path, and every in-process app runs
in 2D with no error surfaced anywhere upstream.

Why a CI check and not just the bundle's own gates: verify-bundle.sh (loader ==
plug-in) and audit-device.sh (device-service libleiaCore-impl.so hash == the
bundle's) only fire once a bundle has been assembled, and a wrong-but-internally
-consistent pair passes every tag-shaped check and fails on hardware.

Exit codes: 0 = agree, or cnsdk_services is not pinned yet (clean skip).
            1 = drift, or the workflow lost its CNSDK_TAG.

Usage: python3 scripts/check_cnsdk_pins.py
"""

import json
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
VERSIONS_JSON = REPO_ROOT / "versions.json"
ANDROID_WORKFLOW = REPO_ROOT / ".github" / "workflows" / "build-android.yml"

# env: CNSDK_TAG: v0.10.68   (optionally quoted, optionally trailing comment)
CNSDK_TAG_RE = re.compile(
    r"^\s*CNSDK_TAG:\s*['\"]?([^'\"#\s]+)['\"]?\s*(?:#.*)?$", re.MULTILINE
)


def fail(msg):
    print(f"::error::{msg}")


def main():
    # --- pin 2: the runtime's own Android workflow -------------------------
    if not ANDROID_WORKFLOW.exists():
        fail(f"{ANDROID_WORKFLOW.relative_to(REPO_ROOT)} is missing.")
        return 1

    workflow_text = ANDROID_WORKFLOW.read_text(encoding="utf-8")
    matches = CNSDK_TAG_RE.findall(workflow_text)
    if not matches:
        fail(
            f"no CNSDK_TAG found in {ANDROID_WORKFLOW.relative_to(REPO_ROOT)}. "
            "The runtime's leia APK variant cannot be built without one; if it "
            "was renamed, rename it here too rather than dropping this guard."
        )
        return 1
    if len(set(matches)) > 1:
        fail(
            f"{ANDROID_WORKFLOW.relative_to(REPO_ROOT)} declares CNSDK_TAG more "
            f"than once with different values: {sorted(set(matches))}. One "
            "workflow, one CNSDK release."
        )
        return 1
    workflow_tag = matches[0]

    # --- pin 1: versions.json ---------------------------------------------
    if not VERSIONS_JSON.exists():
        fail("versions.json is missing.")
        return 1

    try:
        versions = json.loads(VERSIONS_JSON.read_text(encoding="utf-8"))
    except json.JSONDecodeError as exc:
        fail(f"versions.json is not valid JSON: {exc}")
        return 1

    services_tag = versions.get("cnsdk_services")
    if services_tag is None:
        # Not an error: the field is added by a separate change, and this repo
        # built Android APKs long before the tablet bundle pinned the services.
        print(
            "versions.json has no 'cnsdk_services' field yet — nothing to "
            "cross-check. Skipping.\n"
            f"  build-android.yml CNSDK_TAG = {workflow_tag}"
        )
        return 0

    # --- the assertion -----------------------------------------------------
    if services_tag != workflow_tag:
        fail(
            f"CNSDK PIN DRIFT: versions.json[cnsdk_services] = {services_tag} "
            f"but .github/workflows/build-android.yml CNSDK_TAG = {workflow_tag}."
        )
        fail(
            "These name the SAME release on LeiaInc/CNSDK (the bundle takes its "
            "service APKs, the runtime APK takes cnsdk-android-*.zip). This is "
            "NOT A FLOOR -- AN EXACT MATCH: a loader/plug-in apiVersion mismatch "
            "makes leia_cnsdk_create fail and every in-process app run in 2D, "
            "with no error surfaced upstream."
        )
        fail(
            "Fix: set both to the CNSDK_TAG that the pinned leia_plugin release "
            f"({versions.get('leia_plugin', '?')}) was built against, then let "
            "the leia-variant Android build's Inspect-APK step prove loader == "
            "plug-in from the artifacts."
        )
        return 1

    print(
        "CNSDK pins agree. ✓\n"
        f"  versions.json[cnsdk_services]        = {services_tag}\n"
        f"  build-android.yml CNSDK_TAG          = {workflow_tag}\n"
        "  (loader == plug-in for the same drop is asserted from the binaries "
        "by build-android.yml's Inspect-APK step.)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())

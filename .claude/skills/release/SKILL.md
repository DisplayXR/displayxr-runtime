---
name: release
description: Create a tagged release of the displayxr-runtime repo, monitor the Windows CI build, and publish the GitHub Release with the installer asset attached. Syntax /release <version-spec>, where version-spec is vX.Y.Z or patch/minor/major.
allowed-tools: Read, Grep, Glob, Bash, Agent, Edit, Write
---

# Release Skill

Creates a tagged release of the **displayxr-runtime** repo, monitors the Windows CI build, attaches the installer artifact to the GitHub Release, and reports.

## Scope

This skill releases **only** the runtime (this repo). The shell, demos, and extensions each have their own release flows:

| Component | Repo | Release flow |
|---|---|---|
| Runtime | `DisplayXR/displayxr-runtime` (this repo) | **This skill** |
| Shell | `DisplayXR/displayxr-shell-pvt` → `displayxr-shell-releases` | Shell repo's own publish pipeline; not driven from here |
| Extensions | `DisplayXR/displayxr-extensions` | Auto-synced from this repo's `src/external/openxr_includes/` on every push to main (`publish-extensions.yml`); no tag needed |
| Demos (e.g. `displayxr-demo-gaussiansplat`) | Each demo's own repo | Manual: bump installer/build-installer.bat → tag → build installer → `gh release create` in that repo |

## Syntax

```
/release                # ask user for version
/release <version-spec> # release runtime at this version

version-spec:
  vX.Y.Z                explicit
  patch|minor|major     auto-bump from latest v[0-9]+.[0-9]+.[0-9]+ tag
```

## Architecture

```
/release patch
  │   (bumps from latest v[0-9]+.[0-9]+.[0-9]+ tag)
  ├─ Pre-flight (clean tree, on main, tag not taken)
  ├─ Bump CMakeLists.txt VERSION
  ├─ Commit "Release vX.Y.Z" → push main → tag → push tag
  ├─ Monitor build-windows.yml run for this commit
  ├─ Download installer artifact from the build run
  ├─ gh release create with the installer attached + grouped notes
  └─ Report
```

CMakeLists.txt VERSION is always bumped to the new semver.

## CRITICAL: Launch Subagent

**You MUST use the Agent tool with `subagent_type="general-purpose"` to execute this workflow.**

### Argument parsing

Parse `[ARGUMENTS]` as a single token:

1. Zero tokens: ask the user for the version.
2. One token:
   - If it matches `vN.N.N` → use verbatim.
   - If it is `patch|minor|major` → auto-bump from latest tag.
   - Else → ask user.
3. Multiple tokens: ignore extras; they're a leftover from the old per-component skill design (now retired).

Resolve the new version:

- If version-spec is `vN.N.N`, use it verbatim. Set FULL_TAG = the same `vN.N.N`.
- If `patch|minor|major`:
  - Find the latest **canonical** tag: `git tag --sort=-v:refname | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -1`.
  - The strict `\.` pattern is intentional — it ignores oddballs like component-prefixed (`demo-x/v1.0.0`), pre-release (`v1.0.0-rc1`), or stale legacy lineages. The runtime carries one canonical lineage at a time.
  - Strip the leading `v`, split on `.`, bump the requested component, re-prepend `v`. Set FULL_TAG = the bumped value.
- If no canonical tag exists, start at `v1.0.0`.

### Subagent Prompt Template

Replace `[VERSION]` and `[FULL_TAG]` (both equal to `vX.Y.Z`).

```
Execute the DisplayXR runtime release workflow for [VERSION] (tag: [FULL_TAG]).

## Configuration
- Source repo: DisplayXR/displayxr-runtime (this is the public repo — there is no separate "publish" mirror)
- Workflow to monitor: .github/workflows/build-windows.yml (the artifact producer)
- publish-extensions.yml fires automatically on every push to main; it does not need monitoring as part of a tagged release (header sync is independent of tags)
- Shell, demos, displayxr-extensions are out of scope; each has its own flow

---

## PHASE 1: PRE-FLIGHT CHECKS

### Step 1.1: Verify clean state
Run: `git status --short`
- If dirty, STOP: "Working tree is not clean. Commit or stash changes first."

### Step 1.2: Verify on main branch
Run: `git branch --show-current`
- If not `main`, STOP: "Must be on main branch to release."

### Step 1.3: Pull origin/main + verify tag doesn't exist
- `git fetch origin && git pull --ff-only`
- `git tag -l "[FULL_TAG]"` — if non-empty, STOP: "Tag [FULL_TAG] already exists."
- `git ls-remote --tags origin "[FULL_TAG]"` — if non-empty, STOP (remote tag exists).

### Step 1.4: Previous tag for release notes
PREV_TAG = `git tag --sort=-v:refname | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -1`.
If empty, PREV_TAG=<empty> → use "Initial release" in notes.

---

## PHASE 2: UPDATE VERSION AND TAG

### Step 2.1: Bump CMakeLists.txt VERSION
Extract X.Y.Z from [VERSION]. Find the top-level CMakeLists.txt `VERSION X.Y.Z` line (typically line 7) and update it via Edit tool.

### Step 2.2: Commit and tag
```bash
git add CMakeLists.txt
git commit -m "Release [FULL_TAG]"
git tag [FULL_TAG]
git push origin main
git push origin [FULL_TAG]
```
Store the release commit SHA: `RELEASE_SHA=$(git rev-parse HEAD)`.

### Step 2.3: Branch protection note
The runtime repo's `main` is protected. If `git push origin main` fails with "Changes must be made through a pull request", retry with `--no-verify` is NOT acceptable — instead, surface the error to the user and ask whether to use admin override (the user has the authority; you do not). For tag pushes, branch protection does not apply.

---

## PHASE 3: MONITOR BUILD

### Step 3.1: Wait for build to register
`sleep 15`

### Step 3.2: Find the build run
```bash
gh run list --workflow build-windows.yml --limit 10 --json databaseId,status,headSha,event
```
Find the run with `headSha == $RELEASE_SHA` and event=push. Retry up to 6 times with 10s waits.

### Step 3.3: Watch build
`gh run watch $RUN_ID --interval 30 --exit-status` (timeout 1800000ms — Windows CI can take up to 30 min with test apps).

### Step 3.4: Check result
- All required jobs succeed → Phase 4
- Critical job (Runtime, Build) fails → Phase 6 (Rollback)
- Pre-existing-broken jobs (e.g. demo jobs that reference paths moved to standalone repos) fail but artifact still produced → continue to Phase 4 with a flag in the final report

---

## PHASE 4: CREATE GITHUB RELEASE

The build run produces two artifacts the release should attach:
- **Installer** — `DisplayXR-Installer` artifact (contains `DisplayXRSetup-X.Y.Z.BUILD.exe`).
- **Test apps bundle** — `TestApps-<run_number>` artifact (~17 MB folder containing all cube test apps). Forced on every tag push by the `DetectChanges` job (`ref_type == 'tag'`). Builders, OEM integrators, and field testers use this to validate the runtime against the public extension contracts without rebuilding from source.

Check whether the release already exists:

```bash
gh release view [FULL_TAG] --repo DisplayXR/displayxr-runtime 2>/dev/null
```

### Step 4.1: Download artifacts (always runs)
Regardless of whether the release exists, download both artifacts so we can verify or attach them:

```bash
# Installer artifact (matches DisplayXR-Installer + DisplayXR)
gh run download $RUN_ID --repo DisplayXR/displayxr-runtime --pattern "DisplayXR*" --dir _release_assets/

# Test apps bundle — separate pattern because TestApps-* doesn't match DisplayXR*
gh run download $RUN_ID --repo DisplayXR/displayxr-runtime --pattern "TestApps-*" --dir _release_assets/

INSTALLER=$(find _release_assets/ -name "DisplayXRSetup-*.exe" | head -1)
[ -z "$INSTALLER" ] && STOP: "Could not find DisplayXRSetup-*.exe in build artifacts."

# Zip the TestApps folder into a single asset. Pattern: DisplayXR-TestApps-<version>.zip.
TESTAPPS_DIR=$(find _release_assets/ -maxdepth 2 -type d -name "TestApps-*" | head -1)
TESTAPPS_ZIP=""
if [ -n "$TESTAPPS_DIR" ]; then
  TESTAPPS_ZIP="_release_assets/DisplayXR-TestApps-[VERSION].zip"  # [VERSION] without leading v
  (cd "$TESTAPPS_DIR" && zip -rq "../../$TESTAPPS_ZIP" .)
  ls -lh "$TESTAPPS_ZIP"
else
  echo "WARN: no TestApps-* artifact in run — test apps will not be attached. Check DetectChanges output."
fi
```

The TestApps bundle is a soft requirement: if `DetectChanges` mis-detected and didn't produce it, log a warning and continue with installer-only. Don't STOP the release.

### Step 4.2a: If release already exists
The workflow auto-created it (rare path — currently `build-windows.yml` does not auto-create a release, so this branch is unusual). Upload any missing assets:

```bash
gh release upload [FULL_TAG] --repo DisplayXR/displayxr-runtime --clobber "$INSTALLER" ${TESTAPPS_ZIP:+"$TESTAPPS_ZIP"}
```
Then go to Phase 5.

### Step 4.2b: If release does NOT exist
Create it with both assets:

```bash
# Generate release notes
NOTES=$(git log "$PREV_TAG".."[FULL_TAG]" --oneline --no-merges)
# Group commits by prefix (Feature, Fix, CI, Docs, etc.) — see PHASE 5 notes template

gh release create [FULL_TAG] \
  --repo DisplayXR/displayxr-runtime \
  --title "DisplayXR Runtime [FULL_TAG]" \
  --notes "<grouped notes here>" \
  "$INSTALLER" ${TESTAPPS_ZIP:+"$TESTAPPS_ZIP"}
```

---

## PHASE 5: VERIFY AND REPORT

```bash
gh release view [FULL_TAG] --repo DisplayXR/displayxr-runtime --json tagName,name,assets
```

Verify:
- Tag matches [FULL_TAG]
- Asset list contains `DisplayXRSetup-*.exe` with non-zero size
- Asset list contains `DisplayXR-TestApps-*.zip` with non-zero size (warn but don't fail if `DetectChanges` skipped the test-app build — flag in final report so the user can investigate)

### Step 5.1: Cleanup staging dir
Only after the verification above passes. If verification fails, leave the dir for inspection.
```bash
rm -rf _release_assets/
```
Phase 4.2a always downloads into this dir before any upload, so the `rm -rf` is always meaningful — safe to run unconditionally.

### Notes template
Group commits from `git log $PREV_TAG..[FULL_TAG] --oneline --no-merges` by prefix:
- **Highlights** — 1-3 line summary of the release's main change (manually written, not auto-grouped)
- **Features** — `feat:` / `feature:` prefixed
- **Fixes** — `fix:` prefixed
- **CI / Build** — `ci:` / `build:` / `cmake:`
- **Docs** — `docs:` prefixed
- **Other** — everything else

### Final report
```
Release [FULL_TAG] published successfully!

Build:     Windows CI run #RUN_ID — Runtime + cube test apps passed
           [list any pre-existing-broken jobs here, with note that they don't affect the artifact]
Release:   https://github.com/DisplayXR/displayxr-runtime/releases/tag/[FULL_TAG]
Assets:    DisplayXRSetup-X.Y.Z.BUILD.exe (~N MB)
           DisplayXR-TestApps-X.Y.Z.zip (~17 MB)  [or "skipped — DetectChanges did not produce TestApps-*"]
Commits:   N commits since $PREV_TAG

Notable changes:
  [grouped bullet summary]
```

STOP.

---

## PHASE 6: ROLLBACK (on critical BUILD failure only)

Only roll back if a CRITICAL job (Runtime, Build) failed. Pre-existing-broken jobs that produce no artifact change are NOT a rollback condition — flag in the report instead.

### Step 6.1: Delete tag (local + remote)
```bash
git tag -d [FULL_TAG]
git push --delete origin [FULL_TAG]
```

### Step 6.2: Revert the version-bump commit
```bash
git revert HEAD --no-edit
git push origin main
```
If main is protected, surface to the user.

### Step 6.3: Error summary + report
```bash
gh run view $RUN_ID --log-failed | tail -200
```
Report the error and that the tag + commit have been reverted. STOP.

---

## Examples

```
/release v1.2.1
    → explicit version
    → bumps CMakeLists VERSION to 1.2.1
    → tags v1.2.1, runs build-windows.yml
    → creates GH release with installer

/release patch
    → auto-bumps from latest v[0-9]+.[0-9]+.[0-9]+ tag
    → e.g. latest v1.2.0 → tag v1.2.1

/release minor
    → e.g. latest v1.2.5 → tag v1.3.0

/release major
    → e.g. latest v1.5.2 → tag v2.0.0
```

## Lineage / tag hygiene notes

- The repo has had multiple tag lineages over its lifetime (Monado-era v25.x, pre-1.0 v0.5.0, current v1.x). Stale lineages were cleaned on 2026-05-04. The auto-bump regex `^v[0-9]+\.[0-9]+\.[0-9]+$` is intentionally strict to prevent picking up reintroduced stragglers.
- If you need to release a candidate (`-rc1`, `-beta`, etc.), pass it explicitly — auto-bump will not pick those up by design.
- Demo and SR SDK pin tags (`demo-gaussiansplat/*`, `sr-sdk-*`) live in their own namespaces and are never picked up by this skill.

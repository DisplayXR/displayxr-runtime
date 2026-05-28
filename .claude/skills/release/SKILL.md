---
name: release
description: Create a tagged release of the displayxr-runtime repo, monitor the Windows + macOS CI builds, and publish the GitHub Release with the Windows installer + macOS .pkg attached. Syntax /release <version-spec>, where version-spec is vX.Y.Z or patch/minor/major.
allowed-tools: Read, Grep, Glob, Bash, Agent, Edit, Write
---

# Release Skill

Creates a tagged release of the **displayxr-runtime** repo, monitors the Windows + macOS CI builds, and publishes the GitHub Release with both platform installers attached (`DisplayXRSetup-*.exe` and `DisplayXR-Installer.pkg`) plus the test-apps bundle.

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
  ├─ Tag HEAD → push tag
  │
  │  (CI takes over here — build-windows.yml patches CMakeLists.txt
  │   VERSION from the tag at build time per PR #353, then both
  │   workflows attach their artifacts to the v* tag's release via
  │   softprops/action-gh-release@v2; first job to complete creates
  │   the release, others append.)
  │
  ├─ Monitor build-windows.yml + build-macos.yml for this commit
  ├─ gh release edit --title --notes (curated Highlights paragraph
  │   layered on top of the empty body the action created)
  └─ Report
```

The skill no longer touches CMakeLists.txt — CI is authoritative for
the source-tree VERSION (synced from `git describe --tags --abbrev=0`
on every Windows build per PR #353; macOS derives version directly
from `GITHUB_REF` in build-macos.yml). The previous flow's "Release
X.Y.Z" version-bump commit was both redundant (CI overrode it anyway)
and a drift vector if the bump was forgotten — that's exactly what
landed PR #353 (after the in-tree value pinned at 1.2.3 silently
drifted for three releases past v1.3.0 in displayxr-shell-pvt).

The skill also no longer downloads or re-uploads any artifacts — that
moved into the workflows themselves per #290.

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
- Workflows to monitor (both produce artifacts the release attaches):
  - `.github/workflows/build-windows.yml` — produces `DisplayXRSetup-*.exe` (artifact `DisplayXR-Installer`) + `TestApps-*` bundle
  - `.github/workflows/build-macos.yml` — produces `DisplayXR-Installer.pkg` (artifact `DisplayXR-Installer-macOS`) (post-#277)
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

## PHASE 2: TAG

### Step 2.1: Tag HEAD and push the tag
The skill no longer commits anything to `main` — CI patches
CMakeLists.txt VERSION from the tag at build time (PR #353). Just tag
the current `main` HEAD and push the tag.

```bash
git tag [FULL_TAG]
git push origin [FULL_TAG]
```

Store the tagged commit SHA for later monitoring: `RELEASE_SHA=$(git rev-parse [FULL_TAG]^{commit})`.

### Step 2.2: Branch protection note
Branch protection on `main` does not apply to tag pushes, so no
admin override is needed. (Earlier versions of this skill committed a
"Release vX.Y.Z" version-bump to `main`; that step is gone — see the
Architecture diagram.)

---

## PHASE 3: MONITOR BUILDS (Windows + macOS)

### Step 3.1: Wait for builds to register
`sleep 15`

### Step 3.2: Find both build runs
```bash
gh run list --workflow build-windows.yml --limit 10 --json databaseId,status,headSha,event,headBranch
gh run list --workflow build-macos.yml   --limit 10 --json databaseId,status,headSha,event,headBranch
```
For each workflow, find the run where `headSha == $RELEASE_SHA`, `event=push`, AND `headBranch == [FULL_TAG]` (i.e. the run fired by the tag push, not any other push that happens to share the SHA). Retry up to 6 times with 10s waits per workflow. Store `WIN_RUN_ID` and `MAC_RUN_ID`.

### Step 3.3: Watch both runs (in parallel)
Use a background-poll pattern so both watches run concurrently — total wall time is `max(Windows, macOS)`, not sum.

```bash
# Background Windows watch
gh run watch $WIN_RUN_ID --interval 30 --exit-status > /tmp/win.log 2>&1 &
WIN_PID=$!
# Background macOS watch
gh run watch $MAC_RUN_ID --interval 30 --exit-status > /tmp/mac.log 2>&1 &
MAC_PID=$!

wait $WIN_PID; WIN_RC=$?
wait $MAC_PID; MAC_RC=$?
```

Windows CI can take up to 30 min with test apps; macOS is faster (~10-15 min including the installer build). Timeout each at 1800000 ms.

### Step 3.4: Check both results
- Both runs all-required-jobs succeed → Phase 4
- Critical Windows job (`Runtime`, `Build`) fails AND macOS `Build` / `BuildInstaller` also fails → Phase 6 (Rollback) — no release was created
- Pre-existing-broken jobs (e.g. demo jobs that reference paths moved to standalone repos) fail but the artifact-producing job still succeeded → continue to Phase 4 with a flag in the final report
- One side fails but the other succeeded → the release exists (the green side's `softprops/action-gh-release` step ran) but is missing assets. STOP and ask the user whether to rerun the failed workflow or ship degraded; flag clearly. The release tag can be left in place while the rerun happens.

---

## PHASE 4: WRITE CURATED RELEASE NOTES

Since the CI workflows now attach artifacts to the release directly via
`softprops/action-gh-release@v2` (per #290), the release exists by the
time CI completes and already has the Windows installer, macOS .pkg,
and test-apps zip attached. The skill's job is now just to write the
curated Highlights paragraph on top of the auto-created (empty-body)
release.

### Step 4.1: Confirm the release exists
```bash
gh release view [FULL_TAG] --repo DisplayXR/displayxr-runtime --json tagName,assets --jq '{tag: .tagName, assets: [.assets[].name]}'
```
If the release does NOT exist at this point, both CI workflows
catastrophically failed before reaching their attach steps. Go to
Phase 6.

### Step 4.2: Generate + write release notes
Group commits from `git log $PREV_TAG..[FULL_TAG] --oneline --no-merges`
by prefix (see "Notes template" below). Write a 1-3 line Highlights
paragraph at the top describing what's notable for users.

```bash
NOTES=$(cat <<'NOTES_EOF'
## Highlights
<manually written 1-3 line summary>

## Features
<auto-grouped feat: commits>

## Fixes
<auto-grouped fix: commits>

## Docs
<auto-grouped docs: commits>

## CI / Build
<auto-grouped ci: / build: / cmake: commits>

## Other
<everything else>
NOTES_EOF
)

gh release edit [FULL_TAG] \
  --repo DisplayXR/displayxr-runtime \
  --title "DisplayXR Runtime [FULL_TAG]" \
  --notes "$NOTES"
```

`gh release edit --notes` overwrites whatever body the action created
(`softprops/action-gh-release@v2` with no `body`/`body_path` defaults
to an empty body, so there's nothing to merge — just overwrite).

---

## PHASE 5: VERIFY AND REPORT

```bash
gh release view [FULL_TAG] --repo DisplayXR/displayxr-runtime --json tagName,name,assets
```

Verify:
- Tag matches [FULL_TAG]
- Asset list contains `DisplayXRSetup-*.exe` with non-zero size (Windows installer, hard requirement)
- Asset list contains `DisplayXR-Installer-*.pkg` with non-zero size (macOS installer; warn but don't fail if the macOS run skipped it — flag in final report)
- Asset list contains `DisplayXR-TestApps-*.zip` with non-zero size (warn but don't fail if the BundleTestApps job skipped it — flag in final report)
- Title is `DisplayXR Runtime [FULL_TAG]`
- Body starts with `## Highlights`

No local staging dir to clean up — the CI does all artifact handling.

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

There's nothing else to revert — Phase 2 no longer commits anything to
`main`, so deleting the tag fully rolls back the release.

### Step 6.2: Error summary + report
```bash
gh run view $RUN_ID --log-failed | tail -200
```
Report the error and that the tag has been deleted. STOP.

---

## Examples

```
/release v1.2.1
    → explicit version
    → tags current main HEAD as v1.2.1 (no source-tree commit)
    → push v1.2.1 → CI patches CMakeLists VERSION → builds installer
    → workflows attach installer to the GH release

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

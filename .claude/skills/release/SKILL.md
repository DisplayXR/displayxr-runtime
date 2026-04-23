---
name: release
description: Create a tagged release, monitor CI build and publish pipeline, verify all public repos are updated. Use /release v1.0.0 for explicit version, or /release patch|minor|major for auto-bump.
allowed-tools: Read, Grep, Glob, Bash, Agent, Edit, Write
---

# Release Skill

Creates a tagged release of DisplayXR, monitors the CI build and publish pipeline, and verifies all public repos are updated correctly.

## Architecture

```
/release v1.0.0
  │
  ├─ Pre-flight checks (clean tree, on main, version valid)
  ├─ Update CMakeLists.txt version if needed
  ├─ Create tag + push
  ├─ Monitor build-windows.yml
  ├─ Monitor publish-public.yml
  │    ├─ displayxr-runtime: tag + release (no shell)
  │    └─ displayxr-shell-releases: tag + release (installer + shell exe)
  ├─ Monitor publish-extensions.yml
  ├─ Monitor publish-demo-*.yml (one per demo repo)
  │    └─ displayxr-demo-gaussiansplat: tag + release (gaussian_splatting_handle_vk_win.zip)
  ├─ Update release notes on public repos
  └─ Report
```

## CRITICAL: Launch Subagent

**You MUST use the Agent tool with `subagent_type="general-purpose"` to execute this workflow.**

### Parsing the Version Argument

Parse `[ARGUMENTS]` to determine version:

1. If argument matches `vN.N.N` → use as explicit version
2. If argument is `patch` → get latest tag, bump patch (v1.0.0 → v1.0.1)
3. If argument is `minor` → get latest tag, bump minor (v1.0.0 → v1.1.0)
4. If argument is `major` → get latest tag, bump major (v1.0.0 → v2.0.0)
5. If no argument → ask user what version

### Subagent Prompt Template

Replace `[VERSION]` with the resolved version (e.g., `v1.0.0`):

```
Execute the DisplayXR release workflow for version [VERSION].

## Configuration
- Private repo: DisplayXR/displayxr-runtime-pvt
- Public repos: DisplayXR/displayxr-runtime, DisplayXR/displayxr-shell-releases
- Extension repo: DisplayXR/displayxr-extensions
- Per-demo repos: DisplayXR/displayxr-demo-gaussiansplat (add more as demos are split per docs/guides/add-new-demo-repo.md)
- Workflows: build-windows.yml, publish-public.yml, publish-extensions.yml, publish-demo-gaussiansplat.yml (and any other publish-demo-*.yml)

---

## PHASE 1: PRE-FLIGHT CHECKS

### Step 1.1: Verify clean state
Run: `git status --short`
- If dirty, report and STOP: "Working tree is not clean. Commit or stash changes first."

### Step 1.2: Verify on main branch
Run: `git branch --show-current`
- If not `main`, report and STOP: "Must be on main branch to release."

### Step 1.3: Verify version doesn't already exist
Run: `git tag -l "[VERSION]"`
- If tag exists, report and STOP: "Tag [VERSION] already exists."

### Step 1.4: Get previous version for release notes
Run: `git tag --sort=-v:refname | grep '^v' | head -1`
Store as PREV_TAG.

---

## PHASE 2: UPDATE VERSION AND TAG

### Step 2.1: Update CMakeLists.txt version
Extract major.minor.patch from [VERSION] (strip the 'v' prefix).
Read CMakeLists.txt line with `VERSION X.Y.Z` and update it.
Use Edit tool to change it.

### Step 2.2: Commit version bump
```bash
git add CMakeLists.txt
git commit -m "$(cat <<'EOF'
Release [VERSION]
EOF
)"
```

### Step 2.3: Create tag and push
```bash
git tag [VERSION]
git push origin main
git push origin [VERSION]
```
Store the commit SHA: `git rev-parse HEAD`

---

## PHASE 3: MONITOR BUILD

### Step 3.1: Wait for build to register
Run: `sleep 15`

### Step 3.2: Find the build run
```bash
gh run list --limit 10 --json databaseId,status,headSha,displayTitle,event
```
Find the run matching your commit SHA with event=push.
Retry up to 6 times with 10s waits.

### Step 3.3: Watch build
Run: `gh run watch RUN_ID --interval 15` (timeout 600000ms)

### Step 3.4: Check result
Run: `gh run view RUN_ID --json status,conclusion`
- If success: continue to Phase 4
- If failure: Go to PHASE 6 (Rollback)

---

## PHASE 4: MONITOR PUBLISH PIPELINE

### Step 4.1: Find publish-public.yml run
```bash
gh run list --workflow publish-public.yml --limit 5 --json databaseId,status,headSha,conclusion
```
Find the run matching your commit SHA. Wait up to 5 minutes.

### Step 4.2: Watch publish
Run: `gh run watch PUBLISH_RUN_ID --interval 15` (timeout 600000ms)

### Step 4.3: Check publish result
- If failure: report error with logs, but DO NOT rollback (build succeeded)

---

## PHASE 5: VERIFY AND REPORT

### Step 5.1: Verify displayxr-runtime
```bash
gh release view [VERSION] --repo DisplayXR/displayxr-runtime --json tagName,name
```
- Verify tag exists and release was created

### Step 5.2: Verify displayxr-shell-releases
```bash
gh release view [VERSION] --repo DisplayXR/displayxr-shell-releases --json tagName,name,assets
```
- Verify tag exists, release created, installer and shell exe are attached

### Step 5.2b: Verify per-demo repos
For each `publish-demo-*.yml` workflow under `.github/workflows/`, check that the
corresponding `DisplayXR/displayxr-demo-<name>` repo has a release for this version
with its binary zip attached. Today that means:
```bash
gh release view [VERSION] --repo DisplayXR/displayxr-demo-gaussiansplat --json tagName,name,assets
```
- Verify tag exists, source was synced to `main`, and `gaussian_splatting_handle_vk_win-[VERSION].zip` is attached.
- As more demos are split off per `docs/guides/add-new-demo-repo.md`, extend this step to cover each.

### Step 5.3: Generate release notes
```bash
git log PREV_TAG..[VERSION] --oneline --no-merges
```
Group commits by prefix (Feature, Fix, CI, Docs, etc.)

### Step 5.4: Update release notes on public repos
Use `gh release edit` to update the release body on both repos with the generated notes.

### Step 5.5: Report
```
Release [VERSION] published successfully!

Build:
  - Windows CI: PASSED (run #RUN_ID)

Published to:
  - Runtime: https://github.com/DisplayXR/displayxr-runtime/releases/tag/[VERSION]
  - Shell + Installer: https://github.com/DisplayXR/displayxr-shell-releases/releases/tag/[VERSION]
  - Gaussian splat demo: https://github.com/DisplayXR/displayxr-demo-gaussiansplat/releases/tag/[VERSION]

Release notes:
  [generated notes summary]
```

STOP.

---

## PHASE 6: ROLLBACK (on build failure)

### Step 6.1: Delete tag
```bash
git tag -d [VERSION]
git push --delete origin [VERSION]
```

### Step 6.2: Revert version bump
```bash
git revert HEAD --no-edit
git push origin main
```

### Step 6.3: Get error logs
Run: `gh run view RUN_ID --log-failed | tail -200`

### Step 6.4: Report
```
Release [VERSION] FAILED — rolled back.

Error from build:
[error summary]

Tag and version bump have been reverted.
Fix the issue and try again with /release [VERSION]
```

STOP.
```

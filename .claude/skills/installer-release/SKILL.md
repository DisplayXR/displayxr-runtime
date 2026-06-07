---
name: installer-release
description: Cut a DisplayXR meta-installer bundle release. Installer doesn't release on tag push — it's workflow_dispatch-only on publish-bundle.yml. Skill confirms versions.json in-sync with runtime, fires the dispatch with the input tag, watches the build + GH Release land, and reports. Use when bundle is launch-ready, not on every component release.
---

# installer-release — meta-installer bundle release

## When to run this

When you decide the **whole stack** is launch-ready and you want
users to install the latest combination. The meta-installer is NOT
auto-cut on component releases (that would re-publish untested
bundles every few days). It's a deliberate "users should upgrade
now" event.

Preconditions you should be confident about before running:
- `displayxr-runtime/versions.json` is at the matrix you want users
  to install. The auto-bump flow keeps this current automatically;
  no manual edit needed.
- The matrix has been tested end-to-end somewhere (your dev box, a
  staging machine, etc.). The bundle build itself doesn't run
  hardware tests.

## Syntax

```
/installer-release vX.Y.Z          # explicit bundle tag
/installer-release patch           # auto-bump from latest bundle tag
/installer-release minor
/installer-release major
/installer-release vX.Y.Z --prerelease   # mark as prerelease
```

Bundle tag is independent of component tags. Common pattern: bundle
`vN.N.N` where N tracks marketing milestones (e.g. `v1.0.0` =
launch, `v1.1.0` = first post-launch feature drop).

## Runs from the runtime hub — no local installer checkout needed

This skill lives in `displayxr-runtime/.claude/skills/` (symlinked into
`~/.claude/skills/` via `scripts/link-dxr-skills.sh`), so it's invocable
from anywhere. The entire flow is `gh workflow run` + `gh api` + `gh run
view` against `DisplayXR/displayxr-installer` — there is NO step that
needs a local installer checkout. Run it from the runtime hub (or any
directory); the target repo is always addressed explicitly via `-R`.

## CRITICAL: Launch Subagent

Use the Agent tool with `subagent_type="general-purpose"`.

### Subagent prompt template
```
Run the installer-release skill at ~/.claude/skills/installer-release/SKILL.md
(canonical: displayxr-runtime/.claude/skills/installer-release/SKILL.md).
Bundle tag: [TAG]
Prerelease flag: [true|false]
Operate entirely via gh against DisplayXR/displayxr-installer — NO local
checkout needed.
Report the final state in the format defined in PHASE 5.
```

---

## PHASE 1: PRE-FLIGHT

### Step 1.1: Resolve version-spec
Get the latest bundle tag from the remote (no local checkout):
```bash
case "$ARG" in
  v[0-9]*.[0-9]*.[0-9]*)  TAG="$ARG" ;;
  patch|minor|major)      LATEST=$(gh api repos/DisplayXR/displayxr-installer/tags \
                                     --jq '[.[].name | select(test("^v[0-9]+\\.[0-9]+\\.[0-9]+$"))] | .[0]')
                          TAG=$(bump "$LATEST" "$ARG") ;;   # patch/minor/major bump of $LATEST
  *)                      echo "Bad version spec"; exit 1 ;;
esac
```

### Step 1.2: Confirm versions.json is in sync with runtime
This is the SAME assertion `publish-bundle.yml`'s
`assert-versions-in-sync` job runs. Catching drift locally before
firing the dispatch saves a 10-minute CI cycle if there's a
problem.
```bash
diff <(gh api repos/DisplayXR/displayxr-runtime/contents/versions.json   --jq '.content' | base64 -d) \
     <(gh api repos/DisplayXR/displayxr-installer/contents/versions.json --jq '.content' | base64 -d) \
  || { echo "versions.json drift detected. Investigate before tagging the bundle."; exit 1; }
```
If drift exists: the auto-mirror failed somewhere. Re-fire
`displayxr-runtime/.github/workflows/versions-bump.yml` (manual
`workflow_dispatch` with `field=runtime tag=<current runtime>`) and
re-run this skill.

### Step 1.3: Show the bundle composition the user is about to ship
```bash
echo "Bundle $TAG will pin:"
gh api repos/DisplayXR/displayxr-installer/contents/versions.json --jq '.content' \
  | base64 -d | jq -r 'to_entries | .[] | select(.key != "$schema") | "  \(.key): \(.value)"'
```
This is the moment to confirm the matrix is what you want. If a
component pin is stale (e.g. you wanted shell v1.4.1 but the auto-
bump only landed v1.4.0), CANCEL here — wait for the desired
component release first, then re-run.

---

## PHASE 2: MARKER COMMIT + FIRE THE DISPATCH

### Step 2.0: Push the empty release-marker commit
Same pattern as every other DisplayXR repo's release flow (`/release`,
`/dxr-release`, Unity/Unreal plugins): an **empty** "Release vX.Y.Z"
commit on `main` so the history shows an obvious release boundary.
Because the dispatch below runs `--ref main`, the workflow's
`GITHUB_SHA` — and therefore the tag `softprops/action-gh-release`
creates — lands on this marker commit.

```bash
WORK=$(mktemp -d)
gh repo clone DisplayXR/displayxr-installer "$WORK/repo" -- --quiet --depth 30
cd "$WORK/repo"
git commit --allow-empty -m "Release $TAG"
# Retry once if a versions.json mirror commit raced us (empty commit rebases trivially).
git push origin HEAD:main || (git pull --rebase origin main && git push origin HEAD:main)
cd - && rm -rf "$WORK"
```

The marker changes no files, so the pre-checked `versions.json` sync
assertion (Step 1.2 / `assert-versions-in-sync`) is unaffected.

### Step 2.1: Fire the dispatch
`publish-bundle.yml` is `workflow_dispatch`-only. Fire it via the gh
CLI with the tag input.

```bash
gh workflow run publish-bundle.yml \
  -R DisplayXR/displayxr-installer \
  --ref main \
  -f tag="$TAG" \
  -f prerelease="$PRERELEASE"
```

The workflow creates the actual tag itself during the release step
(`softprops/action-gh-release@v2` with `tag_name: ${{ inputs.tag }}`),
so no `git tag && git push` needed beforehand — only the Step 2.0
marker push.

---

## PHASE 3: WATCH THE BUILD

Three jobs in parallel: `assert-versions-in-sync` (the drift guard
we already pre-checked locally), `build-macos`, `build-windows`.
Then a fourth `release` job that downloads both artifacts and
publishes.

```bash
# Find the workflow_dispatch run we just fired
sleep 8
RUN_ID=$(gh run list -R DisplayXR/displayxr-installer \
          --workflow=publish-bundle.yml --event=workflow_dispatch \
          --limit=1 --json databaseId --jq '.[0].databaseId')

# Poll to completion (~15-25min)
while :; do
  S=$(gh run view "$RUN_ID" -R DisplayXR/displayxr-installer \
        --json status,conclusion --jq '.status + "/" + (.conclusion // "?")')
  echo "  bundle: $S"
  [[ "$S" == completed* ]] && break
  sleep 30
done
```

### Step 3.x: Branch on per-job outcome
- All four green → Phase 4
- `assert-versions-in-sync` fails → STOP. Drift detected post-pre-check
  (race: someone else pushed to runtime/main between our 1.3 check and
  the CI run). User decides whether to re-run.
- `build-macos` only fails → release was already created by the
  build-windows path's `softprops/action-gh-release@v2`. Re-run macOS
  manually to attach the `.pkg`. Flag.
- `build-windows` only fails → mirror situation. Re-run Windows.
- Both build jobs fail → no release. Investigate logs.

---

## PHASE 4: VERIFY THE RELEASE

```bash
gh release view "$TAG" -R DisplayXR/displayxr-installer \
  --json tagName,name,assets,prerelease
```

Confirm:
- Tag matches `$TAG`
- Assets contain `DisplayXRBundle-*.pkg` (macOS, non-zero size)
- Assets contain `DisplayXRBundle-*.exe` (Windows, non-zero size)
- Prerelease flag matches input

---

## PHASE 5: REPORT

```
Bundle $TAG published successfully!

Bundle composition:
  runtime:     vX.Y.Z
  shell:       vX.Y.Z
  leia_plugin: vX.Y.Z
  mcp_tools:   vX.Y.Z
  gauss_demo:  vX.Y.Z

Build:      CI run #RUN_ID  ← all 4 jobs green
Release:    https://github.com/DisplayXR/displayxr-installer/releases/tag/$TAG
Assets:     DisplayXRBundle-X.Y.Z.pkg (~N MB)
            DisplayXRBundle-X.Y.Z.exe (~N MB)
Prerelease: true/false

Source of truth verification:
  installer/versions.json == runtime/versions.json    ✓
  (assert-versions-in-sync job confirmed in CI)

Next: notify users. Update marketing pages, post to whatever channel
your release announcements live in. The dev orchestrator
(scripts/setup-displayxr.{sh,bat}) already serves this same matrix
to existing dev boxes — bundle users and dev users converge on the
same set of pins.
```

STOP.

---

## Notes

- Bundle releases are deliberate, not automatic. The auto-bump flow
  (versions.json across runtime + installer) is continuous; the
  bundle release is the punctuation that says "users go install
  this now."
- v1 ships unsigned per `project_meta_installer_284`. The skill
  does NOT codesign anything. If/when signing lands, it'll be a CI
  step inside publish-bundle.yml, not a skill responsibility.
- Don't bump component pins by hand in installer's versions.json —
  the file is auto-mirrored from runtime. If you find yourself
  wanting to, you're working around the system. Investigate why
  the auto-mirror isn't doing what you want.

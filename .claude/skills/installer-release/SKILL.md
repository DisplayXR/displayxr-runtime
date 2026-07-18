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
  # [.] not \\. — the escaped form only survives single quotes; re-quote it once and jq
  # rejects "\." as an invalid escape, silently yielding an empty LATEST.
  patch|minor|major)      LATEST=$(gh api repos/DisplayXR/displayxr-installer/tags \
                                     --jq '[.[].name | select(test("^v[0-9]+[.][0-9]+[.][0-9]+$"))] | .[0]')
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

## PHASE 4.5: CODE-SIGN THE BUNDLE .EXE (capability-gated)

The bundle wraps **already-signed** component installers (each signed at its
own release), so it only needs its own outer `.exe` signed — a post-hoc
**single-file** sign, no inner-binary handling and no rebuild. (signtool on a
finished NSIS `.exe` is safe/ordinary; only *rcedit* corrupts NSIS.) So unlike
components (which rebuild on the runner), the bundle uses the provider's
folder-sign workflow **`sign-artifact`** — the same primitive `sign-hook.sh`
wraps, dispatched inline here so it needs **no local hook file and no Windows
host** (runs from the Mac). Full contract + how to swap providers:
`docs/specs/runtime/release-signing.md`.

### Step 4.5.1: Resolve capability (OS-agnostic)
```bash
SIGN_REPO="${DXR_SIGN_REPO}"   # local env only; the public repo names no provider
if [ -n "$SIGN_REPO" ] && gh workflow view sign-artifact -R "$SIGN_REPO" >/dev/null 2>&1; then
  SIGNED=yes
else
  echo "⚠  BUNDLE UNSIGNED — DXR_SIGN_REPO unset in the env, or the provider is unreachable."
  echo "   The release ships the unsigned CI bundle. Set DXR_SIGN_REPO to a repo"
  echo "   implementing 'sign-artifact', or re-run from a box with provider access."
  SIGNED=no
fi
```

### Step 4.5.2: Download → dispatch `sign-artifact` → re-upload
```bash
if [ "$SIGNED" = yes ]; then
  D=$(mktemp -d)
  # startswith/endswith, NOT test("...\\.exe$"): that regex needs a \\ that survives only in
  # single quotes; one extra layer of double-quoting makes it \. , jq rejects it as an invalid
  # escape, and $( ) silently yields empty. (Hit for real on runtime v2.0.4.) Need a regex
  # here? Write [.] instead of \\. — it needs no escaping.
  EXE=$(gh release view "$TAG" -R DisplayXR/displayxr-installer --json assets \
         --jq '.assets[].name | select(startswith("DisplayXRBundle-") and endswith(".exe"))')
  [ -n "$EXE" ] || { echo "No DisplayXRBundle-*.exe on $TAG — cannot sign"; exit 1; }
  gh release download "$TAG" -R DisplayXR/displayxr-installer -p "$EXE" -D "$D/in"

  # Zip the finished .exe and hand it to the provider's folder-sign workflow.
  # portable zip: git-bash on Windows has no `zip` — fall back to PowerShell.
  if command -v zip >/dev/null; then ( cd "$D/in" && zip -qr "$D/unsigned.zip" . )
  else powershell -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$D/in")\*' -DestinationPath '$(cygpath -w "$D/unsigned.zip")' -Force"; fi
  TMP="sign-bundle-$(date +%s)-$$"
  gh release create "$TMP" -R "$SIGN_REPO" --prerelease --title "$TMP" \
     --notes "temp bundle-signing payload (auto-deleted)" "$D/unsigned.zip"
  SINCE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  gh workflow run sign-artifact -R "$SIGN_REPO" -f release_tag="$TMP"

  RID=""
  for _ in $(seq 1 20); do
    RID=$(gh run list -R "$SIGN_REPO" --workflow sign-artifact --event workflow_dispatch \
            --limit 8 --json databaseId,createdAt \
            --jq "[.[]|select(.createdAt>=\"$SINCE\")]|sort_by(.createdAt)|last|.databaseId // empty")
    [ -n "$RID" ] && break; sleep 4
  done
  if [ -n "$RID" ] && gh run watch "$RID" -R "$SIGN_REPO" --interval 15 --exit-status; then
    gh run download "$RID" -R "$SIGN_REPO" -n signed -D "$D/out"
    # portable unzip (git-bash on Windows has no `unzip`).
    if command -v unzip >/dev/null; then ( cd "$D/out" && unzip -qo signed.zip -d "$D/signed" 2>/dev/null || true )
    else powershell -NoProfile -Command "Expand-Archive -Path '$(cygpath -w "$D/out/signed.zip")' -DestinationPath '$(cygpath -w "$D/signed")' -Force"; fi
    SIGNED_EXE=$(ls "$D/signed/$EXE" 2>/dev/null || ls "$D/out"/**/"$EXE" 2>/dev/null | head -1)
    if [ -n "$SIGNED_EXE" ]; then
      gh release upload "$TAG" "$SIGNED_EXE" --clobber -R DisplayXR/displayxr-installer
      echo "Bundle .exe signed on the provider runner and re-uploaded."
    else
      echo "⚠ signed .exe not returned — leaving the unsigned CI bundle."; SIGNED=no
    fi
  else
    echo "⚠ sign-artifact run failed/absent — leaving the unsigned CI bundle."; SIGNED=no
  fi
  # Always clean up the temp signing release/tag on the provider.
  gh release delete "$TMP" -R "$SIGN_REPO" --yes --cleanup-tag >/dev/null 2>&1 || true
fi
```

Only the Windows `.exe` is covered; the macOS `.pkg` uses an Apple Developer ID
cert + notarization (separate track). Carry `SIGNED` into the report.

---

## PHASE 4.6: MIRROR THE SIGNED BUNDLE TO ONEDRIVE

`publish-bundle.yml` no longer uploads to OneDrive — it ran BEFORE this
skill's signing, so it always mirrored the UNSIGNED bundle (the very gap that
shipped an unsigned v2.0.5 to OneDrive). The upload now lives in
`upload-bundle-onedrive.yml` (workflow_dispatch), which downloads the release's
`DisplayXRBundle-*.exe` — signed by Phase 4.5 above — and pushes it. Fire it
here so OneDrive matches the signed GitHub release.

Only run this when `SIGNED = yes` (never mirror an unsigned bundle). The
workflow itself also fail-closes: it re-verifies the downloaded `.exe` carries
a signature and errors out if not, so a mis-fire can't push an unsigned bundle.

```bash
if [ "$SIGNED" = yes ]; then
  gh workflow run upload-bundle-onedrive.yml -R DisplayXR/displayxr-installer -f tag="$TAG"
  # Locate + watch the dispatched run so the report reflects the real outcome.
  SINCE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  OD_RUN=""
  for _ in $(seq 1 20); do
    OD_RUN=$(gh run list -R DisplayXR/displayxr-installer --workflow upload-bundle-onedrive.yml \
              --event workflow_dispatch --limit 8 --json databaseId,createdAt \
              --jq "[.[]|select(.createdAt>=\"$SINCE\")]|sort_by(.createdAt)|last|.databaseId // empty")
    [ -n "$OD_RUN" ] && break; sleep 5
  done
  if [ -n "$OD_RUN" ] && gh run watch "$OD_RUN" -R DisplayXR/displayxr-installer --interval 20 --exit-status; then
    ONEDRIVE=synced
  else
    ONEDRIVE=failed   # token expiry / secret unset / dispatch miss — flag in report, release still good
    echo "⚠ OneDrive mirror did not complete — the SIGNED GitHub release is authoritative."
    echo "  Retry: gh workflow run upload-bundle-onedrive.yml -R DisplayXR/displayxr-installer -f tag=$TAG"
  fi
else
  ONEDRIVE=skipped     # bundle unsigned → never mirror
fi
```

If the mirror fails on `invalid_grant`, the OneDrive token expired — refresh the
`RCLONE_CONFIG` secret (see the comment in `upload-bundle-onedrive.yml`) and
re-run the one-liner above; no need to re-cut the bundle.

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
Signing:    [signed → "bundle .exe signed on the provider runner (run $RID) and re-uploaded"] | [none → "⚠ bundle UNSIGNED — provider unreachable; set DXR_SIGN_REPO or ship unsigned"]  (macOS .pkg: unsigned, TODO)
OneDrive:   [synced → "signed bundle mirrored to OneDrive (upload-bundle-onedrive.yml run $OD_RUN)"] | [failed → "⚠ OneDrive mirror failed — signed GitHub release is authoritative; retry the dispatch"] | [skipped → "not mirrored (bundle unsigned)"]

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
- Bundle `.exe` signing is a **post-hoc skill step** (Phase 4.5), not a CI
  step — the signing key can't live on GitHub-hosted CI, and the provider
  runner isn't reachable from `displayxr-installer`'s CI. The skill dispatches
  the provider's `sign-artifact` workflow on the repo named by the
  `DXR_SIGN_REPO` **local env var** (this public repo hardcodes no provider path)
  to sign the finished `.exe`, then re-uploads —
  OS-agnostic, no local hook file. The wrapped component installers are already
  signed at their own releases, so no inner-binary handling is needed. Unset env
  → the release ships unsigned (gate fails, flagged in the report);
  swap providers by pointing `DXR_SIGN_REPO` elsewhere. Full contract:
  `docs/specs/runtime/release-signing.md`. macOS `.pkg` signing (Apple
  Developer ID + notarization) is still TODO.
- Don't bump component pins by hand in installer's versions.json —
  the file is auto-mirrored from runtime. If you find yourself
  wanting to, you're working around the system. Investigate why
  the auto-mirror isn't doing what you want.

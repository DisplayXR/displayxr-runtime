---
name: dxr-release
description: Tag-and-publish a release for any DisplayXR sibling component (shell, leia-plugin, mcp, gauss & modelviewer & mediaplayer demos) FROM the displayxr-runtime hub. Takes an explicit component + version — clones the target repo to a temp dir, tags HEAD, watches the repo's CI, watches the dispatched versions-bump.yml on displayxr-runtime, reports the bump + installer-mirror outcome. NOT for displayxr-runtime itself (use /release) and NOT for the bundle (use /installer-release).
---

# dxr-release — component release driven from the runtime hub

## Why this is parameterized (not cwd-detecting)

This skill lives in `displayxr-runtime/.claude/skills/` and is symlinked
into `~/.claude/skills/` (via `scripts/link-dxr-skills.sh`), so it's
invocable from anywhere. The canonical use is: you're working in the
`displayxr-runtime` hub and want to release a sibling component WITHOUT
switching repos. So the skill takes the target component as an argument
and operates on a fresh temp clone — it does NOT rely on the current
working directory being the component repo.

Every sibling repo's CI ends with a `DispatchVersionsBump`-style step
that fires a `repository_dispatch` at `displayxr-runtime/versions-bump.yml`,
which (a) bumps the matching `versions.json` field, (b) mirrors the file
to `displayxr-installer/versions.json` via the publish-bot. This skill
keeps you in the loop on that whole flow from one command.

Spec: [`docs/specs/runtime/versions-json-autobump.md`](../../docs/specs/runtime/versions-json-autobump.md).

## Syntax

```
/dxr-release <component> <version-spec>

  <component>     shell | leia-plugin (leia) | mcp | gauss (demo-gaussiansplat) | modelviewer (demo-modelviewer) | mediaplayer (demo-mediaplayer)
  <version-spec>  vX.Y.Z  |  patch  |  minor  |  major
```

Examples:
```
/dxr-release mcp v0.3.4
/dxr-release leia-plugin patch
/dxr-release shell minor
/dxr-release gauss v1.4.4
```

If no component is given, **STOP** and ask which component. Do not guess
from cwd — that's the old cwd-detecting behavior this skill replaced.

## Component → config map

| Component arg | Repo | versions.json field | CI workflow | Release lands on |
|---|---|---|---|---|
| `shell` | `DisplayXR/displayxr-shell-pvt` | `shell` | `publish-shell-releases.yml` | `displayxr-shell-releases` |
| `leia-plugin` / `leia` | `DisplayXR/displayxr-leia-plugin` | `leia_plugin` | `build-windows.yml` | same repo |
| `mcp` | `DisplayXR/displayxr-mcp` | `mcp_tools` | `build.yml` | same repo |
| `gauss` / `demo-gaussiansplat` | `DisplayXR/displayxr-demo-gaussiansplat` | `gauss_demo` | `build-windows.yml` | same repo |
| `modelviewer` / `demo-modelviewer` | `DisplayXR/displayxr-demo-modelviewer` | `modelviewer_demo` | `build-windows.yml` | same repo |
| `mediaplayer` / `demo-mediaplayer` | `DisplayXR/displayxr-demo-mediaplayer` | `mediaplayer_demo` | `build-windows.yml` | same repo |

`runtime` → tell the user to use `/release` (in-repo). `installer` →
tell them to use `/installer-release`.

## CRITICAL: Launch Subagent

**Use the Agent tool with `subagent_type="general-purpose"`.** The
subagent runs the clone + tag + multi-poll without blocking the main
thread.

### Subagent prompt template
```
Run the dxr-release skill at ~/.claude/skills/dxr-release/SKILL.md
(canonical: displayxr-runtime/.claude/skills/dxr-release/SKILL.md).
Component: [COMPONENT_ARG]   → resolve via the component→config map.
Version-spec: [VERSION_ARG]
Operate on a temp clone of the target repo (do NOT assume cwd is it).
Report the final state in the format defined in PHASE 6.
```

---

## PHASE 1: RESOLVE + PRE-FLIGHT

### Step 1.1: Resolve component → config
```bash
case "$COMPONENT" in
  shell)                         REPO=DisplayXR/displayxr-shell-pvt;          FIELD=shell;       WORKFLOW=publish-shell-releases.yml; REL_REPO=DisplayXR/displayxr-shell-releases ;;
  leia|leia-plugin)              REPO=DisplayXR/displayxr-leia-plugin;        FIELD=leia_plugin; WORKFLOW=build-windows.yml;           REL_REPO=DisplayXR/displayxr-leia-plugin ;;
  mcp)                           REPO=DisplayXR/displayxr-mcp;                FIELD=mcp_tools;   WORKFLOW=build.yml;                   REL_REPO=DisplayXR/displayxr-mcp ;;
  gauss|demo-gaussiansplat)      REPO=DisplayXR/displayxr-demo-gaussiansplat; FIELD=gauss_demo; WORKFLOW=build-windows.yml;           REL_REPO=DisplayXR/displayxr-demo-gaussiansplat ;;
  modelviewer|demo-modelviewer)  REPO=DisplayXR/displayxr-demo-modelviewer;   FIELD=modelviewer_demo; WORKFLOW=build-windows.yml;      REL_REPO=DisplayXR/displayxr-demo-modelviewer ;;
  mediaplayer|demo-mediaplayer)  REPO=DisplayXR/displayxr-demo-mediaplayer;   FIELD=mediaplayer_demo; WORKFLOW=build-windows.yml;      REL_REPO=DisplayXR/displayxr-demo-mediaplayer ;;
  runtime)                       echo "Use /release (in-repo) for the runtime."; exit 1 ;;
  installer)                     echo "Use /installer-release for the bundle.";  exit 1 ;;
  *)                             echo "Unknown component '$COMPONENT'. One of: shell, leia-plugin, mcp, gauss, modelviewer, mediaplayer."; exit 1 ;;
esac
echo "repo=$REPO field=$FIELD workflow=$WORKFLOW"
```

### Step 1.2: Clone the target repo to a temp dir
The hub does NOT keep sibling checkouts; clone fresh each release.
```bash
WORK=$(mktemp -d)
gh repo clone "$REPO" "$WORK/repo" -- --quiet
cd "$WORK/repo"
```

### Step 1.3: Resolve version-spec
- `vX.Y.Z` literal → validate `^v[0-9]+\.[0-9]+\.[0-9]+$`, use as-is.
- `patch`/`minor`/`major` → compute from
  `git tag --sort=-creatordate | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -1`.

### Step 1.4: Pre-flight on the clone
```bash
git fetch origin --tags --quiet
git rev-parse "$NEW_TAG" 2>/dev/null && { echo "Tag $NEW_TAG already exists on $REPO"; exit 1; }
PREV_TAG=$(git tag --sort=-creatordate | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -1)
# Clone defaults to the default branch (main) — no branch check needed.
```

---

## PHASE 2: MARKER COMMIT + TAG

Create an **empty** "Release vX.Y.Z" marker commit on the sibling's
`main` and tag THAT commit — same pattern as `/release` on the runtime
and the Unity/Unreal plugin repos, so every repo's history shows an
obvious release boundary (which release got which commits). Empty
commit = no version content = no drift vector.

```bash
git commit --allow-empty -m "Release $NEW_TAG"
# Retry once if main moved underneath us (the empty commit rebases trivially).
git push origin HEAD:main || (git pull --rebase origin main && git push origin HEAD:main)
git tag -a "$NEW_TAG" -m "$NEW_TAG

Commits since $PREV_TAG:
$(git log --oneline --no-merges "$PREV_TAG..HEAD" 2>/dev/null | head -20)"
git push origin "$NEW_TAG"
```

Notes:
- The marker push fires the sibling's regular main-push CI alongside
  the tag-triggered release run — a harmless (if slightly wasteful)
  duplicate. Siblings can adopt the runtime's empty-diff →
  docs_only=true `DetectChanges` short-circuit to make it ~free.
- If the sibling's ruleset rejects the direct push to main, fall back
  to tagging HEAD directly and flag it in the Phase 6 report.

The temp clone can be deleted after the tag is pushed — the rest of the
flow polls GitHub via `gh api`, no local checkout needed. Keep it until
Phase 6 only if you want `git log` for the report.

---

## PHASE 3: WATCH THE REPO'S CI

### Step 3.1: Find the tag's CI run
```bash
for i in $(seq 1 12); do
  RUN_ID=$(gh run list -R "$REPO" --workflow="$WORKFLOW" --branch="$NEW_TAG" \
            --limit=1 --json databaseId --jq '.[0].databaseId // empty')
  [ -n "$RUN_ID" ] && break
  sleep 10
done
[ -z "$RUN_ID" ] && { echo "No CI run found for $NEW_TAG on $REPO"; exit 1; }
```

### Step 3.2: Poll to completion
Typical wall-clock: leia ~20min, mcp ~5min, shell ~15min, gauss ~25min.
```bash
while :; do
  S=$(gh run view "$RUN_ID" -R "$REPO" --json status,conclusion \
        --jq '.status + "/" + (.conclusion // "?")')
  echo "  ci: $S"
  [[ "$S" == completed* ]] && break
  sleep 30
done
CI_CONC="${S#completed/}"
```

### Step 3.3: Branch on outcome
- `success` → Phase 4
- else → STOP, report failed jobs via `gh run view "$RUN_ID" -R "$REPO" --log-failed`. No rollback — tags are sticky; user retries with a new tag.

---

## PHASE 4: WATCH THE DISPATCHED versions-bump RUN

```bash
BUMP_RUN=""
for i in $(seq 1 12); do
  BUMP_RUN=$(gh run list -R DisplayXR/displayxr-runtime \
              --workflow=versions-bump.yml --event=repository_dispatch \
              --limit=3 --created=">$(date -u -v-15M +%Y-%m-%dT%H:%M:%SZ)" \
              --json databaseId --jq '.[0].databaseId // empty')
  [ -n "$BUMP_RUN" ] && break
  sleep 15
done
while :; do
  S=$(gh run view "$BUMP_RUN" -R DisplayXR/displayxr-runtime --json status,conclusion \
        --jq '.status + "/" + (.conclusion // "?")')
  echo "  bump: $S"
  [[ "$S" == completed* ]] && break
  sleep 15
done
BUMP_CONC="${S#completed/}"
```

`success` for `leia-plugin` is ambiguous (ABI gate may have passed-and-bumped
OR failed-and-skipped — both exit 0). Disambiguate in Step 5.1. `failure`
means the bot push failed (bypass misconfig or push race) — recommend a
manual `workflow_dispatch` on `versions-bump.yml` with `field=$FIELD tag=$NEW_TAG`.

---

## PHASE 5: VERIFY SYNC

### Step 5.1: Confirm runtime/main has the new pin
```bash
PINNED=$(gh api repos/DisplayXR/displayxr-runtime/contents/versions.json \
           --jq '.content' | base64 -d | jq -r ".${FIELD}")
if [ "$PINNED" = "$NEW_TAG" ]; then
  echo "✓ versions.json[$FIELD] = $NEW_TAG on runtime/main"
elif [ "$FIELD" = "leia_plugin" ]; then
  ISSUE=$(gh issue list --repo DisplayXR/displayxr-leia-plugin \
            --state open --label abi-mismatch --search "$NEW_TAG" \
            --json number,url --jq '.[0]')
  echo "ABI gate skipped the bump. Tracking issue: $ISSUE"
else
  echo "Bump did not land — versions.json[$FIELD] = $PINNED, expected $NEW_TAG"
fi
```

### Step 5.2: Confirm installer mirror landed (uncached Contents API)
```bash
diff <(gh api repos/DisplayXR/displayxr-runtime/contents/versions.json   --jq '.content' | base64 -d) \
     <(gh api repos/DisplayXR/displayxr-installer/contents/versions.json --jq '.content' | base64 -d) \
  && echo "✓ installer mirror matches runtime" \
  || echo "✗ installer mirror drifted — check the Mirror step in run $BUMP_RUN"
```

### Step 5.3: Capture SHAs + clean up the temp clone
```bash
RT_BUMP_SHA=$(gh api repos/DisplayXR/displayxr-runtime/commits/main --jq '.sha[0:8]')
IN_MIRROR_SHA=$(gh api repos/DisplayXR/displayxr-installer/commits/main --jq '.sha[0:8]')
rm -rf "$WORK"
```

---

## PHASE 6: REPORT

```
Release $NEW_TAG published successfully!

Component:   $COMPONENT  ($REPO)
CI:          run $RUN_ID — $CI_CONC
Release:     https://github.com/$REL_REPO/releases/tag/$NEW_TAG

Auto-bump:
  versions.json[$FIELD] = $NEW_TAG   via $RT_BUMP_SHA on displayxr-runtime/main
  versions.json mirror              via $IN_MIRROR_SHA on displayxr-installer/main
  [OR for leia ABI miss: "ABI gate skipped the bump — runtime expects a
   different plugin ABI than this leia tag reports. Tracking issue: $ISSUE.
   Rebuild leia against the current runtime headers, tag again; the next
   dispatch clears the path."]

Commits since $PREV_TAG: N
  [first 5 commit oneliners]
```

STOP. Sibling repos use GH's auto-generated release notes; if the user
wants curated notes they run `gh release edit` themselves (different
cadence from the runtime, where curated notes are the norm).

---

## Notes

- **Why a temp clone, not the cwd:** the runtime hub doesn't keep sibling
  checkouts, and the agent is launched in the runtime repo. Cloning fresh
  is the cleanest way to tag a sibling without polluting the hub or
  assuming a particular local layout. The clone is ~seconds for these
  repos and discarded in Step 5.3.
- **publish-bot prereq:** the `displayxr-publish-bot` GitHub App must be
  installed on `displayxr-runtime` + `displayxr-installer` (Contents:write).
  Confirmed for all repos as of 2026-05-29.
- **New sibling repo joining the family?** Add a row to the component→config
  map above, add a `versions.json` field on runtime, add the
  `DispatchVersionsBump` step to the new repo's CI per
  `docs/specs/runtime/versions-json-autobump.md` §"Sibling-side snippets".
- Tags are sticky. Deleting a tag also deletes its GH Release. Prefer
  fixing forward with a patch release.

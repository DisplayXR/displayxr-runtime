---
name: dxr-release
description: Tag-and-publish a release for any DisplayXR sibling component (shell, leia-plugin, mcp, gauss & modelviewer & mediaplayer & avatar & earthview demos, unity plugin) FROM the displayxr-runtime hub. Takes an explicit component + version — clones the target repo to a temp dir, tags HEAD, watches the repo's CI, watches the dispatched versions-bump.yml on displayxr-runtime, reports the bump + installer-mirror outcome. Unity is special-cased: its prebuilt displayxr_unity.dll is signed via the sign-artifact folder hook and re-injected into the .tgz + upm branch (no versions-bump). NOT for displayxr-runtime itself (use /release) and NOT for the bundle (use /installer-release).
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

  <component>     shell | leia-plugin (leia) | mcp | gauss (demo-gaussiansplat) | modelviewer (demo-modelviewer) | mediaplayer (demo-mediaplayer) | avatar (demo-avatar) | earthview (demo-earthview) | unity
  <version-spec>  vX.Y.Z  |  patch  |  minor  |  major
```

Examples:
```
/dxr-release mcp v0.3.4
/dxr-release leia-plugin patch
/dxr-release shell minor
/dxr-release gauss v1.4.4
/dxr-release unity v1.25.0
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
| `avatar` / `demo-avatar` | `DisplayXR/displayxr-demo-avatar` | `avatar_demo` | `build-windows.yml` | same repo |
| `earthview` / `demo-earthview` | `DisplayXR/displayxr-demo-earthview` | `earthview_demo` | `build-macos.yml` | same repo |
| `unity` | `DisplayXR/displayxr-unity` | *(none — not pinned)* | `build-native.yml` | same repo |

**Unity is special** — it ships a prebuilt `displayxr_unity.dll` (not an
installer) in a UPM `.tgz` release asset **and** on the `upm` git branch, has
**no** `versions.json` field (installed via UPM directly), and signs via the
provider's **`sign-artifact`** folder hook (send DLL → sign → re-inject), not
`build-signed-release.yml`. See the Unity branch in Phase 3.5. The macOS
`.bundle` stays unsigned (separate Apple track).

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
  avatar|demo-avatar)            REPO=DisplayXR/displayxr-demo-avatar;        FIELD=avatar_demo;      WORKFLOW=build-windows.yml;      REL_REPO=DisplayXR/displayxr-demo-avatar ;;
  earthview|demo-earthview)      REPO=DisplayXR/displayxr-demo-earthview;     FIELD=earthview_demo;   WORKFLOW=build-macos.yml;       REL_REPO=DisplayXR/displayxr-demo-earthview ;;
  unity)                         REPO=DisplayXR/displayxr-unity;              FIELD="";               WORKFLOW=build-native.yml;      REL_REPO=DisplayXR/displayxr-unity ;;
  runtime)                       echo "Use /release (in-repo) for the runtime."; exit 1 ;;
  installer)                     echo "Use /installer-release for the bundle.";  exit 1 ;;
  *)                             echo "Unknown component '$COMPONENT'. One of: shell, leia-plugin, mcp, gauss, modelviewer, mediaplayer, avatar, earthview, unity."; exit 1 ;;
esac
# FIELD="" (unity) → no versions.json entry; skip the Phase 4 versions-bump watch.
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
SIGN_ONLY=0
if git rev-parse "$NEW_TAG" >/dev/null 2>&1; then
  # unity: an existing tag means the release was ALREADY cut — commonly by a
  # direct `v*` tag push on the displayxr-unity repo (its build-native.yml then
  # publishes the .tgz + upm UNSIGNED, since CI holds no cert). Don't error;
  # switch to SIGN-ONLY: skip Phase 2 (marker+tag) and Phase 3 (CI watch) and go
  # straight to Step 3.5.0 to sign + re-inject the DLL into the existing release.
  # Makes `/dxr-release unity vX.Y.Z` idempotent regardless of WHERE it was cut.
  # (For every other component an existing tag stays a hard error — no post-hoc
  # sign path.)
  if [ "$COMPONENT" = unity ]; then
    SIGN_ONLY=1; echo "unity $NEW_TAG already released — SIGN-ONLY mode (sign + re-inject the existing release)."
  else
    echo "Tag $NEW_TAG already exists on $REPO"; exit 1
  fi
fi
PREV_TAG=$(git tag --sort=-creatordate | grep -E '^v[0-9]+\.[0-9]+\.[0-9]+$' | head -1)
# Clone defaults to the default branch (main) — no branch check needed.
```

---

## PHASE 2: MARKER COMMIT + TAG

**Skip this entire phase when `SIGN_ONLY=1`** (unity, release already cut) — the
tag + release already exist; jump straight to Phase 3.5.

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

**Skip this entire phase when `SIGN_ONLY=1`** (unity, release already cut) — its
CI already ran and published the release; go straight to Phase 3.5.

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
- `success` → Phase 3.5
- else → STOP, report failed jobs via `gh run view "$RUN_ID" -R "$REPO" --log-failed`. No rollback — tags are sticky; user retries with a new tag.

---

## PHASE 3.5: CODE-SIGN THE COMPONENT INSTALLER (capability-gated)

Same model as the runtime's `/release` skill: GitHub-hosted CI builds the
component **unsigned** (contributor PRs/pushes stay unsigned by design; no
secret lives in these public repos). A signed release is produced by the
**self-hosted signing runner** — it rebuilds the tagged commit with the
box-local EV signer (full chain: inner binaries → installer `.exe` → the NSIS
uninstaller) and returns the signed installer as an artifact this skill uploads
over the CI asset. **This box needs no Windows toolchain and holds no secret**
— it only dispatches the runner and re-uploads, so `/dxr-release` runs from any
OS. If the runner is unreachable, the release ships the unsigned CI installer
(signing never gates publishing).

This matters most for **leia-plugin** — its vendor plug-in DLL is in the load
path of every app that uses that display, so Smart App Control blocks it
unsigned. Demos with Windows installers are next; `mcp` ships DLLs too.

The per-component build recipe lives in the runner workflow
(`build-signed-release.yml` on the provider repo named by the `DXR_SIGN_REPO`
**local env var** — this public repo names no provider path) — a single source of
truth — so this skill only names the component + ref, not build commands. Set
`DXR_SIGN_REPO` in your env to sign (unset → unsigned); point it elsewhere to swap
signers. Contract: `docs/specs/runtime/release-signing.md`.

### Step 3.5.0: Unity — sign the prebuilt DLL via `sign-artifact`, then re-inject

Unity is NOT an installer: it ships a prebuilt **`displayxr_unity.dll`** in both
the UPM **`.tgz`** release asset and the **`upm`** git branch (CI produced both
UNSIGNED). There's no NSIS chain to rebuild, so we don't use
`build-signed-release.yml`. Instead **send just the DLL to the provider's
`sign-artifact` folder hook** (no rebuild, no PAT, no local toolchain — the same
primitive `sign-hook.sh` / `/installer-release` use) and re-inject the signed DLL
into both channels. Signing never gates publishing: any failure leaves the
unsigned CI release and is flagged in the report. If `COMPONENT=unity`, run this
block and then **skip the rest of Phase 3.5 and all of Phase 4** (Unity has no
`versions.json` field) — go straight to the Phase 6 report.

This block runs identically whether the release was **cut by this skill** (fresh
tag) or **cut directly on the displayxr-unity repo** (`SIGN_ONLY=1`) — either way
the `.tgz` asset + `upm` branch already exist, and we just sign + re-inject. So
signing a directly-released Unity version = re-run `/dxr-release unity <that
version>`; it detects the existing release and signs it in place.

```bash
if [ "$COMPONENT" = unity ]; then
  SIGN_REPO="${DXR_SIGN_REPO}"   # local env only; unset -> unsigned (public repo names no provider)
  VER="${NEW_TAG#v}"
  TGZ="com.displayxr.unity-${VER}.tgz"
  DLL_REL="Runtime/Plugins/Windows/x64/displayxr_unity.dll"
  UNITY_SIGNED=no

  if ! gh workflow view sign-artifact -R "$SIGN_REPO" >/dev/null 2>&1; then
    echo "⚠ SIGNING SKIPPED for unity — no access to the signing runner ($SIGN_REPO). Ships unsigned."
  else
    D=$(mktemp -d)
    gh release download "$NEW_TAG" -R "$REPO" -p "$TGZ" -D "$D"       # the just-released .tgz
    mkdir -p "$D/x"; tar xzf "$D/$TGZ" -C "$D/x"
    PKGDIR=$(ls -d "$D"/x/com.displayxr.unity-* | head -1)
    DLL="$PKGDIR/$DLL_REL"
    if [ ! -f "$DLL" ]; then
      echo "⚠ $DLL_REL not found in $TGZ — ships unsigned."
    else
      mkdir -p "$D/in"; cp "$DLL" "$D/in/"                            # fold ONLY the DLL into a sign folder
      # portable zip: git-bash on Windows has no `zip` — fall back to PowerShell.
      if command -v zip >/dev/null; then ( cd "$D/in" && zip -qr "$D/unsigned.zip" . )
      else powershell -NoProfile -Command "Compress-Archive -Path '$(cygpath -w "$D/in")\*' -DestinationPath '$(cygpath -w "$D/unsigned.zip")' -Force"; fi
      TMP="sign-unity-$(date +%s)-$$"
      gh release create "$TMP" -R "$SIGN_REPO" --prerelease --title "$TMP" \
         --notes "temp unity-signing payload (auto-deleted)" "$D/unsigned.zip"
      SINCE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
      gh workflow run sign-artifact -R "$SIGN_REPO" -f release_tag="$TMP"
      RID=""
      for _ in $(seq 1 20); do
        RID=$(gh run list -R "$SIGN_REPO" --workflow sign-artifact --event workflow_dispatch \
                --limit 8 --json databaseId,createdAt \
                --jq "[.[]|select(.createdAt>=\"$SINCE\")]|sort_by(.createdAt)|last|.databaseId // empty")
        [ -n "$RID" ] && break; sleep 4
      done
      SIGNED_DLL=""
      if [ -n "$RID" ] && gh run watch "$RID" -R "$SIGN_REPO" --interval 15 --exit-status; then
        gh run download "$RID" -R "$SIGN_REPO" -n signed -D "$D/out"
        # portable unzip (git-bash on Windows has no `unzip`).
        if command -v unzip >/dev/null; then ( cd "$D/out" && unzip -qo signed.zip -d "$D/signed" 2>/dev/null || true )
        else powershell -NoProfile -Command "Expand-Archive -Path '$(cygpath -w "$D/out/signed.zip")' -DestinationPath '$(cygpath -w "$D/signed")' -Force"; fi
        SIGNED_DLL=$(ls "$D/signed/displayxr_unity.dll" 2>/dev/null | head -1)
      fi
      gh release delete "$TMP" -R "$SIGN_REPO" --yes --cleanup-tag >/dev/null 2>&1 || true

      if [ -z "$SIGNED_DLL" ]; then
        echo "⚠ sign-artifact did not return a signed DLL — ships unsigned."
      else
        # Channel 1 — repack the .tgz with the signed DLL, re-upload over the asset.
        cp "$SIGNED_DLL" "$DLL"
        ( cd "$D/x" && tar czf "$D/$TGZ" "$(basename "$PKGDIR")" )
        gh release upload "$NEW_TAG" "$D/$TGZ" --clobber -R "$REPO"
        # Channel 2 — put the signed DLL on the `upm` branch + move its version tag.
        # Uses the Phase-1 clone (cwd = the repo). CI already force-pushed `upm`
        # with the unsigned DLL; we layer the signed one on top and force-push.
        git fetch origin upm --quiet && git checkout -B upm origin/upm
        cp "$SIGNED_DLL" "$DLL_REL"; git add -f "$DLL_REL"
        git commit -q -m "Sign displayxr_unity.dll for ${NEW_TAG} (Leia EV)" || echo "(upm already signed)"
        git push -f origin upm
        git tag -f "upm/${NEW_TAG}" && git push -f origin "upm/${NEW_TAG}"
        UNITY_SIGNED=yes
        echo "✅ unity: signed displayxr_unity.dll re-injected into the .tgz asset + upm branch (Valid/Leia)."
      fi
    fi
    rm -rf "$D"
  fi
  # Verify (optional, if on Windows): Get-AuthenticodeSignature on the re-uploaded DLL.
  # Unity has no versions.json field → SKIP Steps 3.5.1–3.5.3 and Phase 4; go to Phase 6.
fi
```

### Step 3.5.1: Resolve signing capability (OS-agnostic)
*(installer components — skipped for unity, handled in Step 3.5.0)*
The capability is *"can this box dispatch the signing runner?"* — no Windows
host, no local secret.

```bash
SIGN_REPO="${DXR_SIGN_REPO}"   # local env only; the public repo names no provider
if [ -n "$SIGN_REPO" ] && gh workflow view build-signed-release.yml -R "$SIGN_REPO" >/dev/null 2>&1; then
  SIGNED=yes
else
  echo "⚠  SIGNING SKIPPED for $COMPONENT — DXR_SIGN_REPO unset in the env, or the runner is unreachable."
  echo "   Release ships the UNSIGNED CI installer. Re-run /dxr-release $COMPONENT $NEW_TAG"
  echo "   from a box whose gh auth can dispatch that workflow."
  SIGNED=no   # continue — do not fail the release
fi
```

**earthview signs its WINDOWS installer (not its macOS `.pkg`).** earthview ships
BOTH a `DisplayXREarthViewSetup-*.exe` (Windows, from `build-windows.yml`) and a
macOS `.pkg` (from `build-macos.yml` — the workflow this skill watches in Phase 3).
The Windows installer IS signable — it has an `earthview` component in
`build-signed-release.yml` (cesium-native + ezvcpkg + OpenXR-loader build on the
runner) — so it goes through the normal Step 3.5.1–3.5.3 flow. Only the macOS
`.pkg` stays unsigned (needs Apple Developer ID + `productsign` on a Mac — a
separate flow); the fail-closed verify only inspects the `.exe`, so the unsigned
`.pkg` never blocks. **No `SIGNED=no` override** — earthview signs like any other
Windows installer.

**earthview race guard:** Phase 3 waits on `build-macos.yml`, so
`build-windows.yml` (which attaches the UNSIGNED `.exe` via softprops) may still
be running when we sign. In Step 3.5.3, before replacing the asset, wait for the
tag's Windows CI run to finish so its upload can't clobber the signed one:
```bash
if [ "$COMPONENT" = earthview ] || [ "$COMPONENT" = demo-earthview ]; then
  WIN_RUN=$(gh run list -R "$REPO" --workflow build-windows.yml --branch "$NEW_TAG" \
              --limit 1 --json databaseId --jq '.[0].databaseId // empty')
  [ -n "$WIN_RUN" ] && gh run watch "$WIN_RUN" -R "$REPO" --interval 20 --exit-status 2>/dev/null \
     || echo "(earthview: no/failed build-windows run for $NEW_TAG — proceeding; the signed --clobber upload is last-writer)"
fi
```

### Step 3.5.2: Normalize the component name for the workflow
The workflow expects canonical names (`runtime|leia-plugin|mcp|gauss|modelviewer|mediaplayer|avatar|earthview`).
Map the skill's aliases:
```bash
case "$COMPONENT" in
  leia|leia-plugin)              COMP=leia-plugin ;;
  gauss|demo-gaussiansplat)      COMP=gauss ;;
  modelviewer|demo-modelviewer)  COMP=modelviewer ;;
  avatar|demo-avatar)            COMP=avatar ;;
  mediaplayer|demo-mediaplayer)  COMP=mediaplayer ;;
  earthview|demo-earthview)      COMP=earthview ;;
  mcp)                           COMP=mcp ;;
  *)                             COMP="$COMPONENT" ;;
esac
```

### Step 3.5.3: Dispatch the runner build, wait, fetch + replace the asset (only if SIGNED=yes)

```bash
if [ "$SIGNED" = yes ]; then
  SINCE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
  gh workflow run build-signed-release.yml -R "$SIGN_REPO" \
     -f component="$COMP" -f repo="$REPO" -f ref="$NEW_TAG"

  SIGN_RUN=""
  for _ in $(seq 1 20); do
    SIGN_RUN=$(gh run list -R "$SIGN_REPO" --workflow build-signed-release.yml \
                --event workflow_dispatch --limit 8 --json databaseId,createdAt \
                --jq "[.[]|select(.createdAt>=\"$SINCE\")]|sort_by(.createdAt)|last|.databaseId // empty")
    [ -n "$SIGN_RUN" ] && break; sleep 5
  done
  [ -n "$SIGN_RUN" ] || { echo "Could not locate the signing run — ship unsigned"; SIGNED=no; }
fi

if [ "$SIGNED" = yes ]; then
  gh run watch "$SIGN_RUN" -R "$SIGN_REPO" --interval 30 --exit-status \
    || { echo "Signing run failed — ship unsigned CI asset, flag in report"; SIGNED=no; }
fi

if [ "$SIGNED" = yes ]; then
  rm -rf _signed && gh run download "$SIGN_RUN" -R "$SIGN_REPO" -n signed-installer -D _signed
  SIGNED_EXE=$(ls _signed/*Setup-*.exe 2>/dev/null | head -1)
  [ -n "$SIGNED_EXE" ] || { echo "No signed installer in the artifact — ship unsigned"; SIGNED=no; }
fi

if [ "$SIGNED" = yes ]; then
  # The runner already fail-closed-verified Status=Valid AND signer=Leia.
  CI_EXE=$(gh release view "$NEW_TAG" -R "$REPO" --json assets \
             --jq '.assets[].name | select(test("Setup-.*\\.exe$"))')
  [ -n "$CI_EXE" ] && gh release delete-asset "$NEW_TAG" "$CI_EXE" --yes -R "$REPO"
  gh release upload "$NEW_TAG" "$SIGNED_EXE" --clobber -R "$REPO"
fi
```

No local checkout, no local build, no Windows toolchain — the temp clone from
Phase 1 is only used for the tag; signing happens entirely on the runner.

---

## PHASE 4: WATCH THE DISPATCHED versions-bump RUN

**Skip this phase entirely when `FIELD` is empty** (e.g. `unity`, which has no
`versions.json` entry and dispatches no bump) — go straight to Phase 6:
```bash
[ -z "$FIELD" ] && { echo "No versions.json field for $COMPONENT — no bump to watch; skipping to report."; SKIP_BUMP=1; }
```

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
Signing:     [signed → "installer built + signed on the signing runner (full chain incl. uninstaller, run $SIGN_RUN), re-uploaded over the CI asset"]
             [none   → "⚠ UNSIGNED — signing runner unreachable / earthview macOS .pkg; ships the unsigned CI asset"]

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

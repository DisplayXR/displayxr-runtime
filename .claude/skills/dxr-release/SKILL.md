---
name: dxr-release
description: Tag-and-publish a release for any DisplayXR sibling component (shell, leia-plugin, mcp, browser, gauss & modelviewer & mediaplayer & avatar & earthview demos, unity plugin) FROM the displayxr-runtime hub. Takes an explicit component + version — clones the target repo to a temp dir, tags HEAD, watches the repo's CI, watches the dispatched versions-bump.yml on displayxr-runtime, reports the bump + installer-mirror outcome. Unity is special-cased: its prebuilt displayxr_unity.dll is signed via the sign-artifact folder hook and re-injected into the .tgz + upm branch (no versions-bump). NOT for displayxr-runtime itself (use /release) and NOT for the bundle (use /installer-release).
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

  <component>     shell | leia-plugin (leia) | mcp | browser | gauss (demo-gaussiansplat) | modelviewer (demo-modelviewer) | mediaplayer (demo-mediaplayer) | avatar (demo-avatar) | earthview (demo-earthview) | unity
  <version-spec>  vX.Y.Z  |  patch  |  minor  |  major        (browser: preview-X.Y.Z — see "Browser is special")
```

Examples:
```
/dxr-release mcp v0.3.4
/dxr-release leia-plugin patch
/dxr-release shell minor
/dxr-release gauss v1.4.4
/dxr-release unity v1.25.0
/dxr-release browser preview-0.1.24
/dxr-release browser patch
```

If no component is given, **STOP** and ask which component. Do not guess
from cwd — that's the old cwd-detecting behavior this skill replaced.

## Component → config map

| Component arg | Repo | versions.json field | CI workflow | Release lands on |
|---|---|---|---|---|
| `shell` | `DisplayXR/displayxr-shell-pvt` | `shell` | `publish-shell-releases.yml` | `displayxr-shell-releases` |
| `browser` | `DisplayXR/displayxr-browser-pvt` | `browser` | `publish-browser-releases.yml` | `displayxr-browser` (public, keeps its name) |
| `leia-plugin` / `leia` | `DisplayXR/displayxr-leia-plugin` | `leia_plugin` | `build-windows.yml` | same repo |
| `mcp` | `DisplayXR/displayxr-mcp` | `mcp_tools` | `build.yml` | same repo |
| `gauss` / `demo-gaussiansplat` | `DisplayXR/displayxr-demo-gaussiansplat` | `gauss_demo` | `build-windows.yml` | same repo |
| `modelviewer` / `demo-modelviewer` | `DisplayXR/displayxr-demo-modelviewer` | `modelviewer_demo` | `build-windows.yml` | same repo |
| `mediaplayer` / `demo-mediaplayer` | `DisplayXR/displayxr-demo-mediaplayer` | `mediaplayer_demo` | `build-windows.yml` | same repo |
| `avatar` / `demo-avatar` | `DisplayXR/displayxr-demo-avatar` | `avatar_demo` | `build-windows.yml` | same repo |
| `earthview` / `demo-earthview` | `DisplayXR/displayxr-demo-earthview` | `earthview_demo` | `build-macos.yml` | same repo |
| `unity` | `DisplayXR/displayxr-unity` | *(none — not pinned)* | `build-native.yml` | same repo |

**Browser is special** — it is the second private→public split after the shell
(source, patch series, build lanes and the Android keystore live in the PRIVATE
`displayxr-browser-pvt`; releases + assets + user-facing issues stay on the PUBLIC
`displayxr-browser`, which **keeps its name** because `versions.json[browser]`,
`install-android-bundle.sh --links` and tester install URLs all resolve against it).
Four consequences for this skill:
- **Tag shape is `preview-X.Y.Z`, not `vX.Y.Z`.** `versions-bump.yml` has a per-field
  tag-shape carve-out for `browser`; never "normalise" it to `v`. `patch`/`minor`/`major`
  compute from the newest `preview-*` tag (Step 1.3).
- **CI workflow = `publish-browser-releases.yml`** (tag-triggered, mirrors
  `publish-shell-releases.yml`). NOT `pipeline.yml` / `build-box.yml` — those are
  manual `workflow_dispatch` build lanes and never fire on a tag.
- **THE TAG TRIGGER WORKS, BUT ONLY IF A BUILD RAN AT THE COMMIT YOU TAG. Arrange that
  before you tag.** The publish resolves which build to ship by matching a successful
  `build-box*.yml` run against the tagged commit, and the build lanes take their source
  as a `patch_ref` **input** — so a build dispatched `--ref main` with
  `patch_ref=<something else>` does **not** match, on purpose. The release procedure is
  therefore: dispatch BOTH lanes with `--ref` at the commit you are about to tag, let
  them finish, then tag. Proven on `preview-0.1.26`: both lanes dispatched at
  `60382687b3ab`, the tag run then resolved `android=headsha windows=headsha` with no
  fallback and no manual dispatch. Skip that ordering and you get the `preview-0.1.25`
  outcome — the tag run fails at *Resolve the build runs for this commit* and you publish
  by hand:
  `gh workflow run publish-browser-releases.yml -R "$REPO" --ref main -f tag="$NEW_TAG"
  -f android_run_id=<id> -f windows_run_id=<id>`.
  **Never "fix" a non-matching tag by pointing the resolver at the newest build** — the
  artifact now carries a provenance stamp the publish asserts against the tag, and an
  unstamped artifact chosen by recency is refused precisely because nothing ties it to
  the tag.
- **THE UPDATE FEED IS NOT UPDATED BY THIS FLOW — a green release is NOT a shipped
  release.** `feed/feed.json` in the **public** repo is published to
  `https://updates.displayxr.org`, and that URL is compiled into every browser already
  installed. Only the older `pipeline.yml` promote path writes it;
  `publish-browser-releases.yml` does not. So a release cut through this skill is
  invisible to every existing user until the feed moves. This is not hypothetical: as of
  2026-09-04 the feed still advertises `0.1.23` (last commit 2026-08-28) while `0.1.24`,
  `0.1.25` and `0.1.26` have all shipped. **Do not report a browser release as complete
  on the strength of the GitHub release alone** — check the feed and say plainly if it is
  stale:
  ```bash
  FEED=$(curl -s https://updates.displayxr.org/feed.json | jq -r '.latest.version')
  echo "update feed advertises $FEED; this release is ${NEW_TAG#preview-}"
  [ "$FEED" = "${NEW_TAG#preview-}" ] || echo "FEED IS STALE — existing installs will NOT be offered this release."
  ```
- **Signing moves INTO the publish workflow, so Phase 3.5 is skipped — do not
  "fix" this by adding browser to the Phase 3.5 dispatch.** The browser is signed;
  it is just signed one step earlier than every other component. Its Windows
  installer goes through the *same* EV signing runner dispatch as everything else,
  but the publish lane fires it, not this skill — because the lane must also sign
  the **Android APK**, and that uses `apksigner` with a keystore rather than the
  hardware dongle the EV chain needs. One component, two signing mechanisms, one
  place to run them: the lane. By the time Phase 3 goes green the assets are
  already signed, so a hub-side re-sign would rebuild and overwrite a good asset.
- **Asset ops target `$REL_REPO`** (`displayxr-browser`), same trap as shell. The
  `versions-bump` dispatch fires from the pvt publish workflow with a
  `displayxr-publish-bot` token scoped to `displayxr-runtime`.
The browser also stays **opt-in** in the orchestrator (`--with browser`) — a release
through this skill must never promote it to a default install.

**Unity is special** — it ships a prebuilt `displayxr_unity.dll` (not an
installer) in a UPM `.tgz` release asset **and** on the `upm` git branch, has
**no** `versions.json` field (installed via UPM directly), and signs via the
provider's **`sign-artifact`** folder hook (send DLL → sign → re-inject), not
`build-signed-release.yml`. See the Unity branch in Phase 3.5. The macOS
`.bundle` stays unsigned (separate Apple track).

**Unity's version of record is in-tree (`package.json` `version`)** — not
`versions.json`, not the tag alone. It's what the website's org-sync generator
reads for `/platform-support` and what UPM consumers resolve. So the unity
release path (Phase 2) commits a **real** version bump as its marker, and the
sign block reconciles `main` when a release was cut by a direct tag; a website
`org-changed` dispatch refreshes the site (no `versions.json` bump fires for
unity). Consequence of getting this wrong: the site shows the previous version
even though the `.tgz`/`upm` carry the new one.

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
  browser)                       REPO=DisplayXR/displayxr-browser-pvt;        FIELD=browser;     WORKFLOW=publish-browser-releases.yml; REL_REPO=DisplayXR/displayxr-browser ;;
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
  *)                             echo "Unknown component '$COMPONENT'. One of: shell, leia-plugin, mcp, browser, gauss, modelviewer, mediaplayer, avatar, earthview, unity."; exit 1 ;;
esac
# FIELD="" (unity) → no versions.json entry; skip the Phase 4 versions-bump watch.
# Tag shape per component: browser tags are `preview-X.Y.Z` (versions-bump.yml carve-out);
# everything else is `vX.Y.Z`. Every later regex/derivation goes through TAG_PREFIX.
case "$COMPONENT" in browser) TAG_PREFIX=preview- ;; *) TAG_PREFIX=v ;; esac
TAG_RE="^${TAG_PREFIX}[0-9]+\.[0-9]+\.[0-9]+$"
echo "repo=$REPO field=$FIELD workflow=$WORKFLOW rel_repo=$REL_REPO tag_prefix=$TAG_PREFIX"
```

### Step 1.2: Clone the target repo to a temp dir
The hub does NOT keep sibling checkouts; clone fresh each release.
```bash
WORK=$(mktemp -d)
gh repo clone "$REPO" "$WORK/repo" -- --quiet
cd "$WORK/repo"
```

### Step 1.3: Resolve version-spec
- Literal tag → validate against `$TAG_RE` (`^v[0-9]+\.[0-9]+\.[0-9]+$`, or
  `^preview-[0-9]+\.[0-9]+\.[0-9]+$` for browser), use as-is. A `vX.Y.Z` given for
  browser (or `preview-` for anything else) is a hard error, not a rewrite.
- `patch`/`minor`/`major` → compute from
  `git tag --sort=-creatordate | grep -E "$TAG_RE" | head -1`, keeping `$TAG_PREFIX`.

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
PREV_TAG=$(git tag --sort=-creatordate | grep -E "$TAG_RE" | head -1)
# Clone defaults to the default branch (main) — no branch check needed.
```

---

## PHASE 2: MARKER COMMIT + TAG

**Skip this entire phase when `SIGN_ONLY=1`** (unity, release already cut) — the
tag + release already exist; jump straight to Phase 3.5.

Create a "Release vX.Y.Z" marker commit on the sibling's `main` and tag
THAT commit — same pattern as `/release` on the runtime, so every repo's
history shows an obvious release boundary (which release got which
commits). For most components the marker is **empty** (no version content
= no drift vector) — their version lives in `versions.json` (bumped by the
dispatch) or is derived from the tag at build time.

**Unity is the exception — its marker MUST bump the in-tree version.**
Unlike every other component, Unity's version of record is
`package.json`'s `version` field: it's what the website's org-sync
generator reads for the `/platform-support` dashboard and what UPM
consumers resolve. `build-native.yml` patches the version into the `.tgz`
asset and the `upm` branch **from the tag**, but never commits that bump
back to `main` — so an empty marker leaves `main` (and therefore the
website) showing the *previous* version. So for unity the marker is a
**real** commit that bumps `package.json` (and prepends a `CHANGELOG.md`
stub) to the release version, tagged as the release. This is not drift —
the in-tree version IS unity's source of truth.

```bash
if [ "$COMPONENT" = unity ]; then
  VER="${NEW_TAG#v}"
  jq --arg v "$VER" '.version = $v' package.json > package.json.tmp && mv package.json.tmp package.json
  if [ -f CHANGELOG.md ]; then
    # Insert a stub entry before the first "## [" heading (keeps the file title),
    # seeded with the commits since the previous tag — curate afterwards if wanted.
    ENTRY="## [$VER] - $(date +%F)
$(git log --oneline --no-merges "$PREV_TAG..HEAD" 2>/dev/null | sed 's/^/- /')
"
    awk -v e="$ENTRY" 'BEGIN{d=0} /^## \[/&&!d{print e; d=1} {print} END{if(!d)print e}' CHANGELOG.md > CHANGELOG.md.tmp && mv CHANGELOG.md.tmp CHANGELOG.md
    git add CHANGELOG.md
  fi
  git add package.json
  git commit -m "Release $NEW_TAG"                    # real bump commit = the marker
else
  git commit --allow-empty -m "Release $NEW_TAG"      # empty marker (version lives elsewhere)
fi
# Retry once if main moved underneath us (empty commit rebases trivially; the
# unity bump may need a re-apply — `git pull --rebase` handles both).
git push origin HEAD:main || (git pull --rebase origin main && git push origin HEAD:main)
git tag -a "$NEW_TAG" -m "$NEW_TAG

Commits since $PREV_TAG:
$(git log --oneline --no-merges "$PREV_TAG..HEAD" 2>/dev/null | head -20)"
git push origin "$NEW_TAG"
```

For unity, `build-native.yml`'s "Verify package.json version matches tag"
step then becomes a no-op (main already matches) instead of a silent
working-tree-only patch.

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

**Skip this entire phase for `browser`** — signing already happened, one step
earlier. Its publish workflow dispatches the same EV signing runner this phase
would (Windows installer) *and* signs the Android APK with `apksigner` + a
keystore, which the dongle-bound EV chain cannot do. Both must run in the lane,
so both do. The assets are signed by the time Phase 3 goes green; re-dispatching
here would rebuild and clobber them. Verify instead of re-signing:
```bash
if [ "$COMPONENT" = browser ]; then
  gh release view "$NEW_TAG" -R "$REL_REPO" --json assets --jq '.assets[].name'   # expect DisplayXR-Browser-Preview-Setup-X.Y.Z.exe (+ APKs)
  SIGNED=in-ci; SKIP_SIGN=1
fi
```
and go to Phase 3.6.

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

  # ── Reconcile main's in-tree version ──────────────────────────────────────
  # Runs for BOTH paths. On the fresh-tag path Phase 2 already bumped main, so
  # this is a no-op. On the SIGN_ONLY path (release cut by a DIRECT `v*` tag on
  # displayxr-unity — Phase 2 skipped) main's package.json was never bumped and
  # no release marker exists, so the website's /platform-support shows the OLD
  # version even though the .tgz/upm branch carry the new one (build-native.yml
  # patches those from the tag but never commits back to main). Push a forward
  # bump + marker so main is authoritative for the org-sync generator.
  git fetch origin main --quiet 2>/dev/null || true
  MAIN_VER=$(git show origin/main:package.json 2>/dev/null | jq -r .version 2>/dev/null)
  if [ -n "$MAIN_VER" ] && [ "$MAIN_VER" != "$VER" ]; then
    echo "main package.json=$MAIN_VER but tag=$VER — pushing forward bump + marker."
    git checkout -B main origin/main --quiet
    jq --arg v "$VER" '.version = $v' package.json > package.json.tmp && mv package.json.tmp package.json
    git add package.json
    git commit -m "Release $NEW_TAG (post-tag version reconcile)"
    git push origin HEAD:main || (git pull --rebase origin main && git push origin HEAD:main)
    echo "✓ main package.json bumped to $VER (CHANGELOG left for manual curation on this path)."
    UNITY_MAIN_RECONCILED=yes   # surface in the Phase 6 report
  fi

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
      # NOTE: name this SIGN_TAG, never TMP. `TMP` is an exported env var on
      # git-bash/Windows (Go's os.TempDir() reads %TMP%), so assigning to it
      # repoints every child process's temp dir at a relative path — and
      # `gh run download` below then dies with "error initializing temporary
      # file: open <cwd>\sign-unity-...\gh-artifact.zip: The system cannot
      # find the path specified", leaving the release silently unsigned.
      # Windows-only (macOS/Linux Go reads TMPDIR), so it never repros on mac.
      SIGN_TAG="sign-unity-$(date +%s)-$$"
      gh release create "$SIGN_TAG" -R "$SIGN_REPO" --prerelease --title "$SIGN_TAG" \
         --notes "temp unity-signing payload (auto-deleted)" "$D/unsigned.zip"
      SINCE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
      gh workflow run sign-artifact -R "$SIGN_REPO" -f release_tag="$SIGN_TAG"
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
        # Never parse `ls` (an -F alias appends `*` and the suffixed path silently fails
        # downstream — bit installer-release on bundle v2.0.15). Test the path directly.
        SIGNED_DLL="$D/signed/displayxr_unity.dll"; [ -f "$SIGNED_DLL" ] || SIGNED_DLL=""
      fi
      gh release delete "$SIGN_TAG" -R "$SIGN_REPO" --yes --cleanup-tag >/dev/null 2>&1 || true

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
  # Refresh the website now — unity fires no versions-bump, so nothing else kicks
  # the site's org-sync. Best-effort; the daily cron catches it otherwise. (Needs
  # a gh token with access to displayxr-website; the direct-push flow already has it.)
  gh api repos/DisplayXR/displayxr-website/dispatches -f event_type=org-changed >/dev/null 2>&1 \
    && echo "✓ fired org-changed at displayxr-website (regenerates the unity version on /platform-support)" \
    || echo "(could not dispatch org-changed to displayxr-website — the daily cron will catch up)"
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
  # Glob-and-test, NEVER `ls`: an `ls -F` alias appends a `*` classifier to the
  # filename, so `$(ls ...)` yields `...Setup-2.1.0.0.exe*` and every later use
  # of the path silently fails. (Documented from bundle v2.0.15; hit AGAIN on
  # shell v2.1.0 — this line was the last `ls` left in the skill.)
  SIGNED_EXE=""
  for f in _signed/*Setup-*.exe; do [ -f "$f" ] && { SIGNED_EXE="$f"; break; }; done
  [ -n "$SIGNED_EXE" ] || { echo "No signed installer in the artifact — ship unsigned"; SIGNED=no; }
fi

if [ "$SIGNED" = yes ]; then
  # The runner already fail-closed-verified Status=Valid AND signer=Leia.
  #
  # The delete is a SAFETY NET, not the normal path. Every component's NSI now names its
  # installer `<Name>Setup-${VERSION}.exe` — version only, NO build number — so the runner's
  # output has the SAME name as CI's and `--clobber` replaces it in place. This block then
  # finds nothing to delete and correctly prints its NOTE.
  # It stays because the failure it guards is silent and expensive: if a component ever
  # regresses to stamping a build number (the runner rebuilds with 0, CI stamps the run
  # number, so `Setup-2.0.4.0.exe` vs `Setup-2.0.4.1883.exe` never collide), --clobber cannot
  # replace the CI asset and the release ships a signed AND an unsigned installer side by
  # side. The asset-count assertion below is what actually catches that — trust it, not this.
  #
  # ASSET OPS TARGET $REL_REPO, NOT $REPO. For most components they are the
  # same repo, which is why this block long read `$REPO` — but `shell` is the
  # row where they differ: the release object lives on displayxr-shell-releases
  # and `gh release view` against displayxr-shell-pvt fails with "release not
  # found", so the delete + upload silently do nothing and the release keeps
  # its UNSIGNED CI installer. (Hit for real on shell v2.1.0.)
  #
  # startswith/endswith, NOT test("...\\.exe$"): that regex needs a \\ that survives only in
  # single quotes; one extra layer of double-quoting makes it \. , jq rejects it as an invalid
  # escape, $( ) yields empty, and the old `[ -n "$CI_EXE" ] &&` guard swallowed it silently.
  # (Hit for real on runtime v2.0.4.) Need a regex? Write [.] — no escaping required.
  CI_EXE=$(gh release view "$NEW_TAG" -R "$REL_REPO" --json assets \
             --jq '.assets[].name | select(contains("Setup-") and endswith(".exe"))' \
           | grep -v -F "$(basename "$SIGNED_EXE")" || true)
  if [ -n "$CI_EXE" ]; then
    echo "$CI_EXE" | while read -r a; do
      gh release delete-asset "$NEW_TAG" "$a" --yes -R "$REL_REPO"
    done
  else
    echo "NOTE: no CI installer asset found to delete — verify the release has exactly one .exe"
  fi
  gh release upload "$NEW_TAG" "$SIGNED_EXE" --clobber -R "$REL_REPO"

  # Never ship signed + unsigned together.
  N=$(gh release view "$NEW_TAG" -R "$REL_REPO" --json assets \
        --jq '[.assets[].name | select(contains("Setup-") and endswith(".exe"))] | length')
  [ "$N" = 1 ] || echo "WARNING: $N installer .exe assets on $NEW_TAG — expected exactly 1 (signed)."
fi
```

No local checkout, no local build, no Windows toolchain — the temp clone from
Phase 1 is only used for the tag; signing happens entirely on the runner.

---

## PHASE 3.6: WRITE THE RELEASE NOTES

Sibling CI creates the GitHub Release but writes **no body** — `softprops/action-gh-release`
is invoked without `body` / `body_path` / `generate_release_notes`, so every
component released through this skill shipped a 1-byte release until this phase
existed. (Unity is the exception: its workflow extracts the tagged section from
`CHANGELOG.md` into `--notes-file`.)

This matters beyond tidiness. The website's news pipeline
(`/sync-website news`) parses release bodies for a **`## Highlights` / `## Features`**
section to decide what reaches the homepage feed; an empty body is invisible to
it and has to be triaged by reading commits by hand.

### Step 3.6.1: Classify the existing body

There are four kinds of body, and only one of them is off-limits:

```bash
gh release view "$NEW_TAG" -R "$REL_REPO" --json body -q '.body' > "$WORK/body.md"
BODY_LEN=$(wc -c < "$WORK/body.md" | tr -d ' ')
FIRST_HEADING=$(grep -m1 '^#\{1,4\} ' "$WORK/body.md" || true)

if [ "$BODY_LEN" -le 40 ]; then
  NOTES_MODE=fresh      # empty — author the whole body
elif printf '%s' "$FIRST_HEADING" | grep -qi "what's changed"; then
  NOTES_MODE=prepend    # GH auto-notes only — add curated sections ABOVE them
elif [ -z "$FIRST_HEADING" ] && grep -qi 'auto-published from' "$WORK/body.md"; then
  # CI provenance stub, NOT an authored body: shell's publish workflow writes
  # "Auto-published from displayxr-shell-pvt @ <sha>" (~84b, no heading), which
  # the length+heading test scores as `keep` — so this phase would never fire
  # for shell and it shipped an uncurated release. (Hit for real on v2.1.0.)
  NOTES_MODE=prepend    # curated sections above; keep the provenance line below
else
  NOTES_MODE=keep       # someone authored this (unity CHANGELOG, hand-written)
fi
echo "notes mode: $NOTES_MODE (${BODY_LEN}b, first heading: ${FIRST_HEADING:-none})"

if [ "$NOTES_MODE" != keep ]; then
  # PREV_TAG is empty on a repo's very first release — log the whole history then.
  RANGE=${PREV_TAG:+$PREV_TAG..}$NEW_TAG
  git -C "$WORK/repo" log --no-merges --format='%h %s' "$RANGE"
fi
```

Why classify rather than just check length: the demo/mcp/leia workflows now set
`generate_release_notes: true`, so CI fills the body with GitHub's
`## What's Changed` PR-title list *before* this phase runs. A plain length guard
would read that as "notes already present" and this phase would never fire
again. GitHub always leads auto-notes with that heading, and neither unity's
CHANGELOG extract nor a hand-written body does — which is what makes the
first-heading test reliable.

The general rule the four cases encode: **`keep` means a human (or a CHANGELOG)
wrote prose here.** Machine-generated boilerplate — GitHub's PR list, a CI
provenance line — is never `keep`; it is context to preserve *below* curated
notes. When you meet a new CI stub that trips `keep`, add a case rather than
skipping the phase.

- `fresh`  → write the curated body.
- `prepend` → write curated sections, then append the existing auto-notes
  underneath. **Keep them**: the PR list is a genuinely useful "everything that
  changed" appendix below a curated summary, and it costs nothing.
- `keep`   → **never clobber a body someone else authored.** Skip the phase.

### Step 3.6.2: Author the notes  (skip if `NOTES_MODE=keep`)

Read the commit range and write it up. Mirror the runtime's `/release` format so
one parser reads every repo in the org:

```markdown
## Highlights
<Only when something user-visible landed. One short paragraph or 2–4 bullets,
written as what a user can now do — not as commit subjects.>

## Features
- <feature, with (#NN)>

## Fixes
- <fix, with (#NN)>
```

**Omit `## Highlights` entirely when the range is only fixes, chores, CI, or
dependency bumps.** This is the rule that matters most: the news detector treats
Highlights bullets as *claims of new capability*, so a Highlights section
containing "bumped deps" or "fixed a modal" actively poisons the feed — it turns
a candidate that should be auto-skippable into one a human has to read and
reject. A release with only `## Fixes` is a perfectly good release note, and the
pipeline correctly ignores it. Do not manufacture a highlight to fill the
heading.

Write to a file and attach — never inline a heredoc into `--notes`, since bodies
contain backticks and `$`:
```bash
# author the curated sections into "$WORK/notes.md" first, then:
if [ "$NOTES_MODE" = prepend ]; then
  printf '\n---\n\n' >> "$WORK/notes.md"
  cat "$WORK/body.md" >> "$WORK/notes.md"     # keep CI's ## What's Changed below
fi
gh release edit "$NEW_TAG" -R "$REL_REPO" --notes-file "$WORK/notes.md"
gh release view "$NEW_TAG" -R "$REL_REPO" --json body -q '.body' | head -20
```

---

## PHASE 4: WATCH THE DISPATCHED versions-bump RUN

**Skip this phase entirely when `FIELD` is empty** (e.g. `unity`, which has no
`versions.json` entry and dispatches no bump) — go straight to Phase 6:
```bash
[ -z "$FIELD" ] && { echo "No versions.json field for $COMPONENT — no bump to watch; skipping to report."; SKIP_BUMP=1; }
```

**browser — NO BUMP IS THE CORRECT OUTCOME for an Android-only preview.** Do not report
it as a failed or missing bump. `publish-browser-releases.yml` gates its dispatch on a
Windows installer actually shipping (`steps.locate.outputs.exe != ''`), because
`versions.json[browser]` means *"the version the orchestrator can install as a released
asset"*, and `setup-displayxr.bat --with browser` resolves it through `components.sh`'s
`DisplayXR-Browser-Preview-Setup-*.exe` glob. An APK-only release cannot satisfy that
glob, so the pin deliberately stays behind — real and already live: `preview-0.1.24`
shipped Android-only and the pin correctly still names `preview-0.1.23`. **Never
hand-bump the pin to match the newest tag**; it points `--with browser` at a release
with no installer. Decide from the assets, not from the absence of a bump run:
```bash
if [ "$COMPONENT" = browser ]; then
  HAS_EXE=$(gh release view "$NEW_TAG" -R "$REL_REPO" --json assets \
              --jq '[.assets[].name|select(contains("Setup") and endswith(".exe"))]|length')
  if [ "$HAS_EXE" = 0 ]; then
    echo "Android-only preview — no Windows installer, so NO versions.json bump is expected."
    echo "versions.json[browser] intentionally stays behind $NEW_TAG. Do not bump it by hand."
    SKIP_BUMP=1
  fi
fi
```
Spec: `docs/specs/runtime/versions-json-autobump.md` §"The browser pin LAGS the newest
release on purpose".

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
elif [ "$COMPONENT" = browser ] && [ "${SKIP_BUMP:-0}" = 1 ]; then
  echo "versions.json[browser] = $PINNED, intentionally behind $NEW_TAG (Android-only preview — see Phase 4)."
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

**browser — the report must state the update-feed status.** "Published successfully" is
false-in-effect for the browser if the feed did not move: the GitHub release exists and
no existing install will ever be offered it. Include the feed line from the "Browser is
special" check above, and if it is stale say so in the summary rather than in a footnote.

```
Release $NEW_TAG published successfully!

Component:   $COMPONENT  ($REPO)
CI:          run $RUN_ID — $CI_CONC
Release:     https://github.com/$REL_REPO/releases/tag/$NEW_TAG
Notes:       [written → "curated notes written (Highlights + N features / M fixes)"]
             [no-highlights → "notes written — Fixes only, no Highlights section (nothing user-visible shipped)"]
             [kept → "left CI's own notes alone (${BODY_LEN}b already present)"]
Signing:     [signed → "installer built + signed on the signing runner (full chain incl. uninstaller, run $SIGN_RUN), re-uploaded over the CI asset"]
             [none   → "⚠ UNSIGNED — signing runner unreachable / earthview macOS .pkg; ships the unsigned CI asset"]
             [unity  → "displayxr_unity.dll signed + re-injected into the .tgz + upm branch" | "⚠ unity ships unsigned"]

[unity only — Version (in-tree, no versions.json):
  package.json version = $VER on displayxr-unity/main   (marker "Release $NEW_TAG")
  [if UNITY_MAIN_RECONCILED=yes: "⚠ tag was cut directly on displayxr-unity — main was behind; pushed a post-tag reconcile bump. Prefer cutting unity releases THROUGH this skill so the bump + CHANGELOG land in the release marker."]
  website: org-changed dispatched → /platform-support regenerates the version]

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

STOP.

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
- **Private-source / public-release split (shell, browser):** `REPO` is where the
  tag goes and whose CI is watched; `REL_REPO` is where the release object and its
  assets live. Every `gh release` call must use `$REL_REPO`. The browser row was
  added 2026-09 when `displayxr-browser` split into `displayxr-browser-pvt` (source)
  + `displayxr-browser` (releases, name kept); before that the browser released
  through its own `release.sh` and was documented as NOT using this skill.
- **New sibling repo joining the family?** Add a row to the component→config
  map above, add a `versions.json` field on runtime, add the
  `DispatchVersionsBump` step to the new repo's CI per
  `docs/specs/runtime/versions-json-autobump.md` §"Sibling-side snippets".
- **In-tree version = the marker must bump it.** Most components' version lives
  in `versions.json` (dispatch-bumped) or is derived from the tag at build time,
  so an *empty* release marker is correct. But a component whose version of
  record is a committed file — unity's `package.json` `version`, unreal's
  `.uplugin` `VersionName` — must have that file **bumped in the release
  marker**, or the website's org-sync generator (which reads those files on
  `main`) shows the previous version even though the release asset is current.
  The unity path does this (Phase 2 bump + Phase-3.5 reconcile + website
  dispatch); replicate the pattern for any future in-tree-versioned component.
  (Unreal is released by its own on-Windows `/release`, not this skill — but the
  same bump-on-release rule applies there. Website generator source of truth:
  `displayxr-website/scripts/sync-org.mjs` + `docs/org-sync.md`.)
- Tags are sticky. Deleting a tag also deletes its GH Release. Prefer
  fixing forward with a patch release.
- **Release notes feed the website's news feed — that's why PHASE 3.6 exists.**
  `/sync-website news` parses release bodies for a `## Highlights` / `## Features`
  section to decide what reaches the homepage "What's New" ticker. Before 3.6,
  every component released through this skill shipped an empty body (verified
  2026-08-08 across all 5 demos + mcp + leia-plugin), so the news pass had to
  read raw commits to triage anything. Two consequences worth keeping in mind:
  (1) the heading names are a **contract** with
  `displayxr-website/scripts/sync-org.mjs` — don't invent new ones; (2) a
  `## Highlights` section is read as a claim of new capability, so leaving it
  out of a fixes-only release is the *correct* behavior, not laziness.

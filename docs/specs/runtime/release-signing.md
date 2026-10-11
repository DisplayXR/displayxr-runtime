# Release code-signing

How DisplayXR releases get Authenticode-signed, and how to keep releasing when
the signer is unavailable or changes. This is the contract the `/release`,
`/dxr-release`, and `/installer-release` skills implement.

## Two tiers: unsigned CI, signed release

- **Contributor / CI builds are unsigned, by design.** GitHub-hosted CI holds
  no certificate and no secret. Every PR, push, and even tag build produces
  **unsigned** installers. Contributors never need signing access.
- **Releases are signed by a signing provider** — a repo that owns the code-
  signing certificate on a self-hosted runner and exposes it through two
  `workflow_dispatch` workflows (below). The release skills dispatch it,
  download the signed artifact, and replace the unsigned CI asset on the GitHub
  Release. **Signing never gates publishing** — if the provider is unreachable
  the release still ships the unsigned CI installer, with a warning.

The provider repo is supplied **out-of-band** via the `DXR_SIGN_REPO` local env
var (`export DXR_SIGN_REPO=<owner/repo>` on the release machine). The public
repos hardcode **no** provider path — unset the env var and releases ship
unsigned. (The active provider is a private repo owning a DigiCert EV cert.)

## Why signed releases rebuild (not re-sign the CI `.exe`)

Smart App Control checks the **DLLs extracted from the installer at load time**,
so the inner binaries must be signed **before** NSIS packs them — re-signing the
finished `.exe` is not enough. And the two-pass signed NSIS uninstaller needs
`makensis` to run on the machine that holds the cert. Both are only possible
where the cert lives, so a **component** signed release is produced by rebuilding
the tagged commit *on the signing runner* with a runner-local `SIGN_CMD`. The
**bundle** is the exception: it wraps already-signed child installers, so its
outer `.exe` only needs a single-file sign (no rebuild).

## The provider contract

A signing provider is any repo `$DXR_SIGN_REPO` implementing these two
`workflow_dispatch` workflows. Point the skills at a different provider by
setting `DXR_SIGN_REPO=<owner/repo>` in the release box's environment — that is
the entire "switch signers" operation.

### 1. `build-signed-release.yml` — build + sign a component (used by `/release`, `/dxr-release`)

- **Inputs:** `component` (`runtime|leia-plugin|mcp|gauss|modelviewer|mediaplayer|avatar`),
  `repo` (`owner/name`), `ref` (the tag, e.g. `v1.6.0`).
- **Behavior:** checkout `repo@ref` on a Windows runner that has the cert; set
  `SIGN_CMD` to the runner-local signer; run that component's own signed build
  (full chain: inner binaries → installer `.exe` → two-pass uninstaller);
  fail-closed verify `Status=Valid` **and** signer matches the expected subject.
- **Output:** artifact **`signed-installer`** containing `*Setup-*.exe`.
- Reference sketch: staged locally at `C:\displayxr-signing\build-signed-release.yml`
  (gitignored — it is committed only into the provider repo, never here).

### 2. `sign-artifact` — sign a folder of finished files (used by `/installer-release`, ad-hoc)

- **Inputs:** `release_tag` — a temporary prerelease on `$DXR_SIGN_REPO` carrying
  `unsigned.zip` (the files to sign).
- **Behavior:** sign every unsigned binary in the zip with the runner-local
  signer; verify.
- **Output:** artifact **`signed`** containing `signed.zip` (same layout, signed).
- This is what `C:\displayxr-signing\sign-hook.sh` wraps for local use.

## Concurrency — identify the signing run, never take "the newest"

Both workflows serialise on a single concurrency group per workflow that keeps **one
running and one waiting** run, and every release on every component dispatches onto the
same provider. A new dispatch therefore **cancels whichever run is still waiting**, and
under parallel releases "the newest `workflow_dispatch` run created after I dispatched"
is routinely **another component's** run. The provider runs expose no inputs and a bare
`display_title` (just the workflow name), so nothing in the run listing says whose it is.

**Incident, 2026-10-11:** five `/dxr-release` demo releases ran in parallel. Gauss's first
dispatch was cancelled within 2 s by the next component's; the newest-run lookup resolved
gauss to mediaplayer's run; the `*Setup-*.exe` download glob matched mediaplayer's
installer, and `gh release upload --clobber` would have published it on the gauss
release. Only an agent's manual look at the downloaded file name prevented it.

The skills therefore follow three rules:
1. **Track the run `gh workflow run` prints** (recent `gh` prints the new run's URL). Only
   if none is printed, accept a run created since the dispatch that provably carries the
   caller's marker: the job's first step name `Checkout <repo> @ <ref>`
   (`build-signed-release.yml`), or the `release_tag` echoed in the completed run's log
   (`sign-artifact`).
2. **A `cancelled` run was displaced, not failed.** Wait until the workflow has no
   non-`completed` run, re-dispatch **once**, and track the new run. Never fall back to
   "newest" after a cancellation.
3. **Assert artifact identity before uploading.** `build-signed-release.yml`: the artifact
   holds exactly `<Name>Setup-<version>.exe` and the bare version occurs in it (NSIS
   VERSIONINFO, UTF-16LE). `sign-artifact`: the returned file is the payload that was sent
   plus a signature (identical past the PE header, and larger). Anything else ships the
   unsigned CI asset with a flag; signing still never gates publishing.

A provider that wants to make rule 1's fallback cheaper can add `run-name:` with the
component and ref to its workflows; the skills do not depend on it.

## Capability gate + fallback (losing the box)

Each skill probes reachability before signing and degrades gracefully:

```bash
SIGN_REPO="${DXR_SIGN_REPO}"   # local env only; the public repo names no provider
if [ -n "$SIGN_REPO" ] && gh workflow view build-signed-release.yml -R "$SIGN_REPO" >/dev/null 2>&1; then
  SIGNED=yes          # provider reachable → sign
else
  SIGNED=no           # provider gone → ship the unsigned CI installer, warn
fi
```

So there are three ways to keep releasing:
1. **Lose the box, ship unsigned** — do nothing; the gate fails and the release
   publishes with the unsigned CI installer (the report flags it `UNSIGNED`).
2. **Point at another provider** — stand up the two workflows above on any repo,
   set `DXR_SIGN_REPO=<that repo>`. No skill edits.
3. **Local Windows fallback** — on a Windows box that has the cert locally, the
   pre-runner path still works (`build_windows.bat all` with `SIGN_CMD` set,
   full-chain including uninstaller). `/release` keeps this as an explicit
   fallback step; it's the only path that needs no provider at all.

## Runner prerequisites (for whoever hosts the provider)

Runner labelled `[self-hosted, windows, signing]` with: VS 2022 (C++), Ninja,
NSIS, Vulkan SDK, GitHub CLI; the box-local EV signer exposed as the `SIGN_CMD`
secret; `SR_PATH` (Simulated Reality SDK) for the leia-plugin build; a
`COMPONENT_READ_PAT` secret only if signing a **private** component repo. macOS
`.pkg` signing (Apple Developer ID + notarization) is a separate track, not
covered by the Windows runner.

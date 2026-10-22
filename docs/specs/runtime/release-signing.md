# Release code-signing

How DisplayXR releases get Authenticode-signed, and how to keep releasing when
the signer is unavailable or changes. This is the contract the `/release`,
`/dxr-release`, and `/installer-release` skills implement.

## Two tiers: unsigned CI, signed release

- **Contributor / CI builds are unsigned, by design.** GitHub-hosted CI holds
  no certificate and no secret. Every PR, push, and even tag build produces
  **unsigned** Windows installers. Contributors never need signing access.
  (The macOS `.pkg` is the one exception: canonical-repo `v*` tag builds sign
  and notarize it in CI from org-level secrets — see *macOS track in CI*.)
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
covered by the Windows runner — see below.

## macOS: Developer ID + notarization (#280)

One script, `scripts/macos_sign_notarize.sh`, does the whole macOS chain and is
meant to be reused verbatim by every repo that ships a `.pkg` (runtime, MCP,
shell, the five demos, and the bundle last). Like the Windows side it is
**driven entirely by the environment**; nothing in the public repo names a
certificate or a key.

| Env var | Effect when unset |
|---|---|
| `DXR_MACOS_APP_IDENTITY` (`Developer ID Application: …`) | ad-hoc `-` signature — today's contributor / fork-PR behaviour |
| `DXR_MACOS_INSTALLER_IDENTITY` (`Developer ID Installer: …`) | `.pkg` stays unsigned |
| `DXR_NOTARY_PROFILE` (notarytool keychain profile) **or** `DXR_NOTARY_KEY_PATH` / `_KEY_ID` / `_ISSUER` (App Store Connect API key) | signed but not notarized |
| `DXR_MACOS_KEYCHAIN` | default keychain search list |
| `DXR_MACOS_NOTARIZE=0` | (set) sign only, skip notarization |

Modes: `sign-tree <dir> [--entitlements-map <file>]`, `sign-pkg`, `notarize`,
`staple`, `finalize-pkg` (sign-pkg → notarize → staple, each only when its
credentials are present), `verify-pkg`, `status`, and the CI modes
`ci-secrets-check`, `ci-keychain-setup`, `ci-keychain-teardown` (below).

**Rules the script enforces** (each one cost a rejected notarization or a
SIGKILL somewhere before):
- **Sign last.** Every `install_name_tool` invalidates a signature, and an
  invalid signature on arm64 is SIGKILL at load. The ad-hoc re-signs after each
  patch keep the intermediate tree loadable; `sign-tree` runs once, after all
  patching, immediately before `pkgbuild`.
- **Innermost first, each file individually:** dylibs and bundles, then
  executables, then `.app`s deepest first. `--deep` is used only to verify.
- **`--options runtime --timestamp`** on every Mach-O; `sign-tree` refuses to
  finish if either is missing.
- **Entitlements by map**, never blanket. `installer/macos/entitlements/map`
  gives `com.apple.security.cs.disable-library-validation` to the executables
  that load vendor plug-ins signed by other Team IDs (`displayxr-service`,
  `displayxr-cli`, the in-process test app). The runtime dylib gets none:
  library validation inside a host app is the host's choice.
- **No `DYLD_*` under the hardened runtime.** dyld strips them, so a launcher
  that relied on `DYLD_LIBRARY_PATH` breaks once signed. Fix the binary's
  rpath instead (as `create_app_bundle.sh` now does); do not reach for
  `allow-dyld-environment-variables`.
- `finalize-pkg` fails unless notarytool says `Accepted` (printing
  `notarytool log <id>` otherwise), and unless `spctl --assess --type install`
  reports `source=Notarized Developer ID` after stapling.

`installer/macos/build_installer.sh` calls `sign-tree` on the runtime payload
and on `DisplayXRCube.app`, then `finalize-pkg` on the product archive. With
no identities in the environment it produces the same unsigned `.pkg` with
ad-hoc-signed Mach-Os as before, so CI on GitHub-hosted runners (and every
fork PR) stays green with no secrets.

**Local signed build** (a Mac whose login keychain holds both identities and a
notarytool profile):
```bash
export DXR_MACOS_APP_IDENTITY="Developer ID Application: <Org> (<TEAM>)"
export DXR_MACOS_INSTALLER_IDENTITY="Developer ID Installer: <Org> (<TEAM>)"
export DXR_NOTARY_PROFILE=<profile>
./scripts/build_macos.sh --installer      # or: installer/macos/build_installer.sh <artifact-dir> <out.pkg>
```

### macOS track in CI — signed on the tag build itself

Unlike Windows (EV cert on a self-hosted box, so `/release` re-signs after CI),
the macOS identities are ordinary `.p12` files plus an App Store Connect API
key, so the **GitHub-hosted** `BuildInstaller` job in `build-macos.yml` signs
and notarizes the release `.pkg` directly. No provider repo and no
`DXR_SIGN_REPO` are involved.

**Gate** — all three must hold, otherwise the job builds the unsigned `.pkg` it
always did and stays green:
1. `github.repository == DisplayXR/displayxr-runtime` (forks never sign);
2. the ref is a `v*` tag (PRs and `main` pushes never sign);
3. every org-level secret below is non-empty (`ci-secrets-check`; absence is a
   `::notice::`, not a failure — they are being provisioned one by one).

| Org secret | Format |
|---|---|
| `MACOS_APP_CERT_P12` / `MACOS_APP_CERT_PASSWORD` | base64 of the `Developer ID Application` `.p12` (`base64 -i app.p12`) / its export password |
| `MACOS_INSTALLER_CERT_P12` / `MACOS_INSTALLER_CERT_PASSWORD` | base64 of the `Developer ID Installer` `.p12` / its export password |
| `APPLE_TEAM_ID` | 10-char Team ID; both identities must carry `(<TEAM>)` |
| `APPLE_NOTARY_KEY_P8` | the raw `AuthKey_<id>.p8` text, BEGIN/END lines included |
| `APPLE_NOTARY_KEY_ID` / `APPLE_NOTARY_ISSUER_ID` | that API key's Key ID / the App Store Connect Issuer ID |

**Flow** (`build-macos.yml`, `BuildInstaller`):
1. `ci-keychain-setup --env-out "$GITHUB_ENV"` — a throwaway keychain under
   `$RUNNER_TEMP` with a random password, both `.p12`s + Apple's Developer ID
   intermediates imported, `set-key-partition-list` so codesign/productsign
   need no prompt, the `.p8` written `0600`; exports the `DXR_*` variables of
   the table above. The secrets are in the environment of this one step only.
2. Build with `DXR_MACOS_INSTALLER_IDENTITY` withheld: `sign-tree` gives every
   payload Mach-O its Developer ID + hardened-runtime signature as the last step
   after all patching, and the product archive comes out unsigned.
3. A separate step keeps a copy of that unsigned `.pkg`, runs `finalize-pkg`
   (productsign → `notarytool submit --wait` with the API key → staple) and
   `verify-pkg` (`pkgutil --check-signature`, `stapler validate`,
   `spctl --assess --type install` = `source=Notarized Developer ID`).
4. The upload + release-attach steps publish whatever `_package/` holds.
5. `ci-keychain-teardown` (`if: always()`) restores the search list and
   deletes the keychain and key files.

**Fallback — signing never gates publishing.** If identity import fails, the
job builds unsigned. If Developer ID signing of the payload fails (e.g. the
timestamp server is unreachable), `build_installer.sh` falls back to the ad-hoc
tree (`DXR_MACOS_SIGN_FALLBACK=adhoc`) and the notarize step is skipped. If
productsign / notarization / stapling / verification fails, the saved unsigned
`.pkg` is restored. Each of these emits a `::warning::` and the job stays green;
the job summary states which `.pkg` shipped. To retry, re-run the macOS job on
the tag; `softprops/action-gh-release` replaces a same-named asset. `/release` reports the outcome by checking the
attached `.pkg` with `pkgutil --check-signature` (Phase 6).

Rollout (#280): runtime first (this section), then the other `.pkg`-shipping
repos (MCP, shell, demos) reuse the same script + secret names, bundle last.

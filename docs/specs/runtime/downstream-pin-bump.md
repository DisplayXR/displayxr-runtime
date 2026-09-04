---
status: Active (runtime-pin-bump.yml + pin-rot-canary.yml)
owner: David Fattal
---
# Downstream pin bump + pin-rot canary

## Problem

Two pin classes exist downstream of this repo, and **both only move by hand**:

1. **Runtime-tag pins.** A vendor plug-in builds against a pinned runtime tag
   (`DXR_RUNTIME_GIT_TAG` in `CMakeLists.txt`, `RUNTIME_REF` in its
   `build-windows.yml`). When the runtime ships a new ABI slot, the plug-in must
   repin or the slot compiles out and the runtime silently degrades — exactly what
   the append-only D3D11 slot 20 `set_window` needed (runtime v2.7.0 +
   leia-plugin v2.3.0, both repinned manually).
2. **Third-party SDK pins.** `humbletim/install-vulkan-sdk` with a hardcoded
   version, in every demo repo, the Leia plug-in, and the vendor template.

Today the loop runs one way only: a sibling releases, dispatches
`versions-bump`, and this repo records the new pin in `versions.json`. Nothing
pushes the other way. `build-windows.yml` even documents the manual step in prose
("rebuild the leia plug-in against runtime `<tag>` … tag a new release") — that
comment is the gap this spec closes.

Both classes failed in the same week: the Vulkan pin rotted org-wide (LunarG
deleted the 1.3.283.0 installer; six repos' Windows CI broke mid-release-train),
and the runtime-tag pin needed a hand-written script to move.

## Why CI, not the `/release` skill

The skill only runs when a human types `/release` from a checkout. CI fires on the
tag regardless of who cut it, and the `versions.json` bump already lives in CI —
putting this in the skill would duplicate a working mechanism and skip any tag
pushed another way. The skill's role is to **watch and report** the dispatches, as
it already does for `BumpVersionsJsonOnTag`.

## Mechanism

**`downstream-pins.json`** at the repo root — a reviewable manifest declaring,
per repo, every location a pin lives. Centralized on purpose (mirroring how
`versions.json` is pushed into `displayxr-installer`): one implementation to fix,
and onboarding a new vendor repo is a manifest entry, not a new workflow in their
repo.

Each location carries the file, the key, and a `track`. **Not every location
should auto-bump** — `displayxr-leia-plugin` deliberately holds its Linux pin
behind its Windows pin, so a location may be marked `manual` and the bumper must
leave it alone. Where a repo's CI asserts two locations are *equal* (leia's
self-check compares workflow `RUNTIME_REF` against CMake `DXR_RUNTIME_GIT_TAG`),
the manifest lists both and the bumper writes them in **one commit** — updating
one alone breaks that repo's CI.

**`runtime-pin-bump.yml`** — on a canonical `v*` tag, for each manifest entry:
rewrite the auto-tracked locations, push `chore/runtime-pin-<tag>`, open a PR.

- **PR, never a direct commit.** `versions.json` may direct-commit because it
  *records* a pin; a source pin change alters what compiles, so downstream CI must
  gate it and a human merges. If the bump genuinely breaks a plug-in, the PR sits
  red and harms nothing.
- **Branch name carries the tag** — a fixed branch name strands a stale PR and
  silently attaches the next run to it (the website editorial sync hit exactly
  this).
- **Idempotent**: already at the tag → exit 0, no PR, no noise.
- Auth: the `create-github-app-token` publish-bot App, as `versions-bump.yml` uses.

### Bump policy: ABI-gated, never tag-chasing

**Do not bump a runtime-tag pin just because a newer tag exists.** In a vendor
plug-in the pin is load-bearing twice over: `installer/CMakeLists.txt` *derives*
`MIN_RUNTIME_VERSION` from `DXR_RUNTIME_GIT_TAG` by regex, so raising the pin
raises the installer's minimum-runtime floor. Chasing every patch release would
make a plug-in installer refuse a runtime it works perfectly well against
(`exit 5` = "runtime below the ABI floor"), for no gain.

The gate is therefore the plug-in ABI, not the version number: compare
`XRT_PLUGIN_API_VERSION_CURRENT` at the new tag against its value at the tag the
downstream repo currently pins.

| ABI at new tag vs pinned tag | Action |
|---|---|
| unchanged (typical patch release) | no PR — nothing to gain, floor raise is pure cost |
| changed (a slot was appended) | open the PR — the new slot is worth pinning to |

This is the same comparison `scripts/check_plugin_abi.py` already performs in the
opposite direction, so the gate reuses it rather than reimplementing the notion of
ABI compatibility.

**Derived values are not pins.** `installer/CMakeLists.txt` computes its floor from
the pin; a bumper that edits it too would be writing to a value CMake overwrites.
The manifest lists only real pins, and every derived consumer stays out of it.

**`pin-rot-canary.yml`** — scheduled, reads the same manifest's SDK pins and, for
each, HEAD-checks the download URL **and asserts the extracted archive still
contains `Include/` and `Lib/`**. Both halves are load-bearing: LunarG repackaged
the installer at 1.4.313.0 so newer versions extract `Bin/` only, and the action's
own `glslangValidator --version` check still reports green while the build dies
much later at `find_package(Vulkan)`. A URL check alone would therefore pass a
pin that cannot build. Failure updates ONE tracking issue rather than opening a
new one per run.

## Consumer floors — the axis the pins miss

A build pin answers *"what did this repo compile against?"*. It says nothing
about a consumer that never compiles against the runtime at all — the browser
and the engine plug-ins reach it purely over the OpenXR wire, and their real
coupling is the set of extension `SPEC_VERSION`s they were built for.

That gap was not theoretical. `displayxr-browser` hand-typed
`MIN_RUNTIME_VERSION "2.2.3"` in its NSIS while actually requiring
`XR_DXR_weave` spec 8, which first shipped in runtime **v2.8.0**. Every user
between those two versions was told the prerequisite was satisfied and got a
browser whose weave path could not work — the precise failure the check was
added (browser#68) to prevent, reintroduced by the check's own literal going
stale.

`downstream-pins.json`'s `consumer_floors` block closes it, and
`scripts/drift_audit.py::check_consumer_floors` enforces it weekly:

1. **Resolve what each consumer requires.** Scan its `spec_sources` for
   `#define XR_DXR_<ext>_SPEC_VERSION <n>`, live, on every run.
2. **Can the runtime still serve it?** Compare against the runtime's current
   header. A consumer needing a spec the runtime has dropped, or has not
   reached, is a finding.
3. **Derive the true floor.** Binary-search the runtime's release tags for the
   earliest one shipping each required spec; the newest of those is the floor.
4. **Compare against what the consumer advertises.** Under-declaring is a
   finding (users get a broken install told it is fine); over-declaring is only
   a note (needless upgrades, nobody breaks).

### The rule that keeps it honest

**Record where to look, never the version numbers.** The manifest holds file
paths; the numbers are re-read from the consumer's own headers every run, so
the manifest cannot drift from them the way the NSIS literal did. The one
exception is the browser, which vendors no header *file* — it is a
patch series, so `XR_DXR_weave.h` exists only after the patches apply. Its
number is written down, and the audit cross-checks it against prose in
`patches/README.md` (`requires_anchor`), emitting a note when it cannot
confirm it. Since the 2026-09 repo split its key is **`displayxr-browser-pvt`**:
the audit reads the patch series and the NSIS floor, and both live in the private
source repo (the public `displayxr-browser` keeps only releases and assets). That
read needs the publish-bot App token `drift-audit.yml` already mints for
`displayxr-shell-pvt`; on the default `GITHUB_TOKEN` the consumer reads as
unfetchable and is reported, not silently skipped.

Two consequences worth knowing:

- **The binary search assumes `SPEC_VERSION`s never decrease** across
  releases. That is the intended contract and has held for every `XR_DXR_*`
  extension; walking one back would make the search report a wrong floor.
- **`gate` decides how loud a violation is.** `hard` (the browser: its
  installer refuses or chains an upgrade) is worth acting on. `soft` (the
  engine plug-ins: name-based detection, degrade with a warning) is worth
  reporting. Vendor plug-ins are deliberately **excluded** — they have a build
  pin their installer derives its minimum from, so `runtime_tag_pins` already
  covers them and listing them here would double-report.

### Behavioural floors — the one thing spec derivation cannot see

Derivation reads extension `SPEC_VERSION`s, so it is blind by construction to a
runtime change that bumps no spec but that a shipped consumer still requires.
That is not a bug in the audit; it is a limit of what specs express. Real case
(runtime#1347): runtime#1336 routes an opaque present-owner to `CLIENT_TEXTURE`
in the service, touching no extension header at all — and the opaque browser
(browser-pvt#2) does not present correctly without it. Its true floor is
**v2.16.3**, the first release carrying #1336, and nothing in any header says so.

A consumer entry may therefore carry a `behavioural_floor`:

```json
"behavioural_floor": {
  "min_runtime": "v2.16.3",
  "commit": "9399479b4...",
  "why":  "the opaque present-owner path ...",
  "ref":  "runtime#1336 = commit above, first released in v2.16.3"
}
```

The audit folds it into the same `max()` as the derived floors, so the advertised
minimum is checked against whichever is stricter. This **deliberately breaks the
rule above** — it hardcodes a version — and the reason it is safe to is the reason
the rule exists: a number copied from a header drifts from the header, but a
behavioural floor has no header to drift from. It is a fact about which release
first carried a behaviour, and unchanging once the release is cut — **so the audit
checks it rather than trusting it.** `commit` is required, and on every run the
audit asks the GitHub compare API whether that commit is identical to or behind
the `min_runtime` tag (the checkout-free equivalent of `git merge-base
--is-ancestor`). A missing `commit`, a tag or commit that does not exist, or a
commit that is not an ancestor of the tag is a `consumer-floor-unverifiable`
finding and the floor is **not** applied — loud, never silently trusted. That is
what stops the hand-typed number from becoming the very "true when written" claim
the rest of this file is designed to avoid. Keep the entry to the behaviours a
consumer genuinely cannot run without.

## Invariant

A pin that no job verifies is a pin that rots silently until a release train
trips over it. Every pin in the manifest is either auto-bumped or
canary-checked — and every consumer floor is re-derived rather than trusted.

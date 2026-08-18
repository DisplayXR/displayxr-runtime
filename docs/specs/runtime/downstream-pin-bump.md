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

## Invariant

A pin that no job verifies is a pin that rots silently until a release train
trips over it. Every pin in the manifest is either auto-bumped or canary-checked.
